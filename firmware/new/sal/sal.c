/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Kernel/Device/PciEcamAddress.hpp
 */
/* Intel SAL §9; ECAM address validation. */
#include "sal.h"
#define PA_LIMIT (UINT64_C(1) << IA64VMM_PHYS_ADDR_BITS)
/* SAL_GET_STATE_INFO_SIZE: the largest record SAL could log for a type.
 * Every record holds at least the 40-byte Record Header, RECORD_LEN
 * including it (SAL 245359-005 Appendix B.2.1), and the platform can take
 * an INIT, so 0 is not a possible maximum; the OS sizes its buffers from
 * this value (§9, SAL_GET_STATE_INFO_SIZE). The reference firmware's
 * IA64_RAS_MAX_RECORD_SIZE (firmware/include/hw/ia64/ia64_ras_abi.h). */
#define SAL_MAX_RECORD_SIZE 512u
void sal_init(struct sal_state *s, const struct ia64vmm_handoff *h, uint64_t pal)
{
    table_zero(s, sizeof(*s));
    /* The VMM window and MCFG base denote bus zero. The ECAM helper
     * uses a first-bus-relative region, so adapt its base and size here. */
    uint64_t first = (uint64_t)h->ecam_bus_first << 20;
    s->ecam_available = h->ecam_bus_first <= h->ecam_bus_last && h->ecam_bus_last <= 255 &&
                        first < h->ecam_size && first <= UINTPTR_MAX - h->ecam_base;
    if (s->ecam_available) {
        s->ecam = (volatile uint8_t *)(uintptr_t)(h->ecam_base + first);
        s->ecam_size = h->ecam_size - first;
    }
    s->bus_first = h->ecam_bus_first;
    s->bus_last = h->ecam_bus_last;
    s->pal_entry = pal;
}
bool sal_ecam(const struct sal_state *s, uint64_t address, uint64_t width, uint64_t mode,
              uint64_t *offset)
{
    if (!s || !offset || (width != 1 && width != 2 && width != 4) || mode > 1 ||
        s->bus_first > s->bus_last || s->bus_last > 255)
        return false;
    unsigned bus, devfn, reg;
    if (!mode) {
        if (address >> 24)
            return false; /* Only segment zero exists. */
        reg = address & 255;
        devfn = (address >> 8) & 255;
        bus = (address >> 16) & 255;
    } else {
        if (address >> 28)
            return false;
        reg = address & 4095;
        devfn = (address >> 12) & 255;
        bus = (address >> 20) & 255;
    }
    if (bus < s->bus_first || bus > s->bus_last || (reg & (width - 1)) ||
        width > (mode ? 4096u : 256u) - reg)
        return false;
    uint64_t at = ((uint64_t)(bus - s->bus_first) << 20) | ((uint64_t)devfn << 12) | reg;
    if (at > s->ecam_size || width > s->ecam_size - at || at > UINTPTR_MAX - (uintptr_t)s->ecam ||
        width - 1 > UINTPTR_MAX - (uintptr_t)s->ecam - at)
        return false;
    *offset = at;
    return true;
}
static bool handler(uint64_t entry, uint64_t length_cs)
{
    /* SAL §9 SAL_SET_VECTORS length_cs: bits 31:0 handler length, a multiple
     * of 16 and 0 when not given;
     * bit 32 checksum valid; bits 47:40 checksum; the rest reserved.
     * Address 0 is a valid registration that clears the vector. The spec
     * names the checks: phys_addr_n 16-byte aligned, and it "may be
     * checked as to whether it points into legal memory space". gp_n is
     * stored and handed to the handler as given. Bit 63
     * names the uncacheable alias (SAL §4.1: an uncacheable OS_MCA);
     * SALE_ENTRY reads the checksum and enters the handler through it. */
    uint64_t size = length_cs & UINT32_MAX, pa = entry & ~(UINT64_C(1) << 63);
    if ((length_cs & UINT64_C(0xffff00fe00000000)) || (size & 15))
        return false;
    return !entry || (!(entry & 15) && pa < PA_LIMIT && size <= PA_LIMIT - pa);
}
static void cache_sync(void)
{
    /* Coherent caches and store-time code invalidation (platform contract).
     * SAL §9 CACHE_FLUSH/INIT require completion and instruction visibility. */
#ifdef __ia64__
    __asm__ volatile("mf;; sync.i;; srlz.i;;" ::: "memory");
#endif
}
struct sal_call_result sal_dispatch(struct sal_state *s, uint64_t index, uint64_t a, uint64_t b,
                                    uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g)
{
    struct sal_call_result r;
    table_zero(&r, sizeof(r));
    uint64_t at;
    switch (index) {
    case 0x01000000:
        /* Only OS_INIT (1) has a second (slave) handler; OS_MCA and
         * OS_BOOT_RENDEZ reserve the second triple. */
        if (a > 2 || !handler(b, d) || (a == 1 ? !b != !e || !handler(e, g) : e || f || g))
            r.status = -2;
        /* SAL §3.2.6: APs enter the registered handler from ap.S. */
        else if (a == 2 && !s->ap)
            r.status = -4;
        else {
            struct sal_vector *v = &s->vectors[a];
            v->entry[0] = b;
            v->gp[0] = c;
            v->size[0] = d;
            v->entry[1] = e;
            v->gp[1] = f;
            v->size[1] = g;
            const struct sal_handler h[2] = {{b, c, d}, {e, f, g}};
            if (a == 2)
                ap_register(s->ap, b, c);
            else if (s->mc && a == 1)
                sal_handlers_set(s->mc, s->mc->os_init, h, 2);
            else if (s->mc)
                sal_handlers_set(s->mc, &s->mc->os_mca, h, 1);
        }
        break;
    case 0x01000001:
        r.status = a > 3 || b || (c & 7) || d || e || f || g ? -2 : -5;
        break;
    case 0x01000002:
    case 0x01000003:
        if (a > 3 || b || c || d || e || f || g)
            r.status = -2;
        /* The VMM supplies no MCA/INIT/CMC/CPE error sources or records,
         * so GET_STATE_INFO reports -5; the size is still the format's. */
        else if (index == 0x01000002)
            r.v0 = SAL_MAX_RECORD_SIZE;
        break;
    case 0x01000004:
        r.status = a || b || c || d || e || f || g ? -2 : -3;
        break;
    case 0x01000005:
        if (a < 1 || a > 3 || b < 1 || b > 2 || (a != 2 && b != 1) || f || g ||
            (b == 1 ? c < 16 || c > 255 : !c || (c & 7)) || (a == 1 && (e & ~UINT64_C(3))))
            r.status = -2;
        else {
            s->mc_params[a - 1][0] = b;
            s->mc_params[a - 1][1] = c;
            s->mc_params[a - 1][2] = d;
            s->mc_params[a - 1][3] = e;
        }
        break;
    case 0x01000006:
        if (a || !b || b >= PA_LIMIT || (b & 15) || c || d || e || f || g)
            r.status = -2;
        else {
            s->pal_entry = b;
            if (s->mc)
                s->mc->pal_proc = b;
        }
        break;
    case 0x01000008:
        if (a < 1 || a > 4 || b || c || d || e || f || g)
            r.status = -2;
        else
            cache_sync();
        break;
    case 0x01000009:
        if (a || b || c || d || e || f || g)
            r.status = -2;
        else
            cache_sync();
        break;
    case 0x01000010:
    case 0x01000011: {
        bool write = index == 0x01000011;
        if ((write ? e || f || g : d || e || f || g) || !sal_ecam(s, a, b, write ? d : c, &at)) {
            r.status = -2;
            break;
        }
        if (!s->ecam_available) {
            r.status = -4;
            break;
        }
        volatile uint8_t *p = s->ecam + at;
        /* Configuration accesses retain their requested bus width. */
        if (write) {
            if (b == 1)
                *(volatile uint8_t *)p = (uint8_t)c;
            else if (b == 2)
                *(volatile uint16_t *)p = (uint16_t)c;
            else
                *(volatile uint32_t *)p = (uint32_t)c;
        } else {
            if (b == 1)
                r.v0 = *(volatile uint8_t *)p;
            else if (b == 2)
                r.v0 = *(volatile uint16_t *)p;
            else
                r.v0 = *(volatile uint32_t *)p;
        }
        break;
    }
    case 0x01000012:
        if (a > 2 || b || c || d || e || f || g)
            r.status = -2;
        else if (a)
            r.status = -3; /* Optional platform timer/RTC inputs absent. */
        else
            r.v0 = IA64VMM_PLATFORM_BASE_HZ;
        break;
    case 0x01000020:
        r.status = -3;
        r.v0 = (uint64_t)-4; /* Immutable firmware image. */
        break;
    default:
        r.status = -1;
        break;
    }
    return r;
}
