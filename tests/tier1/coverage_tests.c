/* SPDX-License-Identifier: GPL-2.0-or-later */
/* COV: whole and in-place differential restoration, random operands,
 * predicates, NaTVal, modified bits, rotating names and wrapping BOFs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/semantics.h"
#include "engine/cache/cache.h"
#include "tests/tier1/um_t1.h"
#include "engine/tier1/region.h"
#include "engine/tier1/helpers.h"
#include "tests/tier1/helper_formats.h"
#include "ia64/decode/decode.h"
#include "ia64/fp.h"
#include "ia64/tlb.h"
#include "ia64vmm/guest_mem.h"
#include "tests/tier1/engine_tier0t1.h"
#include "tests/tier1/engine_tier1.h"

static uint64_t rng = UINT64_C(0x9a8b7c6d5e4f3210);
static uint64_t random64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void restore_homes(struct vcpu *v, const struct vcpu *src, const struct t1_insn *ti,
                          const struct t1_hset *sets, unsigned n)
{
    for (unsigned k = 0; k < n; ++k) {
        const struct t1_hset *set = &sets[k];
        unsigned reg = set->slot == T1_HS_R1   ? ti->insn.operands.common.destination
                       : set->slot == T1_HS_R2 ? ti->insn.operands.common.source1
                       : set->slot == T1_HS_P1 ? ti->insn.operands.common.auxiliary1
                       : set->slot == T1_HS_P2 ? ti->insn.operands.common.auxiliary2
                                               : ti->insn.operands.common.source2;
        if (set->loc == T1_HL_PR) {
            uint64_t bit = UINT64_C(1) << ia64_pr_index(&v->s, reg & 63);
            if (reg & 63)
                v->s.pr = (v->s.pr & ~bit) | (src->s.pr & bit);
        } else if (set->loc == T1_HL_FR) {
            unsigned p = ia64_fr_index(&v->s, reg);
            v->s.fr[p] = src->s.fr[p];
        } else if (set->loc == T1_HL_AR)
            v->s.ar[reg] = src->s.ar[reg];
        else if (set->loc == T1_HL_BR)
            v->s.br[reg & 7] = src->s.br[reg & 7];
        else if (set->loc == T1_HL_UNAT)
            v->s.ar[AR_UNAT] = src->s.ar[AR_UNAT];
        else if (set->loc == T1_HL_CSD)
            v->s.ar[AR_CSD] = src->s.ar[AR_CSD];
        else if (set->loc == T1_HL_CCV)
            v->s.ar[AR_CCV] = src->s.ar[AR_CCV];
        else if (set->loc == T1_HL_FPSR)
            v->s.ar[AR_FPSR] = src->s.ar[AR_FPSR];
        else if (set->loc == T1_HL_PSR_MF)
            v->s.psr = (v->s.psr & ~T1_PSR_MF_BITS) | (src->s.psr & T1_PSR_MF_BITS);
    }
}

/* Randomize homes outside reads and conditional old destinations. Their
 * values must not change the result or the memory transaction. Contract
 * state (CFM, translation and PSR) is held fixed, as in formation. */
static void read_fuzz(struct vcpu *v, struct vcpu *before, const struct t1_insn *ti,
                      const struct t1_helper_row *row)
{
    struct vcpu *other = um_vcpu(UM_CODE);
    other->s = before->s;
    for (unsigned k = 1; k < 128; ++k) {
        other->s.gr[k] = random64();
        other->s.nat[k] = random64() & 1;
        other->s.br[k & 7] = random64();
        if (k >= 2)
            other->s.fr[k] =
                (struct ia64_fr){.significand = random64(), .exponent = IA64_FP_INTEGER_EXP};
    }
    other->s.pr = random64() | 1;
    const unsigned ars[] = {AR_UNAT, AR_CSD, AR_CCV, AR_FPSR, AR_LC, AR_PFS, AR_EC, 0, 7};
    for (unsigned k = 0; k < sizeof(ars) / sizeof(ars[0]); ++k)
        other->s.ar[ars[k]] = random64();
    uint16_t elements[T1_N_ELEMENTS];
    for (unsigned w = 0; w < 2; ++w) {
        unsigned n = t1_helper_elements(ti, w, elements);
        for (unsigned k = 0; k < n; ++k) {
            uint64_t value;
            UM_CHECK(t1_home_read(before, elements[k], &value));
            t1_home_write(other, elements[k], value);
        }
        restore_homes(other, before, ti, w ? row->write : row->read, w ? row->nwrite : row->nread);
    }
    /* Cover every guest byte in this fixture, including code and IVT,
     * rather than only the expected transaction's destination. */
    static uint8_t initial[UM_RAM_SIZE], expected[UM_RAM_SIZE];
    memcpy(initial, um_ram(UM_RAM_BASE), sizeof(initial));
    struct ia64_result a = ia64_execute(v, &ti->insn);
    memcpy(expected, um_ram(UM_RAM_BASE), sizeof(expected));
    if (!(row->flags & T1_HROW_STORE))
        UM_CHECK(!memcmp(initial, expected, sizeof(initial)));
    um_load(UM_RAM_BASE, initial, sizeof(initial));
    struct ia64_result b = ia64_execute(other, &ti->insn);
    UM_CHECK(a.kind == b.kind && a.fault == b.fault && a.exit == b.exit && a.info == b.info);
    UM_CHECK(!memcmp(expected, um_ram(UM_RAM_BASE), sizeof(expected)));
    unsigned n = t1_helper_elements(ti, true, elements);
    for (unsigned k = 0; k < n; ++k) {
        uint64_t x, y;
        UM_CHECK(t1_home_read(v, elements[k], &x) && t1_home_read(other, elements[k], &y) &&
                 x == y);
    }
    struct ia64_state result = other->s;
    restore_homes(other, v, ti, row->write, row->nwrite);
    UM_CHECK(!memcmp(&other->s, &result, sizeof(result)));
    um_load(UM_RAM_BASE, expected, sizeof(expected));
    vcpu_destroy(other);
}

/* Compare all rows with an independent register-role schema, then
 * exercise the decoder's actual operand assignment for every format.
 * Enumerating major/x fields also covers register/immediate increments,
 * pair loads, and speculative/check variants sharing a row. */
