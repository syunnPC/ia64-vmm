/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included by the existing hosted runner to share its memory/clock fixtures. */
#include "ia64/tlb.h"
static void test_arch_memory(void)
{
    struct vcpu *v = new_vcpu();
    const Ia64Opcode loads[] = {IA64_OP_LD1S, IA64_OP_LD2S, IA64_OP_LD4S, IA64_OP_LD8S,
        IA64_OP_LD1SA, IA64_OP_LD2SA, IA64_OP_LD4SA, IA64_OP_LD8SA};
    for (unsigned k = 0; k < 8; ++k) {
        Ia64Instruction i = instruction(loads[k]);
        unsigned size = 1u << (k & 3);
        v->s.gr[10] = ENTRY + 0x8000;
        uint64_t bytes = UINT64_C(0x8877665544332211);
        CHECK(gm_write(v->s.gr[10], &bytes, 8));
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        CHECK(v->s.nat[8]);
        i.imm_base_update = true; i.operands.memory.immediate = 8;
        v->s.nat[10] = true;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.nat[8]);
        CHECK(v->s.nat[10] && v->s.gr[10] == ENTRY + 0x8008);
        v->s.nat[10] = false; v->s.gr[10] = UINT64_C(1) << 51;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.nat[8]);
        v->s.gr[10] = ENTRY + 0x20000;
        unsigned reads = mmio.reads;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.nat[8]);
        CHECK(mmio.reads == reads && v->s.gr[10] == ENTRY + 0x20008);
        v->s.gr[10] = (UINT64_C(1) << 63) | (ENTRY + 0x8000);
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.nat[8]);
        v->s.gr[10] = ENTRY + RAM_SIZE; v->s.psr = IA64_PSR_ED;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.nat[8]);
        if (size > 1) {
            v->s.gr[10] = ENTRY + 0x8001; v->s.psr = IA64_PSR_IC;
            v->s.gr[8] = 123;
            CHECK(ia64_execute(v, &i).fault == IA64_FAULT_UNALIGNED);
            CHECK(v->s.gr[10] == ENTRY + 0x8001 && v->s.gr[8] == 123);
            v->s.psr = 0;
            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.nat[8]);
        }
        v->s.psr = IA64_PSR_DT; v->s.gr[10] = ENTRY;
        struct ia64_result r = ia64_execute(v, &i);
        CHECK(r.kind == IA64_STEP_COMPLETE && v->s.nat[8]);
        v->s.psr = 0;
    }
    Ia64Instruction i = instruction(IA64_OP_LFETCH_FAULT);
    i.reg_base_update = true; v->s.gr[10] = ENTRY + 3; v->s.gr[9] = 8; v->s.nat[9] = true;
    unsigned reads = mmio.reads;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.nat[10]);
    CHECK(v->s.gr[10] == ENTRY + 11 && mmio.reads == reads);
    v->s.psr = IA64_PSR_ED;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    v->s.psr = 0;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT);
    i.opcode = IA64_OP_LFETCH; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    i.opcode = IA64_OP_LFETCH_COUNT; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    i.reg_base_update = false;
    uint64_t unchanged = v->s.gr[10];
    i.opcode = IA64_OP_LFETCH;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    i.opcode = IA64_OP_LFETCH_COUNT;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    v->s.psr = IA64_PSR_ED; i.opcode = IA64_OP_LFETCH_FAULT;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[10] == unchanged && v->s.nat[10] && mmio.reads == reads);
    v->s.psr = 0;
    v->s.nat[10] = false; v->s.gr[10] = (UINT64_C(1) << 51) | 1;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_UNIMPL_DATA);
    v->s.gr[10] = ENTRY + 0x20000;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && mmio.reads == reads);
    i.opcode = IA64_OP_FC; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && mmio.reads == reads);
    i.opcode = IA64_OP_FC_I; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && mmio.reads == reads);
    i = instruction(IA64_OP_SRLZ); v->s.psr = UINT64_C(1) << 32;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    v->s.psr = 0; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    i.opcode = IA64_OP_SRLZ_D; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    /* Fill the decoded cache, change backing bytes without notifying it,
     * then prove fc.i makes the new instruction visible. */
    bundle_write(ENTRY, 0, 0, 0, 0);
    const struct ia64_bundle *b;
    CHECK(code_get_bundle(v, ENTRY, &b).kind == IA64_STEP_COMPLETE);
    uint8_t old = ram[1]; ram[1] ^= 1;
    i = instruction(IA64_OP_FC_I); v->s.gr[10] = ENTRY;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(code_get_bundle(v, ENTRY, &b).kind == IA64_STEP_COMPLETE);
    CHECK(b->low != 0); ram[1] = old; code_notify_vmm_write(ENTRY, 16);
    vcpu_destroy(v);
}

static void test_arch_memory16(void)
{
    struct vcpu *v = new_vcpu();
    Ia64Instruction i = instruction(IA64_OP_ST16);
    v->s.gr[10] = ENTRY + 0x8200; v->s.gr[9] = UINT64_C(0x1122334455667788);
    v->s.ar[AR_CSD] = UINT64_C(0x99aabbccddeeff00);
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    i.opcode = IA64_OP_LD16;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == v->s.gr[9] && v->s.ar[AR_CSD] == UINT64_C(0x99aabbccddeeff00));
    i.opcode = IA64_OP_CMP8XCHG16;
    v->s.ar[AR_CCV] = v->s.gr[9]; v->s.gr[9] = 11; v->s.ar[AR_CSD] = 22;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == UINT64_C(0x1122334455667788));
    v->s.gr[10] += 8; v->s.ar[AR_CCV] = 22; v->s.gr[9] = 33; v->s.ar[AR_CSD] = 44;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 22);
    v->s.ar[AR_CCV] = 1; v->s.gr[9] = 55;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 44);
    i.opcode = IA64_OP_LD16; v->s.gr[10] -= 8;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 33 && v->s.ar[AR_CSD] == 44);
    v->s.gr[10] += 8; v->s.gr[8] = 123;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_UNALIGNED && v->s.gr[8] == 123);
    v->s.gr[10] = UINT64_C(1) << 63;
    v->s.gr[10] |= ENTRY + 0x8200;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_UNSUPPORTED_DATA);
    v->s.nat[10] = true; CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT);
    v->s.nat[10] = false; v->s.gr[10] = ENTRY + RAM_SIZE;
    CHECK(ia64_execute(v, &i).exit == VCPU_EXIT_BAD_ACCESS);
    v->s.gr[10] = ENTRY + 0x20000; unsigned reads = mmio.reads, writes = mmio.writes;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(mmio.reads == reads + 2 && mmio.writes == writes);
    CHECK(v->s.gr[8] == UINT64_C(0xfedcba9876543210) && v->s.ar[AR_CSD] == v->s.gr[8]);
    vcpu_destroy(v);
}

