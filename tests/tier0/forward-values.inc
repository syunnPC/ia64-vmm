/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the generated ABI at every prefix, including a pending request
 * which the outer dispatcher would otherwise consume before region entry. */
static void forward_values_region(struct vcpu *v, struct vcpu *ref,
                                  Ia64Instruction *insns, unsigned count,
                                  unsigned start_slot, bool native)
{
    unsigned slot = start_slot;
    uint64_t ip = BASE;
    struct tier0_region *r = tier0_region_allocate(); CHECK(r != NULL);
    *r = (struct tier0_region){.valid = true, .count = count};
    for (unsigned at = 0; at < count; ++at) {
        insns[at].valid = true; insns[at].address = ip; insns[at].slot = slot;
        r->slots[at] = insns[at].opcode == IA64_OP_MOVL || insns[at].opcode == IA64_OP_HINT_X ||
            (insns[at].opcode == IA64_OP_NOP && insns[at].unit == IA64_UNIT_X) ? 2 : 1;
        slot += r->slots[at];
        if (slot == 3) { slot = 0; ip += 16; }
        CHECK(slot < 3);
    }
    v->s.ip = BASE; v->s.slot = start_slot;
    struct vcpu original = *v;
    struct tier0_homes homes = {0};
    for (unsigned reg = 32; reg < 128; ++reg) homes.gr_index[reg - 32] = ia64_gr_index(&v->s, reg);
    for (unsigned p = 16; p < 64; ++p) homes.pr_mask[p - 16] = UINT64_C(1) << ia64_pr_index(&v->s, p);
    CHECK(tier0_stage(r, insns, &v->s)); tier0_seal(v, r->chunk);
    void (*fn)(struct vcpu *,uint64_t,struct tier0_execution *,const struct tier0_homes *) = r->entry;
    for (unsigned prefix = 0; prefix < count + 2; ++prefix) {
        *v = original; *ref = original;
        bool requested = prefix == count + 1;
        unsigned budget = requested ? count : prefix;
        if (requested) vcpu_request_exit(v, VCPU_EXIT_HALT);
        enum ia64_fault_kind fault = IA64_FAULT_NONE;
        bool stop = false;
        for (unsigned at = 0; at < count && at < budget; ++at) {
            const Ia64Instruction *i = &insns[at];
            ref->s.ip = i->address; ref->s.slot = i->slot;
            uint64_t psr = ref->s.psr;
            struct ia64_result result = ia64_execute(ref, i);
            if (result.kind == IA64_STEP_FAULT) { fault = result.fault; break; }
            CHECK(result.kind == IA64_STEP_COMPLETE);
            expect_advance(ref, i, &result);
            /* Long NOP/HINT, like MOVL, consume the L+X pair. */
            if (r->slots[at] == 2) {
                ref->s.ip = i->address + 16; ref->s.slot = 0;
            }
            if (psr & IA64_PSR_IC) ref->last_successful_bundle = i->address;
            if (requested) { stop = true; break; }
        }
        struct tier0_execution e = {.region = r, .stats = tier0_local_stats()};
        uint64_t helpers = native_helper_calls;
        tier0_arena.executing = true; fn(v, budget, &e, &homes); tier0_arena.executing = false;
        if (memcmp(&v->s, &ref->s, sizeof(v->s)))
            LOGE("forward %s/%s prefix=%u request=%u bof=%u cfm=%llx",
                 ia64_opcode_info(insns[0].opcode)->name, ia64_opcode_info(insns[1].opcode)->name,
                 prefix, requested, original.s.rse_bof, (unsigned long long)original.s.cfm);
        CHECK(!memcmp(&v->s, &ref->s, sizeof(v->s)) && v->retired == ref->retired);
        CHECK(v->last_successful_bundle == ref->last_successful_bundle);
        CHECK(e.fault == (fault != IA64_FAULT_NONE) && (!e.fault || e.result.fault == fault));
        CHECK(e.stop == stop && !v->requested_exit);
        if (stop) CHECK(e.out.reason == VCPU_EXIT_HALT && e.out.ip == insns[0].address && e.out.slot == insns[0].slot);
        if (native) CHECK(native_helper_calls == helpers);
    }
    tier0_region_retire(r, false); tier0_reclaim();
}

