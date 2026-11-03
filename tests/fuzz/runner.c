/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Differential-fuzz runner: executes a flat IA-64 image on the hosted
 * interpreter and prints the complete architectural state that the fork's
 * `-d cpu` IA64STATE dump also provides (GR+NaT, BR, logical PR, ARs, CFM).
 *
 *   ia64-fuzz-runner [--checkpoint ADDR] [--trace] [--budget N] [--repeat] image.bin
 *
 * --checkpoint: also dump the state the first time execution reaches slot 0
 *               of the bundle at ADDR (the fork logs the same point).
 * --trace:      dump the state at every bundle entry (the fork logs one
 *               block per executed bundle with one-insn-per-tb).
 * --repeat:     run the program once without output, restore RAM (changed
 *               bytes only, through gm_write) and the reset state, then run
 *               and report it again: engines with code caches execute the
 *               second run from their warm (Tier0: sealed native) code.
 * The run ends at `break 0x1ffffe` (state after the break, like the fork's
 * IVT entry), at the first architectural fault (state at the fault, no
 * effects of the faulting instruction), or at any other engine exit.
 * Host FP results are checked against the soft-float (ia64/hostfp.h
 * fp_check: a mismatch panics).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/semantics.h"
#include "ia64/hostfp.h"
#include "ia64/mmu.h"
#include "ia64/interrupt.h"
#include "engine/cache/cache.h"
#include "core/log_internal.h"
#include "ia64vmm/guest_mem.h"

#define ENTRY UINT64_C(0x100000)
#define RAM_SIZE 0x100000u
#define END_BREAK 0x1ffffe
static uint8_t ram[RAM_SIZE], image[RAM_SIZE];

static bool end_hook(struct vcpu *v, uint64_t imm, uint64_t ip)
{
    (void)ip;
    if (imm != END_BREAK)
        return false;
    vcpu_request_exit(v, VCPU_EXIT_DEBUG);
    return true;
}

static uint64_t logical_pr(const struct ia64_state *s)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 64; ++i)
        value |= (uint64_t)ia64_pr_read(s, i) << i;
    return value;
}

static void dump(const struct vcpu *v, const char *tag)
{
    const struct ia64_state *s = &v->s;
    printf("STATE %s ip=%016llx slot=%u\n", tag, (unsigned long long)s->ip, s->slot);
    /* Static GRs (current bank) and the logical (renamed/rotated) current
     * frame, which is what the fork's env->gr[32..] caches. Registers past
     * CFM.sof are architecturally inaccessible and are not dumped. */
    unsigned sof = s->cfm & 127;
    for (unsigned i = 0; i < 32 + sof && i < 128; ++i)
        printf("GR %u %016llx %u\n", i, (unsigned long long)ia64_gr_read(s, i),
               (unsigned)ia64_nat_read(s, i));
    /* FRs in the 16-byte spill layout used by the fork's dump (logical). */
    for (unsigned i = 0; i < 128; ++i) {
        const struct ia64_fr *f = &s->fr[ia64_fr_index(s, i)];
        uint64_t high = (f->exponent & 0x1ffff) | ((uint64_t)f->sign << 17);
        bool natval = f->exponent == 0x1fffe && !f->significand && !f->sign;
        printf("FR %u %016llx %016llx %u\n", i, (unsigned long long)f->significand,
               (unsigned long long)high, (unsigned)natval);
    }
    for (unsigned i = 0; i < 8; ++i)
        printf("BR %u %016llx\n", i, (unsigned long long)s->br[i]);
    /* The fork's env->pr[] is the logical (rotated) view. */
    printf("PR %016llx\n", (unsigned long long)logical_pr(s));
    static const struct {
        const char *name;
        unsigned index;
    } ars[] = {
        {"k0", 0},
        {"k1", 1},
        {"k2", 2},
        {"k3", 3},
        {"k4", 4},
        {"k5", 5},
        {"k6", 6},
        {"k7", 7},
        {"rsc", AR_RSC},
        {"bsp", AR_BSP},
        {"bspstore", AR_BSPSTORE},
        {"rnat", AR_RNAT},
        {"ccv", AR_CCV},
        {"unat", AR_UNAT},
        {"fpsr", AR_FPSR},
        {"csd", 25},
        {"ssd", 26},
        {"pfs", AR_PFS},
        {"lc", AR_LC},
        {"ec", AR_EC},
    };
    for (unsigned i = 0; i < sizeof(ars) / sizeof(ars[0]); ++i)
        printf("AR %s %016llx\n", ars[i].name, (unsigned long long)s->ar[ars[i].index]);
    static const struct {
        const char *name;
        unsigned index;
    } crs[] = {
        {"dcr", CR_DCR}, {"iva", CR_IVA}, {"pta", CR_PTA},   {"ipsr", CR_IPSR}, {"isr", CR_ISR},
        {"iip", CR_IIP}, {"ifa", CR_IFA}, {"itir", CR_ITIR}, {"iipa", CR_IIPA}, {"ifs", CR_IFS},
        {"iim", CR_IIM}, {"iha", CR_IHA}, {"tpr", CR_TPR},   {"itv", CR_ITV},
    };
    for (unsigned i = 0; i < sizeof(crs) / sizeof(crs[0]); ++i)
        printf("CR %s %016llx\n", crs[i].name, (unsigned long long)s->cr[crs[i].index]);
    printf("PSR %016llx\n", (unsigned long long)s->psr);
    uint64_t cfm = s->cfm;
    printf("CFM sof %llx\nCFM sol %llx\nCFM sor %llx\nCFM rrb_gr %llx\n"
           "CFM rrb_fr %llx\nCFM rrb_pr %llx\n",
           (unsigned long long)(cfm & 127), (unsigned long long)((cfm >> 7) & 127),
           (unsigned long long)((cfm >> 14) & 15), (unsigned long long)((cfm >> 18) & 127),
           (unsigned long long)((cfm >> 25) & 127), (unsigned long long)((cfm >> 32) & 63));
    printf("ENDSTATE\n");
}