static void test_arch_rse(void)
{
    struct vcpu *v = new_vcpu();
    Ia64Instruction rotating = instruction(IA64_OP_ALLOC);
    rotating.operands.system.immediate = 0x4010; /* sor=8 > sol=0 */
    CHECK(ia64_execute(v, &rotating).kind == IA64_STEP_COMPLETE && v->s.cfm == 0x4010);
    rotating.operands.system.immediate = 0x4007; /* sor=8 > sof=7 */
    CHECK(ia64_execute(v, &rotating).fault == IA64_FAULT_ILLEGAL && v->s.cfm == 0x4010);
    rotating.operands.system.immediate = 0x10;
    for (unsigned shift = 18; shift <= 32; shift += 7) {
        v->s.cfm = 0x4010 | (UINT64_C(1) << shift);
        CHECK(ia64_execute(v, &rotating).fault == IA64_FAULT_RESERVED);
    }
    v->s.cfm = 0;
    v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = ENTRY + 0x81e8;
    Ia64Instruction alloc = instruction(IA64_OP_ALLOC);
    alloc.operands.system.immediate = 16 | (16u << 7);
    Ia64Instruction call = instruction(IA64_OP_BR_CALL); call.slot = 2;
    call.operands.branch.link = 0;
    uint64_t saved_pfs[24];
    for (unsigned depth = 0; depth < 24; ++depth) {
        CHECK(ia64_execute(v, &alloc).kind == IA64_STEP_COMPLETE);
        saved_pfs[depth] = v->s.ar[AR_PFS];
        for (unsigned j = 0; j < 16; ++j)
            ia64_gr_write(&v->s, 32 + j, depth * 100 + j, (depth + j) % 3 == 0);
        CHECK(v->s.rse_dirty + (v->s.cfm & 127) <= 96);
        CHECK(ia64_execute(v, &call).kind == IA64_STEP_BRANCH);
    }
    CHECK(v->s.ar[AR_BSPSTORE] > ENTRY + 0x8200);
    Ia64Instruction ret = instruction(IA64_OP_BR_RET);
    for (unsigned depth = 24; depth--;) {
        CHECK(ia64_execute(v, &ret).kind == IA64_STEP_BRANCH);
        CHECK(ia64_rse_resume(v).kind == IA64_STEP_COMPLETE);
        for (unsigned j = 0; j < 16; ++j) {
            CHECK(ia64_nat_read(&v->s, 32 + j) == ((depth + j) % 3 == 0));
            if (!ia64_nat_read(&v->s, 32 + j)) CHECK(ia64_gr_read(&v->s, 32 + j) == depth * 100 + j);
        }
        v->s.ar[AR_PFS] = saved_pfs[depth];
    }
    CHECK(v->s.ar[AR_BSP] == ENTRY + 0x81e8 && v->s.rse_bof == 0);
    /* cover/flushrs must include the current frame and its collection word. */
    v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = ENTRY + 0x81e8;
    v->s.rse_dirty = 0; v->s.psr = 0;
    CHECK(ia64_execute(v, &alloc).kind == IA64_STEP_COMPLETE);
    ia64_gr_write(&v->s, 32, 77, false); ia64_gr_write(&v->s, 33, 0, true);
    Ia64Instruction cover = instruction(IA64_OP_COVER);
    CHECK(ia64_execute(v, &cover).kind == IA64_STEP_COMPLETE);
    uint64_t ifs = v->s.cr[CR_IFS];
    CHECK(ifs == ((UINT64_C(1) << 63) | 16 | (16u << 7)) && v->s.cfm == 0);
    Ia64Instruction flush = instruction(IA64_OP_FLUSHRS);
    CHECK(ia64_execute(v, &flush).kind == IA64_STEP_COMPLETE);
    CHECK(!v->s.rse_dirty && v->s.ar[AR_BSPSTORE] == v->s.ar[AR_BSP]);
    uint64_t collection_word = 0; CHECK(gm_read(ENTRY + 0x81f8, &collection_word, 8));
    CHECK(collection_word & (UINT64_C(1) << 62));
    Ia64Instruction loadrs = instruction(IA64_OP_LOADRS);
    v->s.ar[AR_RSC] = (v->s.ar[AR_BSP] - (ENTRY + 0x81e8)) << 16;
    CHECK(ia64_execute(v, &loadrs).kind == IA64_STEP_COMPLETE && v->s.rse_dirty == 16);
    v->s.ar[AR_PFS] = ifs & ~(UINT64_C(1) << 63);
    CHECK(ia64_execute(v, &ret).kind == IA64_STEP_BRANCH);
    CHECK(ia64_gr_read(&v->s, 32) == 77 && ia64_nat_read(&v->s, 33));
    /* loadrs must keep dirty values whose backing-store copy is stale. */
    CHECK(ia64_execute(v, &cover).kind == IA64_STEP_COMPLETE);
    CHECK(ia64_execute(v, &loadrs).kind == IA64_STEP_COMPLETE);
    CHECK(ia64_execute(v, &ret).kind == IA64_STEP_BRANCH && ia64_gr_read(&v->s, 32) == 77);
    v->s.ar[AR_RSC] = 3;
    CHECK(ia64_execute(v, &loadrs).fault == IA64_FAULT_ILLEGAL);
    v->s.ar[AR_RSC] = UINT64_C(1024) << 16;
    CHECK(ia64_execute(v, &loadrs).fault == IA64_FAULT_ILLEGAL);
    alloc.operands.system.immediate = 96 | (96u << 7);
    CHECK(ia64_execute(v, &alloc).kind == IA64_STEP_COMPLETE);
    CHECK(ia64_execute(v, &call).kind == IA64_STEP_BRANCH);
    unsigned bof = v->s.rse_bof; uint64_t cfm = v->s.cfm, bspstore = v->s.ar[AR_BSPSTORE];
    v->s.ar[AR_BSPSTORE] = ENTRY + RAM_SIZE;
    CHECK(ia64_execute(v, &alloc).exit == VCPU_EXIT_BAD_ACCESS);
    CHECK(v->s.rse_bof == bof && v->s.cfm == cfm && v->s.rse_dirty == 96);
    v->s.ar[AR_BSPSTORE] = bspstore;
    v->s.psr |= IA64_PSR_RT;
    struct ia64_result r = ia64_execute(v, &alloc);
    CHECK(r.fault == IA64_FAULT_NESTED_DTLB);
    vcpu_destroy(v);
}

static void test_arch_registers(void)
{
    struct vcpu *v = new_vcpu();
    Ia64Instruction i = instruction(IA64_OP_MOV_CPUID_INDEXED);
    i.operands.system.register_index = 10;
    const uint64_t expected[] = {UINT64_C(0x49656e69756e6547), 0x6c65746e, 0, 0x20000704, 5};
    for (unsigned index = 0; index < 5; ++index) {
        v->s.gr[10] = index;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == expected[index]);
    }
    v->s.gr[10] = 5; CHECK(ia64_execute(v, &i).fault == IA64_FAULT_RESERVED);
    v->s.nat[10] = true; CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT);
    v->s.nat[10] = false;
    for (unsigned bit = 32; bit < 64; ++bit) {
        i = instruction(IA64_OP_TF_Z); i.operands.integer.immediate = bit;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && ia64_pr_read(&v->s, 6) && !ia64_pr_read(&v->s, 7));
        i.opcode = IA64_OP_TF_NZ;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && !ia64_pr_read(&v->s, 6) && ia64_pr_read(&v->s, 7));
        i.qp = 1; i.compare_unc = true; v->s.pr |= 3u << 6;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && !(v->s.pr & (3u << 6)));
    }
    i = instruction(IA64_OP_CLZ); CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
    i.opcode = IA64_OP_MPY4; CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
    v->s.gr[16] = 111; v->s.nat[16] = true;
    i = instruction(IA64_OP_BSW1);
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[16] == 0 && !v->s.nat[16]);
    v->s.gr[16] = 222;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[16] == 222);
    i.opcode = IA64_OP_BSW0;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[16] == 111 && v->s.nat[16]);
    i.opcode = IA64_OP_BSW1; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[16] == 222);
    i.opcode = IA64_OP_MOV_GRPSR; v->s.gr[8] = (UINT64_C(3) << 41) | IA64_PSR_IC;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.psr == (IA64_PSR_BN | IA64_PSR_IC));
    v->s.gr[8] = 1; CHECK(ia64_execute(v, &i).fault == IA64_FAULT_RESERVED);
    i.opcode = IA64_OP_MOV_PSRGR;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == IA64_PSR_IC);
    i.opcode = IA64_OP_SSM; i.operands.system.immediate = IA64_PSR_I;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && (v->s.psr & IA64_PSR_I));
    i.opcode = IA64_OP_RSM;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && !(v->s.psr & IA64_PSR_I));
    v->s.psr = UINT64_C(1) << 32;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_PRIVILEGED);
    i.opcode = IA64_OP_SUM_UM; i.operands.system.immediate = 8;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && (v->s.psr & 8));
    i.opcode = IA64_OP_RUM;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && !(v->s.psr & 8));
    i.opcode = IA64_OP_MOV_GRUM; v->s.gr[8] = 0x30;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && (v->s.psr & 0x3e) == 0x30);
    i.opcode = IA64_OP_MOV_UMGR;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0x30);
    v->s.psr = 0;
    const struct {Ia64Opcode read, write; unsigned limit;} banks[] = {
        {IA64_OP_MOV_PKRGR_INDEXED, IA64_OP_MOV_GRPKR_INDEXED, 16},
        {IA64_OP_MOV_DBRGR_INDEXED, IA64_OP_MOV_GRDBR_INDEXED, 8},
        {IA64_OP_MOV_IBRGR_INDEXED, IA64_OP_MOV_GRIBR_INDEXED, 8},
        {IA64_OP_MOV_MSRGR, IA64_OP_MOV_GRMSR, 1024},
    };
    for (unsigned b = 0; b < IA64VMM_ARRAY_SIZE(banks); ++b) {
        i = instruction(banks[b].write); i.operands.system.register_index = 10;
        v->s.gr[10] = banks[b].limit - 1; v->s.gr[8] = 0x1201;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        i.opcode = banks[b].read; v->s.gr[8] = 0;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0x1201);
        v->s.gr[10] = banks[b].limit;
        struct ia64_result r = ia64_execute(v, &i);
        CHECK(b < 3 ? r.fault == IA64_FAULT_RESERVED : r.kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0);
        v->s.nat[10] = true; CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT);
        v->s.nat[10] = false;
    }
    i = instruction(IA64_OP_MOV_GRRR); i.operands.system.source = 10;
    for (unsigned region = 0; region < 8; ++region) {
        v->s.gr[10] = (uint64_t)region << 61; v->s.gr[8] = ((region + 1) << 8) | 48;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        i.opcode = IA64_OP_MOV_RRGR;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == (((region + 1) << 8) | 48));
        i.opcode = IA64_OP_MOV_GRRR;
    }
    v->s.gr[8] = 2; CHECK(ia64_execute(v, &i).fault == IA64_FAULT_RESERVED);
    /* High index bits are ignored, and CPUID/PMD reads are unprivileged. */
    i = instruction(IA64_OP_MOV_CPUID_INDEXED); i.operands.system.register_index = 10;
    v->s.gr[10] = 0x103; v->s.psr = UINT64_C(3) << 32;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0x20000704);
    i.opcode = IA64_OP_MOV_PMDGR_INDEXED; v->s.gr[10] = 0x104; v->s.pmd[4] = 99;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 99);
    v->s.psr |= UINT64_C(1) << 20;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0);
    v->s.psr = UINT64_C(3) << 32; v->s.pmc[4] = 64;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0);
    i.opcode = IA64_OP_MOV_GRPMD_INDEXED;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_PRIVILEGED);
    v->s.psr = 0; i.opcode = IA64_OP_MOV_GRPKR_INDEXED; v->s.gr[8] = 0x201; v->s.gr[10] = 0x100;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.pkr[0] == 0x201);
    v->s.gr[10] = 1;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.pkr[0] == 0x200 && v->s.pkr[1] == 0x201);
    /* BSPSTORE rebases a nonempty dirty partition, without writing it. */
    v->s.cfm = 8 | (8u << 7); v->s.rse_dirty = 7;
    i = instruction(IA64_OP_MOV_GRAR); i.unit = IA64_UNIT_M;
    i.operands.system.source = AR_BSPSTORE; v->s.ar[AR_RSC] = 0; v->s.gr[8] = ENTRY + 0x81f0;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.ar[AR_BSPSTORE] == ENTRY + 0x81f0 && v->s.ar[AR_BSP] == ENTRY + 0x8230);
    i.opcode = IA64_OP_MOV_ARGR; i.operands.system.source = AR_BSP;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == ENTRY + 0x8230);
    vcpu_destroy(v);
}

