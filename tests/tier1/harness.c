/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tier1 test machine: guest-physical layout, RAM, the
 * test MMIO device with its transaction log, the
 * end-marker break hook and the per-vCPU run logs. See harness.h for the
 * ranges this file registers.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "tests/tier1/harness.h"
#include "ia64vmm/guest_mem.h"

static t1h_ram_alloc_fn ram_allocator;
static uint8_t *ram;
static int image_count;
static char **image_paths;

#define BINDINGS 8u
static struct {
    const struct vcpu *v;
    struct t1h_logs *logs;
} bindings[BINDINGS];

_Noreturn void t1h_fail(const char *file, unsigned line, const char *what)
{
    fprintf(stderr, "%s:%u: check failed: %s\n", file, line, what);
    exit(1);
}

bool t1h_quiet(const char *label)
{
    return !strncmp(label, T1H_EXPECTED_DIFFERENCE, strlen(T1H_EXPECTED_DIFFERENCE));
}

void t1h_print_diff(const char *what, uint64_t index, uint64_t reference, uint64_t actual,
                    void *arg)
{
    struct t1h_diff_printer *p = arg;
    if (t1h_quiet(p->label) || p->printed++ >= p->limit)
        return;
    fprintf(stderr, "  %s: %s[%#llx] reference %#llx actual %#llx\n", p->label, what,
            (unsigned long long)index, (unsigned long long)reference, (unsigned long long)actual);
}

/* ---- RAM --------------------------------------------------------------- */