static const char *reason_name(enum vcpu_exit_reason r)
{
    static const char *names[] = {"budget",        "halt",       "shutdown",
                                  "unimplemented", "bad_access", "debug"};
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}

/* The program from v's current state, one instruction per engine run.
 * Dumps (trace, checkpoint) only when asked; returns the last exit. */
static struct vcpu_exit run_program(struct vcpu *v, uint64_t budget, bool trace,
                                    bool have_checkpoint, uint64_t checkpoint, uint64_t *steps_out)
{
    struct vcpu_exit out = {.reason = VCPU_EXIT_BUDGET};
    bool checkpoint_done = false;
    uint64_t steps = 0;
    /* The fork logs a block whenever a translation block starts: at every
     * bundle entry, including rfi into the middle of a bundle. Mirror that
     * by dumping whenever execution enters a different bundle. */
    uint64_t last_ip = UINT64_MAX;
    for (; steps < budget; ++steps) {
        /* CFLE processing precedes a fork TB, even when the target's
         * instruction page is already resident (SDM Vol.2 §5.3). */
        if (v->s.rse_cfle) {
            struct ia64_translation fetch;
            struct ia64_result r = ia64_translate(v, v->s.ip, IA64_REF_FETCH, IA64_ACCESS_EXECUTE,
                                                  (v->s.psr >> 32) & 3, false, &fetch);
            if (r.kind == IA64_STEP_FAULT) {
                Ia64Instruction target = {
                    .opcode = IA64_OP_NOP, .address = v->s.ip, .slot = v->s.slot};
                ia64_deliver_fault(v, &target, r);
            } else if (r.kind == IA64_STEP_EXIT) {
                out = (struct vcpu_exit){r.exit, v->s.ip, v->s.slot, r.info, r.detail};
                break;
            }
        }
        bool bundle_entry = v->s.ip != last_ip;
        if (bundle_entry) {
            if (have_checkpoint && !checkpoint_done && v->s.ip == checkpoint && v->s.slot == 0) {
                dump(v, "checkpoint");
                checkpoint_done = true;
            }
            if (trace)
                dump(v, "bundle");
            last_ip = v->s.ip;
        }
        v->fault.kind = IA64_FAULT_NONE;
        out = engine_run(v, 1);
        /* Discard a new snapshot only if its first fetch failed. A later
         * slot can fault after an earlier instruction in the bundle retired. */
        if (trace && bundle_entry && v->fault.kind != IA64_FAULT_NONE &&
            v->fault.kind != IA64_FAULT_DEBUG && !v->fault.raw &&
            (v->s.cr[CR_ISR] & (UINT64_C(1) << 32)))
            printf("FETCH_FAULT ip=%016llx\n", (unsigned long long)v->fault.ip);
        /* A non-retiring fault was delivered and the handler's first slot
         * retired in the same step. The generated IVT stubs begin with a
         * nop-only bundle, so this is the vector-entry state. */
        if (v->fault.kind != IA64_FAULT_NONE && v->s.slot != 0 && v->s.ip != last_ip) {
            if (trace)
                dump(v, "bundle");
            last_ip = v->s.ip;
        }
        if (out.reason != VCPU_EXIT_BUDGET)
            break;
    }
    *steps_out = steps;
    return out;
}
/* Mirror the fork's raw-image (-bios image, nvram=none) reset state so
 * that registers a program never writes still compare equal. */
