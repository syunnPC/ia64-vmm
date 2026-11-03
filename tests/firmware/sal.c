/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Tests/Platform/Ia64/Firmware/SalSystemTableParserTests.cpp
 */
/* SAL descriptor fixture layout and invariants. */
#include <assert.h>
#include <stdio.h>
#include "firmware/new/sal/sal.h"
#include "firmware/new/boot-translation.h"
/* The handoff window is based at bus zero even if the root starts at 32. */
static _Alignas(4096) uint8_t ecam[34 * 1024 * 1024];
static struct sal_system_table table;
int main(void)
{
    /* SAL §§3.2.5, 3.3.2.1: one aligned supported identity page must
     * contain PAL and every native handler, including the last byte. */
    struct ipl_boot_translation boot;
    assert(ipl_boot_translation(0x900000, 0x940000, &boot));
    assert(boot.address == 0x900000 && boot.itir == (18u << 2));
    assert(ipl_boot_translation(0x900000, 0x940001, &boot));
    assert(boot.address == 0x900000 && boot.itir == (20u << 2));
    assert(ipl_boot_translation(0x900000, 0xa00000, &boot));
    assert(boot.address == 0x900000 && boot.itir == (20u << 2));
    assert(!ipl_boot_translation(0x9f0000, 0xa10000, &boot));
    assert(!ipl_boot_translation(0xbff000, 0xc01000, &boot));
    assert(!ipl_boot_translation(0x3ffff000, 0x40001000, &boot));
    assert(!ipl_boot_translation(0, 0x1000, &boot));
    assert(!ipl_boot_translation(0x900000, 0x900000, &boot));
    assert(!ipl_boot_translation(0xffff0000, UINT64_C(0x100010000), &boot));

    struct ia64vmm_handoff h = {.ecam_base = (uintptr_t)ecam,
                                .ecam_size = sizeof(ecam),
                                .ecam_bus_first = 32,
                                .ecam_bus_last = 33};
    struct sal_state s;
    sal_init(&s, &h, 0x100000);
    /* OS_BOOT_RENDEZ also needs its AP block (tests/firmware/ap.c). */
    static struct ap_wake_block ap;
    s.ap = &ap;
    assert(s.ecam == ecam + (32u << 20) && s.ecam_size == (2u << 20));
    uint64_t offset;
    assert(sal_ecam(&s, 0x20ffff, 1, 0, &offset) && offset == 0xff0ff);
    assert(sal_ecam(&s, 0x021ffffc, 4, 1, &offset) && offset == 0x1ffffc);
    assert(!sal_ecam(&s, 0x20ffff, 2, 0, &offset));
    assert(!sal_ecam(&s, 0x0120ffff, 1, 0, &offset));
    assert(!sal_ecam(&s, 0x2200000, 4, 1, &offset));
    assert(!sal_ecam(&s, 0x02000000, 8, 1, &offset));
    struct sal_call_result r;
    r = sal_dispatch(&s, 0x01000011, 0x20fffc, 4, 0xcafef00d, 0, 0, 0, 0);
    assert(r.status == 0 && table_get(ecam + 0x20ff0fc, 4) == 0xcafef00d);
    r = sal_dispatch(&s, 0x01000010, 0x020ff0fc, 4, 1, 0, 0, 0, 0);
    assert(r.status == 0 && r.v0 == 0xcafef00d);
    r = sal_dispatch(&s, 0x01000011, 0x20fffd, 1, 0x1234, 0, 0, 0, 0);
    assert(r.status == 0 && ecam[0x20ff0fd] == 0x34);
    s.ecam_available = false;
    assert(sal_dispatch(&s, 0x01000010, 0x20fffc, 4, 0, 0, 0, 0, 0).status == -4);
    assert(sal_dispatch(&s, 0x01000012, 0, 0, 0, 0, 0, 0, 0).v0 == 100000000);
    assert(sal_dispatch(&s, 0x01000012, 3, 0, 0, 0, 0, 0, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000000, 2, 0x200000, 0x201000, 16, 0, 0, 0).status == 0);
    assert(s.vectors[2].entry[0] == 0x200000 && s.vectors[2].size[0] == 16);
    assert(sal_dispatch(&s, 0x01000000, 2, 0x200001, 0x201000, 16, 0, 0, 0).status == -2);
    /* Zero handler lengths are valid; OS_MCA's second triple is zero,
     * while OS_INIT names a slave. */
    assert(sal_dispatch(&s, 0x01000000, 0, 0x210000, 0x201000, 0, 0, 0, 0).status == 0);
    assert(s.vectors[0].entry[0] == 0x210000 && s.vectors[0].entry[1] == 0);
    assert(sal_dispatch(&s, 0x01000000, 1, 0x220000, 0x201000, 0, 0x230000, 0x201000, 0).status ==
           0);
    assert(s.vectors[1].entry[0] == 0x220000 && s.vectors[1].entry[1] == 0x230000);
    assert(sal_dispatch(&s, 0x01000000, 2, 0x240000, 0x201000, 0, 0, 0, 0).status == 0);
    assert(s.vectors[2].entry[0] == 0x240000 && s.vectors[2].size[0] == 0);
    /* SAL_SET_VECTORS preserves gp_n, including a virtual address, and
     * checks only phys_addr_n (SAL §9). */
    const uint64_t virtual_gp = UINT64_C(0xe0000000846dc000);
    assert(sal_dispatch(&s, 0x01000000, 0, 0x210000, virtual_gp, 0, 0, 0, 0).status == 0);
    assert(s.vectors[0].gp[0] == virtual_gp);
    assert(
        sal_dispatch(&s, 0x01000000, 1, 0x220000, virtual_gp, 0, 0x220000, virtual_gp, 0).status ==
        0);
    assert(s.vectors[1].gp[0] == virtual_gp && s.vectors[1].gp[1] == virtual_gp);
    assert(sal_dispatch(&s, 0x01000000, 1, 0x220000, 0x201000, 0, 0x230000, 0x201000, 0).status ==
           0);
    /* Reserved second triples, a missing or extra slave, bad length_cs. */
    assert(sal_dispatch(&s, 0x01000000, 0, 0x210000, 0x201000, 0, 0x230000, 0x201000, 0).status ==
           -2);
    assert(sal_dispatch(&s, 0x01000000, 2, 0x240000, 0x201000, 0, 0, 0, 16).status == -2);
    assert(sal_dispatch(&s, 0x01000000, 1, 0x220000, 0x201000, 0, 0, 0, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000000, 1, 0, 0, 0, 0x230000, 0x201000, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000000, 2, 0x240000, 0x201000, 8, 0, 0, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000000, 2, 0x240000, 0x201000, UINT64_C(1) << 33, 0, 0, 0).status ==
           -2);
    assert(sal_dispatch(&s, 0x01000000, 2, 0x240000, 0x201000,
                        (UINT64_C(0x5a) << 40) | (UINT64_C(1) << 32) | 32, 0, 0, 0)
               .status == 0);
    /* Address 0 clears a vector; OS_INIT clears both handlers together. */
    assert(sal_dispatch(&s, 0x01000000, 1, 0, 0, 0, 0, 0, 0).status == 0);
    assert(s.vectors[1].entry[0] == 0 && s.vectors[1].entry[1] == 0);
    /* SALE_ENTRY's block (sal/mc.h): OS_MCA and both OS_INIT triples as
     * registered, length_cs unchanged; never OS_BOOT_RENDEZ, never a
     * rejected call. SAL_REGISTER_PHYSICAL_ADDR names the PAL_PROC handed
     * to OS_MCA and OS_INIT (SAL §4.8, §5.3: GR8). */
    static struct sal_mc mc;
    s.mc = &mc;
    const uint64_t checked = (UINT64_C(0x5a) << 40) | (UINT64_C(1) << 32) | 32;
    assert(sal_dispatch(&s, 0x01000000, 0, 0x210000, 0x201000, checked, 0, 0, 0).status == 0);
    assert(mc.os_mca.entry == 0x210000 && mc.os_mca.gp == 0x201000 &&
           mc.os_mca.length_cs == checked);
    assert(sal_dispatch(&s, 0x01000000, 1, 0x220000, 0x202000, 16, 0x230000, 0x203000, checked)
               .status == 0);
    assert(mc.os_init[0].entry == 0x220000 && mc.os_init[0].gp == 0x202000 &&
           mc.os_init[0].length_cs == 16);
    assert(mc.os_init[1].entry == 0x230000 && mc.os_init[1].gp == 0x203000 &&
           mc.os_init[1].length_cs == checked);
    assert(sal_dispatch(&s, 0x01000000, 2, 0x240000, 0x204000, 0, 0, 0, 0).status == 0);
    assert(sal_dispatch(&s, 0x01000000, 0, 0x250001, 0x201000, 0, 0, 0, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000000, 1, 0x260000, 0x201000, 0, 0, 0, 0).status == -2);
    assert(mc.os_mca.entry == 0x210000 && mc.os_init[0].entry == 0x220000 &&
           mc.os_init[1].entry == 0x230000);
    assert(sal_dispatch(&s, 0x01000000, 1, 0, 0, 0, 0, 0, 0).status == 0);
    assert(!mc.os_init[0].entry && !mc.os_init[1].entry && mc.os_mca.entry == 0x210000);
    /* Each accepted OS_MCA or OS_INIT registration is one even generation
     * step (sal/mc.h); OS_BOOT_RENDEZ and rejected calls leave it. */
    assert(mc.generation == 6);
    assert(sal_dispatch(&s, 0x01000000, 2, 0x240000, 0x204000, 0, 0, 0, 0).status == 0 &&
           mc.generation == 6);
    assert(sal_dispatch(&s, 0x01000000, 0, 0x250001, 0x201000, 0, 0, 0, 0).status == -2 &&
           mc.generation == 6);
    /* SAL §4.1: bit 63 names an uncacheable handler; the address below it
     * is checked and the entry kept as given. */
    const uint64_t uc = UINT64_C(1) << 63, limit = UINT64_C(1) << IA64VMM_PHYS_ADDR_BITS;
    assert(sal_dispatch(&s, 0x01000000, 0, uc | 0x210000, 0x201000, checked, 0, 0, 0).status == 0);
    assert(mc.os_mca.entry == (uc | 0x210000) && mc.os_mca.length_cs == checked &&
           mc.generation == 8);
    assert(sal_dispatch(&s, 0x01000000, 1, uc | 0x220000, 0x202000, 0, uc | 0x230000, 0x203000, 16)
               .status == 0);
    assert(mc.os_init[0].entry == (uc | 0x220000) && mc.os_init[1].entry == (uc | 0x230000));
    assert(sal_dispatch(&s, 0x01000000, 0, uc | 0x210008, 0x201000, 0, 0, 0, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000000, 0, uc | limit, 0x201000, 0, 0, 0, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000000, 0, uc | (limit - 16), 0x201000, 32, 0, 0, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000000, 0, UINT64_C(1) << 62 | 0x210000, 0x201000, 0, 0, 0, 0)
               .status == -2);
    assert(mc.os_mca.entry == (uc | 0x210000) && mc.generation == 10);
    assert(sal_dispatch(&s, 0x01000006, 0, 0x1a0000, 0, 0, 0, 0, 0).status == 0 &&
           mc.pal_proc == 0x1a0000);
    assert(sal_dispatch(&s, 0x01000006, 0, 0x1a0008, 0, 0, 0, 0, 0).status == -2 &&
           mc.pal_proc == 0x1a0000);
    assert(!mc.active && !mc.sal_proc && !mc.sal_gp);
    s.mc = NULL;
    assert(sal_dispatch(&s, 0x01000000, 0, 0x210000, 0x201000, 0, 0, 0, 0).status == 0);
    for (unsigned i = 0; i < 4; ++i) {
        assert(sal_dispatch(&s, 0x01000001, i, 0, 0, 0, 0, 0, 0).status == -5);
        /* SAL Appendix B.2.1: no record is shorter than its 40-byte header. */
        struct sal_call_result size = sal_dispatch(&s, 0x01000002, i, 0, 0, 0, 0, 0, 0);
        assert(size.status == 0 && size.v0 >= 40 && size.v0 == 512);
        assert(sal_dispatch(&s, 0x01000003, i, 0, 0, 0, 0, 0, 0).status == 0);
    }
    assert(sal_dispatch(&s, 0x01000001, 4, 0, 0, 0, 0, 0, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000004, 0, 0, 0, 0, 0, 0, 0).status == -3);
    assert(sal_dispatch(&s, 0x01000008, 3, 0, 0, 0, 0, 0, 0).status == 0);
    assert(sal_dispatch(&s, 0x01000013, 0, 0, 0, 0, 0, 0, 0).status == -1);
    assert(sal_dispatch(&s, 0x01000005, 1, 1, 0xe0, 100, 3, 0, 0).status == 0);
    assert(s.mc_params[0][0] == 1 && s.mc_params[0][1] == 0xe0 && s.mc_params[0][2] == 100 &&
           s.mc_params[0][3] == 3);
    assert(sal_dispatch(&s, 0x01000005, 2, 2, 0x100000, 0, 0, 0, 0).status == 0);
    assert(s.mc_params[1][0] == 2 && s.mc_params[1][1] == 0x100000);
    assert(sal_dispatch(&s, 0x01000005, 3, 1, 0xe1, 0, 0, 0, 0).status == 0);
    assert(sal_dispatch(&s, 0x01000005, 1, 2, 0x100000, 0, 0, 0, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000005, 2, 2, 0x100001, 0, 0, 0, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000005, 1, 1, 15, 100, 0, 0, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000005, 1, 1, 256, 100, 0, 0, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000005, 1, 1, 0xe0, 100, 4, 0, 0).status == -2);
    assert(sal_dispatch(&s, 0x01000009, 0, 0, 0, 0, 0, 0, 0).status == 0);
    struct sal_memory memory[] = {{0x100000, 0x4000, 0, 1}, {0x200000, 0x8000, 0, 4}};
    struct sal_tr tr = {0x900000, 18u << 2, 0, 0};
    assert(sal_build(&table, 0x400000, 0x100000, 0x200000, 0x201000, 2, memory, 2, &tr, 1));
    assert(table_get(table.bytes + 8, 2) == 0x0320 && table_get(table.bytes + 10, 2) == 7);
    size_t length = (size_t)table_get(table.bytes + 4, 4);
    assert(length == 96 + 48 + 64 + 16 + 32 + 16 + 16 && table_sum(table.bytes, length) == 0);
    size_t at = 96;
    const unsigned types[] = {0, 1, 1, 2, 3, 4, 5};
    const unsigned sizes[] = {48, 32, 32, 16, 32, 16, 16};
    for (unsigned i = 0; i < 7; ++i) {
        assert(table.bytes[at] == types[i]);
        at += sizes[i];
    }
    assert(at == length);
    const uint8_t *itr = table.bytes + 96 + 48 + 64 + 16;
    assert(itr[1] == 0 && itr[2] == 0 && table_get(itr + 8, 8) == 0x900000 &&
           table_get(itr + 16, 8) == (18u << 2));
    const uint8_t *ptc = table.bytes + length - 32;
    assert(table_get(ptc + 4, 4) == 1 && table_get(ptc + 8, 8) == 0x400000 + length);
    assert(table_get(table.bytes + length, 8) == 2);
    assert(table_get(table.bytes + length + 8, 8) == 0x400000 + length + 16);
    assert(table_get(table.bytes + length + 16, 2) == 0 &&
           table_get(table.bytes + length + 18, 2) == 1);
    assert(sal_build(&table, 0x400000, 0x100000, 0x200000, 0x201000, 1, memory, 2, NULL, 0));
    assert(table_get(table.bytes + 10, 2) == 6);
    memory[0].base++;
    assert(!sal_build(&table, 0x400000, 0x100000, 0x200000, 0x201000, 1, memory, 2, NULL, 0));
    puts("SAL: ECAM, vectors, empty logs and types 0-5 structure checks passed");
    return 0;
}