static void test_arch_fault_priority(void)
{
    struct vcpu *v = new_vcpu();
    Ia64Instruction i = instruction(IA64_OP_LD8);
    v->s.gr[10] = (UINT64_C(1) << 51) | 1; v->s.nat[10] = true;
    v->s.gr[8] = 99;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT && v->s.gr[8] == 99);
    i.operands.memory.destination = 0;
    i.reserved_field = true;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
    i.reserved_field = false;
    i.operands.memory.destination = 8; v->s.nat[10] = false;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_UNIMPL_DATA && v->s.gr[8] == 99);
    i = instruction(IA64_OP_MOV_GRCR); i.operands.system.source = 18;
    v->s.psr = UINT64_C(3) << 32; v->s.nat[8] = true;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_PRIVILEGED);
    i.operands.system.source = CR_IVA;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_PRIVILEGED);
    v->s.psr = 0;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT);
    i = instruction(IA64_OP_MOV_GRAR); i.unit = IA64_UNIT_M;
    i.operands.system.source = AR_BSP;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
    i.operands.system.source = 0; v->s.psr = UINT64_C(1) << 32;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT);
    v->s.nat[8] = false;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_PRIVILEGED_REG);
    /* An RSE spill's address fault carries RS/W and commits no memory/BOF. */
    vcpu_reset(v, NULL); v->s.ar[AR_BSPSTORE] = UINT64_C(1) << 51;
    v->s.ar[AR_BSP] = (UINT64_C(1) << 51) + 8; v->s.rse_dirty = 1;
    i = instruction(IA64_OP_FLUSHRS);
    struct ia64_result r = ia64_execute(v, &i);
    CHECK(r.fault == IA64_FAULT_UNIMPL_DATA && v->s.rse_dirty == 1 && v->s.rse_bof == 0);
    v->s.psr = IA64_PSR_IC; v->s.cr[CR_IVA] = ENTRY;
    ia64_deliver_fault(v, &i, r);
    CHECK((v->s.cr[CR_ISR] & (UINT64_C(31) << 33)) == ((UINT64_C(1) << 37) | (UINT64_C(1) << 33)));
    CHECK((v->s.cr[CR_ISR] & 0xffff) == 0x30 && v->s.cr[CR_IFA] == (UINT64_C(1) << 51));
    /* Exercise every implemented CR through its decoded move helper. */
    const unsigned crs[] = {0, 1, 2, 8, 16, 17, 19, 20, 21, 22, 23, 24, 25,
        64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 80, 81};
    vcpu_reset(v, NULL);
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(crs); ++n) {
        i = instruction(IA64_OP_MOV_GRCR); i.operands.system.source = crs[n];
        v->s.gr[8] = crs[n] == CR_PTA ? 60 : 0;
        bool readonly = crs[n] == CR_IVR || (crs[n] >= CR_IRR0 && crs[n] < CR_IRR0 + 4);
        CHECK(readonly ? ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL : ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        i.opcode = IA64_OP_MOV_CRGR;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && !v->s.nat[8]);
    }
    /* CR9 is default storage, while
     * privilege/NaT failures leave it and the read destination untouched. */
    vcpu_reset(v, NULL);
    i = instruction(IA64_OP_MOV_CRGR); i.operands.system.source = 9;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0);
    v->s.psr = IA64_PSR_IC; v->s.gr[8] = UINT64_C(0x123456789abcdef0);
    i.opcode = IA64_OP_MOV_GRCR;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    v->s.nat[8] = true; v->s.gr[8] = 0;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT && v->s.cr[9] == UINT64_C(0x123456789abcdef0));
    i.opcode = IA64_OP_MOV_CRGR;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && !v->s.nat[8] && v->s.gr[8] == v->s.cr[9]);
    v->s.psr |= UINT64_C(1) << 32; v->s.gr[8] = 99;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_PRIVILEGED && v->s.gr[8] == 99);
    vcpu_reset(v, NULL); v->s.gr[8] = 99;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0);
    /* c4.inc exercises these modes through actual interruption delivery. */
    vcpu_destroy(v);
}

static unsigned timer_clock_reads;
static uint64_t timer_clock_hook(const struct vcpu *v, uint64_t value)
{
    (void)v; (void)value;
    ++timer_clock_reads;
    return 777;
}
static uint64_t timer_write_clock_hook(const struct vcpu *v, uint64_t value)
{
    (void)v;
    ++timer_clock_reads;
    return value;
}
static void test_arch_timer_write_checks(void)
{
    struct vcpu *v = new_vcpu();
    const unsigned registers[] = {CR_ITM, CR_ITV, AR_ITC};
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(registers); ++n) {
        vcpu_reset(v, NULL);
        v->clock_hook = timer_write_clock_hook;
        Ia64Instruction i = instruction(n == 2 ? IA64_OP_MOV_GRAR : IA64_OP_MOV_GRCR);
        i.unit = IA64_UNIT_M; i.operands.system.source = registers[n];
        v->s.gr[8] = 150; timer_clock_reads = 0;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        /* Without an armed timer only ITM programming and ITC writes
         * sample the clock; an ITV write needs no clock sample. */
        CHECK(timer_clock_reads == (n == 1 ? 0u : 1u));
        v->s.cr[CR_ITM] = 10; ia64_timer_program(v);
        timer_clock_reads = 0; v->s.psr = UINT64_C(1) << 32;
        CHECK(ia64_execute(v, &i).fault == (n == 2 ?
              IA64_FAULT_PRIVILEGED_REG : IA64_FAULT_PRIVILEGED));
        v->s.psr = 0; v->s.nat[8] = true;
        CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT);
        v->s.nat[8] = false;
        if (n == 1) {
            v->s.gr[8] |= 1u << 8;
            CHECK(ia64_execute(v, &i).fault == IA64_FAULT_RESERVED);
        }
        CHECK(timer_clock_reads == 0);
    }
    vcpu_destroy(v);
}

