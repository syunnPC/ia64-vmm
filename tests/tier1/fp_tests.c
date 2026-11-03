/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FP arithmetic in Tier1 regions (frag_fparith.S): the in-region helpers, the
 * host FP path of fma-family helpers and native FP loads/stores, compared
 * with interp_run in place and at the end (umt_case), with forced exits.
 * Inputs mix the classes the conditions separate (SDM Vol.1 Table 5-2):
 * zeros, normals of the host formats, denormals, infinities, NaNs, values
 * near both range ends and register values beyond double precision. FPSR
 * starts from the Linux default with variants: inexact flag clear,
 * directed rounding, enabled traps (an FP trap commits, the helper undoes
 * it and the lower tier delivers it), random contents.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/um_t1.h"
#include "engine/tier1/region.h"
#include "ia64/fp.h"
#include "ia64/hostfp.h"
#include "tests/tier1/fpx_native.h"

static uint64_t rng = UINT64_C(0x2545f4914f6cdd1d);
static uint64_t r64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}
static unsigned rnd(unsigned n)
{
    return (unsigned)(r64() % n);
}

#define FPSR_LINUX UINT64_C(0x0009804c0270033f)
#define IN0 (UM_DATA)
#define IN1 (UM_DATA + 0x8000)
#define OUT0 (UM_DATA + 0x10000)
#define OUT1 (UM_DATA + 0x18000)

static uint64_t edge_double(void)
{
    static const uint64_t v[] = {
        0,
        UINT64_C(0x8000000000000000),
        UINT64_C(0x3ff0000000000000),
        UINT64_C(0xbff0000000000000),
        UINT64_C(0x0010000000000000),
        UINT64_C(0x0010000000000001),
        UINT64_C(0x000fffffffffffff),
        UINT64_C(0x0000000000000001),
        UINT64_C(0x7fefffffffffffff),
        UINT64_C(0x7ff0000000000000),
        UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000000000),
        UINT64_C(0x7ff4000000000000),
        UINT64_C(0x3fefffffffffffff),
        UINT64_C(0x2000000000000000),
        UINT64_C(0x5ff0000000000000),
        UINT64_C(0x1ff0000000000000),
        UINT64_C(0x3ff0000000000001),
    };
    return v[rnd(sizeof(v) / sizeof(v[0]))];
}
/* typical: normal doubles of moderate exponent; otherwise mixed with edges. */
static uint64_t random_double(bool typical)
{
    if (!typical && rnd(3) == 0)
        return edge_double();
    uint64_t e = typical ? 1023 - 8 + rnd(17) : rnd(2048);
    return (r64() & UINT64_C(0x800fffffffffffff)) | e << 52;
}
static uint32_t random_single(bool typical)
{
    static const uint32_t v[] = {0,          0x80000000, 0x3f800000, 0x00800000, 0x00800001,
                                 0x007fffff, 1,          0x7f7fffff, 0x7f800000, 0xff800000,
                                 0x7fc00000, 0x7fa00000, 0x3f7fffff};
    if (!typical && rnd(3) == 0)
        return v[rnd(sizeof(v) / sizeof(v[0]))];
    uint32_t e = typical ? 127 - 6 + rnd(13) : rnd(256);
    return ((uint32_t)r64() & 0x807fffffu) | e << 23;
}
static struct ia64_fr random_fr(bool typical, bool single)
{
    if (typical || rnd(4))
        return single ? ia64_fp_from_ieee(random_single(typical), true)
                      : ia64_fp_from_ieee(random_double(typical), false);
    switch (rnd(5)) {
    case 4: {
        /* One significand bit beyond the host format's precision. */
        struct ia64_fr x = single ? ia64_fp_from_ieee(random_single(true), true)
                                  : ia64_fp_from_ieee(random_double(true), false);
        x.significand |= UINT64_C(1) << (single ? 39 : 10);
        return x;
    }
    case 0:
        return ia64_fp_nat();
    case 1:
        return (struct ia64_fr){r64() | IA64_FP_INTEGER_BIT, 0xffff - 20 + rnd(41),
                                (bool)(r64() & 1)};
    case 2:
        return (struct ia64_fr){r64() >> rnd(64), IA64_FP_INTEGER_EXP, false};
    default:
        return (struct ia64_fr){r64(), (unsigned)r64() & 0x1ffff, (bool)(r64() & 1)};
    }
}