static unsigned format_audit(void)
{
    unsigned seen[IA64_OP_COUNT] = {0}, rows = 0, cases = 0;
    for (unsigned op = 0; op < IA64_OP_COUNT; ++op) {
        const struct t1_helper_row *row = t1_helper_row(op);
        if (!row) {
            UM_CHECK(!helper_formats[op]);
            continue;
        }
        UM_CHECK(ia64_opcode_reachable(op) && helper_formats[op]);
        ++rows;
        for (unsigned w = 0; w < 2; ++w) {
            uint64_t roles = 0;
            unsigned n = w ? row->nwrite : row->nread;
            const struct t1_hset *set = w ? row->write : row->read;
            for (unsigned k = 0; k < n; ++k) {
                unsigned bank = set[k].loc == T1_HL_GR   ? 0
                                : set[k].loc == T1_HL_FR ? 1
                                : set[k].loc == T1_HL_AR ? 2
                                : set[k].loc == T1_HL_BR ? 3
                                : set[k].loc == T1_HL_PR ? 4
                                                         : 5;
                if (bank < 5) {
                    UM_CHECK(set[k].slot != T1_HS_NONE);
                    uint64_t bit = UINT64_C(1) << (8 * bank + set[k].slot);
                    UM_CHECK(!(roles & bit));
                    roles |= bit;
                } else
                    UM_CHECK(set[k].slot == T1_HS_NONE);
            }
            const struct helper_roles *expected = &format_roles[helper_formats[op]];
            UM_CHECK(roles == (w ? expected->write : expected->read));
        }
    }
    const IA64SlotUnit units[] = {IA64_UNIT_M, IA64_UNIT_I, IA64_UNIT_F};
    for (unsigned unit = 0; unit < 3; ++unit)
        for (unsigned operands = 0; operands < 4; ++operands)
            for (unsigned extension = 0; extension < (1u << 14); ++extension) {
                uint64_t raw = ((uint64_t)extension << 27) | (UINT64_C(40) << 6) |
                               ((uint64_t)(unsigned[]){41, 0, 104, 3}[operands] << 13) |
                               (UINT64_C(42) << 20);
                Ia64Instruction in = ia64_decode_insn(units[unit], raw, UM_CODE, 0);
                const struct t1_helper_row *row = t1_helper_row(in.opcode);
                if (!row || !in.valid || in.encoding_class != IA64_ENCODING_DEFINED)
                    continue;
                ++seen[in.opcode];
                ++cases;
                struct t1_insn ti = {.insn = in,
                                     .qp = 0,
                                     .dst = in.operands.common.destination,
                                     .src1 = in.operands.common.source1,
                                     .src2 = in.operands.common.source2,
                                     .fr = {in.operands.common.destination,
                                            in.operands.common.source1, in.operands.common.source2,
                                            in.operands.common.auxiliary1}};
                uint16_t reads[T1_N_ELEMENTS], writes[T1_N_ELEMENTS];
                unsigned nr = t1_helper_elements(&ti, false, reads),
                         nw = t1_helper_elements(&ti, true, writes);
                for (unsigned k = 0; k < nw; ++k) {
                    if (!((writes[k] >= T1_ELEMENT_FR(2) && writes[k] <= T1_ELEMENT_FR(127)) ||
                          (writes[k] >= T1_ELEMENT_FR_META(2) &&
                           writes[k] <= T1_ELEMENT_FR_META(127))))
                        continue;
                    unsigned r = 0;
                    while (r < nr && reads[r] != writes[k])
                        ++r;
                    UM_CHECK(r < nr);
                }
                uint64_t roles = format_roles[helper_formats[in.opcode]].read |
                                 format_roles[helper_formats[in.opcode]].write;
                for (unsigned slot = T1_HS_R1; slot <= T1_HS_P2; ++slot) {
                    if (!(roles & (UINT64_C(0x0101010101) << slot)))
                        continue;
                    unsigned original = slot == T1_HS_R1   ? in.operands.common.destination
                                        : slot == T1_HS_R2 ? in.operands.common.source1
                                        : slot == T1_HS_R3 ? in.operands.common.source2
                                        : slot == T1_HS_P1 ? in.operands.common.auxiliary1
                                                           : in.operands.common.auxiliary2;
                    /* A real operand varies independently of the opcode. Constant
             * f0 in xmpy.hu is the decoder's explicit F1 alias exception. */
                    if (in.opcode == IA64_OP_XMPY_HU && slot == T1_HS_R2) {
                        UM_CHECK(original == 0);
                        continue;
                    }
                    /* I5/I6 shift-count immediates leave their count-GR slot
             * zero. The row is the union with the register-count format;
             * gr_pair excludes this constant r0 from the effective set. */
                    bool left = in.opcode == IA64_OP_PSHL2 || in.opcode == IA64_OP_PSHL4;
                    bool right = in.opcode == IA64_OP_PSHR2 || in.opcode == IA64_OP_PSHR2_U ||
                                 in.opcode == IA64_OP_PSHR4 || in.opcode == IA64_OP_PSHR4_U;
                    if (in.operands.common.immediate >= 0 &&
                        ((left && slot == T1_HS_R3) || (right && slot == T1_HS_R2))) {
                        UM_CHECK(original == 0);
                        continue;
                    }
                    bool varies = false;
                    for (unsigned bit = 6; bit < 37; ++bit) {
                        Ia64Instruction changed =
                            ia64_decode_insn(units[unit], raw ^ (UINT64_C(1) << bit), UM_CODE, 0);
                        if (!changed.valid || changed.opcode != in.opcode ||
                            changed.encoding_class != IA64_ENCODING_DEFINED)
                            continue;
                        unsigned value = slot == T1_HS_R1   ? changed.operands.common.destination
                                         : slot == T1_HS_R2 ? changed.operands.common.source1
                                         : slot == T1_HS_R3 ? changed.operands.common.source2
                                         : slot == T1_HS_P1 ? changed.operands.common.auxiliary1
                                                            : changed.operands.common.auxiliary2;
                        varies |= value != original;
                    }
                    if (!varies)
                        fprintf(stderr, "nonoperand: %s slot %u raw %llx value %u\n",
                                ia64_opcode_info(in.opcode)->name, slot, (unsigned long long)raw,
                                original);
                    UM_CHECK(varies);
                }
            }
    for (unsigned op = 0; op < IA64_OP_COUNT; ++op)
        if (t1_helper_row(op) && !seen[op]) {
            fprintf(stderr, "helper format missing: %s\n", ia64_opcode_info(op)->name);
            UM_CHECK(seen[op]);
        }
    printf("coverage: %u helper rows, %u decoded format cases audited\n", rows, cases);
    return cases;
}

/* Every declared helper opcode on randomized architectural homes. Any
 * changed home outside the write set is a table defect, including NaT and
 * both members of a paired FR load. The table drives only normalization;
 * ia64_execute supplies the observed changes. */
static unsigned table_fuzz(void)
{
    struct vcpu *v = um_vcpu(UM_CODE), *before = um_vcpu(UM_CODE);
    unsigned runs = 0;
    for (unsigned opcode = 0; opcode < IA64_OP_COUNT; ++opcode) {
        const struct t1_helper_row *row = t1_helper_row((Ia64Opcode)opcode);
        if (!row)
            continue;
        const unsigned ars[] = {AR_UNAT, AR_PFS, AR_EC, AR_CCV, AR_FPSR, AR_CSD, 0, 7};
        bool ar_move =
            opcode == IA64_OP_MOV_ARGR || opcode == IA64_OP_MOV_GRAR || opcode == IA64_OP_MOV_IMMAR;
        for (unsigned variant = 0; variant < (ar_move ? sizeof(ars) / sizeof(ars[0]) : 1);
             ++variant)
            for (unsigned sample = 0; sample < 32; ++sample) {
                struct t1_insn ti = {
                    .insn = {.opcode = (Ia64Opcode)opcode,
                             .valid = true,
                             .encoding_class = IA64_ENCODING_DEFINED,
                             .unit = row->cls == T1_HC_R ? IA64_UNIT_I : IA64_UNIT_M}};
                ti.insn.operands.common.destination = 40;
                ti.insn.operands.common.source1 = 41;
                ti.insn.operands.common.source2 = 2;
                ti.insn.operands.common.auxiliary1 = 42;
                ti.insn.operands.common.auxiliary2 = 43;
                ti.insn.operands.common.immediate = sample & 1 ? 1 : 0;
                if (opcode == IA64_OP_MOV_GRPR)
                    ti.insn.operands.common.immediate = (int64_t)random64();
                if (opcode == IA64_OP_MOV_PR_ROT_IMM)
                    ti.insn.operands.common.immediate = (int64_t)(random64() & ~UINT64_C(0xffff));
                if (row->flags & T1_HROW_POSTINC) {
                    ti.insn.reg_base_update = sample % 4 == 1 && !(row->flags & T1_HROW_STORE);
                    ti.insn.imm_base_update = sample % 4 == 2;
                    if (ti.insn.imm_base_update)
                        ti.insn.operands.common.immediate = 8;
                }
                if (ar_move) {
                    ti.insn.operands.common.source1 = ars[variant];
                    ti.insn.unit = ars[variant] >= 64 ? IA64_UNIT_I : IA64_UNIT_M;
                    if (opcode == IA64_OP_MOV_IMMAR)
                        ti.insn.operands.common.destination = 0;
                }
                if (opcode == IA64_OP_MOV_BRGR || opcode == IA64_OP_MOV_GRBR)
                    ti.insn.operands.common.source1 = 6;
                if (helper_formats[opcode] == HF_I_UNARY || helper_formats[opcode] == HF_M_FETCHADD)
                    ti.insn.operands.common.source1 = 0;
                if (helper_formats[opcode] == HF_I_MUX)
                    ti.insn.operands.common.source2 = 0;
                if (helper_formats[opcode] == HF_M_FETCHADD)
                    ti.insn.operands.common.immediate = 4;
                v->s.cfm = um_cfm(64, 64, 8, 5, 3);
                v->s.rse_bof = random64() % 96;
                ti.fr[0] = ia64_fr_index(&v->s, ti.insn.operands.common.destination & 127);
                ti.fr[1] = ia64_fr_index(&v->s, ti.insn.operands.common.source1 & 127);
                ti.fr[2] = ia64_fr_index(&v->s, ti.insn.operands.common.source2 & 127);
                ti.fr[3] = ia64_fr_index(&v->s, ti.insn.operands.common.auxiliary1 & 127);
                for (unsigned k = 1; k < 128; ++k) {
                    v->s.gr[k] = random64();
                    v->s.nat[k] = sample & 2;
                }
                for (unsigned k = 2; k < 128; ++k)
                    v->s.fr[k] = sample & 4 ? ia64_fp_nat()
                                            : (struct ia64_fr){.significand = random64(),
                                                               .exponent = IA64_FP_INTEGER_EXP};
                um_gr(v, 2, UM_DATA, sample & 8);
                v->s.pr = random64() | 1;
                for (unsigned k = 0; k < sizeof(ars) / sizeof(ars[0]); ++k)
                    v->s.ar[ars[k]] = random64();
                v->s.psr = (v->s.psr & ~(UINT64_C(3) << 32)) | ((uint64_t)(sample % 4) << 32);
                if (ar_move && !(sample & 4)) {
                    if (ars[variant] == AR_FPSR || ars[variant] == AR_PFS)
                        um_gr(v, 40, 0, sample & 2);
                }
                ti.insn.qp = sample & 16 ? 6 : 0;
                ti.qp = ia64_pr_index(&v->s, ti.insn.qp);
                ti.dst = ia64_gr_index(&v->s, ti.insn.operands.common.destination);
                ti.src1 = ia64_gr_index(&v->s, ti.insn.operands.common.source1);
                ti.src2 = ia64_gr_index(&v->s, ti.insn.operands.common.source2);
                unsigned bof = v->s.rse_bof;
                uint8_t *regs[] = {&ti.dst, &ti.src1, &ti.src2};
                for (unsigned k = 0; k < 3; ++k)
                    if (*regs[k] >= 32)
                        *regs[k] = 32 + (*regs[k] - 32 + 96 - bof) % 96;
                before->s = v->s;
                struct vcpu unchanged = *v;
                read_fuzz(v, before, &ti, row);
                uint16_t elements[T1_N_ELEMENTS];
                unsigned n = t1_helper_elements(&ti, true, elements);
                for (unsigned k = 0; k < n; ++k) {
                    uint64_t value;
                    UM_CHECK(t1_home_read(before, elements[k], &value));
                    t1_home_write(v, elements[k], value);
                }
                for (unsigned k = 0; k < row->nwrite; ++k) {
                    const struct t1_hset *set = &row->write[k];
                    unsigned reg = set->slot == T1_HS_R1   ? ti.insn.operands.common.destination
                                   : set->slot == T1_HS_R2 ? ti.insn.operands.common.source1
                                   : set->slot == T1_HS_P1 ? ti.insn.operands.common.auxiliary1
                                   : set->slot == T1_HS_P2 ? ti.insn.operands.common.auxiliary2
                                                           : ti.insn.operands.common.source2;
                    if (set->loc == T1_HL_PR) {
                        uint64_t bit = UINT64_C(1) << ia64_pr_index(&v->s, reg & 63);
                        if (reg & 63)
                            v->s.pr = (v->s.pr & ~bit) | (before->s.pr & bit);
                    } else if (set->loc == T1_HL_FPSR)
                        v->s.ar[AR_FPSR] = before->s.ar[AR_FPSR];
                    else if (set->loc == T1_HL_FR) {
                        unsigned p = ia64_fr_index(&v->s, reg);
                        v->s.fr[p] = before->s.fr[p];
                    } else if (set->loc == T1_HL_AR)
                        v->s.ar[reg] = before->s.ar[reg];
                    else if (set->loc == T1_HL_BR)
                        v->s.br[reg & 7] = before->s.br[reg & 7];
                    else if (set->loc == T1_HL_UNAT)
                        v->s.ar[AR_UNAT] = before->s.ar[AR_UNAT];
                    else if (set->loc == T1_HL_CSD)
                        v->s.ar[AR_CSD] = before->s.ar[AR_CSD];
                    else if (set->loc == T1_HL_PSR_MF)
                        v->s.psr = (v->s.psr & ~T1_PSR_MF_BITS) | (before->s.psr & T1_PSR_MF_BITS);
                }
                struct vcpu normalized = *v;
                if (row->cls == T1_HC_M) {
                    /* MEM/DTLB effects also update the nonarchitectural
                 * physical lookup cache. The walker test below checks
                 * virtual translations and their logged changes. */
                    normalized.physical_region = unchanged.physical_region;
                    normalized.physical_page = unchanged.physical_page;
                }
                if (memcmp(&normalized, &unchanged, sizeof(normalized))) {
                    fprintf(stderr, "helper-table %s sample %u changed an undeclared home\n",
                            ia64_opcode_info(opcode)->name, sample);
                    UM_CHECK(!"helper write set");
                }
                ++runs;
            }
    }
    vcpu_destroy(v);
    vcpu_destroy(before);
    return runs;
}