static void test_arch_timer_deadlines(void)
{
    struct vcpu *v = new_vcpu();
    const struct { uint64_t host_hz, itc_hz, offset, itm, expires; } cases[] = {
        {1000000000, 400000000, 0, 3, 8},
        {10, 3, 0, 7, 24},
        {3, 10, 0, 5, 2}, /* A host tick may cross ITM without sampling equality. */
        {10, 3, UINT64_MAX - 2, 1, 14},
        {10, 3, UINT64_MAX - 2, 0, 10},
        {UINT64_MAX, UINT64_MAX - 1, 0, 3, 4},
        {UINT64_MAX, UINT64_MAX, UINT64_MAX, 2, 3}
    };
    mock_clock = true;
    v->cfg.itc_icount = false;
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(cases); ++i) {
        mock_frequency = cases[i].host_hz; mock_counter = 100;
        v->cfg.itc_hz = cases[i].itc_hz; v->clock_origin = 100;
        v->clock_offset = cases[i].offset;
        v->s.cr[CR_ITV] = 120; v->s.cr[CR_ITM] = cases[i].itm;
        ia64_timer_program(v);
        for (uint64_t tick = 0; tick <= cases[i].expires; ++tick) {
            mock_counter = 100 + tick;
            uint64_t itc = cases[i].offset +
                (uint64_t)((__uint128_t)tick * cases[i].itc_hz / cases[i].host_hz);
            CHECK(ia64_itc_read(v) == itc);
            CHECK(vcpu_next_timer_deadline(v) == 100 + cases[i].expires);
            ia64_timer_poll(v);
            CHECK(v->timer_fired == (tick == cases[i].expires));
        }
        CHECK(vcpu_next_timer_deadline(v) == UINT64_MAX);
        CHECK(ia64_ivr_read(v) == 120); ia64_eoi(v);
        /* Crossing since the last poll must request immediate timer work. */
        mock_counter = 100; ia64_timer_program(v);
        mock_counter = 102 + cases[i].expires;
        CHECK(vcpu_next_timer_deadline(v) == mock_counter);
        ia64_timer_poll(v); CHECK(v->timer_fired);
        CHECK(ia64_ivr_read(v) == 120); ia64_eoi(v);
    }
    mock_frequency = 10; mock_counter = 0;
    v->cfg.itc_hz = 3; v->clock_origin = v->clock_offset = 0;
    v->s.cr[CR_ITM] = 7; ia64_timer_program(v); ia64_timer_poll(v);
    mock_counter = 1; ia64_itc_write(v, 6);
    for (; mock_counter < 4; ++mock_counter) {
        CHECK(vcpu_next_timer_deadline(v) == 4);
        ia64_timer_poll(v); CHECK(!v->timer_fired);
    }
    ia64_timer_poll(v); CHECK(v->timer_fired);
    CHECK(ia64_ivr_read(v) == 120); ia64_eoi(v);
    v->s.cr[CR_ITM] = ia64_itc_read(v); ia64_timer_program(v);
    CHECK(vcpu_next_timer_deadline(v) == mock_counter);
    v->s.cr[CR_ITV] |= 1u << 16;
    CHECK(vcpu_next_timer_deadline(v) == UINT64_MAX);
    ia64_timer_poll(v); CHECK(v->timer_fired && ia64_ivr_read(v) == 15);
    /* A past match waits for unsigned wrap; oversized host times saturate. */
    mock_counter = 0; mock_frequency = 1; v->cfg.itc_hz = 1;
    v->clock_origin = 0; v->clock_offset = 10;
    v->s.cr[CR_ITV] = 120; v->s.cr[CR_ITM] = 9; ia64_timer_program(v);
    CHECK(vcpu_next_timer_deadline(v) == UINT64_MAX);
    mock_counter = 1; CHECK(vcpu_next_timer_deadline(v) == UINT64_MAX);
    ia64_timer_poll(v); CHECK(!v->timer_fired);
    mock_counter = 0; v->cfg.itc_hz = UINT64_MAX; ia64_timer_program(v);
    CHECK(vcpu_next_timer_deadline(v) == 1);
    mock_counter = 1; ia64_timer_poll(v); CHECK(v->timer_fired);
    CHECK(ia64_ivr_read(v) == 120); ia64_eoi(v);
    mock_frequency = UINT64_MAX; mock_counter = 0; v->clock_offset = 0;
    v->cfg.itc_hz = 1;
    v->s.cr[CR_ITM] = UINT64_MAX; ia64_timer_program(v);
    CHECK(vcpu_next_timer_deadline(v) == UINT64_MAX);
    mock_frequency = UINT64_C(0x1084210842108421); v->cfg.itc_hz = 2;
    v->s.cr[CR_ITM] = 31; ia64_timer_program(v);
    CHECK(vcpu_next_timer_deadline(v) == UINT64_MAX); /* Ceiling overflows. */
    mock_frequency = 3; mock_counter = UINT64_MAX - 10;
    v->clock_origin = mock_counter; v->s.cr[CR_ITM] = 100; ia64_timer_program(v);
    CHECK(vcpu_next_timer_deadline(v) == UINT64_MAX); /* Absolute time overflows. */
    /* Elapsed-counter wrap changes the clock formula and requires a poll. */
    mock_frequency = 10; mock_counter = 0; v->cfg.itc_hz = 3;
    v->clock_origin = 2; v->clock_offset = 0;
    v->s.cr[CR_ITM] = ia64_itc_read(v) + 2; ia64_timer_program(v);
    CHECK(vcpu_next_timer_deadline(v) == 2);
    mock_counter = 1; ia64_timer_poll(v); CHECK(!v->timer_fired);
    CHECK(vcpu_next_timer_deadline(v) == 2);
    mock_counter = 2; ia64_timer_poll(v); CHECK(v->timer_fired);
    CHECK(ia64_ivr_read(v) == 120); ia64_eoi(v);
    /* Hooked clocks retain their observed value and one hook per read. */
    mock_counter = 100; v->clock_origin = 100; v->clock_hook = timer_clock_hook;
    timer_clock_reads = 0; v->s.cr[CR_ITM] = 780; ia64_timer_program(v);
    CHECK(timer_clock_reads == 1 && vcpu_next_timer_deadline(v) == 110);
    CHECK(timer_clock_reads == 2);
    ia64_timer_poll(v); CHECK(!v->timer_fired && !v->timer_cache.valid);
    v->clock_hook = NULL;
    v->cfg.itc_icount = true; CHECK(vcpu_next_timer_deadline(v) == UINT64_MAX);
    v->cfg.itc_icount = false; v->timer_armed = false;
    CHECK(vcpu_next_timer_deadline(v) == UINT64_MAX);
    mock_clock = false;
    vcpu_destroy(v);
}

