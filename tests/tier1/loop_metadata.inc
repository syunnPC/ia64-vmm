/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Integer FR metadata is preserved by xma.l unless an operand is NaTVal.
 * The oracle below uses modulo-64 arithmetic and the architectural NaTVal
 * representation, independently of the optimizer's facts. */
static void metadata_known_demand(struct t1_ir *ir)
{
    struct t1_known *full = calloc(ir->nops, sizeof(*full));
    struct t1_known *part = calloc(ir->nops, sizeof(*part));
    uint8_t *roots = calloc(ir->nops, 1);
    UM_CHECK(full && part && roots);
    t1_known_bits(ir, full);
    for (unsigned group = 0; group < 9; ++group) {
        for (unsigned o = 0; o < ir->nops; ++o)
            roots[o] = group == 8 || o % 8 == group;
        memset(part, 0xa5, ir->nops * sizeof(*part));
        UM_CHECK(t1_known_bits_demand(ir, part, roots));
        for (unsigned o = 0; o < ir->nops; ++o) {
            UM_CHECK(part[o].zero == (roots[o] ? full[o].zero : 0));
            UM_CHECK(part[o].one == (roots[o] ? full[o].one : 0));
        }
    }
    free(roots);
    free(part);
    free(full);
}

struct metadata_loop {
    uint32_t meta, mf, repeated, point[3];
};