/* Two single lanes (SDM Vol.3 fpma: the significand holds two singles). */
static uint64_t random_pair(bool typical)
{
    return (uint64_t)random_single(typical) << 32 | random_single(typical);
}
static struct ia64_fr random_packed(bool typical)
{
    if (!typical && rnd(4) == 0)
        return random_fr(false, true);
    return (struct ia64_fr){random_pair(typical), IA64_FP_INTEGER_EXP, false};
}

static uint64_t fpsr(bool typical)
{
    uint64_t sf_i = (UINT64_C(1) << (6 + 12)) | (UINT64_C(1) << (19 + 12));
    /* Linux's sf1 has wre set: its .s/.d results keep the helper (the
     * expansions need the format's own exponent range); half the typical
     * samples clear it. */
    if (typical)
        return (FPSR_LINUX | sf_i) & ~(rnd(2) ? UINT64_C(1) << (19 + 1) : 0);
    switch (rnd(6)) {
    case 0:
        return FPSR_LINUX; /* i clear */
    case 1:
        return FPSR_LINUX | sf_i |
               (uint64_t)(1 + rnd(3)) << (6 + 13 * rnd(2) + 4); /* sf0/sf1 directed rounding */
    case 2:
        return (FPSR_LINUX | sf_i) & ~(UINT64_C(1) << rnd(6)); /* one trap enabled */
    case 3:
        return (FPSR_LINUX | sf_i) | UINT64_C(0x3) << 6; /* sf0 ftz and wre */
    case 4:
        return r64() & UINT64_C(0x03ffffffffffffbf); /* random, sf0.td clear */
    default:
        return FPSR_LINUX | sf_i;
    }
}

static unsigned helper_calls(void)
{
    static const Ia64Opcode ops[] = {IA64_OP_FMA,  IA64_OP_FMS,  IA64_OP_FNMA,  IA64_OP_FADD,
                                     IA64_OP_FSUB, IA64_OP_FMPY, IA64_OP_FNORM, IA64_OP_FPMA,
                                     IA64_OP_FPMS, IA64_OP_FPNMA};
    uint64_t n = 0;
    for (unsigned k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k)
        n += t1_helper_calls(ops[k]);
    return (unsigned)n;
}

enum data { DOUBLE, SINGLE, PACKED };
static unsigned last_calls, last_iterations; /* of the last program() */

/* One program at offset `at` over `samples` inputs: 8-byte doubles, 4-byte
 * singles or 8-byte single pairs. slow >= 0: at most that many fma-family
 * helper calls per iteration of a typical sample (FPSR i flags set, normal
 * operands) plus one. */