static void prime_shadow(struct t1_vcpu *tv, struct vcpu *v, void *arg)
{
    (void)tv;
    (void)arg;
    struct hostwin_vcpu *hw = umt_hostwin();
    struct hostwin_tag tag;
    enum hostwin_exit_reason why;
    hostwin_enter(hw, v);
    UM_CHECK(hostwin_bind(hw, v, ia64_gr_read(&v->s, 3), &tag) == HOSTWIN_OK);
    UM_CHECK(hostwin_resolve(hw, v, ia64_gr_read(&v->s, 3), 8, false, &why) == HOSTWIN_MAPPED);
}

#define COV_DEVICE UINT64_C(0xb00000)
static unsigned device_reads;
static bool device_stops;
static uint64_t device_read(void *opaque, struct vcpu *v, uint64_t offset, unsigned size)
{
    (void)opaque;
    UM_CHECK(offset == 0 && size == 8);
    ++device_reads;
    if (device_stops)
        vcpu_request_exit(v, VCPU_EXIT_DEBUG);
    return UINT64_C(0x12345678);
}
static const struct gm_mmio_ops device_ops = {.read = device_read};

static void device_cases(const char *image)
{
    UM_CHECK(gm_add_region(&(struct gm_region){.base = COV_DEVICE,
                                               .size = 4096,
                                               .kind = GM_EMULATED,
                                               .ops = &device_ops,
                                               .name = "COV single-transaction"}));
    for (unsigned stop = 0; stop < 2; ++stop) {
        struct vcpu *v = um_vcpu(UM_CODE + 0xa00), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        v->s.cfm = um_cfm(64, 64, 0, 0, 0);
        um_gr(v, 2, COV_DEVICE, false);
        um_gr(v, 3, UM_DATA, false);
        v->s.ar[AR_LC] = 99;
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "COV device");
        UM_CHECK(id && t1_region_get(id)->nhelper == 1);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX, .final_only = stop != 0};
        device_reads = 0;
        device_stops = stop != 0;
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 270, id, &arm, &out, false,
                          "COV device") == 0);
        /* Includes the independent reference's transactions. */
        UM_CHECK(out.run.entries && device_reads == (stop ? 2u : 60u));
        if (stop)
            UM_CHECK(v->retired == 1 && v->s.ip == UM_CODE + 0xa00 && v->s.slot == 1);
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    /* The product's upper-tier STOP must carry the notification site too. */
    struct vcpu *v = um_vcpu(UM_CODE + 0xa00), *ref = um_vcpu(UM_CODE);
    um_load_file(UM_CODE, image);
    v->s.cfm = um_cfm(64, 64, 0, 0, 0);
    um_gr(v, 2, COV_DEVICE, false);
    um_gr(v, 3, UM_DATA, false);
    v->s.ar[AR_LC] = 99;
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), v);
    uint32_t old_flags = t1h_tier0t1_flags();
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    struct t1_compile_options opt = {.flags = T1_COMPILE_VERIFY};
    struct t1_compile_report rep;
    UM_CHECK(t1_compile(t1h_tier1_vcpu(ev.ctx), &opt, &rep) == T1_COMPILED);
    device_reads = 0;
    device_stops = true;
    struct vcpu_exit exit = t1h_engine_run(&ev, 270);
    struct um_run run = {0};
    UM_CHECK(um_compare(&snap, ref, &run, v, &exit, 270, "COV product notification") == 0);
    UM_CHECK(device_reads == 2 && v->retired == 1);
    t1h_tier0t1_set_flags(old_flags);
    t1h_engine_release(&ev);
    um_snapshot_free(&snap);
    vcpu_destroy(v);
    vcpu_destroy(ref);
    device_stops = false;
}

static void memory_cases(const char *image)
{
    /* Change the following I instruction in this very bundle (+1 -> +7).
     * Continuing generated code after the store executes stale semantics. */
    struct vcpu *v = um_vcpu(UM_CODE + 0x800), *ref = um_vcpu(UM_CODE);
    um_load_file(UM_CODE, image);
    uint64_t word;
    memcpy(&word, um_ram(UM_CODE + 0x1800), 8);
    um_gr(v, 2, UM_CODE + 0x800, false);
    um_gr(v, 40, word, false);
    v->s.cfm = um_cfm(64, 64, 0, 0, 0);
    um_gr(v, 40, word, false);
    v->s.ar[AR_LC] = 99;
    struct t1_compile_report rep;
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "COV self-store");
    UM_CHECK(id && t1_region_get(id)->nhelper == 1);
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    struct umt_run out;
    struct umt_arm arm = {.point = UINT32_MAX};
    struct t1_stats before, after;
    t1_get_stats(&before);
    UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 100, id, &arm, &out, false,
                      "COV self-store") == 0);
    t1_get_stats(&after);
    UM_CHECK(out.run.entries && after.invalidations > before.invalidations);
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);

    /* A 64 KiB walker insertion replaces a cached 4 KiB translation.
     * The following native load must observe the new physical backing. */
    v = um_vcpu(UM_CODE + 0xa00);
    ref = um_vcpu(UM_CODE);
    um_load_file(UM_CODE, image);
    v->s.cfm = um_cfm(64, 64, 0, 0, 0);
    v->s.psr |= IA64_PSR_DT;
    const uint64_t vrn = UINT64_C(2) << 61, x = vrn | 0x12340000, y = x + 0x3000;
    const uint64_t rights = 1 | 0x20 | 0x40 | (2u << 9);
    v->s.rr[2] = (UINT64_C(0x222) << 8) | (16u << 2) | 1;
    ia64_mmu_changed(v, IA64_MMU_RR, 0, 2);
    v->s.cr[CR_PTA] = (vrn | 0x40000000) | (15u << 2) | 1;
    uint64_t h = ia64_thash(&v->s, y);
    v->s.tr[1][0] = (struct ia64_tlb_entry){.va = h & ia64_low_mask(61) & ~UINT64_C(0x7fff),
                                            .pte = (UM_DATA + 0x10000) | rights,
                                            .itir = 15u << 2,
                                            .rid = 0x222,
                                            .page_shift = 15,
                                            .valid = true};
    ia64_mmu_changed(v, IA64_MMU_TR, 1, 0);
    ia64_tc_insert(v, x, 1, (UM_DATA + 0x1000) | rights, 12u << 2);
    uint64_t pte = UM_DATA | rights, value = UINT64_C(0x8877665544332211), old = ~value;
    um_load(UM_DATA + 0x10000 + (h & 0x7fff), &pte, 8);
    um_load(UM_DATA, &value, 8);
    um_load(UM_DATA + 0x1000, &old, 8);
    um_load(UM_DATA + 0x3000, &old, 8);
    um_gr(v, 2, y, false);
    um_gr(v, 3, x, false);
    v->s.ar[AR_LC] = 99;
    id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "COV walker");
    UM_CHECK(id && t1_region_get(id)->nhelper == 1);
    um_snapshot_take(&snap, v);
    arm.before_run = prime_shadow;
    unsigned tc_next = v->s.tc_next[1];
    UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 100, id, &arm, &out, false, "COV walker") ==
             0);
    UM_CHECK(out.run.entries && v->s.tc_next[1] > tc_next && ia64_gr_read(&v->s, 41) == value);
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}