static struct metadata_loop metadata_build(unsigned mode, bool mf_seed)
{
    const uint16_t e[] = {T1_ELEMENT_FR(6), T1_ELEMENT_FR_META(6), T1_ELEMENT_PSR_MF,
                          T1_ELEMENT_NAT(20)};
    start(e, 4);
    /* Latches precede the header in block and operation numbering. */
    uint16_t a = t1_ir_new_block(IR, 0), b = t1_ir_new_block(IR, 0);
    uint16_t sa = t1_ir_new_block(IR, 0), sb = t1_ir_new_block(IR, 0);
    uint16_t h = t1_ir_new_block(IR, 0), body = t1_ir_new_block(IR, 0);
    uint16_t out = t1_ir_new_block(IR, 0);
    t1_value p[4];
    for (unsigned k = 0; k < 4; ++k)
        p[k] = t1_ir_livein(IR, T1_I1, T1_ELEMENT_PR(6 + k));
    t1_value sig0 = t1_ir_livein(IR, T1_I64, e[0]);
    t1_value meta0 = t1_ir_livein(IR, T1_I64, e[1]);
    t1_value mf0 = t1_ir_livein(IR, T1_I64, e[2]);
    t1_value nat = t1_ir_livein(IR, T1_I1, e[3]);
    t1_value mul0 = t1_ir_livein(IR, T1_I64, T1_ELEMENT_GR(14));
    t1_value mul1 = t1_ir_livein(IR, T1_I64, T1_ELEMENT_GR(15));
    if (mf_seed)
        mf0 = op(T1_HEADER_BLOCK, T1_IR_OR, 0, 0, mf0, K(T1_I64, 16), 0, 2);
    UM_CHECK(t1_ir_set_branch(IR, T1_HEADER_BLOCK, p[0], sa, sb, 0));
    t1_value sig = T1_VALUE(t1_ir_add_param(IR, h, T1_I64, e[0]), 0);
    t1_value meta = T1_VALUE(t1_ir_add_param(IR, h, T1_I64, e[1]), 0);
    t1_value mf = T1_VALUE(t1_ir_add_param(IR, h, T1_I64, e[2]), 0);
    for (unsigned k = 0; k < 2; ++k) {
        uint16_t seed = k ? sb : sa;
        t1_value sm = mode == 1 ? meta0 : K(T1_I64, (mode == 2 && k) || (mode == 4 && !k) ? 0x1fffe : 0x1003e);
        UM_CHECK(t1_ir_set_jump(IR, seed, h));
        UM_CHECK(t1_ir_edge_add_arg(IR, seed, 0, sig0, e[0]));
        UM_CHECK(t1_ir_edge_add_arg(IR, seed, 0, sm, e[1]));
        UM_CHECK(t1_ir_edge_add_arg(IR, seed, 0, mf0, e[2]));
    }
    UM_CHECK(t1_ir_set_branch(IR, h, p[1], body, out, 0));
    t1_value is_nat = op(body, T1_IR_FRNATTEST, 0, 0, sig, meta, 0, 2);
    t1_value raw = op(body, T1_IR_XMA_L, 0, 0, mul0, mul1, sig, 3);
    t1_value ns = op(body, T1_IR_SELECT, T1_I64, 0, is_nat, K(T1_I64, 0), raw, 3);
    t1_value nm = op(body, T1_IR_SELECT, T1_I64, 0, is_nat, K(T1_I64, 0x1fffe),
                     K(T1_I64, 0x1003e), 3);
    ns = op(body, T1_IR_SELECT, T1_I64, 0, p[3], ns, sig, 3);
    nm = op(body, T1_IR_SELECT, T1_I64, 0, p[3], nm, meta, 3);
    t1_value nf = op(body, T1_IR_OR, 0, 0, mf, K(T1_I64, 16), 0, 2);
    struct metadata_loop r = {T1_VALUE_OP(meta), T1_VALUE_OP(mf), T1_VALUE_OP(nf), {0}};
    UM_CHECK(t1_ir_set_branch(IR, body, p[2], a, b, 0));
    IR->block[body].charge = 1;
    for (unsigned k = 0; k < 2; ++k) {
        uint16_t back = k ? b : a;
        t1_value bad = mode == 3 && k ? meta0 : nm;
        t1_value frame[] = {ns, bad, nf, nat};
        uint32_t pt = t1_ir_new_point(IR, T1_PT_CHECKPOINT, 0x5000, 0, 0, back, 0);
        UM_CHECK(t1_ir_set_frame(IR, pt, e, frame, NULL, 4));
        UM_CHECK(t1_ir_set_backedge(IR, back, h, pt, 0));
        UM_CHECK(t1_ir_edge_add_arg(IR, back, 0, ns, e[0]));
        UM_CHECK(t1_ir_edge_add_arg(IR, back, 0, bad, e[1]));
        UM_CHECK(t1_ir_edge_add_arg(IR, back, 0, nf, e[2]));
        r.point[k] = pt;
    }
    t1_value frame[] = {sig, meta, mf, nat};
    r.point[2] = exit_with(out, e, frame, 4);
    verify(0, "metadata loop before");
    metadata_known_demand(IR);
    return r;
}

static bool metadata_event(void *arg, uint64_t iteration)
{
    return iteration == *(unsigned *)arg;
}