static void test_arch_interrupts(void)
{
    struct vcpu *v = new_vcpu();
    /* Break diagnostics apply to every immediate and privilege level. */
    const uint64_t break_immediates[] = {0, 0x100000, 0x1fffff};
    for (unsigned cpl = 0; cpl < 4; ++cpl)
        for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(break_immediates); ++n) {
            vcpu_reset(v, NULL);
            v->s.ip = ENTRY; v->s.cr[CR_IVA] = ENTRY;
            v->s.psr = IA64_PSR_IC | ((uint64_t)cpl << 32);
            Ia64Instruction brk = instruction(IA64_OP_BREAK);
            vcpu_set_stop_fault(IA64_FAULT_BREAK);
            ia64_deliver_fault(v, &brk, ia64_fault(IA64_FAULT_BREAK, break_immediates[n]));
            CHECK(v->requested_exit == VCPU_EXIT_DEBUG + 1);
            CHECK(v->s.ip == ENTRY + 0x2c00 && v->s.cr[CR_IIM] == break_immediates[n]);
        }
    vcpu_set_stop_fault(0);
    const struct {enum ia64_fault_kind kind; unsigned vector, code;} cases[] = {
        {IA64_FAULT_ILLEGAL, 0x5400, 0}, {IA64_FAULT_NAT, 0x5600, 0x10},
        {IA64_FAULT_RESERVED, 0x5400, 0x30}, {IA64_FAULT_PRIVILEGED, 0x5400, 0x10},
        {IA64_FAULT_BREAK, 0x2c00, 0}, {IA64_FAULT_UNALIGNED, 0x5a00, 0},
        {IA64_FAULT_UNSUPPORTED_DATA, 0x5b00, 0}, {IA64_FAULT_UNIMPL_DATA, 0x5400, 0x30},
        {IA64_FAULT_PRIVILEGED_REG, 0x5400, 0x20}, {IA64_FAULT_ALT_DTLB, 0x1000, 0},
        {IA64_FAULT_PAGE_NOT_PRESENT, 0x5000, 0},
        {IA64_FAULT_DATA_NAT, 0x5600, 0x20}, {IA64_FAULT_KEY_MISS, 0x1c00, 0},
    };
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(cases); ++k) {
        vcpu_reset(v, NULL);
        v->s.ip = ENTRY + 0x100; v->s.slot = 2; v->s.cr[CR_IVA] = ENTRY;
        v->s.psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN;
        v->s.cr[CR_IFA] = 99; v->s.cr[CR_IIM] = 88;
        v->last_successful_bundle = ENTRY + 0x80;
        Ia64Instruction i = instruction(cases[k].kind == IA64_FAULT_NAT ? IA64_OP_MOV_GRBR : IA64_OP_LD8);
        uint64_t psr = v->s.psr;
        ia64_deliver_fault(v, &i, ia64_fault(cases[k].kind, 123));
        CHECK(v->s.ip == ENTRY + cases[k].vector && v->s.slot == 0 && v->s.psr == 0);
        CHECK(v->s.cr[CR_IPSR] == (psr | (UINT64_C(2) << 41)));
        CHECK(v->s.cr[CR_IIP] == ENTRY + 0x100 && v->s.cr[CR_IIPA] == ENTRY + 0x80 && v->s.cr[CR_IFS] == 0);
        CHECK((v->s.cr[CR_ISR] & 0xffff) == cases[k].code && ((v->s.cr[CR_ISR] >> 41) & 3) == 2);
        CHECK(!(v->s.cr[CR_ISR] & (UINT64_C(1) << 39)));
        CHECK(v->s.cr[CR_IIM] == (cases[k].kind == IA64_FAULT_BREAK ? 123 : 88));
        i = instruction(IA64_OP_RFI);
        struct ia64_result r = ia64_execute(v, &i);
        CHECK(r.kind == IA64_STEP_BRANCH && r.info == ENTRY + 0x100 && r.next_slot == 2);
        CHECK(v->s.psr == v->s.cr[CR_IPSR]);
    }
    /* With collection disabled, saved interruption state survives nesting. */
    v->s.psr = 0; uint64_t ipsr = v->s.cr[CR_IPSR], iip = v->s.cr[CR_IIP];
    Ia64Instruction i = instruction(IA64_OP_BREAK);
    ia64_deliver_fault(v, &i, ia64_fault(IA64_FAULT_BREAK, 456));
    CHECK(v->s.cr[CR_IPSR] == ipsr && v->s.cr[CR_IIP] == iip && v->s.cr[CR_IIM] == 88);
    CHECK(v->s.cr[CR_ISR] & (UINT64_C(1) << 39));
    uint64_t isr = v->s.cr[CR_ISR];
    ia64_deliver_fault(v, &i, ia64_fault(IA64_FAULT_NESTED_DTLB, 789));
    CHECK(v->s.ip == ENTRY + 0x1400 && v->s.cr[CR_ISR] == isr);
    CHECK(v->s.cr[CR_IPSR] == ipsr && v->s.cr[CR_IIP] == iip);
    /* IVR acceptance is separate from vectoring, ignores PSR.i, and EOI
     * completes only the highest in-service vector. */
    vcpu_reset(v, NULL); v->s.cr[CR_IVA] = ENTRY;
    vcpu_raise_external(v, 0); vcpu_raise_external(v, 256);
    CHECK(ia64_ivr_read(v) == 15);
    vcpu_raise_external(v, 80); vcpu_raise_external(v, 90); vcpu_raise_external(v, 100);
    v->s.cr[CR_TPR] = 0x50;
    CHECK(ia64_ivr_read(v) == 100 && ia64_ivr_read(v) == 15);
    vcpu_raise_external(v, 101); CHECK(ia64_ivr_read(v) == 101);
    ia64_eoi(v); CHECK(ia64_ivr_read(v) == 15); ia64_eoi(v);
    v->s.cr[CR_TPR] = 0;
    CHECK(ia64_ivr_read(v) == 90); ia64_eoi(v); CHECK(ia64_ivr_read(v) == 80); ia64_eoi(v);
    vcpu_raise_external(v, 200); v->s.cr[CR_TPR] = 1u << 16;
    CHECK(ia64_ivr_read(v) == 15); v->s.cr[CR_TPR] = 0;
    CHECK(!ia64_external_deliver(v));
    v->s.ip = ENTRY; v->s.slot = 1; v->s.psr = IA64_PSR_I | IA64_PSR_IC;
    CHECK(ia64_external_deliver(v) && v->s.ip == ENTRY + 0x3000);
    CHECK(v->s.cr[CR_IPSR] == (IA64_PSR_I | IA64_PSR_IC | (UINT64_C(1) << 41)));
    CHECK(ia64_ivr_read(v) == 200); ia64_eoi(v);
    /* ITM equality, masking, past programming and unsigned wrap. */
    vcpu_reset(v, NULL); v->s.cr[CR_ITV] = 120; v->s.cr[CR_ITM] = 10;
    ia64_timer_program(v); v->retired = 9; ia64_timer_poll(v); CHECK(ia64_ivr_read(v) == 15);
    v->retired = 10; ia64_timer_poll(v); CHECK(ia64_ivr_read(v) == 120); ia64_eoi(v);
    CHECK(ia64_ivr_read(v) == 15);
    v->s.cr[CR_ITM] = 5; ia64_timer_program(v); v->retired = 20; ia64_timer_poll(v);
    CHECK(ia64_ivr_read(v) == 15);
    ia64_itc_write(v, UINT64_MAX - 2); v->s.cr[CR_ITM] = 1; ia64_timer_program(v);
    v->retired += 4; ia64_timer_poll(v); CHECK(ia64_ivr_read(v) == 120); ia64_eoi(v);
    v->s.cr[CR_ITM] = ia64_itc_read(v) + 5; v->s.cr[CR_ITV] |= 1u << 16; ia64_timer_program(v);
    v->retired += 5; ia64_timer_poll(v); v->s.cr[CR_ITV] &= ~(1u << 16);
    CHECK(ia64_ivr_read(v) == 15 && vcpu_next_timer_deadline(v) == UINT64_MAX);
    test_arch_timer_deadlines(); test_arch_timer_write_checks();
    /* CR access restrictions and reserved-field writes are before effect. */
    i = instruction(IA64_OP_MOV_GRCR); i.operands.system.source = CR_IVA; v->s.gr[8] = ENTRY + 123;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.cr[CR_IVA] == ENTRY);
    i.operands.system.source = CR_TPR; v->s.gr[8] = 0x100f0;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.cr[CR_TPR] == 0x100f0);
    v->s.gr[8] = 0x100; CHECK(ia64_execute(v, &i).fault == IA64_FAULT_RESERVED && v->s.cr[CR_TPR] == 0x100f0);
    i.operands.system.source = CR_IVR; CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
    i.operands.system.source = 18;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.cr[18] == 0x100);
    i.opcode = IA64_OP_MOV_CRGR; i.operands.system.source = CR_IIP; v->s.psr = IA64_PSR_IC;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
    v->s.psr = 0; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    /* A covered interrupted frame can be spilled by the handler and filled
     * by rfi; it restores IFM, not PFM.sol, and leaves ar.pfs/ar.ec alone. */
    vcpu_reset(v, NULL); v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = ENTRY + 0x8600;
    v->s.cfm = 16 | (8u << 7); ia64_gr_write(&v->s, 32, 55, false);
    i = instruction(IA64_OP_COVER); CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    i.opcode = IA64_OP_ALLOC; i.operands.system.immediate = 96 | (96u << 7);
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    i.opcode = IA64_OP_RFI; v->s.cr[CR_IIP] = ENTRY; v->s.cr[CR_IPSR] = IA64_PSR_IC | IA64_PSR_BN | (UINT64_C(1) << 41);
    v->s.ar[AR_PFS] = 123; v->s.ar[AR_EC] = 12;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_BRANCH && v->s.cfm == (16 | (8u << 7)));
    CHECK(ia64_gr_read(&v->s, 32) == 55 && v->s.ar[AR_PFS] == 123 && v->s.ar[AR_EC] == 12);
    /* Run rfi through the engine into slot 1, then retire only that slot. */
    uint64_t nop = UINT64_C(1) << 27;
    bundle_write(ENTRY, 1, nop, nop, nop);
    bundle_write(ENTRY + 16, 0x11, nop, nop, UINT64_C(8) << 27);
    v->s.psr = 0; v->s.ip = ENTRY + 16; v->s.slot = 2; v->s.cr[CR_IFS] = 0;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.ip == ENTRY && v->s.slot == 1);
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.slot == 2);
    /* ITM vectors through the engine at the exact next instruction boundary. */
    vcpu_reset(v, NULL); v->s.ip = ENTRY; v->s.slot = 1; v->s.cr[CR_IVA] = ENTRY;
    v->s.psr = IA64_PSR_I | IA64_PSR_IC; v->s.cr[CR_ITM] = 1; v->s.cr[CR_ITV] = 150;
    bundle_write(ENTRY + 0x3000, 0, nop, nop, nop);
    ia64_timer_program(v);
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.slot == 2);
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.ip == ENTRY + 0x3000 && v->s.slot == 1);
    CHECK(v->s.cr[CR_IIP] == ENTRY && ((v->s.cr[CR_IPSR] >> 41) & 3) == 2);
    CHECK(v->s.cr[CR_IIPA] == ENTRY && v->retired == 2 && ia64_ivr_read(v) == 150);
    vcpu_destroy(v);
}