static void exact_nat_cases(const char *image)
{
    const unsigned offsets[] = {0x1000, 0x1200, 0x1400, 0x1600, 0x5000};
    for (unsigned fragment = 0; fragment < 5; ++fragment) {
        struct vcpu *v = um_vcpu(UM_CODE + offsets[fragment]), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        v->s.cfm = um_cfm(64, 64, 8, 5, 3);
        v->s.rse_bof = 95;
        um_gr(v, 2, fragment ? UM_DATA : UINT64_C(0xdead0000), false);
        um_gr(v, 40, UINT64_C(0x1234), fragment != 3);
        um_gr(v, 46, UINT64_C(0x5678), true);
        um_pr(v, 6, false);
        v->s.ar[AR_LC] = 99;
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "COV exact NaT/PR");
        UM_CHECK(id && (fragment == 3 || t1_region_get(id)->loop));
        UM_CHECK(t1_region_get(id)->nhelper == (fragment == 3 ? 3u : fragment == 2 ? 2u : 1u));
        if (fragment == 4) {
            const struct t1_region *r = t1_region_get(id);
            const struct t1_helper_info *h = &r->helper[0];
            uint64_t predicates = 0;
            UM_CHECK(h->nreads == 0);
            const struct t1_point *p = &r->point[h->point];
            for (unsigned k = 0; k < p->frame_count; ++k) {
                const struct t1_frame_entry *f = &r->frame[p->frame_first + k];
                if (f->element >= T1_ELEMENT_PR(1) && f->element < T1_ELEMENT_LC) {
                    UM_CHECK(f->kind == T1_LOC_HOME);
                    predicates |= UINT64_C(1) << (f->element - T1_ELEMENT_PR(0));
                }
            }
            UM_CHECK(predicates == ~UINT64_C(1));
        }
        UM_CHECK(t1_nat_guard_holds(t1_region_get(id), v));
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX};
        struct t1_stats before, after;
        t1_get_stats(&before);
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 270, id, &arm, &out, false,
                          "COV exact NaT/PR") == 0);
        t1_get_stats(&after);
        UM_CHECK(out.run.entries && after.nat_guard_fails == before.nat_guard_fails);
        UM_CHECK(after.tier1_retired - before.tier1_retired >= (fragment == 3 ? 9u : 250u));
        if (fragment < 3)
            UM_CHECK(ia64_nat_read(&v->s, fragment == 1 ? 46 : 40));
        if (fragment == 1 || fragment == 2)
            UM_CHECK(v->s.ar[AR_UNAT] & 1);
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
}

/* A loop of a PR_ALL helper (mov pr = r, mask, with r changing in each
 * iteration) at four frame bases and a rotated predicate base: the helper
 * writes back the set its region's tables hold (frame-relative), the run equals the reference, and the in-region helper count of
 * its opcode grows by one per iteration. */
static void pr_loop_cases(const char *image)
{
    for (unsigned b = 0; b < 4; ++b) {
        struct vcpu *v = um_vcpu(UM_CODE + 0x5200), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        v->s.cfm = um_cfm(64, 64, 8, 5, 3);
        v->s.rse_bof = (unsigned[]){0, 44, 90, 95}[b];
        um_gr(v, 40, UINT64_C(0x123456789abcdef0) + b, false);
        um_gr(v, 42, 0, false);
        v->s.ar[AR_LC] = 49;
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "COV PR loop");
        UM_CHECK(id && t1_region_get(id)->nhelper == 1 && t1_region_get(id)->loop &&
                 t1_region_get(id)->ninsns == 6);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX};
        uint64_t calls = t1_helper_calls(IA64_OP_MOV_GRPR);
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 600, id, &arm, &out, false,
                          "COV PR loop") == 0);
        UM_CHECK(out.run.entries >= 1);
        UM_CHECK(t1_helper_calls(IA64_OP_MOV_GRPR) - calls == 50);
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
}

/* Speculative, advanced and check loads (SDM Vol.3 ld; ia64/load_store.c
 * models an ALAT without entries: ld.c always reloads, ld.a reads as ld
 * does). .Lspec_loads runs 50 iterations of ld8.a, ld1.s with
 * post-increment, ld8.c.clr, ld4.sa, ld2.c.nc and ld8.s, each run equal to
 * the reference:
 *   WB       PSR.dt = 1, a WB page: every load is a native window load,
 *            the region holds no helper and no in-region helper runs;
 *   page end the ld1.s walks onto a page without a translation (DCR.dm,
 *            code page ed): it exits at its site and the lower tier defers
 *            it, r41 ends NaT;
 *   UC       the page is UC: every load exits at its site; the lower tier
 *            returns zero for ld8.a, defers ld.s and ld.sa (NaT) and reads
 *            for ld.c;
 *   physical PSR.dt = 0: ld.s and ld.sa always defer in the emulator and
 *            stay in-region helpers (one call per iteration each). */
static void spec_load_cases(const char *image)
{
    static const Ia64Opcode ops[] = {IA64_OP_LD8A,  IA64_OP_LD1S,    IA64_OP_LD8C_CLR,
                                     IA64_OP_LD4SA, IA64_OP_LD2C_NC, IA64_OP_LD8S};
    const uint64_t x = (UINT64_C(2) << 61) | 0x12340000, value = UINT64_C(0x8877665544332211);
    for (unsigned mode = 0; mode < 4; ++mode) {
        static const char *const label[] = {"COV spec loads WB", "COV spec loads page end",
                                            "COV spec loads UC", "COV spec loads physical"};
        struct vcpu *v = um_vcpu(UM_CODE + 0x5400), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        v->s.cfm = um_cfm(64, 64, 0, 0, 0);
        v->s.ar[AR_LC] = 49;
        for (unsigned k = 0; k < 512; ++k) {
            uint64_t word = value + k;
            um_load(UM_DATA + 0x1000 + 8 * k, &word, 8);
        }
        if (mode < 3) {
            um_virtual_code(v);
            v->s.tr[0][0].pte |= UINT64_C(1) << 52;
            ia64_mmu_changed(v, IA64_MMU_TR, 0, 0);
            v->s.cr[CR_DCR] = UINT64_C(1) << 8;
            v->s.psr |= IA64_PSR_DT;
            uint64_t pte =
                (UM_DATA + 0x1000) | 1 | 0x20 | 0x40 | (2u << 9) | (mode == 2 ? 4u << 2 : 0);
            ia64_tc_insert(v, x, 1, pte, 12u << 2);
            um_gr(v, 2, x + 0x40, false);
            um_gr(v, 3, x + (mode == 1 ? 0x1000 - 20 : 0x100), false);
        } else {
            um_gr(v, 2, UM_DATA + 0x1040, false);
            um_gr(v, 3, UM_DATA + 0x1100, false);
        }
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, label[mode]);
        UM_CHECK(id && t1_region_get(id)->loop);
        UM_CHECK(t1_region_get(id)->nhelper == (mode == 3 ? 3u : 0u));
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        uint64_t calls[6];
        for (unsigned k = 0; k < 6; ++k)
            calls[k] = t1_helper_calls(ops[k]);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX};
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 450, id, &arm, &out, false,
                          label[mode]) == 0);
        UM_CHECK(out.run.entries >= 1);
        for (unsigned k = 0; k < 6; ++k) {
            bool helper = mode == 3 && (ops[k] == IA64_OP_LD1S || ops[k] == IA64_OP_LD4SA ||
                                        ops[k] == IA64_OP_LD8S);
            UM_CHECK(t1_helper_calls(ops[k]) - calls[k] == (helper ? 50u : 0u));
        }
        uint64_t first = value + 8; /* the word at x + 0x40 */
        UM_CHECK(ia64_gr_read(&v->s, 40) == (mode == 2 ? 0 : first) && !ia64_nat_read(&v->s, 40));
        UM_CHECK(ia64_gr_read(&v->s, 42) == first && !ia64_nat_read(&v->s, 42));
        UM_CHECK(ia64_nat_read(&v->s, 41) == (mode != 0));
        UM_CHECK(ia64_nat_read(&v->s, 43) == (mode >= 2) &&
                 ia64_nat_read(&v->s, 45) == (mode >= 2));
        if (mode == 0)
            UM_CHECK(ia64_gr_read(&v->s, 45) == first &&
                     ia64_gr_read(&v->s, 43) == (uint32_t)first);
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
}

