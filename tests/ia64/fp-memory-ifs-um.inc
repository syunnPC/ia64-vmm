/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/fp.h"

static void test_fp_memory_access(void)
{
    const struct { Ia64Opcode op; bool store, pair; } cases[] = {
        {IA64_OP_LDFS, false, false}, {IA64_OP_LDFD, false, false},
        {IA64_OP_LDFE, false, false}, {IA64_OP_LDF8, false, false},
        {IA64_OP_LDF_FILL, false, false}, {IA64_OP_LDFPS, false, true},
        {IA64_OP_LDFPD, false, true}, {IA64_OP_LDFP8, false, true},
        {IA64_OP_STFS, true, false}, {IA64_OP_STFD, true, false},
        {IA64_OP_STFE, true, false}, {IA64_OP_STF8, true, false},
        {IA64_OP_STF_SPILL, true, false},
    };
    struct vcpu *v = new_vcpu();
    uint8_t memory[16];
    memset(memory, 0xa6, sizeof(memory));
    CHECK(gm_write(ENTRY + 0x8000, memory, sizeof(memory)));
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(cases); ++k)
        for (unsigned bank = 0; bank < (cases[k].pair ? 3u : 2u); ++bank)
            for (unsigned spec = 0; spec < (cases[k].store ? 1u : 2u); ++spec) {
                vcpu_reset(v, NULL);
                unsigned first = bank == 1 ? 32 : 8;
                unsigned second = bank == 2 ? 33 : first + 1;
                unsigned code = bank == 2 ? 3 : bank == 1 ? 2 : 1;
                v->s.psr = IA64_PSR_IC | IA64_PSR_DFL | IA64_PSR_DFH;
                v->s.ip = ENTRY; v->s.slot = 1; v->s.cr[CR_IVA] = ENTRY;
                v->s.gr[10] = ENTRY + 0x8000;
                v->s.fr[first] = (struct ia64_fr){123, IA64_FP_BIAS, true};
                v->s.fr[second] = (struct ia64_fr){456, IA64_FP_BIAS, false};
                struct ia64_state before = v->s;
                Ia64Instruction i = instruction(cases[k].op);
                i.operands.memory = (IA64MemoryOperands){.base = 10, .destination = first,
                    .source = cases[k].store ? first : cases[k].pair ? second : 0, .immediate = 16};
                i.imm_base_update = true; i.fp_load_speculative = spec;
                struct ia64_result r = ia64_execute(v, &i);
                CHECK(r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_FP_DISABLED && r.info == code);
                CHECK(!memcmp(&before, &v->s, sizeof(before)));
                CHECK(!memcmp(memory, ram + 0x8000, sizeof(memory)));
                ia64_deliver_fault(v, &i, r);
                uint64_t access = UINT64_C(1) << (cases[k].store ? 33 : 34);
                if (spec) access |= UINT64_C(1) << 36;
                CHECK(v->s.cr[CR_ISR] == (code | access | (UINT64_C(1) << 41)));
                CHECK(v->s.ip == ENTRY + 0x5500 && v->s.cr[CR_IIP] == ENTRY);
            }
    /* Execute real bundles through each engine, including Tier0 fault replay. */
    const uint64_t raw[] = {UINT64_C(0xc080900200), UINT64_C(0xc0c8a12200), UINT64_C(0xcc00a10000)};
    bundle_write(ENTRY + 0x5500, 1, UINT64_C(1) << 27, UINT64_C(1) << 27, UINT64_C(1) << 27);
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(raw); ++k) {
        vcpu_reset(v, NULL);
        v->s.ip = ENTRY; v->s.cr[CR_IVA] = ENTRY;
        v->s.psr = IA64_PSR_IC | IA64_PSR_DFL;
        v->s.gr[9] = v->s.gr[10] = ENTRY + 0x8000;
        bundle_write(ENTRY, 1, raw[k], UINT64_C(1) << 27, UINT64_C(1) << 27);
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.ip == ENTRY + 0x5500 && v->s.slot == 1 && v->retired == 1);
        CHECK(v->fault.kind == IA64_FAULT_FP_DISABLED && v->fault.ip == ENTRY);
        CHECK(v->s.cr[CR_ISR] == (1 | (UINT64_C(1) << (k == 2 ? 33 : 34))));
        CHECK(!memcmp(memory, ram + 0x8000, sizeof(memory)));
    }
    vcpu_destroy(v);
}

