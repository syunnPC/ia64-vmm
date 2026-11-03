/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "tests/review/review.h"
#include "ia64/mmu.h"
#include "ia64/rse.h"
#include "ia64/interrupt.h"
#include "ia64/tlb.h"
#include "ia64vmm/guest_mem.h"

static uint8_t ram[8192] __attribute__((aligned(4096)));
static uint64_t ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void fast_path_bench(void)
{
    if (!getenv("FAST_PATH_BENCH"))
        return;
    struct vcpu_config cfg = {.itc_hz = 400000000};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v);
    struct gm_region region = {.base = 0x100000, .size = sizeof(ram), .kind = GM_RAM, .hva = ram};
    CHECK(gm_add_region(&region));
    v->s.psr = IA64_PSR_DT | IA64_PSR_RT;
    v->s.tc[1][IA64_TC_ENTRIES - 1] = (struct ia64_tlb_entry){
        .va = 0x200000, .pte = 0x100000 | 0x661, .itir = 12 << 2, .page_shift = 12, .valid = true};
    ia64_mmu_changed(v, IA64_MMU_ALL, 0, 0);
    struct ia64_translation t;
    uint64_t begin = ns();
    for (unsigned i = 0; i < 3000000; ++i)
        if (ia64_translate(v, 0x200000 + (i & 63) * 8, IA64_REF_DATA, 1, 0, false, &t).kind !=
            IA64_STEP_COMPLETE)
            abort();
    printf("fast-path-bench microtlb ns/access %.3f\n", (double)(ns() - begin) / 3000000);
    /* Kernel-like transitions use the semantic instructions and real fault
     * entry/rfi. DT persists through entry (SDM Vol.2 §5.5). */
    Ia64Instruction rsm = {.opcode = IA64_OP_RSM}, ssm = {.opcode = IA64_OP_SSM};
    rsm.operands.system.immediate = ssm.operands.system.immediate = IA64_PSR_I;
    Ia64Instruction bsw0 = {.opcode = IA64_OP_BSW0}, bsw1 = {.opcode = IA64_OP_BSW1};
    Ia64Instruction rfi = {.opcode = IA64_OP_RFI},
                    at = {.opcode = IA64_OP_NOP, .address = 0x300000};
    v->s.psr |= IA64_PSR_IC | IA64_PSR_I;
    v->s.ip = at.address;
    begin = ns();
    for (unsigned i = 0; i < 300000; ++i) {
        if (ia64_system_registers(v, &rsm).kind != IA64_STEP_COMPLETE)
            abort();
        if (ia64_translate(v, 0x200000, IA64_REF_DATA, 1, 0, false, &t).kind != IA64_STEP_COMPLETE)
            abort();
        if (ia64_system_registers(v, &ssm).kind != IA64_STEP_COMPLETE)
            abort();
        if (ia64_system_registers(v, &bsw1).kind != IA64_STEP_COMPLETE)
            abort();
        if (ia64_translate(v, 0x200008, IA64_REF_DATA, 1, 0, false, &t).kind != IA64_STEP_COMPLETE)
            abort();
        if (ia64_system_registers(v, &bsw0).kind != IA64_STEP_COMPLETE)
            abort();
        ia64_deliver_fault(v, &at, ia64_fault(IA64_FAULT_BREAK, 0));
        if (ia64_translate(v, 0x200010, IA64_REF_DATA, 1, 0, false, &t).kind != IA64_STEP_COMPLETE)
            abort();
        struct ia64_result returned = ia64_control(v, &rfi);
        if (returned.kind != IA64_STEP_BRANCH)
            abort();
        v->s.ip = returned.info;
        v->s.slot = returned.next_slot;
        if (ia64_translate(v, 0x200018, IA64_REF_DATA, 1, 0, false, &t).kind != IA64_STEP_COMPLETE)
            abort();
    }
    printf("fast-path-bench kernel ns/iteration %.3f\n", (double)(ns() - begin) / 300000);
    Ia64Instruction flush = {.opcode = IA64_OP_FLUSHRS};
    begin = ns();
    for (unsigned i = 0; i < 100000; ++i) {
        v->s.rse_dirty = 96;
        v->s.ar[AR_BSPSTORE] = 0x200000;
        if (ia64_rse(v, &flush).kind != IA64_STEP_COMPLETE)
            abort();
    }
    printf("fast-path-bench rse ns/flush %.3f\n", (double)(ns() - begin) / 100000);
    v->s.cr[CR_ITM] = ia64_itc_read(v) + UINT64_C(40000000000);
    ia64_timer_program(v);
    begin = ns();
    for (unsigned i = 0; i < 3000000; ++i)
        ia64_timer_poll(v);
    printf("fast-path-bench timer ns/poll %.3f\n", (double)(ns() - begin) / 3000000);
    /* Fixed 64-page working set: identical before/after capacity changes.
     * Cycling pages defeats the direct-mapped microTLB. Negative lookups
     * cannot bind and measure the full miss path separately. */
    vcpu_reset(v, NULL);
    v->s.psr = IA64_PSR_DT | IA64_PSR_IC;
    for (unsigned i = 0; i < 64; ++i)
        ia64_tc_insert(v, 0x400000 + i * 4096, 1, 0x100000 | 0x661, 12 << 2);
    begin = ns();
    for (unsigned i = 0; i < 3000000; ++i)
        if (ia64_translate(v, 0x400000 + (i & 63) * 4096, IA64_REF_DATA, 1, 0, false, &t).kind !=
            IA64_STEP_COMPLETE)
            abort();
    printf("fast-path-bench uncached-hit ns/access %.3f\n", (double)(ns() - begin) / 3000000);
    begin = ns();
    for (unsigned i = 0; i < 3000000; ++i)
        if (ia64_translate(v, 0x800000 + (i & 63) * 4096, IA64_REF_DATA, 1, 0, false, &t).fault !=
            IA64_FAULT_ALT_DTLB)
            abort();
    printf("fast-path-bench cold-miss ns/access %.3f\n", (double)(ns() - begin) / 3000000);
    /* Instruction-page ED queries on data-fault/deferral paths. Put code
     * at the final TC slot so the linear baseline visits all TRs and TCs. */
    vcpu_reset(v, NULL);
    v->s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
    v->s.ip = 0x1000000 + 127 * 4096;
    for (unsigned i = 0; i < 128; ++i)
        ia64_tc_insert(v, 0x1000000 + i * 4096, 0, 0x100000 | 0x661 | (UINT64_C(1) << 52), 12 << 2);
    begin = ns();
    for (unsigned i = 0; i < 3000000; ++i)
        if (!ia64_defer_fault(v, IA64_FAULT_DTLB))
            abort();
    printf("fast-path-bench instruction-ed ns/query %.3f\n", (double)(ns() - begin) / 3000000);
    CHECK(!v->timer_fired);
    vcpu_destroy(v);
}
REVIEW_SUITE("fast-path-bench", fast_path_bench)