/* Parallel compares and tbit (SDM Vol.3 cmp, tbit; ia64/integer.c
 * predicate_result): native, .and/.or/.or.andcm reading the old targets,
 * .unc clearing both under a false qp, a false qp otherwise keeping them, a
 * NaT source clearing a normal pair; tnat stays the in-region helper.
 * .Lpcmp runs 30 iterations at two predicate rotations: the region's only
 * helper is the tnat, the only in-region helper call (once per iteration),
 * and the run equals the reference. A NaT compare source stays exact for
 * the tnat helper, so the native compares handle its predicate results
 * without an entry-guard decline (SDM Vol.1 Table 4-10). */
static uint64_t helper_calls_all(void)
{
    uint64_t n = 0;
    for (unsigned op = 0; op < IA64_OP_COUNT; ++op)
        n += t1_helper_calls(op);
    return n;
}

static void pcmp_cases(const char *image)
{
    for (unsigned c = 0; c < 3; ++c) {
        static const char *const label[] = {
            "COV parallel compares", "COV parallel compares rotated", "COV parallel compares NaT"};
        struct vcpu *v = um_vcpu(UM_CODE + 0x5600), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        v->s.cfm = c == 1 ? um_cfm(64, 64, 8, 5, 3) : um_cfm(64, 64, 0, 0, 0);
        v->s.ar[AR_LC] = 29;
        um_gr(v, 40, 0x1234, false);
        um_gr(v, 41, 0x1230, false);
        um_gr(v, 42, 0x20, c == 2);
        um_gr(v, 43, UINT64_C(0x8000000000000001), false);
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, label[c]);
        UM_CHECK(id && t1_region_get(id)->loop && t1_region_get(id)->nhelper == 1);
        UM_CHECK(t1_region_get(id)->helper[0].insn.opcode ==
                 IA64_OP_TNAT_Z); /* tnat.nz: swapped targets */
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        uint64_t calls = helper_calls_all(), tnat = t1_helper_calls(IA64_OP_TNAT_Z);
        struct t1_stats before, after;
        t1_get_stats(&before);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX};
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 810, id, &arm, &out, false, label[c]) ==
                 0);
        t1_get_stats(&after);
        UM_CHECK(helper_calls_all() - calls == 30);
        UM_CHECK(t1_helper_calls(IA64_OP_TNAT_Z) - tnat == 30);
        UM_CHECK(out.run.entries >= 1 && after.tier1_retired - before.tier1_retired == 810);
        UM_CHECK(after.nat_guard_fails == before.nat_guard_fails);
        if (c == 2)
            UM_CHECK(ia64_nat_read(&v->s, 42) && ia64_pr_read(&v->s, 22) &&
                     !ia64_pr_read(&v->s, 23));
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
}

static void boundary_policy_cases(const char *image)
{
    struct vcpu *v = um_vcpu(UM_CODE + 0x1400);
    um_load_file(UM_CODE, image);
    v->s.cfm = um_cfm(64, 64, 0, 0, 0);
    struct t1_form form;
    const char *why;
    UM_CHECK(t1_form(v, T1_COMPILE_UNDO_LOG, T1_MAX_INSNS, &form, &why) == T1_COMPILED);
    UM_CHECK(form.end_ip == UM_CODE + 0x1410 && form.end_slot == 0 &&
             !strcmp(form.end_reason, "checked memory helper boundary"));
    v->s.ip = UM_CODE + 0x1a00;
    v->s.psr |= UINT64_C(3) << 32;
    UM_CHECK(t1_form(v, 0, T1_MAX_INSNS, &form, &why) == T1_REJECTED);
    UM_CHECK(!strcmp(why, "privileged AR write"));
    struct ia64_bundle privileged;
    uint64_t low, high;
    memcpy(&low, um_ram(v->s.ip), 8);
    memcpy(&high, um_ram(v->s.ip + 8), 8);
    ia64_decode_bundle(low, high, v->s.ip, &privileged);
    struct ia64_result result = ia64_execute(v, &privileged.insn[0]);
    UM_CHECK(result.kind == IA64_STEP_FAULT && result.fault == IA64_FAULT_PRIVILEGED_REG);
    v->s.psr &= ~(UINT64_C(3) << 32);
    /* M-bank ar.lc is an illegal form even when its qp is false. */
    struct ienc_bundle lc = {IENC_STOP(IENC_MIB),
                             {(UINT64_C(1) << 37) | (UINT64_C(0x2a) << 27) |
                                  ((uint64_t)AR_LC << 20) | (UINT64_C(40) << 13) | 6,
                              ienc_nop_i(), ienc_nop_b()}};
    um_emit(UM_CODE + 0x1a00, &lc, 1);
    UM_CHECK(t1_form(v, 0, T1_MAX_INSNS, &form, &why) == T1_REJECTED);
    UM_CHECK(!strcmp(why, "ar.lc in the M bank"));
    um_load_file(UM_CODE, image);
    v->s.ip = UM_CODE + 0x1c00;
    um_gr(v, 40, UINT64_C(1) << 63, false);
    v->s.ar[AR_LC] = 99;
    struct t1_compile_report rep;
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "COV reserved FPSR");
    UM_CHECK(id && t1_region_get(id)->nhelper == 1);
    result = ia64_execute(v, &t1_region_get(id)->helper[0].insn);
    UM_CHECK(result.kind == IA64_STEP_FAULT && result.fault == IA64_FAULT_RESERVED);
    struct t1_region *r = (struct t1_region *)t1_region_get(id);
    const struct t1_point *helper = NULL;
    for (unsigned p = 0; p < r->npoints; ++p)
        if (r->point[p].kind == T1_PT_HELPER)
            helper = &r->point[p];
    UM_CHECK(helper);
    struct vcpu *ref = um_vcpu(UM_CODE);
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    struct umt_run out;
    struct umt_arm arm = {.point = UINT32_MAX};
    UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 100, id, &arm, &out, false,
                      "COV reserved FPSR") == 0);
    r->policy = true;
    static struct t1_windows windows;
    for (unsigned n = 0; n < 16; ++n)
        UM_CHECK(!t1_demote_after_exit(&windows, r, helper, 0));
    UM_CHECK(t1_demote_after_exit(&windows, r, helper, 0) && !r->valid);
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(ref);
    t1_holds_clear();

    vcpu_destroy(v);
    v = um_vcpu(UM_CODE + 0x1e00);
    v->s.cfm = um_cfm(64, 64, 0, 0, 0);
    v->s.ar[AR_LC] = 99;
    um_gr(v, 2, UM_DATA, false);
    um_gr(v, 40, 123, false);
    id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "COV native store");
    UM_CHECK(id && t1_region_get(id)->nhelper == 0);
    r = (struct t1_region *)t1_region_get(id);
    r->policy = true;
    const struct t1_point *store = NULL;
    for (unsigned p = 0; p < r->npoints; ++p)
        if (r->point[p].kind == T1_PT_SITE && r->point[p].access_write)
            store = &r->point[p];
    UM_CHECK(store);
    for (unsigned n = 0; n < 16; ++n)
        UM_CHECK(!t1_demote_after_exit(&windows, r, store, 0));
    UM_CHECK(t1_demote_after_exit(&windows, r, store, 0));
    struct t1_compile_request rq;
    v->upper = umt_tv(v);
    UM_CHECK(t1_upper_hot(v, UM_CODE + 0x1e00, true) == T1_HOT_REJECTED);
    UM_CHECK(t1_upper_hot(v, UM_CODE + 0x1e00, true) == T1_HOT_LINKED);
    uint64_t link = t1_upper_probe(v, UM_CODE + 0x1e00);
    UM_CHECK(link && t1_region_from_link(link)->nhelper == 1);
    UM_CHECK(t1_request_make(umt_tv(v), UM_CODE + 0x1e00, true, &rq));
    rq.slow_memory = t1_head_memory_helpers(UM_CODE + 0x1e00, v->s.ip, v->s.slot);
    UM_CHECK(rq.slow_memory);
    struct t1_compile_ctx *ctx = t1_compile_ctx_create();
    UM_CHECK(ctx);
    UM_CHECK(t1_compile_request(ctx, &rq, &rep) == T1_COMPILED);
    id = rep.region_id;
    UM_CHECK(t1_region_get(id)->nhelper == 1);
    um_snapshot_take(&snap, v);
    UM_CHECK(umt_case(v, ref = um_vcpu(UM_CODE), &snap, T1_COMPILE_VERIFY, 100, id, &arm, &out,
                      false, "COV promoted store") == 0);
    UM_CHECK(out.run.entries);
    rq.opt.flags |= T1_COMPILE_UNDO_LOG;
    struct ia64_state names;
    UM_CHECK(t1_form_request(&rq, &names, T1_MAX_INSNS, &form, &why) == T1_REJECTED);
    UM_CHECK(!strcmp(why, "checked memory helper boundary"));
    t1_compile_ctx_destroy(ctx);
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(ref);
    vcpu_destroy(v);
    t1_holds_clear();
}