static void test_tr_overlap(void)
{
    /* SDM Rev 2.3 Vol.2 Table 4-1 and §4.1.1.1: same-stream
     * TR overlaps require MCA, including the slot being written and ITC
     * inserts. Without a PALE_CHECK (the reference firmware) the CPU stops
     * exactly; tests/ia64/pal-events.inc covers the machine check. */
    for (unsigned side = 0; side < 2; ++side) {
        struct vcpu *v = new_vcpu();
        v->s.rr[0] = v->s.rr[1] = 0x100030;
        v->s.psr = 0;
        v->s.tr[side][0] = (struct ia64_tlb_entry){.va=0x900000, .rid=0x1000,
            .page_shift=20, .itir=80, .pte=0x900661, .valid=true};
        Ia64Instruction insn = instruction(side ? IA64_OP_ITR_D : IA64_OP_ITR_I);
        insn.operands.system.source = 9; insn.operands.system.register_index = 10;
        v->s.gr[9] = 0x940661;
        v->s.cr[CR_IFA] = (UINT64_C(1)<<61) | 0x940000;
        v->s.cr[CR_ITIR] = 18u<<2;
        uint64_t seq = v->mmu_seq;
        for (unsigned n = 0; n < 3; ++n) {
            v->s.gr[10] = n;
            if (n == 2) insn.opcode = side ? IA64_OP_ITC_D : IA64_OP_ITC_I;
            struct ia64_result r = ia64_execute(v, &insn);
            CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_UNIMPLEMENTED);
            CHECK(r.detail && !strcmp(r.detail, "TLB insert overlaps TR; MCA delivery unavailable"));
            CHECK(v->mmu_seq == seq && v->s.tr[side][0].pte == 0x900661);
            CHECK(!v->s.tr[side][1].valid && !v->s.tc[side][0].valid);
        }
        /* Different RID, opposite stream and adjacent pages are legal. */
        insn.opcode = side ? IA64_OP_ITR_D : IA64_OP_ITR_I;
        v->s.gr[10] = 1; v->s.rr[1] = 0x200030;
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
        v->s.rr[1] = 0x100030; v->s.cr[CR_IFA] = 0xa00000;
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
        insn.opcode = side ? IA64_OP_ITR_I : IA64_OP_ITR_D;
        v->s.cr[CR_IFA] = 0x940000;
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
        /* A larger insert enclosing the old page also overlaps. */
        insn.opcode = side ? IA64_OP_ITR_D : IA64_OP_ITR_I;
        v->s.cr[CR_IFA] = 0x800000; v->s.cr[CR_ITIR] = 22u<<2;
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_EXIT);
        vcpu_destroy(v);
    }
    /* Table 4-1 and Vol.2 §5.2.2.2.3: ptc.l and the local ptc.g/ptc.ga
     * must machine check on a TR overlap of either stream. Without a
     * PALE_CHECK the CPU stops before any TC purge or broadcast. */
    const Ia64Opcode purges[] = {IA64_OP_PTC_L, IA64_OP_PTC_G, IA64_OP_PTC_GA};
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned n = 0; n < 3; ++n) {
            struct vcpu *v = new_vcpu();
            v->s.rr[0] = v->s.rr[1] = 0x100030;
            v->s.psr = IA64_PSR_IC;
            v->s.tr[side][0] = (struct ia64_tlb_entry){.va=0x940000, .rid=0x1000,
                .page_shift=12, .itir=48, .pte=0x940661, .valid=true};
            ia64_tc_insert(v, 0x950000, side, 0x950661, 12u<<2);
            ia64_tc_insert(v, 0x940000, !side, 0x940661, 12u<<2);
            Ia64Instruction purge = instruction(purges[n]);
            purge.operands.system.source = 9; purge.operands.system.register_index = 10;
            v->s.gr[9] = 20u<<2; v->s.gr[10] = (UINT64_C(1)<<61) | 0x900000;
            uint64_t seq = v->mmu_seq;
            struct ia64_result p = ia64_execute(v, &purge);
            CHECK(p.kind == IA64_STEP_EXIT && p.exit == VCPU_EXIT_UNIMPLEMENTED);
            CHECK(p.detail && !strcmp(p.detail, "TLB purge overlaps TR; MCA delivery unavailable"));
            CHECK(v->mmu_seq == seq && v->s.tr[side][0].valid && !v->pal_mca_pending);
            CHECK(ia64_tlb_find(&v->s, 0x950000, side) && ia64_tlb_find(&v->s, 0x940000, !side));
            /* The page beside the TR and another RID do not overlap it. */
            v->s.gr[9] = 12u<<2; v->s.gr[10] = 0x941000;
            CHECK(ia64_execute(v, &purge).kind == IA64_STEP_COMPLETE);
            v->s.rr[1] = 0x200030; v->s.gr[10] = (UINT64_C(1)<<61) | 0x940000;
            CHECK(ia64_execute(v, &purge).kind == IA64_STEP_COMPLETE);
            CHECK(v->s.tr[side][0].valid && !v->pal_mca_pending);
            vcpu_destroy(v);
        }
}

