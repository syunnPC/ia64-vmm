/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/pmu.h"
#include "ia64/semantics.h"
#include "ia64vmm/interrupts.h"

/* Intel 251110-003 §10.3.11; 308065-001 §3.3.12: PAL reset defaults. */
void ia64_pmu_reset(struct vcpu *v)
{
    if (v->cfg.cpu_model == IA64_CPU_MADISON) {
        v->s.pmc[4] = UINT64_C(1) << 23;
        v->s.pmc[8] = v->s.pmc[9] = UINT64_MAX;
        v->s.pmc[13] = UINT64_C(0x2078fefefefe);
        v->s.pmc[14] = 0xdb6;
        v->s.pmc[15] = UINT64_C(0xfffffff0);
    } else {
        for (unsigned n = 32; n < 36; ++n)
            v->s.pmc[n] = UINT64_MAX;
        v->s.pmc[41] = UINT64_C(0x2078fefefefe);
        v->s.pmc[38] = 0xdb6;
        v->s.pmc[36] = UINT64_C(0xfffffff0);
    }
}

static unsigned enabled(const struct vcpu *v)
{
    const struct ia64_state *s = &v->s;
    if (!(s->psr & ((UINT64_C(1) << 2) | (UINT64_C(1) << 21))) || (s->pmc[0] & 1))
        return 0;
    if (v->cfg.cpu_model == IA64_CPU_MADISON && !(s->pmc[4] & (UINT64_C(1) << 23)))
        return 0;
    unsigned mask = 0, cpl = (s->psr >> 32) & 3, isa = (s->psr >> 34) & 1;
    for (unsigned n = 4; n < 4 + ia64_model_pmu_counters(v->cfg.cpu_model); ++n) {
        uint64_t pmc = s->pmc[n];
        if ((pmc & (UINT64_C(1) << cpl)) && !(pmc & (UINT64_C(1) << (24 + isa))) &&
            (s->psr & (UINT64_C(1) << ((pmc & 64) ? 21 : 2))))
            mask |= 1u << n;
    }
    return mask;
}

bool ia64_pmu_active(const struct vcpu *v)
{
    return enabled(v) != 0;
}

/* Intel 251110-003 §§10.3.4-5; 308065-001 §§3.3.5-6. */
static bool opcode_match(const struct vcpu *v, const Ia64Instruction *i, unsigned channel,
                         bool *match)
{
    const uint64_t *p = v->s.pmc;
    bool madison = v->cfg.cpu_model == IA64_CPU_MADISON;
    *match = true;
    if (p[madison ? 15 : 36] & (UINT64_C(1) << channel))
        return true;
    unsigned reg = (madison ? 8 : 32) + (channel & 1) * (madison ? 1 : 2);
    uint64_t mask =
        madison ? (p[reg] >> 3) & UINT64_C(0x7ffffff) : p[reg] & UINT64_C(0x1ffffffffff);
    uint64_t value = madison ? (p[reg] >> 33) & UINT64_C(0x7ffffff) : p[reg + 1];
    unsigned types = (p[reg] >> (madison ? 60 : 48)) & 15;
    uint64_t all = madison ? UINT64_C(0x7ffffff) : UINT64_C(0x1ffffffffff);
    if (mask == all && types == 15)
        return true;
    unsigned major = (i->raw >> 37) & 15;
    /* SDM Vol.3 Table 4-3: A encodings are valid in M and I slots.
     * Partial M/I selection is not resolved by the PMU match formula. */
    if ((i->unit == IA64_UNIT_M || i->unit == IA64_UNIT_I) &&
        (major == 8 || major == 9 || (major >= 12 && major <= 14)) && (types & 12) != 0 &&
        (types & 12) != 12)
        return false;
    unsigned type;
    switch (i->unit) {
    case IA64_UNIT_M:
        type = 8;
        break;
    case IA64_UNIT_I:
        type = 4;
        break;
    case IA64_UNIT_F:
        type = 2;
        break;
    case IA64_UNIT_B:
        type = 1;
        break;
    default:
        /* The manuals do not define the matcher syllable/type for L+X. */
        return false;
    }
    uint64_t raw = madison ? (i->raw & 0x1fff) | ((i->raw >> 14) & UINT64_C(0x7ffe000)) : i->raw;
    *match = (types & type) && !((raw ^ value) & ~mask & all);
    return true;
}