static void metadata_oracle(unsigned mode, bool mf_seed)
{
    struct vcpu *v = um_vcpu(UM_CODE);
    for (unsigned bits = 0; bits < 32; ++bits)
        for (unsigned rounds = 0; rounds < 3; ++rounds) {
            unsigned count = rounds == 2 ? 7 : rounds;
            uint64_t a = UINT64_MAX - bits, b = UINT64_C(0x8000000000000001) + bits;
            uint64_t sig = bits & 1 ? 0 : UINT64_MAX - rounds;
            uint64_t oldmeta = bits & 2 ? 0x1fffe : 0x3003e;
            uint64_t meta = mode == 1 ? oldmeta : (mode == 2 && !(bits & 1)) || (mode == 4 && (bits & 1)) ? 0x1fffe : 0x1003e;
            uint64_t psr = UINT64_C(0x2000) | (bits & 4 ? 32 : 0);
            v->s.fr[6].significand = sig;
            v->s.fr[6].exponent = oldmeta & 0x1ffff;
            v->s.fr[6].sign = (oldmeta >> 17) & 1;
            v->s.psr = psr;
            um_gr(v, 14, a, false);
            um_gr(v, 15, b, false);
            um_gr(v, 20, UINT64_MAX, bits & 16);
            um_pr(v, 6, bits & 1);
            um_pr(v, 7, count != 0);
            um_pr(v, 8, bits & 2);
            um_pr(v, 9, bits & 8);
            if (mf_seed)
                psr |= 16;
            for (unsigned k = 0; k < count; ++k) {
                bool nat = sig == 0 && meta == 0x1fffe;
                if (bits & 8) {
                    sig = nat ? 0 : a * b + sig;
                    meta = nat ? 0x1fffe : 0x1003e;
                }
                if (mode == 3 && !(bits & 2))
                    meta = oldmeta;
                psr |= 16;
            }
            struct ir_eval_hooks hooks = {.event = metadata_event, .arg = &count};
            struct ir_eval_result result;
            UM_CHECK(ir_eval_run(IR, v, 100, &hooks, &result));
            UM_CHECK(v->s.fr[6].significand == sig);
            UM_CHECK(v->s.fr[6].exponent == (meta & 0x1ffff));
            UM_CHECK(v->s.fr[6].sign == ((meta >> 17) & 1));
            UM_CHECK(v->s.psr == psr && v->s.nat[20] == !!(bits & 16));
            UM_CHECK(result.iterations == count && result.consumed == count);
        }
    vcpu_destroy(v);
}

/* A proof depending on a failed hypothesis must be checked again. */
static void metadata_dependency(void)
{
    uint16_t e[] = {T1_ELEMENT_GR(20), T1_ELEMENT_GR(21)};
    start(e, 2);
    uint16_t h = T1_HEADER_BLOCK, back = t1_ir_new_block(IR, 0);
    uint16_t out = t1_ir_new_block(IR, 0);
    t1_value a = T1_VALUE(t1_ir_add_param(IR, h, T1_I64, e[0]), 0);
    t1_value b = T1_VALUE(t1_ir_add_param(IR, h, T1_I64, e[1]), 0);
    UM_CHECK(t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, K(T1_I64, 16), e[0]));
    UM_CHECK(t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, K(T1_I64, 16), e[1]));
    t1_value next = op(h, T1_IR_ADD, 0, 0, b, K(T1_I64, 1), 0, 2);
    t1_value condition = t1_ir_livein(IR, T1_I1, T1_ELEMENT_PR(6));
    UM_CHECK(t1_ir_set_branch(IR, h, condition, back, out, 0));
    t1_value frame[] = {b, next};
    uint32_t ck = t1_ir_new_point(IR, T1_PT_CHECKPOINT, 0x5000, 0, 0, back, 0);
    UM_CHECK(t1_ir_set_frame(IR, ck, e, frame, NULL, 2));
    UM_CHECK(t1_ir_set_backedge(IR, back, h, ck, 0));
    UM_CHECK(t1_ir_edge_add_arg(IR, back, 0, b, e[0]));
    UM_CHECK(t1_ir_edge_add_arg(IR, back, 0, next, e[1]));
    t1_value final[] = {a, b};
    uint32_t pt = exit_with(out, e, final, 2);
    verify(0, "dependent constants before");
    t1_opt_fold(IR);
    verify(0, "dependent constants after");
    UM_CHECK(code_of(entry(pt, 0)->value) != T1_IR_CONST);
    UM_CHECK(code_of(entry(pt, 1)->value) != T1_IR_CONST);
}

/* Two seeded hypotheses depend on each other through modular arithmetic.
 * Changing one feedback addend invalidates both, including the first one
 * whose feedback initially appeared to preserve its seed. */
