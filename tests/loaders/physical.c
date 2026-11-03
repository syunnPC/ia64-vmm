/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Amd64/Firmware/DiagnosticSinkSelection.cpp
 */
/* Exercise the real bus fallback and CPU physical path, without host MMIO. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/platform.h"
#include "ia64/memory.h"
#include "ia64/mmu.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"

static unsigned logs, allocated;
static bool fail_alloc;
static char last_log[256];
static unsigned guest_bytes;
void vmm_log_guest_text(const char *text, size_t size)
{
    (void)text;
    guest_bytes += size;
}
void vmm_guest_serial_write(const char *text, size_t size)
{
    vmm_log_guest_text(text, size);
}
void vmm_log(enum log_level level, const char *fmt, ...)
{
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    ++logs;
}
void *hal_alloc_pages(size_t pages, unsigned flags)
{
    (void)flags;
    if (fail_alloc)
        return NULL;
    ++allocated;
    return calloc(pages, 4096);
}
void hal_free_pages(void *p, size_t pages)
{
    (void)pages;
    --allocated;
    free(p);
}
uint64_t vcpu_instruction_pointer(const struct vcpu *v, unsigned *slot)
{
    *slot = v->s.slot;
    return v->s.ip;
}
void vcpu_request_exit(struct vcpu *v, enum vcpu_exit_reason reason)
{
    (void)v;
    assert(reason == VCPU_EXIT_BAD_ACCESS);
}
bool platform_pci_handoff(struct ia64vmm_handoff *h)
{
    h->pci_mmio_base = 0xe0000000;
    h->pci_mmio_size = 0x1000000;
    h->pci_mmio64_base = UINT64_C(0x100000000);
    h->pci_mmio64_size = 0x10000000;
    return true;
}

int main(void)
{
    struct platform_state uart = {0};
    const struct gm_mmio_ops *u = &platform_uart_ops;
    /* Exercise each scratch bit and modem-loopback wiring. */
    for (unsigned bit = 0; bit < 8; ++bit) {
        for (unsigned invert = 0; invert < 2; ++invert) {
            unsigned value = (1u << bit) ^ (invert ? 255 : 0);
            u->write(&uart, NULL, 7, value, 1);
            assert(u->read(&uart, NULL, 7, 1) == value);
        }
    }
    for (unsigned outputs = 0; outputs < 16; ++outputs) {
        u->write(&uart, NULL, 4, 0x10 | outputs, 1);
        assert((u->read(&uart, NULL, 6, 1) & 0xf0) ==
               (((outputs & 2) << 3) | ((outputs & 1) << 5) | ((outputs & 12) << 4)));
    }
    u->write(&uart, NULL, 7, 0x5a, 1);
    assert(u->read(&uart, NULL, 7, 1) == 0x5a);
    u->write(&uart, NULL, 7, 0xa5, 1);
    assert(u->read(&uart, NULL, 7, 1) == 0xa5);
    u->write(&uart, NULL, 4, 0x1e, 1);
    assert(u->read(&uart, NULL, 6, 1) == 0xd0);
    u->write(&uart, NULL, 3, 0x80, 1);
    u->write(&uart, NULL, 0, 12, 1);
    u->write(&uart, NULL, 1, 1, 1);
    assert(u->read(&uart, NULL, 0, 1) == 12 && u->read(&uart, NULL, 1, 1) == 1);
    assert(!guest_bytes);
    u->write(&uart, NULL, 3, 3, 1);
    u->write(&uart, NULL, 0, 0x55, 1);
    assert(u->read(&uart, NULL, 5, 1) == 0x61);
    assert(u->read(&uart, NULL, 0, 1) == 0x55 && u->read(&uart, NULL, 5, 1) == 0x60);
    u->write(&uart, NULL, 2, 1, 1);
    assert(u->read(&uart, NULL, 2, 1) == 0xc1);
    u->write(&uart, NULL, 4, 0, 1);
    u->write(&uart, NULL, 0, 'x', 1);
    assert(guest_bytes == 1);
    /* PC16550D §8.6: IIR reports THRE for polling without a wired IRQ. */
    assert(u->read(&uart, NULL, 2, 1) == 0xc1); /* ETBEI off. */
    u->write(&uart, NULL, 1, 2, 1);
    assert(u->read(&uart, NULL, 2, 1) == 0xc2); /* Enable arms. */
    assert(u->read(&uart, NULL, 2, 1) == 0xc1); /* Read clears. */
    u->write(&uart, NULL, 0, 'y', 1);
    assert(guest_bytes == 2);
    assert(u->read(&uart, NULL, 2, 1) == 0xc2); /* THR re-arms. */
    u->write(&uart, NULL, 1, 2, 1);
    u->write(&uart, NULL, 1, 0, 1);
    assert(u->read(&uart, NULL, 2, 1) == 0xc1); /* Disable drops. */
    u->write(&uart, NULL, 2, 0, 1);
    u->write(&uart, NULL, 1, 3, 1);
    u->write(&uart, NULL, 4, 0x10, 1);
    u->write(&uart, NULL, 0, 0x33, 1);
    assert(u->read(&uart, NULL, 2, 1) == 0x04); /* RX first. */
    assert(u->read(&uart, NULL, 0, 1) == 0x33);
    assert(u->read(&uart, NULL, 2, 1) == 0x02 && u->read(&uart, NULL, 2, 1) == 0x01);
    assert(guest_bytes == 2);
    u->write(&uart, NULL, 4, 0, 1);
    u->write(&uart, NULL, 1, 0, 1);
    static uint64_t ram[512], device[512];
    struct gm_region r = {0xb9000, 4096, GM_RAM, "ram", (void *)ram, NULL, NULL};
    assert(gm_add_region(&r));
    r = (struct gm_region){0xe0010000, 4096, GM_DIRECT_MMIO, "device", (void *)device, NULL, NULL};
    assert(gm_add_region(&r));
    struct platform_state s = {0};
    assert(platform_abort_init(&s));
    struct vcpu v = {0};
    v.s.ip = UINT64_C(0xa0000001007a6a60);
    v.s.slot = 2;
    unsigned before = logs;
    for (unsigned size = 1; size <= 8; size *= 2) {
        uint64_t value;
        assert(ia64_physical_read(&v, 0xb8000, size, &value).kind == IA64_STEP_COMPLETE);
        assert(value == (size == 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1));
        assert(ia64_physical_write(&v, 0xb8000, size, 0).kind == IA64_STEP_COMPLETE);
    }
    assert(logs == before + 1);
    assert(strstr(last_log, "ip=a0000001007a6a60 slot=2"));
    assert(!gm_lookup(0xb8000) && !gm_direct_ptr(0xb8000, 2));
    uint64_t value = 0;
    assert(!gm_read(0xb8000, &value, 2) && !gm_write(0xb8000, &value, 2));
    /* VGA's 0xaa55 presence probe must fail, while real claims still work. */
    assert(ia64_physical_write(&v, 0xb8000, 2, 0xaa55).kind == IA64_STEP_COMPLETE);
    assert(ia64_physical_read(&v, 0xb8000, 2, &value).kind == IA64_STEP_COMPLETE &&
           value == 0xffff);
    assert(ia64_physical_write(&v, 0xb9000, 8, 42).kind == IA64_STEP_COMPLETE);
    assert(ia64_physical_read(&v, 0xb9000, 8, &value).kind == IA64_STEP_COMPLETE && value == 42);
    assert(ia64_physical_write(&v, 0xe0010000, 8, 17).kind == IA64_STEP_COMPLETE);
    assert(device[0] == 17);
    for (uint64_t at = 0xe0000000; at < 0xe0003000; at += 4096)
        assert(ia64_physical_read(&v, at, 8, &value).kind == IA64_STEP_COMPLETE &&
               value == UINT64_MAX);
    assert(ia64_physical_read(&v, UINT64_C(0x100001000), 8, &value).kind == IA64_STEP_COMPLETE);
    assert(logs == before + 5 && allocated == 3);
    assert(!platform_unclaimed_region(0xb8fff, 2));
    /* Chipset subtractive decode (PCI 3.0 §3.3.3) covers every gap below
     * 2^IA64VMM_PHYS_ADDR_BITS: a hole between RAM ranges, the space above
     * guest RAM and host memory that build_guest_map never registers
     * (the VMM's reservation, host-reserved RAM). Reads are all-ones and
     * writes are dropped; nothing acquires backing. */
    before = logs;
    assert(ia64_physical_read(&v, 0x200000, 8, &value).kind == IA64_STEP_COMPLETE &&
           value == UINT64_MAX);
    assert(ia64_physical_read(&v, UINT64_C(0x100000034), 4, &value).kind == IA64_STEP_COMPLETE &&
           value == UINT32_MAX);
    assert(ia64_physical_write(&v, UINT64_C(0x100000034), 4, 0).kind == IA64_STEP_COMPLETE);
    assert(ia64_physical_read(&v, UINT64_C(0x100000034), 4, &value).kind == IA64_STEP_COMPLETE &&
           value == UINT32_MAX);
    static uint64_t reservation[512]; /* host memory at a never-registered guest PA */
    memset(reservation, 0x5a, sizeof(reservation));
    const uint64_t vmm_reserved = 0x400000;
    assert(ia64_physical_read(&v, vmm_reserved, 8, &value).kind == IA64_STEP_COMPLETE &&
           value == UINT64_MAX);
    assert(ia64_physical_write(&v, vmm_reserved, 8, 0).kind == IA64_STEP_COMPLETE);
    assert(reservation[0] == UINT64_C(0x5a5a5a5a5a5a5a5a));
    assert(!gm_direct_ptr(vmm_reserved, 8) && !gm_read(vmm_reserved, &value, 8));
    const uint64_t top = (UINT64_C(1) << IA64VMM_PHYS_ADDR_BITS) - 8;
    assert(ia64_physical_read(&v, top, 8, &value).kind == IA64_STEP_COMPLETE &&
           value == UINT64_MAX);
    assert(!platform_unclaimed_region(top + 8, 8)); /* the CPU MMU's (unimplemented) */
    assert(!platform_unclaimed_region(0xb8ffc, 8)); /* straddles RAM at 0xb9000 */
    assert(logs == before + 4);                     /* one sparse line per page */
    /* CPU translation still faults before any out-of-width bus reference. */
    assert(!ia64_physical_region(&v, UINT64_C(1) << 50, 8));
    fail_alloc = true;
    assert(ia64_physical_read(&v, UINT64_C(0x108001000), 8, &value).kind == IA64_STEP_COMPLETE);
    assert(strstr(last_log, "allocation failed"));
    platform_abort_cleanup(&s);
    assert(!allocated && !platform_unclaimed_region(0xb8000, 2));
    /* A region registered after the ranges were built makes them stale:
     * they then answer nothing rather than shadow the new claim. */
    assert(platform_abort_init(&s) && platform_unclaimed_region(0x600000, 8));
    static uint64_t late[512];
    r = (struct gm_region){0x600000, 4096, GM_RAM, "late", (void *)late, NULL, NULL};
    assert(gm_add_region(&r) && !platform_unclaimed_region(0x200000, 8));
    /* n sorted, disjoint regions leave at most n + 1 gaps: a full table
     * with a gap around every region still fits. */
    static uint64_t pages[GM_MAX_REGIONS][512];
    for (uint64_t at = UINT64_C(0x10000000); gm_region_count() < GM_MAX_REGIONS; at += 0x2000) {
        r = (struct gm_region){at,   4096, GM_RAM, "fill", (void *)pages[gm_region_count()],
                               NULL, NULL};
        assert(gm_add_region(&r));
    }
    assert(platform_abort_init(&s) && s.abort_range_count == GM_MAX_REGIONS + 1);
    assert(platform_unclaimed_region(UINT64_C(0x10001000), 8) &&
           !platform_unclaimed_region(UINT64_C(0x10000000), 8));
    platform_abort_cleanup(&s);
    puts("master-abort policy: widths, VGA probe, claims, sparse logs and boundaries passed");
}