static void test_native_forward_values(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode producers[] = {IA64_OP_ADD,IA64_OP_ADDS,IA64_OP_SUB,IA64_OP_ANDCM,
        IA64_OP_SHRP_IMM,IA64_OP_DEP,IA64_OP_SXT4,IA64_OP_MOVL,IA64_OP_DEPZ_IMM,IA64_OP_MOV_CURRENT_IP};
    const Ia64Opcode consumers[] = {IA64_OP_ADD,IA64_OP_ADDS,IA64_OP_SUB,IA64_OP_SUB_IMM,
        IA64_OP_ANDCM,IA64_OP_AND_IMM,IA64_OP_EXTRU,IA64_OP_DEPZ,IA64_OP_SHL,IA64_OP_SHR,
        IA64_OP_SHL_IMM,IA64_OP_DEP,IA64_OP_SHRP_IMM,IA64_OP_XOR_IMM,IA64_OP_SXT2};
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush(); hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(producers); ++op)
            for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
                for (unsigned variant = 0; variant < 8; ++variant) {
                    randomized(v); v->s.pr &= ~UINT64_C(2);
                    v->s.rse_bof = bof; v->s.psr = variant & 1 ? IA64_PSR_IC : 0;
                    v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(12) << 14) |
                        ((uint64_t)((bof * 7) % 96) << 18);
                    unsigned x = bof & 1 ? 32 : 8, y = bof & 1 ? 127 : 9, z = bof & 1 ? 49 : 10;
                    ia64_gr_write(&v->s, x, random64(), variant & 1);
                    ia64_gr_write(&v->s, y, variant == 4 ? 64 : random64() & 63, variant & 2);
                    ia64_gr_write(&v->s, z, random64(), variant & 4);
                    Ia64Instruction insns[3] = {
                        {.opcode = producers[op], .unit = IA64_UNIT_I, .qp = variant == 1 ? 1 : 0,
                         .operands.integer = {.destination = x, .source1 = variant == 4 ? x : y,
                             .source2 = z, .immediate = 7 | (13 << 6) | (0x55 << 13)}},
                        {.opcode = consumers[(op + bof) % IA64VMM_ARRAY_SIZE(consumers)], .unit = IA64_UNIT_I,
                         .qp = variant == 2 ? 1 : 0, .operands.integer = {.destination = variant & 4 ? x : y,
                             .source1 = x, .source2 = variant == 6 ? 0 : z, .immediate = 7 | (13 << 6)}},
                        {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
                         .operands.integer = {.destination = z, .source2 = variant & 4 ? x : y, .immediate = -17}},
                    };
                    enum ia64_nat_rule rule = ia64_opcode_info(insns[1].opcode)->nat_rule;
                    if (rule == IA64_NAT_SOURCE_R3 || variant & 1) {
                        insns[1].operands.integer.source2 = x;
                        insns[1].operands.integer.source1 = variant == 5 ? x : z;
                    }
                    if (producers[op] == IA64_OP_MOVL) insns[0].unit = IA64_UNIT_L;
                    forward_values_region(v, ref, insns, 3, producers[op] == IA64_OP_MOVL ? 1 : 0, true);
                }
    }
    /* A helper, native frame write, not-taken branch, or fault between
     * producer and consumer must prevent a stale result from being used. */
    for (unsigned boundary = 0; boundary < 5; ++boundary) {
        randomized(v); v->s.pr &= ~UINT64_C(2); v->s.psr = IA64_PSR_IC;
        v->s.cfm = 96 | (UINT64_C(96) << 7); v->s.rse_dirty = 0;
        Ia64Instruction insns[3] = {
            {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
             .operands.integer = {.destination = 32, .source2 = 127, .immediate = 1}},
            {.opcode = IA64_OP_POPCNT, .unit = IA64_UNIT_I,
             .operands.integer = {.destination = 32, .source2 = 32}},
            {.opcode = IA64_OP_ADDS, .unit = IA64_UNIT_I,
             .operands.integer = {.destination = 127, .source2 = 32, .immediate = 7}},
        };
        if (boundary == 1) {
            insns[1].opcode = IA64_OP_ALLOC;
            insns[1].operands.system = (IA64SystemOperands){.destination = 32, .immediate = 96 | 96 << 7};
        } else if (boundary == 2) {
            insns[1].opcode = IA64_OP_BR_COND; insns[1].unit = IA64_UNIT_B; insns[1].qp = 1;
        } else if (boundary == 3) {
            insns[1].opcode = IA64_OP_ADDS; insns[1].reserved_field = true;
        } else if (boundary == 4) {
            /* Native memory guard -> successful cold exact helper ->
             * continuation. The consumer must read the helper's result. */
            insns[1].opcode = IA64_OP_LD8; insns[1].unit = IA64_UNIT_M;
            insns[1].operands.memory = (IA64MemoryOperands){.destination = 32, .base = 10};
            ia64_gr_write(&v->s, 10, DEVICE + 8, false);
        }
        forward_values_region(v, ref, insns, 3, boundary == 2 ? 1 : 0, boundary == 1 || boundary == 2);
    }
    tier0_flush(); hal_linux_host_isa_mask(old);
}
