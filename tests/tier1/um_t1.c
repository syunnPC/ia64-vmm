/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Unit-test machine (see um_t1.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/um_t1.h"
#include "tests/tier1/hostwin_linux.h"
#include "engine/tier1/exec.h"
#include "engine/tier1/x86enc.h"
#include "engine/hostwin/hostwin.h"
#include "engine/cache/validity.h"
#include "engine/interp/interp.h"

static struct hostwin_vcpu *hw;
#define MAX_CAP 4096
static struct um_capture *caps;
static struct umt_run *cur;

static struct {
    struct vcpu *v;
    struct t1_vcpu *tv;
} bound[16];

static void observer(struct t1_vcpu *tv, const struct vcpu *v, const struct t1_restore_record *r,
                     void *arg)
{
    (void)tv;
    (void)arg;
    if (!cur)
        return;
    ++cur->records;
    if (r->forced)
        ++cur->forced;
    if (r->path == T1_PATH_TRAP)
        ++cur->traps;
    if (r->path == T1_PATH_MISS_STUB)
        ++cur->misses;
    if (!r->materialized) {
        ++cur->retries;
        return;
    }
    ++cur->materialized;
    UM_CHECK(r->retired == v->retired);
    if (cur->run.ncaptures < cur->run.max_captures) {
        struct um_capture *c = &cur->run.captures[cur->run.ncaptures++];
        c->v = *v;
        c->data_hash = um_data_hash();
        c->point = r->point_id;
        c->forced = r->forced;
    }
}

void umt_init(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    um_machine_init();
    UM_CHECK(hostwin_linux_init());
    hostwin_set_trap_client(t1_trap_client);
    code_validity_set_track_hook(hostwin_code_page_event);
    struct hostwin_config c = {40, HOSTWIN_MAX_SLOTS, true}; /* with the completeness check */
    hw = hostwin_create(&c, hostwin_linux_backend(), hostwin_linux_ctx());
    if (!hw) {
        c.w = 36;
        hw = hostwin_create(&c, hostwin_linux_backend(), hostwin_linux_ctx());
    }
    UM_CHECK(hw != NULL);
    UM_CHECK(t1_init(&(struct t1_config){0}));
    caps = calloc(MAX_CAP, sizeof(*caps));
    UM_CHECK(caps != NULL);
}

struct hostwin_vcpu *umt_hostwin(void)
{
    umt_init();
    return hw;
}

struct t1_vcpu *umt_tv(struct vcpu *v)
{
    umt_init();
    for (unsigned k = 0; k < 16; ++k)
        if (bound[k].v == v)
            return bound[k].tv;
    for (unsigned k = 0; k < 16; ++k)
        if (!bound[k].v) {
            bound[k].v = v;
            bound[k].tv = t1_vcpu_create(v, hw);
            UM_CHECK(bound[k].tv != NULL);
            return bound[k].tv;
        }
    UM_CHECK(!"too many Tier1 vCPUs");
    return NULL;
}

void umt_release(struct vcpu *v)
{
    for (unsigned k = 0; k < 16; ++k)
        if (bound[k].v == v) {
            t1_vcpu_destroy(bound[k].tv);
            bound[k].v = NULL;
            bound[k].tv = NULL;
        }
}

uint32_t umt_compile(struct vcpu *v, uint32_t flags, uint16_t pin_element, uint8_t pin_reg,
                     struct t1_compile_report *rep, const char *label)
{
    struct t1_vcpu *tv = umt_tv(v);
    struct t1_compile_options opt = {flags, pin_element, pin_reg};
    struct t1_compile_report local;
    if (!rep)
        rep = &local;
    enum t1_compile_status st = t1_compile(tv, &opt, rep);
    t1_test_set_compile_options(tv, &opt);
    if (st != T1_COMPILED) {
        fprintf(stderr, "%s: compile status %d: %s at %#llx:%u\n", label, st,
                rep->reject_reason ? rep->reject_reason : "?", (unsigned long long)rep->reject_ip,
                rep->reject_slot);
        return 0;
    }
    return rep->region_id;
}

unsigned umt_case(struct vcpu *v, struct vcpu *ref, const struct um_snapshot *snap, uint32_t flags,
                  uint64_t budget, uint32_t region, const struct umt_arm *arm, struct umt_run *out,
                  bool expect_diff, const char *label)
{
    struct t1_vcpu *tv = umt_tv(v);
    um_snapshot_restore(snap, v);
    t1_vcpu_state_restored(tv);
    struct t1_compile_options opt = {flags, 0, 0};
    t1_test_set_compile_options(tv, &opt);
    if (arm && arm->point != UINT32_MAX) {
        if (arm->corrupt && !arm->force_too) {
            UM_CHECK(
                t1_test_corrupt(tv, region, arm->point, arm->occurrence, arm->element, arm->mask));
        } else {
            UM_CHECK(t1_test_force(tv, region, arm->point, arm->occurrence, arm->action,
                                   arm->before, arm->before_arg));
            if (arm->corrupt)
                UM_CHECK(t1_test_corrupt(tv, region, arm->cpoint, arm->coccurrence, arm->element,
                                         arm->mask));
        }
    }
    memset(out, 0, sizeof(*out));
    out->run.captures = caps;
    out->run.max_captures = MAX_CAP;
    cur = out;
    t1_test_set_restore_observer(observer, NULL);
    t1_test_set_store_observer(NULL, NULL);
    t1_test_set_entry_hook(NULL, NULL);
    if (arm && arm->before_run)
        arm->before_run(tv, v, arm->run_arg);
    uint64_t start = v->retired;
    out->exit = t1_run(tv, budget);
    while (arm && arm->resume_requested && out->exit.reason == VCPU_EXIT_DEBUG &&
           v->retired - start < budget) {
        ++out->resumed;
        out->exit = t1_run(tv, budget - (v->retired - start));
    }
    cur = NULL;
    if (arm && arm->final_only)
        out->run.ncaptures = 0;
    /* A test-injected exit request is pending in the captures taken before
     * the interpreter consumed it; the reference has none. */
    if (arm && arm->resume_requested)
        for (unsigned k = 0; k < out->run.ncaptures; ++k)
            out->run.captures[k].v.requested_exit = 0;
    out->nexits = t1_exit_count(tv);
    out->run.entries = out->nexits;
    UM_CHECK(out->run.ncaptures < MAX_CAP);
    unsigned d = um_compare(snap, ref, &out->run, v, &out->exit, budget,
                            expect_diff ? "(expected difference)" : label);
    if (d && !expect_diff) {
        fprintf(stderr, "%s: budget %llu, %u exits, %u captures\n", label,
                (unsigned long long)budget, out->nexits, out->run.ncaptures);
        UM_CHECK(!"Tier1 differs from interp_run");
    }
    return d;
}

bool umt_no_stacked_home(const uint8_t *code, size_t size, const char *label, unsigned *image)
{
    const int64_t gr_lo = (int64_t)offsetof(struct vcpu, s.gr) + 8 * 32;
    const int64_t gr_hi = (int64_t)offsetof(struct vcpu, s.gr) + 8 * 128;
    const int64_t nat_lo = (int64_t)offsetof(struct vcpu, s.nat) + 32;
    const int64_t nat_hi = (int64_t)offsetof(struct vcpu, s.nat) + 128;
    *image = 0;
    for (size_t at = 0; at < size;) {
        struct t1_x86_decoded d;
        if (!t1_x86_decode(code + at, size - at, &d)) {
            fprintf(stderr, "%s: undecodable code at +%#zx\n", label, at);
            return false;
        }
        int64_t disp = d.mem.disp;
        if (d.has_mem && d.mem.base == T1_R12 &&
            ((disp >= gr_lo && disp < gr_hi) || (disp >= nat_lo && disp < nat_hi))) {
            fprintf(stderr, "%s: generated code addresses a stacked home at +%#zx (disp %#llx)\n",
                    label, at, (unsigned long long)disp);
            return false;
        }
        if (d.has_mem && d.mem.base == T1_R14 && disp >= (int64_t)X_FRAME_IN(0) &&
            disp < (int64_t)X_FRAME_IN(T1_MAX_FRAME_IN))
            ++*image;
        at += d.length;
    }
    return true;
}
