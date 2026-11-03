/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real emitted PCOPY execution against simultaneous-copy semantics.
 * The optional reference emitter adds baseline byte/state comparison in
 * the standalone benchmark; the ordinary runtime suite needs no old code. */
typedef bool (*pc_emit_fn)(const struct t1_mir *, struct t1_emitted *, char *, size_t);
enum { PC_REGS = 11, PC_SLOTS = 8, PC_MAX = 20 };
static const uint8_t pc_regs[PC_REGS] = {
    T1_RAX, T1_RCX, T1_RDX, T1_RBX, T1_RBP, T1_RSI,
    T1_RDI, T1_R8, T1_R9, T1_R10, T1_R15,
};
struct pc_graph { struct t1_loc dst[PC_MAX], src[PC_MAX]; unsigned count; };
struct pc_state { uint64_t reg[16], slot[PC_SLOTS]; };
static uint64_t pc_rng = UINT64_C(0xbad5eedfeadf00d);
static uint64_t pc_random(void)
{
    pc_rng ^= pc_rng << 13; pc_rng ^= pc_rng >> 7; pc_rng ^= pc_rng << 17;
    return pc_rng;
}
static struct t1_loc pc_r(unsigned r) { return (struct t1_loc){T1_LOC_REG, pc_regs[r], 0, 0}; }
static struct t1_loc pc_s(unsigned s) { return (struct t1_loc){T1_LOC_SPILL, 0, (uint16_t)s, 0}; }
static void pc_add(struct pc_graph *g, struct t1_loc dst, struct t1_loc src)
{
    CHECK(g->count < PC_MAX);
    g->dst[g->count] = dst; g->src[g->count++] = src;
}
static void pc_shuffle(struct pc_graph *g)
{
    for (unsigned k = g->count; k > 1; --k) {
        unsigned j = (unsigned)(pc_random() % k);
        struct t1_loc d = g->dst[k - 1], s = g->src[k - 1];
        g->dst[k - 1] = g->dst[j]; g->src[k - 1] = g->src[j];
        g->dst[j] = d; g->src[j] = s;
    }
}
static uint64_t pc_value(const struct pc_state *s, struct t1_loc p)
{
    if (p.kind == T1_LOC_REG) return s->reg[p.reg];
    if (p.kind == T1_LOC_CONST) return p.value;
    CHECK(p.kind == T1_LOC_SPILL && p.slot < PC_SLOTS);
    return s->slot[p.slot];
}
static void pc_step(struct pc_state *s, const struct pc_graph *g)
{
    struct pc_state old = *s;
    for (unsigned k = 0; k < g->count; ++k) {
        uint64_t value = pc_value(&old, g->src[k]);
        struct t1_loc d = g->dst[k];
        if (d.kind == T1_LOC_REG) s->reg[d.reg] = value;
        else { CHECK(d.kind == T1_LOC_SPILL && d.slot < PC_SLOTS); s->slot[d.slot] = value; }
    }
}
static void pc_label(struct t1_mir *m, uint16_t op, unsigned block)
{
    struct t1_mir_insn *in = t1_mir_append(m, op);
    CHECK(in != NULL);
    in->nopnd = 1;
    in->opnd[0] = (struct t1_mir_opnd){T1_MO_LABEL, T1_X86_NOREG, T1_VREG_NONE, block};
}
static struct t1_mir *pc_mir(struct t1_ir *ir, const struct pc_graph *g, bool loop)
{
    t1_ir_init(ir, NULL);
    for (unsigned k = 0; k < (loop ? 3u : 1u); ++k) t1_ir_new_block(ir, 0);
    uint32_t p = t1_ir_new_point(ir, T1_PT_EXIT, UM_CODE, 0, 0, loop ? 2 : 0, 0);
    CHECK(p != T1_NO_POINT && t1_ir_set_frame(ir, p, NULL, NULL, NULL, 0));
    ir->point[p].frame_count = ir->nuses = PC_REGS;
    struct t1_mir *m = t1_mir_create(ir);
    CHECK(m != NULL);
    m->nframe = m->point[p].frame_count = PC_REGS;
    for (unsigned k = 0; k < PC_REGS; ++k) {
        m->frame[k] = (struct t1_mir_frame){T1_ELEMENT_GR(16 + k), T1_MF_VREG, k, 0};
        m->frame_loc[k] = pc_r(k);
    }
    m->flags = T1_COMPILE_VERIFY;
    m->nblocks = loop ? 3 : 1;
    for (unsigned k = 0; k < PC_REGS; ++k) {
        struct t1_mir_insn *in = t1_mir_append(m, T1_MIR_LIVEIN);
        CHECK(in != NULL);
        in->nopnd = 2;
        in->opnd[0] = (struct t1_mir_opnd){T1_MO_VREG, pc_regs[k], k, 0};
        in->opnd[1] = (struct t1_mir_opnd){T1_MO_IMM, T1_X86_NOREG, T1_VREG_NONE, T1_ELEMENT_GR(16 + k)};
    }
    if (loop) {
        m->block[0].term = m->ninsns;
        pc_label(m, T1_X86_JMP, 1);
        m->block[0].count = m->ninsns;
        m->block[1].first = m->ninsns;
        m->block[1].loop = true;
        m->block[1].depth = 1;
        m->block[1].loop_header = 1;
    }
    uint32_t first = t1_mir_new_copies(m, g->count);
    CHECK(first != UINT32_MAX);
    for (unsigned k = 0; k < g->count; ++k) {
        m->copy[first + k].dloc = g->dst[k];
        m->copy[first + k].sloc = g->src[k];
        m->copy[first + k].dst = m->copy[first + k].src = 0;
    }
    struct t1_mir_insn *in = t1_mir_append(m, T1_MIR_PCOPY);
    CHECK(in != NULL);
    in->copies_first = first; in->ncopies = (uint16_t)g->count;
    if (loop) {
        in = t1_mir_append(m, T1_X86_SUB64_RI);
        CHECK(in != NULL);
        in->nopnd = 2;
        in->opnd[0] = (struct t1_mir_opnd){T1_MO_PREG, T1_X86_NOREG, T1_R13, 0};
        in->opnd[1] = (struct t1_mir_opnd){T1_MO_IMM, T1_X86_NOREG, T1_VREG_NONE, 1};
        m->block[1].term = m->ninsns;
        pc_label(m, T1_X86_JCC + T1_CC_NE, 1);
        pc_label(m, T1_X86_JMP, 2);
        m->block[1].count = m->ninsns - m->block[1].first;
        m->block[2].first = m->ninsns;
    }
    unsigned exit_block = loop ? 2 : 0;
    m->block[exit_block].term = m->ninsns;
    in = t1_mir_append(m, T1_MIR_EXIT);
    CHECK(in != NULL);
    in->point = p;
    m->point[p].insn = m->ninsns - 1;
    m->block[exit_block].count = m->ninsns - m->block[exit_block].first;
    return m;
}
static unsigned pc_scratch_accesses(const struct t1_emitted *e)
{
    unsigned n = 0;
    for (size_t at = 0; at < e->size;) {
        struct t1_x86_decoded d;
        CHECK(t1_x86_decode(e->buf + at, e->size - at, &d));
        n += d.has_mem && d.mem.base == T1_R14 && d.mem.disp == (int32_t)X_SPILL(T1_SPILL_SCRATCH);
        at += d.length;
    }
    return n;
}
static void pc_initial(struct pc_state *s, unsigned vector)
{
    memset(s, 0, sizeof(*s));
    for (unsigned k = 0; k < PC_REGS; ++k) {
        uint64_t value = vector == 0 ? UINT64_C(0x1234567800000000) + k :
                         vector == 1 ? UINT64_C(1) << (k * 5) :
                         vector == 2 ? ~(UINT64_C(1) << (k * 5)) : pc_random();
        s->reg[pc_regs[k]] = value;
    }
    for (unsigned k = 0; k < PC_SLOTS; ++k) s->slot[k] = pc_random();
}
static void pc_load(struct vcpu *v, struct t1_exec *x, const struct pc_state *s)
{
    memset(x, 0, sizeof(*x));
    for (unsigned k = 0; k < PC_REGS; ++k) um_gr(v, 16 + k, s->reg[pc_regs[k]], false);
    for (unsigned k = 0; k < PC_SLOTS; ++k) x->spill[k] = s->slot[k];
}
static void pc_check(const struct t1_exec *x, const struct pc_state *want, uint64_t budget)
{
    CHECK(x->point == 0 && x->raw_r13 == budget);
    for (unsigned k = 0; k < PC_REGS; ++k) CHECK(x->regs[pc_regs[k]] == want->reg[pc_regs[k]]);
    for (unsigned k = 0; k < PC_SLOTS; ++k) CHECK(x->spill[k] == want->slot[k]);
}
static void pc_case(struct vcpu *v, const struct pc_graph *g, pc_emit_fn reference,
                    bool no_scratch, bool execute)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    struct t1_mir *m = pc_mir(ir, g, false);
    struct t1_emitted e[2];
    char err[160];
    CHECK(t1_emit(m, &e[0], err, sizeof(err)));
    CHECK(!no_scratch || pc_scratch_accesses(&e[0]) == 0);
    unsigned versions = reference ? 2 : 1;
    if (reference) {
        CHECK(reference(m, &e[1], err, sizeof(err)));
        if (!no_scratch) CHECK(e[0].size == e[1].size && !memcmp(e[0].buf, e[1].buf, e[0].size));
        else CHECK(e[0].size <= e[1].size);
    }
    size_t bytes = e[0].size;
    if (reference && e[1].size > bytes) bytes = e[1].size;
    size_t pages = (bytes + 4095) / 4096;
    uint8_t *code = t1_arena_alloc(pages);
    CHECK(code != NULL);
    static struct t1_exec x;
    struct pc_state seed[8], want[8];
    if (execute) for (unsigned k = 0; k < 8; ++k) { pc_initial(&seed[k], k); want[k] = seed[k]; pc_step(&want[k], g); }
    for (unsigned version = 0; version < versions; ++version) {
        hal_code_set_writable(code, pages);
        t1_emit_place(&e[version], code);
        hal_code_set_executable(code, pages);
        if (execute) for (unsigned k = 0; k < 8; ++k) {
            pc_load(v, &x, &seed[k]);
            CHECK(((t1_entry_fn)(uintptr_t)code)(v, 99, &x) == T1_RET_POINT);
            pc_check(&x, &want[k], 99);
        }
    }
    t1_arena_free(code, pages);
    for (unsigned k = 0; k < versions; ++k) t1_emit_free(&e[k]);
    t1_mir_destroy(m); free(ir);
}
static bool pc_permutation_next(unsigned *p, unsigned n)
{
    unsigned i = n - 1;
    while (i && p[i - 1] >= p[i]) --i;
    if (!i) return false;
    unsigned j = n - 1;
    while (p[j] <= p[i - 1]) --j;
    unsigned swap = p[i - 1]; p[i - 1] = p[j]; p[j] = swap;
    for (unsigned a = i, b = n - 1; a < b; ++a, --b) { swap = p[a]; p[a] = p[b]; p[b] = swap; }
    return true;
}
static void emit_register_cycles(pc_emit_fn reference)
{
    struct vcpu *v = um_vcpu(UM_CODE);
    unsigned count = 0;
    for (unsigned a = 0; a < PC_REGS; ++a) for (unsigned b = a + 1; b < PC_REGS; ++b)
        for (unsigned order = 0; order < 2; ++order) {
            struct pc_graph g = {0};
            pc_add(&g, pc_r(order ? b : a), pc_r(order ? a : b));
            pc_add(&g, pc_r(order ? a : b), pc_r(order ? b : a));
            pc_case(v, &g, reference, true, true); ++count;
        }
    unsigned p[6] = {0, 1, 2, 3, 4, 5};
    do {
        struct pc_graph g = {0};
        for (unsigned k = 0; k < 6; ++k) pc_add(&g, pc_r(k), pc_r(p[k]));
        pc_shuffle(&g); pc_case(v, &g, reference, true, true); ++count;
    } while (pc_permutation_next(p, 6));
    for (unsigned trial = 0; trial < 1024; ++trial) {
        struct pc_graph g = {0};
        for (unsigned k = 0; k < PC_REGS; ++k) pc_add(&g, pc_r(k), pc_r((unsigned)(pc_random() % PC_REGS)));
        pc_shuffle(&g); pc_case(v, &g, reference, true, true); ++count;
    }
    for (unsigned trial = 0; trial < 64; ++trial) {
        struct pc_graph g = {0};
        pc_add(&g, pc_r(0), pc_r(1)); pc_add(&g, pc_r(1), pc_r(0));
        pc_add(&g, pc_r(2), pc_r(0)); pc_add(&g, pc_r(3), pc_r(2));
        pc_add(&g, pc_r(4), pc_r(5)); pc_add(&g, pc_r(5), pc_r(6)); pc_add(&g, pc_r(6), pc_r(4));
        pc_add(&g, pc_r(7), pc_r(5)); pc_add(&g, pc_r(8), pc_r(8));
        pc_add(&g, pc_s(7), pc_s(7)); /* A removed identity does not borrow scratch. */
        pc_shuffle(&g); pc_case(v, &g, reference, true, true); ++count;
    }
    for (unsigned trial = 0; trial < 64; ++trial) {
        struct pc_graph g = {0};
        pc_add(&g, pc_r(0), pc_r(1)); pc_add(&g, pc_r(1), pc_r(0));
        pc_add(&g, pc_s(0), pc_r(0)); pc_add(&g, pc_r(2), pc_s(1)); pc_add(&g, pc_s(1), pc_r(2));
        pc_add(&g, pc_s(2), pc_s(3)); pc_add(&g, pc_s(3), pc_s(2));
        pc_add(&g, pc_r(3), (struct t1_loc){T1_LOC_CONST, 0, 0, UINT64_C(0xfedcba9876543210)});
        pc_add(&g, pc_s(4), (struct t1_loc){T1_LOC_CONST, 0, 0, UINT64_C(0x8765432112345678)});
        pc_shuffle(&g); pc_case(v, &g, reference, false, true); ++count;
    }
    struct pc_graph diagnostic = {0};
    pc_add(&diagnostic, pc_r(0), pc_r(1)); pc_add(&diagnostic, pc_r(1), pc_r(0));
    pc_add(&diagnostic, pc_r(2), (struct t1_loc){T1_LOC_REG, T1_R11, 0, 0});
    pc_case(v, &diagnostic, reference, false, false); ++count;
    vcpu_destroy(v);
    printf("unit_runtime: register cycles: %u graphs, 8 value vectors, all 11 register pairs, "
           "720 permutations, random fanout, disjoint cycles, identities and mixed/R11 fallback%s\n",
           count, reference ? "; baseline bytes and state compared" : "");
}