static void test_arch_tlb(void)
{
    test_tr_overlap();
    struct vcpu *v = new_vcpu();
    const uint64_t va = 0x400000, pte = 0x300000 | 1 | (3u << 7) | (3u << 9);
    v->s.rr[0] = 48 | 0x100; v->s.cr[CR_ITIR] = 48 | 0x300;
    Ia64Instruction insert = instruction(IA64_OP_ITR_D);
    insert.operands.system.source = 9; insert.operands.system.register_index = 10;
    for (unsigned side = 0; side < 2; ++side) {
        insert.opcode = side ? IA64_OP_ITR_D : IA64_OP_ITR_I;
        for (unsigned slot = 0; slot < 32; ++slot) {
            v->s.gr[10] = slot; v->s.gr[9] = pte; v->s.cr[CR_IFA] = va + slot * 0x1000;
            v->s.psr = IA64_PSR_DT | IA64_PSR_IT | IA64_PSR_RT;
            CHECK(ia64_execute(v, &insert).kind == IA64_STEP_COMPLETE);
            CHECK(v->s.tr[side][slot].valid && v->s.tr[side][slot].va == v->s.cr[CR_IFA]);
        }
    }
    /* Every TR is indexable; reserved index/ITIR and NaT do not overwrite it. */
    v->s.gr[10] = 32;
    CHECK(ia64_execute(v, &insert).fault == IA64_FAULT_RESERVED);
    v->s.gr[10] = 0; v->s.cr[CR_ITIR] = 44;
    CHECK(ia64_execute(v, &insert).fault == IA64_FAULT_RESERVED && v->s.tr[1][0].pte == pte);
    v->s.nat[9] = true;
    CHECK(ia64_execute(v, &insert).fault == IA64_FAULT_NAT);
    v->s.nat[9] = false; v->s.cr[CR_ITIR] = 48 | 0x300; v->s.psr = IA64_PSR_IC;
    Ia64Instruction query = instruction(IA64_OP_TPA); query.operands.system.register_index = 10;
    v->s.gr[10] = va + 0x123;
    CHECK(ia64_execute(v, &query).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0x300123);
    query.opcode = IA64_OP_TAK;
    CHECK(ia64_execute(v, &query).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0x300);
    v->s.rr[0] = 48 | 0x200;
    CHECK(ia64_execute(v, &query).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 1);
    query.opcode = IA64_OP_TPA;
    CHECK(ia64_execute(v, &query).fault == IA64_FAULT_ALT_DTLB);
    v->s.psr = 0;
    CHECK(ia64_execute(v, &query).fault == IA64_FAULT_NESTED_DTLB);
    v->s.rr[0] = 48 | 0x100; v->s.psr = IA64_PSR_IC;
    /* Non-access ISR qualifications and collected translation state. */
    v->s.cr[CR_IVA] = ENTRY; v->s.cr[CR_PTA] = 0x800000 | (16u << 2);
    v->s.gr[10] = va + 0x21000;
    struct ia64_result r = ia64_execute(v, &query);
    ia64_deliver_fault(v, &query, r);
    CHECK(v->s.ip == ENTRY + 0x1000 && v->s.cr[CR_IFA] == va + 0x21000);
    CHECK(v->s.cr[CR_ITIR] == (48 | 0x100));
    CHECK((v->s.cr[CR_ISR] & (UINT64_C(7) << 33)) == (UINT64_C(1) << 35));
    v->s.psr = IA64_PSR_IC;
    /* Non-present, NaT-page, rights, privilege clamping and protection keys. */
    Ia64Instruction probe = instruction(IA64_OP_PROBE_R); probe.probe_imm = true;
    probe.operands.memory.immediate = 0; v->s.gr[10] = va;
    CHECK(ia64_execute(v, &probe).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 1);
    v->s.tr[1][0].pte &= ~UINT64_C(1);
    CHECK(ia64_execute(v, &probe).fault == IA64_FAULT_PAGE_NOT_PRESENT);
    v->s.tr[1][0].pte = pte | 28;
    CHECK(ia64_execute(v, &probe).fault == IA64_FAULT_DATA_NAT);
    v->s.tr[1][0].pte = 0x300001;
    probe.opcode = IA64_OP_PROBE_W;
    CHECK(ia64_execute(v, &probe).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0);
    probe.opcode = IA64_OP_PROBE_R; v->s.psr |= UINT64_C(1) << 32;
    CHECK(ia64_execute(v, &probe).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0);
    v->s.psr = IA64_PSR_IC | (UINT64_C(1) << 15); v->s.tr[1][0].pte = pte;
    CHECK(ia64_execute(v, &probe).fault == IA64_FAULT_KEY_MISS);
    v->s.pkr[5] = 0x305;
    CHECK(ia64_execute(v, &probe).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0);
    v->s.pkr[5] = 0x301;
    CHECK(ia64_execute(v, &probe).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 1);
    v->s.nat[10] = true; probe.operands.memory.destination = 0;
    CHECK(ia64_execute(v, &probe).fault == IA64_FAULT_ILLEGAL);
    probe.operands.memory.destination = 8;
    r = ia64_execute(v, &probe); CHECK(r.fault == IA64_FAULT_NAT);
    ia64_deliver_fault(v, &probe, r);
    CHECK((v->s.cr[CR_ISR] & 0xffff) == 0x12 && (v->s.cr[CR_ISR] & (UINT64_C(1) << 35)));
    v->s.nat[10] = false; v->s.psr = IA64_PSR_IC; probe.probe_fault = true;
    v->s.gr[10] = ENTRY + 0x20000; unsigned reads = mmio.reads;
    CHECK(ia64_execute(v, &probe).kind == IA64_STEP_COMPLETE && mmio.reads == reads);
    v->s.gr[10] = UINT64_C(1) << 51;
    CHECK(ia64_execute(v, &probe).fault == IA64_FAULT_UNIMPL_DATA);
    v->s.psr |= IA64_PSR_DT;
    r = ia64_execute(v, &probe);
    CHECK(r.fault == IA64_FAULT_ALT_DTLB);
    /* Hash/tag are unprivileged and work while translations are enabled. */
    query.opcode = IA64_OP_THASH; v->s.psr |= UINT64_C(3) << 32;
    v->s.gr[10] = (UINT64_C(2) << 61) | 0x456789; v->s.rr[2] = 48 | 0x700;
    CHECK(ia64_execute(v, &query).kind == IA64_STEP_COMPLETE && v->s.gr[8] == ((UINT64_C(2) << 61) | 0x8022b0));
    query.opcode = IA64_OP_TTAG;
    CHECK(ia64_execute(v, &query).kind == IA64_STEP_COMPLETE && v->s.gr[8] == ((UINT64_C(7) << 39) ^ 0x456));
    v->s.nat[10] = true;
    CHECK(ia64_execute(v, &query).kind == IA64_STEP_COMPLETE && v->s.nat[8]);
    v->s.nat[10] = false; query.opcode = IA64_OP_THASH; v->s.cr[CR_PTA] |= 0x100;
    CHECK(ia64_execute(v, &query).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0x808a20);
    /* SDM Vol.3 itc/itr: PSR.ic=1 is Illegal Operation, ahead of the CPL
     * check, and inserts nothing. */
    v->s.cr[CR_ITIR] = 48; v->s.cr[CR_IFA] = 0x500000; v->s.gr[9] = pte; v->s.gr[10] = 2;
    const struct ia64_tlb_entry tr2[2] = {v->s.tr[0][2], v->s.tr[1][2]};
    const Ia64Opcode inserts[] = {IA64_OP_ITC_I, IA64_OP_ITC_D, IA64_OP_ITR_I, IA64_OP_ITR_D};
    for (unsigned n = 0; n < 4; ++n) {
        insert.opcode = inserts[n];
        v->s.psr = IA64_PSR_IC;
        CHECK(ia64_execute(v, &insert).fault == IA64_FAULT_ILLEGAL);
        v->s.psr = IA64_PSR_IC | (UINT64_C(3) << 32);
        CHECK(ia64_execute(v, &insert).fault == IA64_FAULT_ILLEGAL);
        v->s.psr = UINT64_C(3) << 32;
        CHECK(ia64_execute(v, &insert).fault == IA64_FAULT_PRIVILEGED);
    }
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned n = 0; n < 128; ++n) CHECK(!v->s.tc[side][n].valid);
    CHECK(!memcmp(&tr2[0], &v->s.tr[0][2], sizeof(tr2[0])) && !memcmp(&tr2[1], &v->s.tr[1][2], sizeof(tr2[1])));
    /* 128 entries per side, replacement, RID isolation, and all purge forms. */
    v->s.psr = 0;
    for (unsigned side = 0; side < 2; ++side) {
        insert.opcode = side ? IA64_OP_ITC_D : IA64_OP_ITC_I;
        for (unsigned n = 0; n < 129; ++n) {
            v->s.cr[CR_IFA] = 0x500000 + n * 4096; v->s.gr[9] = pte;
            CHECK(ia64_execute(v, &insert).kind == IA64_STEP_COMPLETE);
        }
        for (unsigned n = 0; n < 128; ++n) CHECK(v->s.tc[side][n].valid);
        CHECK(v->s.tc[side][0].va == 0x580000);
    }
    v->s.psr = IA64_PSR_IC;
    query.opcode = IA64_OP_TPA; v->s.gr[10] = 0x500000;
    CHECK(ia64_execute(v, &query).fault == IA64_FAULT_ALT_DTLB);
    v->s.gr[10] = 0x501000;
    CHECK(ia64_execute(v, &query).kind == IA64_STEP_COMPLETE);
    Ia64Instruction purge = instruction(IA64_OP_PTC_L);
    purge.operands.system.source = 9; purge.operands.system.register_index = 10; v->s.gr[9] = 48;
    CHECK(ia64_execute(v, &purge).kind == IA64_STEP_COMPLETE);
    CHECK(!v->s.tc[0][1].valid && !v->s.tc[1][1].valid && v->s.tr[1][0].valid);
    purge.opcode = IA64_OP_PTC_G; v->s.gr[10] = 0x502000;
    CHECK(ia64_execute(v, &purge).kind == IA64_STEP_COMPLETE && !v->s.tc[0][2].valid && !v->s.tc[1][2].valid);
    purge.opcode = IA64_OP_PTC_GA; v->s.gr[10] = 0x503000;
    CHECK(ia64_execute(v, &purge).kind == IA64_STEP_COMPLETE && !v->s.tc[1][3].valid);
    v->s.rr[0] = 48 | 0x200; purge.opcode = IA64_OP_PTR_D; v->s.gr[10] = va;
    CHECK(ia64_execute(v, &purge).kind == IA64_STEP_COMPLETE && v->s.tr[1][0].valid);
    v->s.rr[0] = 48 | 0x100;
    CHECK(ia64_execute(v, &purge).kind == IA64_STEP_COMPLETE && !v->s.tr[1][0].valid && v->s.tr[0][0].valid);
    purge.opcode = IA64_OP_PTR_I;
    CHECK(ia64_execute(v, &purge).kind == IA64_STEP_COMPLETE && !v->s.tr[0][0].valid);
    /* A tiny purge inside a large TR removes the entire overlapping mapping. */
    insert.opcode = IA64_OP_ITR_D; v->s.cr[CR_ITIR] = 64; v->s.cr[CR_IFA] = 0x600000; v->s.gr[10] = 0;
    v->s.psr = 0;
    CHECK(ia64_execute(v, &insert).kind == IA64_STEP_COMPLETE);
    v->s.psr = IA64_PSR_IC;
    purge.opcode = IA64_OP_PTR_D; v->s.gr[10] = 0x60f000;
    CHECK(ia64_execute(v, &purge).kind == IA64_STEP_COMPLETE && !v->s.tr[1][0].valid);
    purge.opcode = IA64_OP_PTC_E;
    CHECK(ia64_execute(v, &purge).kind == IA64_STEP_COMPLETE);
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned n = 0; n < 128; ++n) CHECK(!v->s.tc[side][n].valid);
        CHECK(v->s.tr[side][1].valid);
    }
    vcpu_destroy(v);
}