static bool address_match(const struct vcpu *v, const Ia64Instruction *i, unsigned channel,
                          bool *match)
{
    const struct ia64_state *s = &v->s;
    bool madison = v->cfg.cpu_model == IA64_CPU_MADISON;
    uint64_t opcode = s->pmc[madison ? 8 : 32], ranges = s->pmc[madison ? 14 : 38];
    *match = true;
    if ((ranges & (UINT64_C(1) << (1 + 3 * channel))) ||
        (!channel && (opcode & (UINT64_C(1) << (madison ? 0 : 57)))))
        return true;
    bool invert = !channel && (opcode & (UINT64_C(1) << (madison ? 1 : 56)));
    bool fine = ranges & (UINT64_C(1) << 13);
    if (fine && channel > 1) {
        /* Madison Table 10-12, §10.3.5.2: paired fine tags share the
         * lower pair's comparator. PMC8 ig_ad/inv scope is unresolved. */
        if (!madison || (opcode & 3))
            return false;
        channel &= 1;
    }
    /* Debug-owned ranges need a separate tag model. */
    if (madison && (s->psr & IA64_PSR_DB))
        return false;
    if (madison)
        for (unsigned n = 0; n < 4; ++n)
            if (s->ibr[2 * n + 1] >> 63)
                return false;
    uint64_t control = s->ibr[2 * channel + 1], address = s->ibr[2 * channel];
    if (control >> 63)
        return false;
    uint64_t mask =
        (UINT64_C(0xff00000000000000) | (control & UINT64_C(0x00ffffffffffffff))) & ~UINT64_C(15);
    bool privilege = control & (UINT64_C(1) << (56 + ((s->psr >> 32) & 3)));
    /* The inv descriptions do not resolve fine-mode or PLM/ISA interaction. */
    if (invert && (fine || !privilege || (s->psr & IA64_PSR_IS)))
        return false;
    if (fine) {
        unsigned bits = madison ? 12 : 16;
        uint64_t low = (UINT64_C(1) << bits) - 1, upper = s->ibr[2 * channel + 4];
        if ((address & ~low) != (upper & ~low) || (address & low) >= (upper & low) ||
            (s->ibr[2 * channel + 5] >> 63))
            return false;
        if (!madison && (ranges & (UINT64_C(1) << (7 + 3 * channel))))
            return false;
        *match = !((i->address ^ address) & mask & ~low) && (i->address & low) > (address & low) &&
                 (i->address & low) < (upper & low);
    } else {
        *match = !((i->address ^ address) & mask);
        /* Intel 251110-003 Table 10-9; 308065-001 Table 3-11. */
        if (invert)
            *match = !*match;
    }
    *match = *match && privilege && !(s->psr & IA64_PSR_IS);
    return true;
}

struct ia64_result ia64_pmu_prepare(const struct vcpu *v, const Ia64Instruction *i,
                                    struct ia64_pmu_retirement *out)
{
    unsigned mask = enabled(v);
    *out = (struct ia64_pmu_retirement){0};
    if (!mask)
        return ia64_complete();
    bool predicate = ia64_pr_read(&v->s, i->qp);
    for (unsigned n = 4; n < 16; ++n) {
        if (!(mask & (1u << n)))
            continue;
        uint64_t pmc = v->s.pmc[n];
        unsigned event = (pmc >> 8) & 255;
        if (event != 8 && event != 0x50 && event != 0x51)
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, pmc, "PMU event not implemented");
        if (event != 8 && (pmc & UINT64_C(0xf0000)))
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, pmc, "PMU event unit mask not implemented");
        if (pmc & UINT64_C(0x700000))
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, pmc, "PMU threshold requires cycle model");
        if (v->cfg.cpu_model != IA64_CPU_MADISON && n < 10 && (pmc & (UINT64_C(1) << 26)))
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, pmc, "PMU event qualifier not implemented");
        if (pmc & 32)
            out->interrupt |= 1u << n;
        bool address, opcode;
        unsigned channel = (pmc >> 16) & 3;
        if (!address_match(v, i, channel, &address))
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, pmc,
                             "PMU instruction address qualifier not implemented");
        if (!address) {
            mask &= ~(1u << n);
            continue;
        }
        if (!opcode_match(v, i, channel, &opcode))
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, pmc,
                             "PMU instruction opcode qualifier not implemented");
        if (!opcode)
            mask &= ~(1u << n);
        /* Intel 251110-003 pp.186-187; 308065-001 p.162. */
        if (event == 0x50) {
            if (i->encoding_class == IA64_ENCODING_IGNORED ||
                (i->opcode == IA64_OP_NOP && i->unit == IA64_UNIT_X))
                return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, pmc,
                                 "PMU nop event encoding not implemented");
            if (!predicate || i->opcode != IA64_OP_NOP)
                mask &= ~(1u << n);
        } else if (event == 0x51) {
            if (predicate || i->unit == IA64_UNIT_B)
                mask &= ~(1u << n);
        } else if (v->cfg.cpu_model == IA64_CPU_MADISON && !predicate &&
                   ia64_opcode_info(i->opcode)->family != IA64_FAMILY_BRANCH) {
            mask &= ~(1u << n);
        }
    }
    /* Active writes have an implementation-dependent serialization interval
     * (SDM Vol.2 §7.2.1). Use entry controls; a PMD replacement wins over
     * this instruction's increment of that same counter. */
    if (predicate && (i->opcode == IA64_OP_MOV_GRPMD || i->opcode == IA64_OP_MOV_GRPMD_INDEXED)) {
        unsigned n = i->opcode == IA64_OP_MOV_GRPMD
                         ? (unsigned)i->operands.system.source - 64u
                         : (unsigned)ia64_gr_read(&v->s, i->operands.system.register_index) & 255u;
        if (n < 16)
            mask &= ~(1u << n);
    }
    out->count = mask;
    return ia64_complete();
}

void ia64_pmu_retire(struct vcpu *v, struct ia64_pmu_retirement pending)
{
    unsigned overflow = 0;
    for (unsigned n = 4; n < 16; ++n) {
        if (!(pending.count & (1u << n)))
            continue;
        uint64_t old = v->s.pmd[n];
        v->s.pmd[n] = (old + 1) & UINT64_C(0xffffffffffff);
        if ((old & UINT64_C(0x7fffffffffff)) == UINT64_C(0x7fffffffffff))
            overflow |= 1u << n;
    }
    v->s.pmc[0] |= overflow;
    if (overflow & pending.interrupt) {
        v->s.pmc[0] |= 1;
        vcpu_raise_local_interrupt(v, VCPU_LOCAL_PMV);
    }
}