static void metadata_modular_cycle(void)
{
    for (unsigned bad = 0; bad < 2; ++bad) {
        uint16_t e[] = {T1_ELEMENT_GR(20), T1_ELEMENT_GR(21)};
        start(e, 2);
        uint16_t h = T1_HEADER_BLOCK, back = t1_ir_new_block(IR, 0);
        uint16_t out = t1_ir_new_block(IR, 0);
        t1_value a = T1_VALUE(t1_ir_add_param(IR, h, T1_I64, e[0]), 0);
        t1_value b = T1_VALUE(t1_ir_add_param(IR, h, T1_I64, e[1]), 0);
        UM_CHECK(t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, K(T1_I64, UINT64_MAX), e[0]));
        UM_CHECK(t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, K(T1_I64, 0), e[1]));
        t1_value next[] = {op(h, T1_IR_SUB, 0, 0, b, K(T1_I64, 1), 0, 2),
                           op(h, T1_IR_ADD, 0, 0, a, K(T1_I64, 1 + bad), 0, 2)};
        t1_value condition = t1_ir_livein(IR, T1_I1, T1_ELEMENT_PR(6));
        UM_CHECK(t1_ir_set_branch(IR, h, condition, back, out, 0));
        uint32_t ck = t1_ir_new_point(IR, T1_PT_CHECKPOINT, 0x5000, 0, 0, back, 0);
        UM_CHECK(t1_ir_set_frame(IR, ck, e, next, NULL, 2));
        UM_CHECK(t1_ir_set_backedge(IR, back, h, ck, 0));
        UM_CHECK(t1_ir_edge_add_arg(IR, back, 0, next[0], e[0]));
        UM_CHECK(t1_ir_edge_add_arg(IR, back, 0, next[1], e[1]));
        t1_value final[] = {a, b};
        uint32_t pt = exit_with(out, e, final, 2);
        verify(0, "modular hypotheses before");
        t1_opt_fold(IR);
        verify(0, "modular hypotheses after");
        for (unsigned k = 0; k < 2; ++k) {
            t1_value v = entry(pt, k)->value;
            UM_CHECK((code_of(v) == T1_IR_CONST) == !bad);
            if (!bad)
                UM_CHECK((uint64_t)IR->op[T1_VALUE_OP(v)].imm == (k ? 0 : UINT64_MAX));
        }
    }
}

/* No constant may be inferred from an unseeded unreachable cycle or an
 * irreducible cycle with two entries. The latter is rejected by the IR
 * verifier, but the induction pass must not mistake it for a natural loop. */
static void metadata_cfg_cycles(void)
{
    for (unsigned reachable = 0; reachable < 2; ++reachable) {
        uint16_t e = T1_ELEMENT_GR(20);
        start(&e, 1);
        uint16_t a = t1_ir_new_block(IR, 0), b = t1_ir_new_block(IR, 0);
        t1_value pa = T1_VALUE(t1_ir_add_param(IR, a, T1_I64, e), 0);
        t1_value pb = T1_VALUE(t1_ir_add_param(IR, b, T1_I64, e), 0);
        t1_value sig = t1_ir_livein(IR, T1_I64, T1_ELEMENT_GR(14));
        if (reachable) {
            t1_value cond = t1_ir_livein(IR, T1_I1, T1_ELEMENT_PR(6));
            UM_CHECK(t1_ir_set_branch(IR, T1_HEADER_BLOCK, cond, a, b, 0));
            UM_CHECK(t1_ir_edge_add_arg(IR, T1_HEADER_BLOCK, 0, K(T1_I64, 0x1003e), e));
            UM_CHECK(t1_ir_edge_add_arg(IR, T1_HEADER_BLOCK, 1, K(T1_I64, 0x1003e), e));
        } else
            exit_with(T1_HEADER_BLOCK, &e, &sig, 1);
        for (unsigned k = 0; k < 2; ++k) {
            uint16_t from = k ? b : a, to = k ? a : b;
            t1_value phi = k ? pb : pa;
            t1_value nat = op(from, T1_IR_FRNATTEST, 0, 0, sig, phi, 0, 2);
            t1_value next = op(from, T1_IR_SELECT, T1_I64, 0, nat,
                               K(T1_I64, 0x1fffe), K(T1_I64, 0x1003e), 3);
            uint32_t ck = t1_ir_new_point(IR, T1_PT_CHECKPOINT, 0x5000, 0, 0, from, 0);
            UM_CHECK(t1_ir_set_frame(IR, ck, &e, &next, NULL, 1));
            UM_CHECK(t1_ir_set_backedge(IR, from, to, ck, 0));
            UM_CHECK(t1_ir_edge_add_arg(IR, from, 0, next, e));
        }
        uint16_t idom[T1_MAX_BLOCKS];
        UM_CHECK(t1_ir_dominators(IR, idom));
        UM_CHECK(!t1_ir_dominates(idom, a, b) && !t1_ir_dominates(idom, b, a));
        UM_CHECK(!t1_ir_verify(IR, 0, err, sizeof(err)));
        metadata_known_demand(IR);
        t1_opt_fold(IR);
        UM_CHECK(code_of(t1_ir_resolve(IR, pa)) != T1_IR_CONST);
        UM_CHECK(code_of(t1_ir_resolve(IR, pb)) != T1_IR_CONST);
    }
}

