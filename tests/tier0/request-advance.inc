/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Enter the generated ABI with a pending request: the outer dispatcher
 * normally consumes it before entry. Compare the retirement boundary and
 * complete architectural state for ordinary forms, MLX, and branches. */
static void test_request_advance(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_ADDS, IA64_OP_MOVL, IA64_OP_NOP, IA64_OP_MF,
        IA64_OP_BR_COND, IA64_OP_BR_CLOOP, IA64_OP_BR_CALL, IA64_OP_BR_CALL_INDIRECT, IA64_OP_BR_RET};
    for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op)
        for (unsigned start = 0; start < 3; ++start)
            for (unsigned count = 1; count <= 3; count += 2)
                for (unsigned mode = 0; mode < 6; ++mode) {
                    bool branch = op >= 4, cloop = ops[op] == IA64_OP_BR_CLOOP;
                    if ((ops[op] == IA64_OP_MOVL && start != 1) || (cloop && start != 2)) continue;
                    vcpu_reset(v, NULL); v->s.ip = BASE; v->s.slot = start;
                    v->s.cfm = 96; v->s.pr = mode % 3 == 2 ? 3 : 1;
                    v->s.psr = mode & 1 ? IA64_PSR_IC : 0;
                    v->s.gr[8] = 17; v->s.gr[9] = 23; v->s.br[2] = BASE + 256;
                    v->s.ar[AR_LC] = mode & 1; v->retired = 7;
                    Ia64Instruction insns[3];
                    unsigned slot = start; uint64_t ip = BASE;
                    struct tier0_region *r = tier0_region_allocate(); CHECK(r != NULL);
                    *r = (struct tier0_region){.valid = true, .count = count};
                    for (unsigned at = 0; at < count; ++at) {
                        insns[at] = (Ia64Instruction){.opcode = at ? IA64_OP_ADDS : ops[op],
                            .valid = true, .unit = IA64_UNIT_I, .address = ip, .slot = slot,
                            .operands.integer = {.destination = 8, .source2 = 9, .immediate = 13}};
                        if (!at) {
                            insns[at].qp = mode % 3 && !cloop ? 1 : 0;
                            if (branch) {
                                insns[at].unit = IA64_UNIT_B;
                                insns[at].operands.branch = (IA64BranchOperands){.target = 2, .link = 1, .displacement = 256};
                            } else if (ops[op] == IA64_OP_MOVL) insns[at].unit = IA64_UNIT_L;
                            else if (ops[op] == IA64_OP_MF) insns[at].unit = IA64_UNIT_M;
                        }
                        r->slots[at] = insns[at].opcode == IA64_OP_MOVL ? 2 : 1;
                        slot += r->slots[at];
                        if (slot == 3) { slot = 0; ip += 16; }
                    }
                    CHECK(tier0_stage(r, insns, &v->s)); tier0_seal(v, r->chunk);
                    struct vcpu original = *v;
                    void (*fn)(struct vcpu *,uint64_t,struct tier0_execution *,const struct tier0_homes *) = r->entry;
                    for (unsigned budget = 0; budget <= count; ++budget)
                        for (unsigned request = 0; request < 2; ++request) {
                            *v = original; *ref = original;
                            if (request) vcpu_request_exit(v, VCPU_EXIT_DEBUG);
                            bool stop = false, pending = request;
                            struct ia64_result result = ia64_complete();
                            for (unsigned at = 0; at < budget; ++at) {
                                const Ia64Instruction *i = &insns[at];
                                ref->s.ip = i->address; ref->s.slot = i->slot;
                                result = ia64_execute(ref, i);
                                CHECK(result.kind == IA64_STEP_COMPLETE || result.kind == IA64_STEP_BRANCH);
                                expect_advance(ref, i, &result);
                                if (original.s.psr & IA64_PSR_IC) ref->last_successful_bundle = i->address;
                                /* Taken middle BR_COND/CLOOP leaves directly;
                                 * the next dispatcher consumes its request. */
                                bool direct_leave = result.kind == IA64_STEP_BRANCH && at + 1 < count &&
                                    (i->opcode == IA64_OP_BR_COND || i->opcode == IA64_OP_BR_CLOOP);
                                if (request && !direct_leave) { stop = true; pending = false; }
                                if (stop || result.kind == IA64_STEP_BRANCH) break;
                            }
                            struct tier0_execution e = {.region = r, .stats = tier0_local_stats()};
                            tier0_arena.executing = true; fn(v, budget, &e, NULL); tier0_arena.executing = false;
                            CHECK(!memcmp(&v->s, &ref->s, sizeof(v->s)) && v->retired == ref->retired);
                            CHECK(v->last_successful_bundle == ref->last_successful_bundle && !e.fault);
                            CHECK(e.result.kind == result.kind && (result.kind != IA64_STEP_BRANCH || e.result.info == result.info));
                            CHECK(e.stop == stop && v->requested_exit == (pending ? VCPU_EXIT_DEBUG + 1u : 0));
                            if (stop) CHECK(e.out.reason == VCPU_EXIT_DEBUG && e.out.ip == BASE && e.out.slot == start);
                        }
                    tier0_region_retire(r, false); tier0_reclaim();
                }
}