static void test_ifs_read(void)
{
    struct vcpu *v = new_vcpu();
    Ia64Instruction i = instruction(IA64_OP_MOV_GRCR);
    i.operands.system = (IA64SystemOperands){.source = CR_IFS, .destination = 8};
    /* Vol.2 Table 3-9 / Vol.3 mov cr: only bits 62:38 are reserved,
     * even for frame markers that would be invalid when restored. */
    const uint64_t markers[] = {0, 65 | (15u << 7) | (5u << 14),
        UINT64_C(0x260e1947c1), (UINT64_C(1) << 38) - 1};
    for (unsigned valid = 0; valid < 2; ++valid)
        for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(markers); ++k) {
            vcpu_reset(v, NULL);
            uint64_t value = ((uint64_t)valid << 63) | markers[k];
            v->s.gr[8] = value;
            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.cr[CR_IFS] == value);
            Ia64Instruction read = i; read.opcode = IA64_OP_MOV_CRGR;
            CHECK(ia64_execute(v, &read).kind == IA64_STEP_COMPLETE && v->s.gr[8] == value);
            for (unsigned bit = 38; bit <= 62; ++bit) {
                v->s.gr[8] = value | (UINT64_C(1) << bit);
                CHECK(ia64_execute(v, &i).fault == IA64_FAULT_RESERVED && v->s.cr[CR_IFS] == value);
            }
        }
    v->s.gr[8] = UINT64_C(0x800000260e1947c1); v->s.psr = 0;
    bundle_write(ENTRY, 1, UINT64_C(0x2161710000), UINT64_C(1) << 27, UINT64_C(1) << 27);
    v->s.ip = ENTRY;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->retired == 1);
    CHECK(v->s.cr[CR_IFS] == v->s.gr[8]);
    v->s.nat[8] = true;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT);
    v->s.nat[8] = false; v->s.psr = UINT64_C(1) << 32;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_PRIVILEGED);
    v->s.psr = IA64_PSR_IC;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
    /* Moving arbitrary IFM bits must not bypass the existing restoration
     * guard, and a rotating region extending beyond SOL is usable. */
    vcpu_reset(v, NULL);
    v->s.cr[CR_IFS] = (UINT64_C(1) << 63) | 127;
    i = instruction(IA64_OP_RFI);
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_RESERVED);
    v->s.cr[CR_IFS] = (UINT64_C(1) << 63) | 16 | (1u << 14);
    v->s.cr[CR_IIP] = ENTRY; v->s.ar[AR_BSP] = ENTRY + 0x8100; v->s.rse_dirty = 16;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_BRANCH);
    CHECK(v->s.cfm == (16 | (1u << 14)));
    vcpu_destroy(v);
}

static void test_user_mask_ops(void)
{
    const Ia64Opcode ops[] = {IA64_OP_SUM_UM, IA64_OP_RUM, IA64_OP_MOV_GRUM};
    const uint64_t raw[] = {UINT64_C(0x20000100), UINT64_C(0x28000100), UINT64_C(0x2148010000)};
    struct vcpu *v = new_vcpu();
    /* Vol.3 sum/rum/mov um: secure monitoring freezes UP while the other
     * user-mask bits and unprivileged access retain their normal behavior. */
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(ops); ++k)
        for (unsigned sp = 0; sp < 2; ++sp)
            for (unsigned old = 0; old <= 0x3e; old += 2)
                for (unsigned value = 0; value <= 0x3e; value += 2)
                    for (unsigned cpl = 0; cpl < 4; ++cpl) {
                        uint64_t psr = IA64_PSR_IC | ((uint64_t)sp << 20) | ((uint64_t)cpl << 32) | old;
                        v->s.psr = psr; v->s.gr[8] = value;
                        Ia64Instruction i = instruction(ops[k]);
                        i.operands.system = (IA64SystemOperands){.destination = 8, .immediate = value};
                        uint64_t expected = k == 0 ? psr | value : k == 1 ? psr & ~(uint64_t)value : (psr & ~UINT64_C(0x3e)) | value;
                        if (sp) expected = (expected & ~UINT64_C(4)) | (psr & 4);
                        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.psr == expected);
                        i.operands.system.immediate = 0x40; v->s.gr[8] = 0x40;
                        CHECK(ia64_execute(v, &i).fault == IA64_FAULT_RESERVED && v->s.psr == expected);
                        i.qp = 6; v->s.pr = 1;
                        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.psr == expected);
                    }
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(ops); ++k)
        for (unsigned sp = 0; sp < 2; ++sp)
            for (unsigned up = 0; up < 2; ++up) {
                vcpu_reset(v, NULL);
                v->s.ip = ENTRY; v->s.psr = ((uint64_t)sp << 20) | (up << 2);
                v->s.gr[8] = up ? 0 : 4;
                bundle_write(ENTRY, 1, raw[k], UINT64_C(1) << 27, UINT64_C(1) << 27);
                uint64_t expected_up = sp ? up : k == 0 ? 1 : k == 1 ? 0 : !up;
                CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->retired == 1);
                CHECK(v->s.psr == (((uint64_t)sp << 20) | (expected_up << 2)));
            }
    Ia64Instruction i = instruction(IA64_OP_MOV_GRUM);
    i.operands.system.destination = 8; v->s.nat[8] = true;
    uint64_t psr = v->s.psr;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT && v->s.psr == psr);
    vcpu_destroy(v);
}

static void test_fp_memory_ifs_um(const char *only)
{
    if (!only || !strcmp(only, "fp-memory")) test_fp_memory_access();
    if (!only || !strcmp(only, "ifs")) test_ifs_read();
    if (!only || !strcmp(only, "user-mask")) test_user_mask_ops();
}