static unsigned program(const char *image, unsigned at, enum data data, unsigned samples, int slow,
                        const char *label)
{
    bool single = data != DOUBLE;
    unsigned runs = 0, typical_iterations = 0, typical_calls = 0;
    for (unsigned sample = 0; sample < samples; ++sample) {
        /* Samples 4k+2: typical but some FRs one bit beyond the format. */
        bool typical = sample % 4 != 3, beyond = sample % 4 == 2;
        struct vcpu *v = um_vcpu(UM_CODE + at), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        unsigned n = 1 + rnd(12);
        for (unsigned k = 0; k < 64; ++k) {
            if (data == PACKED) {
                uint64_t x = random_pair(typical), y = random_pair(typical);
                memcpy(um_ram(IN0 + 8 * k), &x, 8);
                memcpy(um_ram(IN1 + 8 * k), &y, 8);
            } else if (single) {
                uint32_t x = random_single(typical), y = random_single(typical);
                memcpy(um_ram(IN0 + 4 * k), &x, 4);
                memcpy(um_ram(IN1 + 4 * k), &y, 4);
            } else {
                uint64_t x = random_double(typical), y = random_double(typical);
                memcpy(um_ram(IN0 + 8 * k), &x, 8);
                memcpy(um_ram(IN1 + 8 * k), &y, 8);
            }
        }
        if (beyond) {
            /* Products at the min normal: (1 - 2^-p) 2^emin is tiny and
             * rounds to it (an underflow), (1 - 2^1-p)(1 + 2^1-p) 2^emin is
             * not tiny (SDM Vol.1 5.4.3). */
            static const uint64_t dpairs[][2] = {
                {UINT64_C(0x3fefffffffffffff), UINT64_C(0x0010000000000000)},
                {UINT64_C(0x3feffffffffffffe), UINT64_C(0x0010000000000001)}};
            static const uint32_t spairs[][2] = {
                {0x3f7fffff, 0x00800000},
                {0x3f7ffffe, 0x00800001},
                /* A denormal source with a zero factor (the host sets DE). */
                {0x00000001, 0x00000000},
                {0x807fffff, 0x80000000},
                /* Exact denormal products: tiny without inexact. */
                {0x00800000, 0x3f000000},
                {0x80800000, 0x3e800000}};
            for (unsigned k = 0; k < 64; k += 1 + rnd(4)) {
                unsigned j = rnd(data == PACKED ? 6 : 2);
                if (data == PACKED) {
                    unsigned lane = 32 * rnd(2);
                    for (unsigned i = 0; i < 2; ++i) {
                        uint64_t x;
                        memcpy(&x, um_ram((i ? IN1 : IN0) + 8 * k), 8);
                        x = (x & ~(UINT64_C(0xffffffff) << lane)) | (uint64_t)spairs[j][i] << lane;
                        memcpy(um_ram((i ? IN1 : IN0) + 8 * k), &x, 8);
                    }
                } else if (single) {
                    memcpy(um_ram(IN0 + 4 * k), &spairs[j][0], 4);
                    memcpy(um_ram(IN1 + 4 * k), &spairs[j][1], 4);
                } else {
                    memcpy(um_ram(IN0 + 8 * k), &dpairs[j][0], 8);
                    memcpy(um_ram(IN1 + 8 * k), &dpairs[j][1], 8);
                }
            }
        }
        um_gr(v, 2, IN0, false);
        um_gr(v, 3, IN1, false);
        um_gr(v, 4, OUT0, false);
        um_gr(v, 5, OUT1, false);
        v->s.ar[AR_LC] = n - 1;
        v->s.ar[AR_FPSR] = fpsr(typical);
        /* Exact tiny results with sf0/sf1 ftz or the underflow trap enabled
         * (SDM Vol.1 5.4.3). */
        if (beyond && data == PACKED)
            switch (rnd(3)) {
            case 0:
                v->s.ar[AR_FPSR] |= UINT64_C(1) << (6 + 13 * rnd(2));
                break;
            case 1:
                v->s.ar[AR_FPSR] &= ~(uint64_t)FP_U;
                break;
            default:
                break;
            }
        um_gr(v, 20, random_double(typical), !typical && rnd(8) == 0);
        um_gr(v, 21, random_single(typical), !typical && rnd(8) == 0);
        /* Physical FRs: f40/f41 rotate with CFM.rrb.fr in a third of the samples. */
        if (sample % 3 == 1)
            v->s.cfm = (v->s.cfm & ~(UINT64_C(0x7f) << 25)) | (uint64_t)rnd(96) << 25;
        for (unsigned f = 6; f < 128; ++f) {
            if (f >= 22 && f < 32)
                continue;
            v->s.fr[f] = data == PACKED && (f < 16 || f >= 32) ? random_packed(typical)
                                                               : random_fr(typical, single);
            if (beyond && data != PACKED)
                switch (rnd(4)) {
                case 0:
                    v->s.fr[f].significand |= UINT64_C(1) << (single ? 39 : 10);
                    break;
                /* Exponents just outside the format's normal range. */
                case 1:
                    v->s.fr[f].exponent = single ? 0x1007f : 0x103ff;
                    break;
                case 2:
                    v->s.fr[f].exponent = single ? 0x0ff80 : 0x0fc00;
                    break;
                default:
                    break;
                }
        }
        v->s.pr = r64() | 1;
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, label);
        if (!id || !t1_region_get(id)->loop)
            fprintf(stderr, "%s: region of %u instructions ends at %#llx:%u: %s\n", label,
                    rep.insns, (unsigned long long)rep.end_ip, rep.end_slot,
                    rep.end_reason ? rep.end_reason : "?");
        UM_CHECK(id && t1_region_get(id)->loop);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX};
        if (sample % 8 == 5) {
            /* A forced exit at a random helper or site point. */
            const struct t1_region *r = t1_region_get(id);
            unsigned p = rnd(r->npoints);
            if (r->point[p].kind == T1_PT_HELPER || r->point[p].kind == T1_PT_SITE) {
                arm.point = p;
                arm.occurrence = 1 + rnd(2);
                arm.action =
                    r->point[p].kind == T1_PT_HELPER ? T1_FORCE_HELPER_EXIT : T1_FORCE_MISS_EXIT;
            }
        }
        unsigned calls = helper_calls();
        /* A budget for every iteration, or one ending inside the loop. */
        bool full = sample % 4 != 1;
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, full ? 64 + 64 * n : 40 + 16 * n, id,
                          &arm, &out, false, label) == 0);
        UM_CHECK(out.run.entries || !typical); /* a NaT source GR fails the entry guard */
        if (typical && !beyond && full && arm.point == UINT32_MAX) {
            typical_iterations += n;
            typical_calls += helper_calls() - calls;
        }
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(ref);
        vcpu_destroy(v);
        ++runs;
    }
    printf("%s: %u fma-family helper calls in %u typical iterations\n", label, typical_calls,
           typical_iterations);
    last_calls = typical_calls;
    last_iterations = typical_iterations;
    if (slow >= 0 && typical_calls > ((unsigned)slow + 1) * typical_iterations) {
        fprintf(stderr, "%s: %u fma-family helper calls for %u typical iterations\n", label,
                typical_calls, typical_iterations);
        UM_CHECK(!"host FP path not taken");
    }
    return runs;
}