static void stitch_page_cases(const char *image)
{
    for (unsigned change = 0; change < 2; ++change) {
        struct vcpu *v = um_vcpu(UM_CODE + 0x2000), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        v->s.cfm = um_cfm(64, 64, 0, 0, 0);
        v->s.ar[AR_LC] = 99;
        um_gr(v, 40, 0, false);
        um_virtual_code(v);
        /* Three request pages, but formation skips the middle page. */
        for (unsigned p = 0; p < 3; ++p) {
            uint64_t va = UM_CODE + 0x2000 + 0x1000 * p;
            v->s.tr[0][p] = (struct ia64_tlb_entry){.va = va,
                                                    .pte = va | 1 | 0x20 | 0x40 | (1u << 9),
                                                    .itir = 12u << 2,
                                                    .rid = 0x100,
                                                    .page_shift = 12,
                                                    .valid = true};
        }
        ia64_mmu_changed(v, IA64_MMU_TR, 0, 0);
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "COV later-page stitch");
        UM_CHECK(id && t1_region_get(id)->loop);
        const struct t1_region *r = t1_region_get(id);
        UM_CHECK(r->npages == 2 && r->page[1].pa == UM_CODE + 0x4000);
        UM_CHECK(r->dep[1].mask[0] == 1 && !r->dep[1].mask[1] && !r->dep[1].mask[2] &&
                 !r->dep[1].mask[3]);
        if (change) {
            /* DMA to a covered bundle: validation must reject stale code. */
            uint64_t low, high;
            /* Keep the branch from the old bundle; change only its M op. */
            struct ienc_bundle patch = {
                IENC_STOP(IENC_MIB),
                {ienc_adds(0, 40, 7, 40), ienc_nop_i(), ienc_br_cloop(-0x2000)}};
            ienc_pack(&patch, &low, &high);
            memcpy(um_ram(UM_CODE + 0x4000), &low, 8);
            memcpy(um_ram(UM_CODE + 0x4008), &high, 8);
        } else {
            /* Remap only the stitched source. No bytes of the entry page change. */
            struct ienc_bundle patch = {
                IENC_STOP(IENC_MIB),
                {ienc_adds(0, 40, 7, 40), ienc_nop_i(), ienc_br_cloop(-0x2000)}};
            um_emit(UM_CODE + 0x6000, &patch, 1);
            v->s.tr[0][2].pte = (UM_CODE + 0x6000) | 1 | 0x20 | 0x40 | (1u << 9);
            ia64_mmu_changed(v, IA64_MMU_TR, 0, 2);
        }
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX};
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 180, id, &arm, &out, false,
                          "COV later-page change") == 0);
        UM_CHECK(ia64_gr_read(&v->s, 40) >= 140);
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(ref);
        vcpu_destroy(v);
    }
}

void coverage_tests(const char *image)
{
    umt_init();
    format_audit();
    unsigned table_runs = table_fuzz();
    memory_cases(image);
    exact_nat_cases(image);
    pr_loop_cases(image);
    spec_load_cases(image);
    pcmp_cases(image);
    boundary_policy_cases(image);
    stitch_page_cases(image);
    device_cases(image);
    unsigned runs = 0;
    const unsigned offsets[] = {0, 0x200, 0x400, 0x600, 0xe00};
    for (unsigned fragment = 0; fragment < sizeof(offsets) / sizeof(offsets[0]); ++fragment) {
        struct vcpu *v = um_vcpu(UM_CODE + offsets[fragment]), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        v->s.cfm = um_cfm(64, 64, 8, 5, 3);
        v->s.rse_bof = 95;
        um_gr(v, 2, UM_DATA, false);
        v->s.ar[AR_LC] = 99;
        um_gr(v, 42, 10, false);
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "COV");
        UM_CHECK(id && t1_region_get(id)->loop);
        if (fragment == 0)
            UM_CHECK(t1_region_get(id)->nhelper >= 4);
        if (fragment == 1)
            UM_CHECK(t1_region_get(id)->nhelper == 0);
        if (fragment == 2)
            UM_CHECK(t1_region_get(id)->nhelper == 0);
        struct ia64_state entry_state = v->s;
        for (unsigned sample = 0; sample < 32; ++sample) {
            v->s = entry_state;
            v->s.ip = UM_CODE + offsets[fragment];
            v->s.slot = 0;
            v->retired = 0;
            v->s.rse_bof = random64() % 96;
            v->s.psr = (v->s.psr & ~T1_PSR_MF_BITS) | (random64() & T1_PSR_MF_BITS);
            for (unsigned reg = 32; reg < 96; ++reg)
                um_gr(v, reg, random64(), false);
            if ((fragment == 1 || fragment == 2) && (sample & 8))
                um_gr(v, 41, random64(), true);
            um_gr(v, 2, UM_DATA, false);
            if (fragment == 2)
                um_gr(v, 42, 10, false);
            v->s.ar[AR_LC] = 99;
            um_pr(v, 6, sample & 1);
            um_pr(v, 7, sample & 2);
            for (unsigned f = 32; f <= 35; ++f) {
                unsigned p = ia64_fr_index(&v->s, f);
                v->s.fr[p] = sample & 4 ? ia64_fp_nat()
                                        : (struct ia64_fr){.significand = random64(),
                                                           .exponent = IA64_FP_INTEGER_EXP};
            }
            struct um_snapshot snap;
            um_snapshot_take(&snap, v);
            struct umt_run out;
            struct umt_arm arm = {.point = UINT32_MAX};
            bool allowed = t1_nat_guard_holds(t1_region_get(id), v);
            UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 180, id, &arm, &out, false,
                              "COV random") == 0);
            if (allowed)
                UM_CHECK(out.run.entries > 0);
            ++runs;
            if (!sample) {
                const struct t1_region *r = t1_region_get(id);
                for (unsigned p = 0; p < r->npoints; ++p)
                    if (r->point[p].kind == T1_PT_HELPER) {
                        arm.point = p;
                        arm.occurrence = 1;
                        arm.action = T1_FORCE_HELPER_EXIT;
                        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 180, id, &arm, &out,
                                          false, "COV HELPER_EXIT") == 0);
                        ++runs;
                    }
            }
            um_snapshot_free(&snap);
        }
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    printf("coverage: %u helper-table cases, %u random and forced-restoration runs passed\n",
           table_runs, runs);
}

/* Each FPI opcode runs through decoding, selection and every guest-state
 * comparison. Inputs include arbitrary exponents/signs and all NaTVal
 * positions, both banks, false qp, rotating FRs and wrapping GR homes. */