static void reset(struct vcpu *v)
{
    struct ia64_entry_regs regs = {
        .ip = ENTRY, .rsc = 3, .br0 = ENTRY, .bsp = 0x6e00000, .bspstore = 0x6e00000};
    regs.gr[1] = ENTRY;
    regs.gr[12] = 0x77ffff0;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = 0x10000; /* fork machine reset (programs set their own) */
    v->s.cr[CR_PTA] = 0x3c;
}
int main(int argc, char **argv)
{
    uint64_t checkpoint = 0, budget = 200000;
    bool trace = false, have_checkpoint = false, repeat = false;
    const char *path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--checkpoint") && i + 1 < argc) {
            checkpoint = strtoull(argv[++i], NULL, 0);
            have_checkpoint = true;
        } else if (!strcmp(argv[i], "--budget") && i + 1 < argc) {
            budget = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--trace")) {
            trace = true;
        } else if (!strcmp(argv[i], "--repeat")) {
            repeat = true;
        } else if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr,
                    "usage: %s [--checkpoint ADDR] [--trace] [--budget N] [--repeat] image.bin\n",
                    argv[0]);
            return 2;
        }
    }
    if (!path) {
        fprintf(stderr, "missing image\n");
        return 2;
    }
    log_set_level(LOG_ERROR);
    ia64_hostfp_configure(IA64_HOSTFP_CFG_CHECK);
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 2;
    }
    size_t n = fread(ram, 1, sizeof(ram), f);
    fclose(f);
    if (!n || n >= sizeof(ram)) {
        fprintf(stderr, "bad image size\n");
        return 2;
    }
    memcpy(image, ram, n);
    if (!gm_add_region(&(struct gm_region){
            .base = ENTRY, .size = RAM_SIZE, .kind = GM_RAM, .hva = ram, .name = "fuzz-ram"}))
        return 2;
    code_notify_vmm_write(ENTRY, RAM_SIZE);

    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true, .itc_hz = 1000000};
    struct vcpu *v = vcpu_create(&cfg);
    if (!v) {
        fprintf(stderr, "vcpu_create failed\n");
        return 2;
    }
    reset(v);
    engine_set_break_hook(end_hook);

    if (repeat) {
        uint64_t ignored;
        run_program(v, budget, false, false, 0, &ignored);
        /* Unchanged bytes keep their translations; changed ones are
         * notified like any VMM write. */
        for (size_t at = 0; at < RAM_SIZE;) {
            size_t end = at;
            while (end < RAM_SIZE && ram[end] != image[end])
                ++end;
            if (end > at && !gm_write(ENTRY + at, image + at, end - at))
                return 2;
            at = end + 1;
        }
        reset(v);
    }
    uint64_t steps;
    struct vcpu_exit out = run_program(v, budget, trace, have_checkpoint, checkpoint, &steps);
    const char *kind = out.reason == VCPU_EXIT_DEBUG ? "end" : "stop";
    const struct ia64_fault_site *fs = &v->fault;
    /* A fault is delivered through the IVT. Our empty IVT stops at its first fetch,
     * while the recorded FaultSite still names the original instruction. */
    if ((out.reason == VCPU_EXIT_UNIMPLEMENTED || out.reason == VCPU_EXIT_BAD_ACCESS) &&
        fs->kind != IA64_FAULT_NONE) {
        kind = "fault";
        printf(
            "EXIT kind=fault fault=%s ip=%016llx slot=%u info=%016llx raw=%016llx retired=%llu\n",
            ia64_fault_name(fs->kind), (unsigned long long)fs->ip, fs->slot,
            (unsigned long long)fs->info, (unsigned long long)fs->raw,
            (unsigned long long)v->retired);
    } else {
        printf("EXIT kind=%s reason=%s ip=%016llx slot=%u info=%016llx retired=%llu detail=%s\n",
               steps >= budget ? "budget" : kind, reason_name(out.reason),
               (unsigned long long)out.ip, out.slot, (unsigned long long)out.info,
               (unsigned long long)v->retired, out.detail ? out.detail : "-");
    }
    dump(v, "final");
    vcpu_destroy(v);
    return 0;
}