/* A region whose first instruction is an FP store: a NaTVal source fails
 * the entry guard (an exit before the store would retire nothing and
 * re-enter forever), others run the loop. */
static unsigned store_head(const char *image)
{
    unsigned runs = 0;
    for (unsigned sample = 0; sample < 40; ++sample) {
        struct vcpu *v = um_vcpu(UM_CODE + 0x1800), *ref = um_vcpu(UM_CODE);
        um_load_file(UM_CODE, image);
        unsigned n = 1 + rnd(8);
        um_gr(v, 4, OUT0, false);
        v->s.ar[AR_LC] = n - 1;
        v->s.ar[AR_FPSR] = fpsr(true);
        bool natval = sample % 2;
        v->s.fr[6] = natval ? ia64_fp_nat() : random_fr(true, false);
        v->s.fr[7] = random_fr(true, false);
        v->s.fr[8] = random_fr(true, false);
        struct t1_compile_report rep;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "fparith store head");
        UM_CHECK(id && t1_region_get(id)->loop);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct umt_run out;
        struct umt_arm arm = {.point = UINT32_MAX};
        uint64_t entries = t1_region_get(id)->entries;
        UM_CHECK(umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 64 + 16 * n, id, &arm, &out, false,
                          "fparith store head") == 0);
        entries =
            t1_region_get(id)->entries - entries; /* this region only; the run may enter others */
        UM_CHECK(natval ? !entries : entries);
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(ref);
        vcpu_destroy(v);
        ++runs;
    }
    return runs;
}

/* The exact expansions themselves (fpfast.rules, emitted by the product's
 * walker and run natively) against the soft-float: wherever an expansion
 * reaches $DONE with qp = 1, ia64_fp on the same register values gives
 * the same FR[f1] and PSR modified bit, leaves FPSR unchanged and
 * completes. Inputs: zeros, normals of the format with exponents near 1.0
 * and at both range ends, values one bit too precise, unnormals, specials,
 * NaTVal and single pairs; FPSR with the promise's controls mostly. */
static uint64_t fpx_reg(bool packed, bool single, uint64_t *meta)
{
    unsigned frac = single ? 23 : 52, low = single ? 0xff81 : 0xfc01,
             high = single ? 0x1007e : 0x103fe;
    uint64_t sign = (r64() & 1) << 17;
    if (packed) {
        *meta = rnd(25) ? 0x1003e : 0x1fffe;
        return (uint64_t)random_single(rnd(2)) << 32 | random_single(rnd(2));
    }
    switch (rnd(10)) {
    case 0:
        *meta = sign;
        return 0;
    case 1:
        *meta = sign | (rnd(2) ? low : high);
        return UINT64_C(1) << 63 | (r64() << (63 - frac) & ~(UINT64_C(1) << 63));
    case 2:
        *meta = sign | (0xffff - 10 + rnd(21));
        return r64() >> 1; /* unnormal */
    case 3:
        *meta = rnd(2) ? 0x1ffff : 0x1fffe;
        return rnd(2) ? UINT64_C(1) << 63 : 0; /* inf, NaTVal */
    case 4:
        *meta = sign | (0xffff - 10 + rnd(21));
        return r64() | UINT64_C(1) << 63; /* 64 bits */
    default:
        *meta = sign | (0xffff - 30 + rnd(61));
        return UINT64_C(1) << 63 | ((r64() >> (64 - frac)) << (63 - frac));
    }
}

