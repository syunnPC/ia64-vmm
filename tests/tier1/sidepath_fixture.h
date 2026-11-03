/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_TESTS_TIER1_SIDEPATH_FIXTURE_H
#define IA64VMM_TESTS_TIER1_SIDEPATH_FIXTURE_H
/* A conditional side path on the next page, with an acquire load,
 * release store and a conditional return to the original loop head. */
static void sidepath_fixture(uint64_t head, uint64_t side)
{
    struct ienc_bundle main[] = {
        {IENC_STOP(IENC_MIB),
         {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_br_cond(6, (int64_t)(side - head))}},
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_adds(0, 15, 2, 15), ienc_br_cond(7, -16)}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
        {IENC_MII, {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}}};
    struct ienc_bundle path[] = {
        {IENC_STOP(IENC_MMI), {ienc_ld(9, 8, true, 22, 33), ienc_nop_m(), ienc_adds(0, 16, 3, 16)}},
        {IENC_STOP(IENC_MMI), {ienc_st(9, 8, true, 34, 22), ienc_nop_m(), ienc_adds(0, 17, 5, 17)}},
        {IENC_STOP(IENC_MIB),
         {ienc_nop_m(), ienc_adds(0, 18, 7, 18), ienc_br_cond(8, (int64_t)(head - side - 32))}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
        {IENC_MII, {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}}};
    um_emit(head, main, sizeof(main) / sizeof(main[0]));
    um_emit(side, path, sizeof(path) / sizeof(path[0]));
}
#endif