static uint8_t *default_ram(gpa_t base, uint64_t size)
{
    (void)base;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void t1h_set_ram_allocator(t1h_ram_alloc_fn fn)
{
    T1H_CHECK(!ram); /* the gm table cannot be re-registered */
    ram_allocator = fn;
}

uint8_t *t1h_ram(void)
{
    return ram;
}

/* ---- test MMIO device -------------------------------------------------- */

static void device_record(struct vcpu *v, uint64_t offset, uint64_t value, unsigned size,
                          bool write)
{
    struct t1h_logs *logs = t1h_logs_of(v);
    if (!logs)
        return; /* unbound vCPU: callers check t1h_logs_ok */
    if (logs->device_n == T1H_DEVICE_MAX) {
        t1h_error(logs, "device log overflow");
        return;
    }
    logs->device[logs->device_n++] = (struct compare_device_record){
        T1H_DEVICE + offset, value & ia64_low_mask(size * 8), (uint8_t)size, write};
}
/* Reads return a fixed function of the offset: the device has no state
 * besides its log, so a snapshot only needs the log length. */
static uint64_t device_read(void *opaque, struct vcpu *v, uint64_t offset, unsigned size)
{
    (void)opaque;
    uint64_t value = UINT64_C(0x0123456789abcdef) ^ offset;
    device_record(v, offset, value, size, false);
    return value;
}
static void device_write(void *opaque, struct vcpu *v, uint64_t offset, uint64_t value,
                         unsigned size)
{
    (void)opaque;
    device_record(v, offset, value, size, true);
}
static const struct gm_mmio_ops device_ops = {device_read, device_write};

/* ---- break hook -------------------------------------------------------- */

static bool break_hook(struct vcpu *v, uint64_t imm, uint64_t ip)
{
    if (imm != T1H_BREAK_END && imm != T1H_BREAK_UNEXPECTED)
        return false;
    struct t1h_logs *logs = t1h_logs_of(v);
    if (logs) {
        ++logs->breaks;
        logs->last_break_imm = imm;
        logs->last_break_ip = ip;
    }
    vcpu_request_exit(v, VCPU_EXIT_DEBUG);
    return true;
}

void t1h_init(void)
{
    if (ram)
        return;
    ram = (ram_allocator ? ram_allocator : default_ram)(T1H_RAM_BASE, T1H_RAM_SIZE);
    T1H_CHECK(ram && !((uintptr_t)ram & 4095));
    T1H_CHECK(gm_add_region(&(struct gm_region){
        .base = T1H_RAM_BASE, .size = T1H_RAM_SIZE, .kind = GM_RAM, .hva = ram, .name = "t1-ram"}));
    T1H_CHECK(gm_add_region(&(struct gm_region){.base = T1H_DEVICE,
                                                .size = 4096,
                                                .kind = GM_EMULATED,
                                                .ops = &device_ops,
                                                .name = "t1-device"}));
    T1H_CHECK(!gm_lookup(T1H_UNMAPPED));
    engine_set_break_hook(break_hook);
}

/* ---- images ------------------------------------------------------------ */

void t1h_set_images(int argc, char **argv)
{
    image_count = argc;
    image_paths = argv;
}

const char *t1h_image(const char *name)
{
    size_t n = strlen(name);
    for (int i = 0; i < image_count; ++i) {
        size_t len = strlen(image_paths[i]);
        if (len >= n && !strcmp(image_paths[i] + len - n, name) &&
            (len == n || image_paths[i][len - n - 1] == '/'))
            return image_paths[i];
    }
    fprintf(stderr, "tier1-test: image %s not given\n", name);
    exit(1);
}

void t1h_load_image(const char *name)
{
    t1h_init();
    FILE *f = fopen(t1h_image(name), "rb");
    T1H_CHECK(f != NULL);
    memset(ram, 0, T1H_RAM_SIZE);
    size_t n = fread(ram, 1, T1H_RAM_SIZE, f);
    T1H_CHECK(n > 0 && !ferror(f) && feof(f));
    fclose(f);
    code_notify_vmm_write(T1H_RAM_BASE, T1H_RAM_SIZE);
}

/* ---- run logs ---------------------------------------------------------- */

void t1h_bind(struct vcpu *v, struct t1h_logs *logs)
{
    for (unsigned i = 0; i < BINDINGS; ++i)
        if (bindings[i].v == v || !bindings[i].v) {
            bindings[i].v = v;
            bindings[i].logs = logs;
            return;
        }
    T1H_CHECK(!"too many bound vCPUs");
}

void t1h_unbind(struct vcpu *v)
{
    for (unsigned i = 0; i < BINDINGS; ++i)
        if (bindings[i].v == v)
            bindings[i] = (__typeof__(bindings[i])){0};
}

struct t1h_logs *t1h_logs_of(const struct vcpu *v)
{
    for (unsigned i = 0; i < BINDINGS; ++i)
        if (bindings[i].v == v)
            return bindings[i].logs;
    return NULL;
}

void t1h_logs_reset(struct t1h_logs *logs)
{
    bool on = logs->stores_on;
    memset(logs, 0, sizeof(*logs));
    logs->stores_on = on;
}

void t1h_error(struct t1h_logs *logs, const char *fmt, ...)
{
    if (logs->errors++)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(logs->error, sizeof(logs->error), fmt, ap);
    va_end(ap);
}

bool t1h_logs_ok(const struct t1h_logs *logs)
{
    if (!logs->errors)
        return true;
    fprintf(stderr, "harness error (%u): %s\n", logs->errors, logs->error);
    return false;
}

uint64_t t1h_bank0(const struct vcpu *v, unsigned reg)
{
    T1H_CHECK(reg >= 16 && reg < 32);
    return (v->s.psr & IA64_PSR_BN) ? v->s.bank_gr[reg - 16] : v->s.gr[reg];
}

uint64_t t1h_bank1(const struct vcpu *v, unsigned reg)
{
    T1H_CHECK(reg >= 16 && reg < 32);
    return (v->s.psr & IA64_PSR_BN) ? v->s.gr[reg] : v->s.bank_gr[reg - 16];
}

uint64_t t1h_page_hash(const uint8_t *page)
{
    /* Word-wise multiply-xorshift; position-dependent so moved words differ. */
    uint64_t h = UINT64_C(0x9e3779b97f4a7c15);
    for (unsigned i = 0; i < 4096; i += 8) {
        uint64_t w;
        memcpy(&w, page + i, 8);
        h ^= w + i;
        h *= UINT64_C(0xbf58476d1ce4e5b9);
        h ^= h >> 31;
    }
    return h;
}