void fpi_tests(const char *image)
{
    umt_init();
    unsigned runs = 0;
    for (unsigned op = 0; op < 17; ++op)
        for (unsigned bank = 0; bank < 2; ++bank) {
            struct vcpu *v = um_vcpu(UM_CODE + (op * 2 + bank) * 256), *ref = um_vcpu(UM_CODE);
            um_load_file(UM_CODE, image);
            v->s.cfm = um_cfm(64, 64, 8, 5, 3);
            v->s.rse_bof = 95;
            v->s.ar[AR_LC] = 3;
            struct t1_compile_report rep;
            uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "FPI");
            UM_CHECK(id && t1_region_get(id)->loop && t1_region_get(id)->nhelper == 0);
            struct ia64_state entry = v->s;
            const struct t1_region *region = t1_region_get(id);
            /* Physical guard indices include CFM rotation. Reject precisely
         * NaTVal, not a nonzero significand or a different sign. */
            for (unsigned f = 2; f < 128; ++f)
                if ((region->fr_guard[f / 64] >> (f % 64)) & 1) {
                    struct ia64_fr old = v->s.fr[f];
                    v->s.fr[f] = ia64_fp_nat();
                    UM_CHECK(!t1_nat_guard_holds(region, v));
                    v->s.fr[f].sign = true;
                    UM_CHECK(t1_nat_guard_holds(region, v));
                    v->s.fr[f].sign = false;
                    v->s.fr[f].significand = 1;
                    UM_CHECK(t1_nat_guard_holds(region, v));
                    v->s.fr[f] = old;
                }
            for (unsigned sample = 0; sample < 64; ++sample) {
                v->s = entry;
                v->retired = 0;
                v->s.rse_bof = (unsigned[]){0, 44, 90, 95}[sample % 4];
                v->s.psr = (v->s.psr & ~T1_PSR_MF_BITS) | (random64() & T1_PSR_MF_BITS);
                v->s.ar[AR_FPSR] = random64();
                um_pr(v, 6, sample & 1);
                um_gr(v, 41, random64(), sample & 2);
                um_gr(v, 40, random64(), sample & 4);
                unsigned d = bank ? 40 : 10;
                for (unsigned f = d; f <= d + 3; ++f) {
                    unsigned phys = ia64_fr_index(&v->s, f);
                    v->s.fr[phys] = sample & (1u << (f - d + 2))
                                        ? ia64_fp_nat()
                                        : (struct ia64_fr){random64(), random64() & 0x1ffff,
                                                           (bool)(random64() & 1)};
                }
                struct um_snapshot snap;
                um_snapshot_take(&snap, v);
                struct umt_run out;
                struct umt_arm arm = {.point = UINT32_MAX};
                bool allowed = t1_nat_guard_holds(t1_region_get(id), v);
                UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 40, id, &arm, &out, false,
                                  "FPI opcode") == 0);
                /* Guards use entry state; lower-tier NaTVal propagation stays exact. */
                if (allowed)
                    UM_CHECK(out.run.entries > 0);
                ++runs;
                um_snapshot_free(&snap);
            }
            /* Disabling the opposite bank must retain the whole native loop. */
            for (unsigned qp = 0; qp < 2; ++qp) {
                v->s = entry;
                v->retired = 0;
                v->s.psr |= bank ? IA64_PSR_DFL : IA64_PSR_DFH;
                um_pr(v, 6, qp);
                uint32_t enabled =
                    umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "FPI opposite bank");
                UM_CHECK(enabled && t1_region_get(enabled)->loop &&
                         !t1_region_get(enabled)->nhelper);
                struct um_snapshot snap;
                um_snapshot_take(&snap, v);
                struct umt_run out;
                struct umt_arm arm = {.point = UINT32_MAX};
                UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 40, enabled, &arm, &out, false,
                                  "FPI opposite bank") == 0);
                UM_CHECK(out.run.entries);
                ++runs;
                um_snapshot_free(&snap);
            }
            /* PSR.dfl/dfh are contract fields: the native
         * loop compiled with its bank enabled is not entered once the bank
         * is disabled, where the reference raises Disabled FP Register. */
            for (unsigned qp = 0; qp < 2; ++qp) {
                v->s = entry;
                v->retired = 0;
                um_pr(v, 6, qp);
                uint32_t native = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "FPI bank enabled");
                UM_CHECK(native && t1_region_get(native)->loop && !t1_region_get(native)->nhelper);
                v->s.psr |= bank ? IA64_PSR_DFH : IA64_PSR_DFL;
                struct um_snapshot snap;
                um_snapshot_take(&snap, v);
                struct umt_run out;
                struct umt_arm arm = {.point = UINT32_MAX};
                UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 40, native, &arm, &out, false,
                                  "FPI bank disabled after compile") == 0);
                ++runs;
                um_snapshot_free(&snap);
            }
            /* A disabled-bank instruction must never be selected natively.
         * Its exact lower-tier fault/false-qp behavior is compared here. */
            for (unsigned qp = 0; qp < 2; ++qp) {
                v->s = entry;
                v->retired = 0;
                v->s.psr |= bank ? IA64_PSR_DFH : IA64_PSR_DFL;
                um_pr(v, 6, qp);
                struct t1_compile_options options = {T1_COMPILE_VERIFY, 0, 0};
                enum t1_compile_status st = t1_compile(umt_tv(v), &options, &rep);
                UM_CHECK(st == T1_REJECTED || (st == T1_COMPILED && rep.insns == 1 &&
                                               !strcmp(rep.end_reason, "disabled FR bank")));
                uint32_t prefix = st == T1_COMPILED ? rep.region_id : 0;
                struct um_snapshot snap;
                um_snapshot_take(&snap, v);
                struct umt_run out;
                struct umt_arm arm = {.point = UINT32_MAX};
                UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 40, prefix, &arm, &out, false,
                                  "FPI disabled bank") == 0);
                ++runs;
                um_snapshot_free(&snap);
            }
            umt_release(v);
            vcpu_destroy(ref);
            vcpu_destroy(v);
        }
    for (unsigned bank = 0; bank < 2; ++bank) {
        struct vcpu *v = um_vcpu(UM_CODE + 0x2400 + bank * 256), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        v->s.cfm = um_cfm(64, 64, 8, 5, 3);
        um_gr(v, 41, UINT64_MAX, false);
        um_gr(v, 2, UM_DATA + 1, false);
        v->s.psr &= ~T1_PSR_MF_BITS;
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "FPI dirty fault");
        UM_CHECK(id && t1_region_get(id)->nsites == 1 && t1_region_get(id)->nhelper == 0);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX};
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 6, id, &arm, &out, false,
                          "FPI dirty fault") == 0);
        UM_CHECK(v->s.psr & (bank ? IA64_PSR_MFH : IA64_PSR_MFL));
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(ref);
        vcpu_destroy(v);
        ++runs;
    }
    for (unsigned bank = 0; bank < 2; ++bank) {
        struct vcpu *v = um_vcpu(UM_CODE + 0x2600 + bank * 256), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        v->s.cfm = um_cfm(64, 64, 8, 5, 3);
        um_gr(v, 41, UINT64_MAX, false);
        um_gr(v, 2, UM_DATA, false);
        v->s.psr &= ~T1_PSR_MF_BITS;
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "FPI/helper dirty bits");
        UM_CHECK(id && t1_region_get(id)->nhelper == 1);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX};
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 6, id, &arm, &out, false,
                          "FPI/helper dirty bits") == 0);
        UM_CHECK(out.run.entries && (v->s.psr & T1_PSR_MF_BITS) == T1_PSR_MF_BITS);
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(ref);
        vcpu_destroy(v);
        ++runs;
    }
    for (unsigned bank = 0; bank < 2; ++bank) {
        struct vcpu *v = um_vcpu(UM_CODE + 0x2800 + bank * 256), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        v->s.cfm = um_cfm(64, 64, 8, 5, 3);
        um_gr(v, 2, UM_DATA, false);
        uint64_t spill[2] = {0, IA64_FP_NAT_EXP};
        memcpy(um_ram(UM_DATA), spill, sizeof(spill));
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "FPI native NaTVal");
        UM_CHECK(id && t1_region_get(id)->nhelper == 1);
        unsigned source = ia64_fr_index(&v->s, bank ? 41 : 11);
        UM_CHECK(!((t1_region_get(id)->fr_guard[source / 64] >> (source % 64)) & 1));
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX};
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 9, id, &arm, &out, false,
                          "FPI native NaTVal") == 0);
        UM_CHECK(out.run.entries && ia64_nat_read(&v->s, 40));
        UM_CHECK(ia64_fr_is_nat(ia64_fr_read(&v->s, bank ? 40 : 10)));
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(ref);
        vcpu_destroy(v);
        ++runs;
    }
    for (unsigned op = 0; op < 5; ++op)
        for (unsigned high_increment = 0; high_increment < 2; ++high_increment) {
            struct vcpu *v = um_vcpu(UM_CODE + 0x2a00 + op * 256), *ref = um_vcpu(UM_CODE);
            um_load_file(UM_CODE, image);
            um_gr(v, 8, UINT64_C(0x123456789abcdef0), false);
            um_gr(v, 3, UM_DATA, false);
            um_gr(v, 9, 16, false);
            v->s.fr[9] = (struct ia64_fr){123, IA64_FP_INTEGER_EXP, false};
            if (high_increment) {
                /* r40 is an increment GR, never an operand in the disabled FR bank. */
                uint64_t lo;
                memcpy(&lo, um_ram(v->s.ip + 16), 8);
                lo = (lo & ~(UINT64_C(127) << 18)) | (UINT64_C(40) << 18);
                um_load(v->s.ip + 16, &lo, 8);
                v->s.cfm = 64;
                um_gr(v, 40, 16, false);
                v->s.psr |= IA64_PSR_DFH;
            }
            struct t1_compile_report rep;
            uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "FPI postincrement");
            UM_CHECK(id && rep.insns == 9);
            UM_CHECK(op < 3 ? t1_region_get(id)->nsites == 1 && !t1_region_get(id)->nhelper
                            : t1_region_get(id)->nhelper == 1);
            struct um_snapshot snap;
            um_snapshot_take(&snap, v);
            struct umt_run out;
            struct umt_arm arm = {.point = UINT32_MAX};
            UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 9, id, &arm, &out, false,
                              "FPI postincrement") == 0);
            UM_CHECK(out.run.entries && ia64_gr_read(&v->s, 10) == UINT64_C(0x123456789abcdef0));
            UM_CHECK(v->s.fr[9].significand == UINT64_C(0x123456789abcdef0) &&
                     ia64_gr_read(&v->s, 3) == UM_DATA + 16);
            um_snapshot_free(&snap);
            umt_release(v);
            vcpu_destroy(ref);
            vcpu_destroy(v);
            ++runs;
        }
    {
        struct vcpu *v = um_vcpu(UM_CODE + 0x2f00), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        um_gr(v, 3, UM_DATA, true);
        uint64_t lo, hi;
        memcpy(&lo, um_ram(v->s.ip), 8);
        memcpy(&hi, um_ram(v->s.ip + 8), 8);
        struct ia64_bundle bundle;
        ia64_decode_bundle(lo, hi, v->s.ip, &bundle);
        UM_CHECK(bundle.insn[0].fp_load_speculative &&
                 ia64_execute(v, &bundle.insn[0]).kind == IA64_STEP_COMPLETE);
        UM_CHECK(ia64_fr_is_nat(v->s.fr[11]));
        v->s.ip += 16;
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "FPI entry NaTVal");
        UM_CHECK(id && !t1_region_get(id)->fr_guard[0] && !t1_region_get(id)->fr_guard[1]);
        UM_CHECK(t1_nat_guard_holds(t1_region_get(id), v));
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct t1_stats before, after;
        t1_get_stats(&before);
        for (unsigned n = 0; n < 40; ++n) {
            struct umt_run out;
            struct umt_arm arm = {.point = UINT32_MAX};
            UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 6, id, &arm, &out, false,
                              "FPI entry NaTVal") == 0);
            UM_CHECK(out.run.entries && ia64_nat_read(&v->s, 10));
            ++runs;
        }
        t1_get_stats(&after);
        UM_CHECK(after.nat_guard_fails == before.nat_guard_fails &&
                 after.demote_nat == before.demote_nat);
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(ref);
        vcpu_destroy(v);
    }
    {
        struct vcpu *v = um_vcpu(UM_CODE + 0x3000), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        v->s.cfm = 64;
        v->s.psr |= IA64_PSR_DFL;
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "FPI f0 with DFL");
        UM_CHECK(id && rep.insns == 6 && !t1_region_get(id)->nhelper);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX};
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 6, id, &arm, &out, false,
                          "FPI f0 with DFL") == 0);
        UM_CHECK(out.run.entries);
        ++runs;
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(ref);
        vcpu_destroy(v);
    }
    {
        /* SDM Vol.3 pp.143,242,276: all sources are non-NaT, so
         * xma.l computes (3 * f3 + 5) mod 2^64 and getf clears NaT. */
        struct vcpu *v = um_vcpu(UM_CODE + 0x3100), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        um_gr(v, 8, 3, false);
        um_gr(v, 9, 0, true);
        v->s.fr[3] = (struct ia64_fr){7, IA64_FP_INTEGER_EXP, false};
        v->s.fr[4] = (struct ia64_fr){5, IA64_FP_INTEGER_EXP, false};
        v->s.ar[AR_LC] = 3;
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "FPI hash chain");
        UM_CHECK(id && t1_region_get(id)->loop && !t1_region_get(id)->nhelper);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX};
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 36, id, &arm, &out, false,
                          "FPI hash chain") == 0);
        UM_CHECK(out.run.entries && ia64_gr_read(&v->s, 9) == 767 && !ia64_nat_read(&v->s, 9));
        UM_CHECK(v->s.fr[3].significand == 767 && v->s.fr[3].exponent == IA64_FP_INTEGER_EXP &&
                 !v->s.fr[3].sign);
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(ref);
        vcpu_destroy(v);
        ++runs;
    }
    {
        /* SDM Vol.3 setf/getf/xma: low-64-bit products plus the old
         * significands, with clear NaT. Large significands and eleven
         * live integer results exercise allocation pressure. Reproducing
         * the old bug depends on split choices; the prover's fresh upper
         * bits cover the class of pure byte definitions. */
        struct vcpu *v = um_vcpu(UM_CODE + 0x3200), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        v->s.fr[2] = (struct ia64_fr){UINT64_C(0x123456789abcdef3), IA64_FP_INTEGER_EXP, false};
        for (unsigned k = 0; k < 4; ++k) {
            um_gr(v, 8 + k, 7 + k, false);
            v->s.fr[3 + k] =
                (struct ia64_fr){UINT64_C(0x9876543210abcde5) + k, IA64_FP_INTEGER_EXP, false};
        }
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "FPI hash trace");
        UM_CHECK(id && !t1_region_get(id)->nhelper);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX};
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 54, id, &arm, &out, false,
                          "FPI hash trace") == 0);
        UM_CHECK(out.run.entries);
        for (unsigned k = 0; k < 4; ++k) {
            uint64_t expected =
                (7 + k) * UINT64_C(0x123456789abcdef3) + UINT64_C(0x9876543210abcde5) + k;
            UM_CHECK(ia64_gr_read(&v->s, 8 + k) == expected && !ia64_nat_read(&v->s, 8 + k));
            UM_CHECK(v->s.fr[3 + k].significand == expected);
            UM_CHECK(v->s.fr[3 + k].exponent == IA64_FP_INTEGER_EXP && !v->s.fr[3 + k].sign);
        }
        umt_release(v);
        for (unsigned e = 0; e <= t1h_engine_count(); ++e) {
            const struct t1h_engine *engine =
                e == t1h_engine_count() ? t1h_engine_checked() : t1h_engine_at(e);
            um_snapshot_restore(&snap, v);
            struct t1h_engine_vcpu ev;
            t1h_engine_bind(&ev, engine, v);
            /* Compile explicitly so this short trace exercises the upper
             * tier even before the product's hotness threshold. */
            if (e >= 2) {
                struct t1_compile_options opt = {.flags = T1_COMPILE_VERIFY};
                UM_CHECK(t1_compile(t1h_tier1_vcpu(ev.ctx), &opt, &rep) == T1_COMPILED);
            }
            struct vcpu_exit exit = t1h_engine_run(&ev, 54);
            struct um_run run = {0};
            UM_CHECK(um_compare(&snap, ref, &run, v, &exit, 54, engine->name) == 0);
            for (unsigned k = 0; k < 4; ++k) {
                uint64_t expected =
                    (7 + k) * UINT64_C(0x123456789abcdef3) + UINT64_C(0x9876543210abcde5) + k;
                UM_CHECK(ia64_gr_read(&v->s, 8 + k) == expected && !ia64_nat_read(&v->s, 8 + k));
            }
            t1h_engine_release(&ev);
            ++runs;
        }
        um_snapshot_free(&snap);
        vcpu_destroy(ref);
        vcpu_destroy(v);
        ++runs;
    }
    printf("fpi: %u decoded native differential/restoration cases passed\n", runs);
}