static unsigned fpx_promise(void)
{
    static uint8_t buf[4096];
    void *page =
        mmap(NULL, sizeof(buf), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    UM_CHECK(page != MAP_FAILED);
    bool fma3 = fpx_host_fma3();
    unsigned done = 0;
    for (unsigned k = 0; k < t1_fpx_rule_count; ++k) {
        const struct t1_fpx_rule *r = &t1_fpx_rules[k];
        if (r->mode != T1_FPX_EXACT || (r->fma3 && !fma3))
            continue;
        bool packed =
            r->opcode == IA64_OP_FPMA || r->opcode == IA64_OP_FPMS || r->opcode == IA64_OP_FPNMA;
        unsigned rule_done = 0;
        for (unsigned n = 0; n < 3000; ++n) {
            struct vcpu *v = um_vcpu(UM_CODE);
            unsigned sf = rnd(4);
            uint64_t fpsr = (FPSR_LINUX | UINT64_C(1) << (6 + 13 * sf + 12)) &
                            ~(UINT64_C(0x32) << (6 + 13 * sf));
            if (!rnd(8))
                fpsr ^= UINT64_C(1) << (6 + 13 * sf + rnd(13));
            struct fpx_block blk;
            blk.in[T1_FPX_QP] = 1;
            blk.in[T1_FPX_FPSR] = fpsr;
            for (unsigned role = 0; role < 3; ++role) {
                uint64_t meta, sig = fpx_reg(packed, r->precision == 1, &meta);
                if (role == 0 && r->addend == T1_FPX_F0)
                    sig = meta = 0;
                blk.in[T1_FPX_A_SIG + 2 * role] = sig;
                blk.in[T1_FPX_A_META + 2 * role] = meta;
                v->s.fr[7 + role] =
                    (struct ia64_fr){sig, (uint32_t)(meta & 0x1ffff), (meta >> 17) & 1};
            }
            blk.in[T1_FPX_D_SIG] = blk.in[T1_FPX_D_META] = 0;
            unsigned exit;
            UM_CHECK(
                fpx_native_run(r, sf, (int64_t)IA64_PSR_MFL, &blk, &exit, buf, page, sizeof(buf)));
            if (exit != 1) {
                UM_CHECK(blk.out[0] == FPX_POISON && blk.out[1] == FPX_POISON &&
                         blk.out[2] == FPX_POISON);
                vcpu_destroy(v);
                continue;
            }
            /* The fields the decoder gives the pseudo-ops (fnorm: f2 = f0,
             * f4 = f1; fadd, fsub: f4 = f1; fmpy: f4 read as the second factor). */
            Ia64Instruction insn = {.valid = true,
                                    .opcode = r->opcode,
                                    .unit = IA64_UNIT_F,
                                    .slot = 0,
                                    .address = UM_CODE};
            unsigned f2 = r->addend == T1_FPX_F0 || r->opcode == IA64_OP_FNORM ? 0 : 7;
            unsigned f4 =
                r->opcode == IA64_OP_FNORM || r->opcode == IA64_OP_FADD || r->opcode == IA64_OP_FSUB
                    ? 1
                : r->opcode == IA64_OP_FMPY ? 8
                                            : 9;
            insn.operands.floating = (IA64FloatingOperands){.destination = 6,
                                                            .source1 = f2,
                                                            .source2 = 8,
                                                            .auxiliary1 = f4,
                                                            .precision = r->precision,
                                                            .status_field = sf};
            v->s.ar[AR_FPSR] = fpsr;
            v->s.psr &= ~(IA64_PSR_MFL | IA64_PSR_MFH);
            struct ia64_result res = ia64_fp(v, &insn);
            const struct ia64_fr *d = &v->s.fr[6];
            if (res.kind != IA64_STEP_COMPLETE || d->significand != blk.out[0] ||
                (d->exponent | (uint64_t)d->sign << 17) != blk.out[1] || v->s.ar[AR_FPSR] != fpsr ||
                (v->s.psr & (IA64_PSR_MFL | IA64_PSR_MFH)) != blk.out[2]) {
                fprintf(
                    stderr,
                    "fpx promise: %s sf %u fpsr %llx: soft %u %llx %x/%d fpsr %llx psr.mf %llx, "
                    "expansion %llx %llx %llx\n",
                    r->name, sf, (unsigned long long)fpsr, res.kind,
                    (unsigned long long)d->significand, d->exponent, d->sign,
                    (unsigned long long)v->s.ar[AR_FPSR],
                    (unsigned long long)(v->s.psr & (IA64_PSR_MFL | IA64_PSR_MFH)),
                    (unsigned long long)blk.out[0], (unsigned long long)blk.out[1],
                    (unsigned long long)blk.out[2]);
                UM_CHECK(!"expansion differs from the soft-float");
            }
            ++rule_done;
            vcpu_destroy(v);
        }
        UM_CHECK(rule_done > 0);
        done += rule_done;
    }
    munmap(page, sizeof(buf));
    return done;
}

void fparith_tests(const char *image)
{
    umt_init();
    unsigned runs = 0;
    ia64_hostfp_configure(0);
    bool fma3 = ia64_hostfp_options() & IA64_HOSTFP_OPT_FMA3;
    /* Stable mode uses the checked host-FP helper and native bit transfers. */
    runs += program(image, 0x0, DOUBLE, 100, -1, "fparith stable");
    UM_CHECK(last_iterations && last_calls >= 10 * last_iterations);
    runs += program(image, 0x400, SINGLE, 100, -1, "fparith stable single");
    runs += program(image, 0x1000, DOUBLE, 100, -1, "fparith stable getf/setf");
    runs += program(image, 0x1400, PACKED, 100, -1, "fparith stable parallel");
    for (unsigned pass = 0; pass < 2; ++pass) {
        /* With and without the host FMA3 (Sandy Bridge): the fused forms
         * (up to 5 per iteration) then use the exact helper. The reference
         * interpreter checks each host result against the soft-float
         * (fp_check). */
        ia64_hostfp_configure(IA64_HOSTFP_CFG_EXPERIMENTAL | IA64_HOSTFP_CFG_CHECK |
                              (pass ? IA64_HOSTFP_CFG_NOFMA3 : 0));
        int fused = !pass && fma3 ? 0 : 5;
        runs += program(image, 0x0, DOUBLE, 400, fused, "fparith double");
        runs += program(image, 0x400, SINGLE, 400, fused + 4, "fparith single"); /* 4 on sf1 */
        runs += program(image, 0x800, DOUBLE, 300, -1, "fparith mixed");
        runs += program(image, 0xc00, DOUBLE, 100, -1, "fparith 64-bit");
        runs += program(image, 0x1000, DOUBLE, 200, -1, "fparith getf/setf");
        /* The predicated sf2 fpma has the i flag clear. */
        runs += program(image, 0x1400, PACKED, 400, 1 + fused, "fparith parallel");
    }
    /* fp_approx: Tier1 and the interpreter both run the 64-bit model on
     * double; they must still agree with each other. */
    ia64_hostfp_configure(IA64_HOSTFP_CFG_EXPERIMENTAL | IA64_HOSTFP_CFG_APPROX);
    runs += program(image, 0xc00, DOUBLE, 200, 0, "fparith approx");
    runs += program(image, 0x800, DOUBLE, 100, -1, "fparith approx mixed");
    ia64_hostfp_configure(IA64_HOSTFP_CFG_EXPERIMENTAL | IA64_HOSTFP_CFG_APPROX |
                          IA64_HOSTFP_CFG_NOFMA3);
    runs +=
        program(image, 0x1400, PACKED, 100, -1, "fparith approx parallel"); /* fused: the helper */
    runs += store_head(image);
    /* fp_host=0: no host FP in Tier1 either. */
    ia64_hostfp_configure(IA64_HOSTFP_CFG_SOFT | IA64_HOSTFP_CFG_EXPERIMENTAL);
    runs += program(image, 0x0, DOUBLE, 50, -1, "fparith soft");
    UM_CHECK(last_iterations && last_calls >= 10 * last_iterations); /* 10 fma-family operations */
    ia64_hostfp_configure(0);
    unsigned promise = fpx_promise();
    printf(
        "fparith: %u differential runs passed, %u expansion results equal the soft-float's (host FMA3 %s)\n",
        runs, promise, fma3 ? "yes" : "no");
}