static void test_arch_rnat_and_calls(void)
{
    struct vcpu *v = new_vcpu();
    /* loadrs starts on an RNAT slot, then flushrs must retain earlier NaTs. */
    uint64_t saved = UINT64_C(1) << 10, value = 55;
    CHECK(gm_write(ENTRY + 0x81f8, &saved, 8)); CHECK(gm_write(ENTRY + 0x8200, &value, 8));
    v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = ENTRY + 0x8208;
    v->s.ar[AR_RSC] = 16u << 16;
    Ia64Instruction i = instruction(IA64_OP_LOADRS);
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.rse_dirty == 1);
    i.opcode = IA64_OP_FLUSHRS;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.ar[AR_BSPSTORE] == ENTRY + 0x8208);
    CHECK(gm_read(ENTRY + 0x81f8, &value, 8) && value == saved);
    /* Collection words do not consume physical registers in the 96 limit. */
    v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = ia64_rse_advance(ENTRY + 0x8600, 96);
    v->s.ar[AR_RSC] = (v->s.ar[AR_BSP] - (ENTRY + 0x8600)) << 16;
    i.opcode = IA64_OP_LOADRS;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.rse_dirty == 96);
    v->s.ar[AR_RSC] += 16u << 16;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL && v->s.rse_dirty == 96);
    v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = ENTRY + 0x8200; v->s.ar[AR_RSC] = 8u << 16;
    i.opcode = IA64_OP_LOADRS;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && !v->s.rse_dirty);
    i.opcode = IA64_OP_FLUSHRS;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.ar[AR_BSPSTORE] == ENTRY + 0x8200);
    CHECK(gm_read(ENTRY + 0x81f8, &value, 8) && value == saved);
    /* Capture an indirect target before overwriting its link register. */
    vcpu_reset(v, NULL); v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = ENTRY + 0x8500;
    v->s.cfm = 16 | (8u << 7) | (1u << 14) | (3u << 18);
    v->s.rse_bof = 91; v->s.ar[AR_EC] = 17; v->s.psr = UINT64_C(2) << 32;
    ia64_gr_write(&v->s, 32, 77, true); uint64_t cfm = v->s.cfm;
    i = instruction(IA64_OP_BR_CALL_INDIRECT); i.operands.branch.link = i.operands.branch.target = 1;
    v->s.br[1] = ENTRY + 0x40;
    struct ia64_result r = ia64_execute(v, &i);
    CHECK(r.kind == IA64_STEP_BRANCH && r.info == ENTRY + 0x40 && v->s.br[1] == ENTRY + 16);
    CHECK(v->s.cfm == 8 && v->s.rse_bof == 3 && v->s.ar[AR_PFS] == (cfm | (UINT64_C(17) << 52) | (UINT64_C(2) << 62)));
    v->s.ar[AR_EC] = 1; v->s.psr = 0; i.opcode = IA64_OP_BR_RET;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_BRANCH && v->s.cfm == cfm && v->s.rse_bof == 91);
    CHECK(v->s.ar[AR_EC] == 17 && ((v->s.psr >> 32) & 3) == 2);
    CHECK(ia64_gr_read(&v->s, 32) == 77 && ia64_nat_read(&v->s, 32));
    i.opcode = IA64_OP_BRL_CALL; i.operands.branch.displacement = 0x80;
    CHECK(ia64_execute(v, &i).info == ENTRY + 0x80 && v->s.cfm == 8);
    vcpu_destroy(v);
}

/* SDM Vol.2 §7.2 and Tables 7-5/7-7; Intel 251110-003 Tables 10-7/10-8;
 * Intel 308065-001 Tables 3-4/3-7/3-8. */
static void test_arch_pmu_registers(void)
{
    const Ia64Opcode reads[] = {IA64_OP_MOV_PMCGR_INDEXED, IA64_OP_MOV_PMDGR_INDEXED};
    const Ia64Opcode writes[] = {IA64_OP_MOV_GRPMC_INDEXED, IA64_OP_MOV_GRPMD_INDEXED};
    for (unsigned model = 0; model < 2; ++model) {
        struct vcpu *v = model_vcpu(model);
        for (unsigned bank = 0; bank < 2; ++bank) {
            Ia64Instruction i = instruction(writes[bank]);
            i.operands.system.register_index = 10;
            for (unsigned n = 64; n < 256; ++n) {
                v->s.gr[10] = n; v->s.gr[8] = UINT64_MAX;
                CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
                CHECK((bank ? v->s.pmd[n] : v->s.pmc[n]) == 0);
                i.opcode = reads[bank];
                CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0);
                i.opcode = writes[bank];
            }
            /* Unimplemented indices still undergo privilege and NaT checks. */
            v->s.gr[10] = 255; v->s.nat[10] = true; v->s.nat[8] = true;
            v->s.psr = UINT64_C(3) << 32;
            CHECK(ia64_execute(v, &i).fault == IA64_FAULT_PRIVILEGED);
            i.opcode = reads[bank];
            CHECK(ia64_execute(v, &i).fault == (bank ? IA64_FAULT_NAT : IA64_FAULT_PRIVILEGED));
            v->s.psr = 0;
            struct ia64_result fault = ia64_execute(v, &i);
            CHECK(fault.fault == IA64_FAULT_NAT && fault.info == 255);
            i.opcode = writes[bank];
            fault = ia64_execute(v, &i);
            CHECK(fault.fault == IA64_FAULT_NAT && fault.info == 255);
            v->s.nat[10] = false;
            CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT);
            v->s.nat[8] = false;
        }
        Ia64Instruction i = instruction(IA64_OP_MOV_GRPMC_INDEXED);
        i.operands.system.register_index = 10;
        for (unsigned n = 0; n < 4; ++n) {
            v->s.gr[10] = n; v->s.gr[8] = UINT64_MAX;
            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
            i.opcode = IA64_OP_MOV_PMCGR_INDEXED;
            uint64_t expected = n ? 0 : model ? 0xf1 : 0xfff1;
            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == expected);
            i.opcode = IA64_OP_MOV_GRPMC_INDEXED; v->s.gr[8] = 0;
            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
            i.opcode = IA64_OP_MOV_PMCGR_INDEXED;
            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0);
            i.opcode = IA64_OP_MOV_GRPMC_INDEXED;
        }
        const struct { uint64_t written, stored; } writes_valid[] = {
            {0, 0}, {1, 1},
            {UINT64_C(0xffff7fffffffffff), UINT64_C(0x7fffffffffff)}
        };
        const struct { uint64_t state, read[2]; } reads_count[] = {
            {0, {0, 0}}, {1, {1, 1}},
            {UINT64_C(0x3fffffffffff),
                {UINT64_C(0x3fffffffffff), UINT64_C(0x3fffffffffff)}},
            {UINT64_C(0x400000000000),
                {UINT64_C(0xffffc00000000000), UINT64_C(0x400000000000)}},
            {UINT64_C(0x800000000000), {0, UINT64_C(0xffff800000000000)}},
            {UINT64_C(0xffffffffffff), {UINT64_MAX, UINT64_MAX}}
        };
        for (unsigned n = 4; n < (model ? 8u : 16u); ++n) {
            for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(writes_valid); ++k) {
                i.opcode = IA64_OP_MOV_GRPMD_INDEXED;
                v->s.gr[10] = 0x100 + n; v->s.gr[8] = writes_valid[k].written;
                CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
                CHECK(v->s.pmd[n] == writes_valid[k].stored);
            }
            for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(reads_count); ++k) {
                i.opcode = IA64_OP_MOV_PMDGR_INDEXED;
                v->s.pmd[n] = reads_count[k].state;
                CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE &&
                      v->s.gr[8] == reads_count[k].read[model]);
            }
        }
        for (unsigned n = 4; n < (model ? 8u : 16u); ++n)
            for (unsigned cpl = 0; cpl < 4; ++cpl)
                for (unsigned sp = 0; sp < 2; ++sp)
                    for (unsigned pm = 0; pm < 2; ++pm) {
                        v->s.pmd[n] = 99; v->s.pmc[n] = (uint64_t)pm << 6;
                        v->s.psr = ((uint64_t)cpl << 32) | ((uint64_t)sp << 20);
                        v->s.gr[10] = n;
                        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
                        CHECK(v->s.gr[8] == (cpl && (sp || pm) ? 0 : 99));
                    }
        v->s.psr = 0;
        /* Constant-index forms have the same masks and counter read behavior. */
        i = instruction(IA64_OP_MOV_GRPMC); i.operands.system.source = 32;
        v->s.gr[8] = UINT64_MAX;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        i.opcode = IA64_OP_MOV_PMCGR;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == (model ? 0xf1 : 0xfff1));
        i.opcode = IA64_OP_MOV_GRPMD; i.operands.system.source = 71;
        v->s.gr[8] = UINT64_C(0xffff400000000000);
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        i.opcode = IA64_OP_MOV_PMDGR;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE &&
              v->s.gr[8] == (model ? UINT64_C(0x400000000000) : UINT64_C(0xffffc00000000000)));
        vcpu_destroy(v);
    }
}