/* Serialize the unoptimized single-instruction IR and exit frame. The
 * conformance driver evaluates this graph with the proved op models;
 * no hand-written IA-64 expression stands in for lift.c. */
void fpi_ir_dump(const char *image)
{
    um_machine_init();
    struct t1_form *f = calloc(1, sizeof(*f));
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    UM_CHECK(f && ir);
    um_load_file(UM_CODE, image);
    printf("[");
    for (unsigned k = 0; k < 17; ++k) {
        struct vcpu *v = um_vcpu(UM_CODE + k * 512);
        v->s.cfm = 64;
        v->s.slot = k < 4 ? 0 : 1;
        const char *why;
        unsigned fail;
        UM_CHECK(t1_form(v, 0, 1, f, &why) == T1_COMPILED && f->ninsns == 1);
        UM_CHECK(t1_lift(f, ir, &fail));
        printf("%s{\"ops\":[", k ? "," : "");
        for (unsigned o = 0; o < ir->nops; ++o) {
            const struct t1_op *op = &ir->op[o];
            printf("%s[\"%s\",%lld,[", o ? "," : "", t1_op_desc(op->code)->name,
                   (long long)op->imm);
            for (unsigned u = 0; u < op->nuse; ++u)
                printf("%s%u", u ? "," : "", t1_ir_resolve(ir, ir->use[op->use_first + u].value));
            printf("]]");
        }
        const struct t1_ir_point *point = &ir->point[ir->npoints - 1];
        printf("],\"frame\":{");
        for (unsigned u = 0; u < point->frame_count; ++u) {
            const struct t1_use *use = &ir->use[point->frame_first + u];
            printf("%s\"%u\":%u", u ? "," : "", use->element, t1_ir_resolve(ir, use->value));
        }
        printf("}}");
        vcpu_destroy(v);
    }
    printf("]\n");
    free(ir);
    free(f);
}