/* Analysis is also used between forwarding and parameter compaction.
 * A removed PARAM remains linked; an unlinked operand remains TOP during
 * the full fixed point, including for an otherwise absorbing AND zero. */
static void metadata_demand_rewrites(void)
{
    uint16_t e = T1_ELEMENT_GR(20), h = T1_HEADER_BLOCK;
    start(&e, 1);
    uint32_t param = t1_ir_add_param(IR, h, T1_I64, e);
    t1_value seed = K(T1_I64, 16);
    UM_CHECK(t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, seed, e));
    t1_value next = op(h, T1_IR_OR, 0, 0, T1_VALUE(param, 0), seed, 0, 2);
    t1_value consumer = op(h, T1_IR_AND, 0, 0, next, K(T1_I64, 0), 0, 2);
    uint32_t ck = t1_ir_new_point(IR, T1_PT_CHECKPOINT, 0x5000, 0, 0, h, 0);
    UM_CHECK(t1_ir_set_frame(IR, ck, &e, &consumer, NULL, 1));
    UM_CHECK(t1_ir_set_backedge(IR, h, h, ck, 0));
    UM_CHECK(t1_ir_edge_add_arg(IR, h, 0, next, e));
    verify(0, "demand rewriting before");
    metadata_known_demand(IR);
    t1_ir_forward(IR, param, seed);
    metadata_known_demand(IR);
    t1_ir_remove(IR, param);
    metadata_known_demand(IR);
    t1_ir_resolve_all(IR);
    t1_ir_compact_params(IR);
    metadata_known_demand(IR);
    t1_ir_remove(IR, T1_VALUE_OP(next));
    metadata_known_demand(IR);
    t1_ir_kill_block(IR, h);
    metadata_known_demand(IR);
}

static void loop_metadata(void)
{
    metadata_dependency();
    metadata_modular_cycle();
    metadata_cfg_cycles();
    metadata_demand_rewrites();
    for (unsigned mode = 0; mode < 5; ++mode)
        for (unsigned mf_seed = 0; mf_seed < 2; ++mf_seed) {
            struct metadata_loop r = metadata_build(mode, mf_seed);
            metadata_oracle(mode, mf_seed);
            t1_opt_fold(IR);
            verify(0, "metadata loop folded");
            metadata_known_demand(IR);
            metadata_oracle(mode, mf_seed);
            t1_value meta = entry(r.point[2], 1)->value;
            if (mode == 0) {
                UM_CHECK(code_of(meta) == T1_IR_CONST);
                UM_CHECK((uint64_t)IR->op[T1_VALUE_OP(meta)].imm == 0x1003e);
            } else
                UM_CHECK(code_of(meta) != T1_IR_CONST);
            UM_CHECK(!!(IR->op[r.repeated].flags & T1_OPF_DEAD) == !!mf_seed);
            /* The MFH bit has never been assigned a constant. */
            UM_CHECK(code_of(entry(r.point[2], 2)->value) != T1_IR_CONST);
        }
}
