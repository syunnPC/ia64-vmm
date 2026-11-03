/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_TESTS_TIER1_TAIL_FIXTURE_H
#define IA64VMM_TESTS_TIER1_TAIL_FIXTURE_H

/* A source with an alternate exit and a direct jump to a previous page.
 * The tail compares rotating GR/PR names and leaves on either outcome. */
static void tail_fixture(uint64_t head, uint64_t tail)
{
    const struct ienc_bundle source[] = {
        {IENC_STOP(IENC_MIB),
         {ienc_ld(8, 8, false, 18, 33), ienc_adds(0, 36, 3, 36), ienc_br_cond(9, 0x100)}},
        {IENC_STOP(IENC_MIB),
         {ienc_nop_m(), ienc_adds(0, 15, 2, 15), ienc_br_cond(0, (int64_t)(tail - head - 16))}},
        {IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}}};
    const struct ienc_bundle end = {IENC_STOP(IENC_MIB),
                                    {ienc_nop_m(),
                                     ienc_cmp(10, IENC_LTU, false, false, 16, 17, 34, 35),
                                     ienc_br_cond(16, -0x80)}};
    um_emit(head, source, sizeof(source) / sizeof(source[0]));
    um_emit(tail, &end, 1);
    const uint64_t target[] = {head + 0x100, tail - 0x80, tail + 16};
    for (unsigned k = 0; k < 3; ++k) {
        if (target[k] == head)
            continue;
        const struct ienc_bundle spin = {
            IENC_STOP(IENC_MIB),
            {ienc_nop_m(), ienc_adds(0, 21 + k, 1, 21 + k), ienc_br_cond(0, 0)}};
        um_emit(target[k], &spin, 1);
    }
}
/* Separate resident 4 KiB mappings, with independently revocable target
 * rights/key and region ID. The source remains executable at CPL 3. */
static void tail_virtual_pages(struct vcpu *v, uint64_t head, uint64_t physical_head, uint64_t tail,
                               uint64_t physical_tail)
{
    um_virtual_code(v);
    const uint64_t va[] = {head, tail}, pa[] = {physical_head, physical_tail};
    for (unsigned k = 0; k < 2; ++k) {
        unsigned region = va[k] >> 61;
        v->s.rr[region] = ((UINT64_C(0x100) + region) << 8) | (12u << 2);
        ia64_mmu_changed(v, IA64_MMU_RR, 0, region);
        v->s.tr[0][k + 1] =
            (struct ia64_tlb_entry){.va = va[k] & ((UINT64_C(1) << 61) - 4096),
                                    .pte = (pa[k] & ~UINT64_C(4095)) | 1 | (1u << 5) | (1u << 6) |
                                           (1u << 9) | (k ? 0 : 3u << 7),
                                    .itir = (12u << 2) | (k ? 0x100u : 0),
                                    .rid = 0x100u + region,
                                    .page_shift = 12,
                                    .valid = true};
        ia64_mmu_changed(v, IA64_MMU_TR, 0, k + 1);
    }
    v->s.pkr[0] = 1;
    v->s.pkr[1] = 0x101;
    ia64_mmu_changed(v, IA64_MMU_PKR, 0, 0);
    ia64_mmu_changed(v, IA64_MMU_PKR, 0, 1);
    v->s.psr |= IA64_PSR_PK;
}

static void tail_virtual_change(struct vcpu *v, unsigned change, uint64_t tail)
{
    switch (change) {
    case 0:
        break;
    case 1:
        v->s.tr[0][2].valid = false;
        break;
    case 2:
        v->s.tr[0][2].pte &= ~(UINT64_C(7) << 9);
        break;
    case 3:
        v->s.pkr[1] = 0;
        break;
    case 4:
        v->s.pkr[1] |= 8;
        break;
    case 5:
        v->s.tr[0][2].pte &= ~(UINT64_C(1) << 5);
        break;
    case 6:
        v->s.psr |= UINT64_C(3) << 32;
        break;
    case 7:
        v->s.rr[tail >> 61] ^= UINT64_C(1) << 8;
        break;
    case 8:
        v->s.tr[0][2].pte &= ~UINT64_C(1);
        break;
    case 9:
        v->s.tr[0][2].pte |= UINT64_C(7) << 2;
        break;
    case 10:
        v->s.tr[0][2].pte = (v->s.tr[0][2].pte & 4095) | (UM_CODE + 0x78000);
        break;
    }
    ia64_mmu_changed(v, IA64_MMU_TR, 0, 2);
    ia64_mmu_changed(v, IA64_MMU_PKR, 0, 1);
    ia64_mmu_changed(v, IA64_MMU_RR, 0, tail >> 61);
}
#endif
