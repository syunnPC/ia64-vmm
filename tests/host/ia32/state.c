/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/state.h"
#include "ia64vmm/ia32_state.h"
#include "ia64/ia32/segment.h"

/* Expectations are numerical SDM Rev.2.3 Vol.1 Tables 6-1/5/6/7,
 * Figs.6-4/5, Vol.2 Tables 10-1/2/4 and Ch.9, Vol.4 JMPE.
 * No expected field is computed from a production mask. */
static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static struct ia64_state s, before;
static struct ia32_native_state n;
static uint64_t get(const uint8_t *p, unsigned bytes)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; ++i)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void put(uint8_t *p, uint64_t v, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = v >> (8 * i);
}
static void init(void)
{
    memset(&s, 0, sizeof(s));
    s.cpu_model = IA64_CPU_MADISON;
    s.psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) | (UINT64_C(1) << 36);
    s.ar[21] = 0x37f;
    s.ar[28] = 0x55550000;
}
static void descriptors(void)
{
    const uint32_t limits[] = {0, 1, 0xfff, 0x1000, 0x7ffff, 0xfffff};
    for (unsigned system = 0; system < 2; ++system)
        for (unsigned a = 0; a < 4096; ++a)
            for (unsigned j = 0; j < sizeof(limits) / sizeof(limits[0]); ++j) {
                uint64_t d = UINT64_C(0x87654321) | (uint64_t)limits[j] << 32 | (uint64_t)a << 52,
                         back;
                struct ia32_segment seg;
                ia32_segment_decode(d, 0xabcd, system, &seg);
                CHECK(seg.base == 0x87654321 && seg.selector == 0xabcd && !seg.unusable);
                CHECK(seg.limit == ((a & 0x800) ? (limits[j] << 12) | 0xfff : limits[j]));
                CHECK(seg.attrib == ((a & 0xff) | (a & (system ? 0x800 : 0xc00))));
                CHECK(ia32_segment_encode(&seg, d, system, false, &back) == IA32_STATE_OK);
                CHECK(back == d);
                CHECK(ia32_segment_encode(&seg, d, system, true, &back) == IA32_STATE_OK);
                CHECK(back == (d & ~(UINT64_C(0x3) << 60 | (system ? UINT64_C(1) << 62 : 0))));
            }
    struct ia32_segment seg;
    uint64_t back = 123;
    ia32_segment_decode(0, 0, false, &seg);
    seg.base = UINT64_C(1) << 32;
    CHECK(ia32_segment_encode(&seg, 0, false, false, &back) == IA32_STATE_UNREPRESENTABLE);
    CHECK(back == 123);
    seg.base = 0;
    seg.limit = 0x100000;
    CHECK(ia32_segment_encode(&seg, 0, false, false, &back) == IA32_STATE_UNREPRESENTABLE);
    seg.attrib = 0x800;
    seg.limit = 0x12345000;
    CHECK(ia32_segment_encode(&seg, 0, false, false, &back) == IA32_STATE_UNREPRESENTABLE);
    seg.limit = 0xfff;
    seg.attrib |= 0x200;
    CHECK(ia32_segment_encode(&seg, 0, false, false, &back) == IA32_STATE_UNREPRESENTABLE);
    seg.attrib = 0x100;
    seg.limit = 0;
    CHECK(ia32_segment_encode(&seg, 0, false, true, &back) == IA32_STATE_OK && back == 0);
    CHECK(ia32_segment_encode(&seg, 0, false, false, &back) == IA32_STATE_UNREPRESENTABLE);
    /* Intel SDM Vol.3C §25.4.1: null means index=0 AND TI=0.
     * Unusable VMCS cache fields are not authoritative. SDM Vol.2
     * Table 10-2: P=0 makes an IA-32 segment reference fault. */
    seg = (struct ia32_segment){
        .selector = 3, .unusable = true, .base = UINT64_MAX, .limit = 0x12345678, .attrib = 0xffff};
    CHECK(ia32_segment_encode(&seg, UINT64_C(0xcb12345687654321), false, false, &back) ==
          IA32_STATE_OK);
    CHECK(back == UINT64_C(0xc312345687654321));
    CHECK(ia32_segment_encode(&seg, UINT64_C(0xfb12345687654321), false, true, &back) ==
          IA32_STATE_OK);
    CHECK(back == UINT64_C(0xc312345687654321));
    seg.selector = 4; /* LDT entry zero is not a null selector. */
    CHECK(ia32_segment_encode(&seg, 0, false, false, &back) == IA32_STATE_UNREPRESENTABLE);
    seg.selector = 8;
    CHECK(ia32_segment_encode(&seg, 0, false, false, &back) == IA32_STATE_UNREPRESENTABLE);
    seg.selector = 0;
    /* VMX also permits a null unusable LDTR; TSS is rejected by import. */
    CHECK(ia32_segment_encode(&seg, UINT64_C(0xfb12345687654321), true, false, &back) ==
          IA32_STATE_OK);
    CHECK(back == UINT64_C(0xf312345687654321));
    CHECK(ia32_segment_encode(&seg, UINT64_C(0xfb12345687654321), true, true, &back) ==
          IA32_STATE_OK);
    CHECK(back == UINT64_C(0x8312345687654321));
}
static void segment_boundaries(void)
{
    /* Vol.2 Fig.10-1/Table 10-2 apply G scaling to GDTD too. Native
     * GDTR has only base/16-bit limit (Intel SDM Vol.3C §25.4.1). */
    for (unsigned g = 0; g < 2; ++g)
        for (unsigned attributes = 0; attributes < 2048; ++attributes) {
            init();
            uint64_t raw =
                ((uint64_t)g << 63) | (uint64_t)attributes << 52 | UINT64_C(7) << 32 | 0x12345678;
            s.gr[31] = raw;
            CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
            CHECK(n.gdtr.base == 0x12345678 && n.gdtr.limit == (g ? 0x7fff : 7));
            CHECK(n.gdtr.attrib == ((attributes & 0xff) | (g ? 0x800 : 0)) &&
                  !n.gdtr.selector && !n.gdtr.unusable);
            CHECK(ia32_state_import(&s, &n, IA32_DIRTY_SEGMENTS, 0, false) == IA32_STATE_OK);
            CHECK(s.gr[31] == raw);
            n.gdtr.base = 0x87654321;
            n.gdtr.limit = g ? 0x8fff : 8;
            CHECK(ia32_state_import(&s, &n, IA32_DIRTY_SEGMENTS, 0, false) == IA32_STATE_OK);
            CHECK(s.gr[31] ==
                  ((raw & UINT64_C(0xfff0000000000000)) | UINT64_C(8) << 32 | 0x87654321));
        }
    init();
    s.gr[31] = UINT64_C(0x8000001000000000);
    before = s;
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    CHECK(n.gdtr.limit == 0x10fff && !memcmp(&s, &before, sizeof(s)));
    init();
    s.gr[31] = UINT64_C(0x0800001000000000);
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK && n.gdtr.attrib == 0x80);
    before = s;
    n.gdtr.attrib = 0;
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_SEGMENTS, 0, false) == IA32_STATE_UNREPRESENTABLE);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    init();
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    n.gdtr.limit = 0x100000;
    before = s;
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_SEGMENTS, 0, false) == IA32_STATE_UNREPRESENTABLE);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    init();
    s.gr[31] = UINT64_C(0x8000000000000000);
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    n.gdtr.limit = 0x1234;
    before = s;
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_SEGMENTS, 0, false) == IA32_STATE_UNREPRESENTABLE);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    init();
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    n.gdtr.base = UINT64_C(1) << 32;
    before = s;
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_SEGMENTS, 0, false) == IA32_STATE_UNREPRESENTABLE);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    for (unsigned seg = 0; seg < 8; ++seg) {
        init();
        s.gr[28] = UINT64_C(0x4b12345687654321);
        s.gr[30] = UINT64_C(0xcb12345687654321);
        CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
        struct ia32_segment *segments[] = {&n.es, &n.cs, &n.ss,   &n.ds,
                                           &n.fs, &n.gs, &n.ldtr, &n.tr};
        segments[seg]->unusable = true;
        segments[seg]->selector = 3;
        before = s;
        enum ia32_state_status expected =
            seg == 1 || seg == 2 || seg == 7 ? IA32_STATE_UNREPRESENTABLE : IA32_STATE_OK;
        CHECK(ia32_state_import(&s, &n, IA32_DIRTY_SEGMENTS, 0, false) == expected);
        if (expected != IA32_STATE_OK)
            CHECK(!memcmp(&s, &before, sizeof(s)));
        else if (seg == 4)
            CHECK(s.gr[28] == UINT64_C(0x4312345687654321));
        else if (seg == 6)
            CHECK(s.gr[30] == UINT64_C(0xc312345687654321) && (uint16_t)(s.gr[17] >> 32) == 3);
    }
    /* Vol.2 Table 10-1: KR1 IS TSSD, not a second control register. */
    const uint64_t groups[] = {IA32_DIRTY_CONTROL, IA32_DIRTY_SEGMENTS, IA32_DIRTY_ALL};
    for (unsigned i = 0; i < 3; ++i) {
        init();
        s.ar[1] = UINT64_C(0x8b00010087654321);
        CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
        before = s;
        n.kr1 ^= 1;
        CHECK(ia32_state_import(&s, &n, groups[i], 0, false) == IA32_STATE_UNREPRESENTABLE);
        CHECK(!memcmp(&s, &before, sizeof(s)));
        n.kr1 ^= 1;
        n.tr.base = UINT64_C(1) << 32;
        CHECK(ia32_state_import(&s, &n, groups[i], 0, false) == IA32_STATE_UNREPRESENTABLE);
        CHECK(!memcmp(&s, &before, sizeof(s)));
        n.tr.base = 0x12345678;
        n.kr1 = UINT64_C(0x8b00010012345678);
        CHECK(ia32_state_import(&s, &n, groups[i], 0, false) == IA32_STATE_OK);
        CHECK(s.ar[1] == UINT64_C(0x8b00010012345678));
    }
}
static void segment_integrity(void)
{
    /* Vol.1 Table 6-4: checks happen on references, including caches
     * established directly by Itanium instructions. */
    n = (struct ia32_native_state){.eflag = 2,
                                    .cs = {.attrib = 0x93},
                                    .ss = {.attrib = 0x93},
                                    .ds = {.attrib = 0x93},
                                    .es = {.attrib = 0x93},
                                    .fs = {.attrib = 0x93},
                                    .gs = {.attrib = 0x93}};
    CHECK(ia32_segment_valid(&n, IA32_SEG_CS, IA32_SEGMENT_FETCH));
    n.cs.attrib = 0x97;
    CHECK(!ia32_segment_valid(&n, IA32_SEG_CS, IA32_SEGMENT_FETCH));
    n.cs.attrib = 0x9b;
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (bit != 0 && bit != 4 && bit != 7)
            continue;
        n.cs.attrib = 0x9b ^ (1u << bit);
        CHECK(!ia32_segment_valid(&n, IA32_SEG_CS, IA32_SEGMENT_FETCH));
    }
    n.cs.attrib = 0x49b;
    for (unsigned pe = 0; pe < 2; ++pe)
        for (unsigned vm = 0; vm < 2; ++vm)
            for (unsigned cpl = 0; cpl < 4; ++cpl) {
                n.cflg = pe;
                n.eflag = 2 | (vm ? 0x20000 : 0);
                n.cpl = cpl;
                n.cs.attrib = vm ? 0xf3 : 0x49b;
                CHECK(ia32_segment_valid(&n, IA32_SEG_CS, IA32_SEGMENT_FETCH) ==
                      (vm ? pe && cpl == 3 : pe || cpl == 0));
            }
    n.cflg = 1;
    n.cpl = 3;
    n.eflag = 0x20002;
    n.cs.attrib = 0x4f3;
    CHECK(!ia32_segment_valid(&n, IA32_SEG_CS, IA32_SEGMENT_FETCH));
    n.cs.attrib = 0xd3;
    CHECK(!ia32_segment_valid(&n, IA32_SEG_CS, IA32_SEGMENT_FETCH));
    n.cs.attrib = 0xf7;
    CHECK(!ia32_segment_valid(&n, IA32_SEG_CS, IA32_SEGMENT_FETCH));
    n.cs.attrib = 0xf3;
    for (unsigned vm = 0; vm < 2; ++vm)
        for (unsigned virtual = 0; virtual < 4; ++virtual)
            for (unsigned flags = 0; flags < 4; ++flags) {
                n.cflg = 1 | ((uint64_t)virtual << 32);
                n.eflag = 2 | (vm ? 0x20000 : 0) | flags << 19;
                CHECK(ia32_segment_valid(&n, IA32_SEG_CS, IA32_SEGMENT_FETCH) ==
                      !(flags == 3 && ((virtual & 2) || (vm && (virtual & 1)))));
            }
    const unsigned data[] = {IA32_SEG_ES, IA32_SEG_SS, IA32_SEG_DS, IA32_SEG_FS, IA32_SEG_GS};
    struct ia32_segment *segments[] = {&n.es, &n.ss, &n.ds, &n.fs, &n.gs};
    for (unsigned mode = 0; mode < 3; ++mode) {
        n.cpl = mode == 2 ? 3 : 0;
        n.cflg = mode != 0;
        n.eflag = mode == 2 ? 0x20002 : 2;
        for (unsigned i = 0; i < 5; ++i) {
            unsigned good = mode == 2 ? 0xf3 : 0x493;
            segments[i]->attrib = good;
            CHECK(ia32_segment_valid(&n, data[i], IA32_SEGMENT_READ));
            CHECK(ia32_segment_valid(&n, data[i], IA32_SEGMENT_WRITE));
            const unsigned missing[] = {1, 0x10, 0x80};
            for (unsigned bit = 0; bit < 3; ++bit) {
                segments[i]->attrib = good ^ missing[bit];
                CHECK(!ia32_segment_valid(&n, data[i], IA32_SEGMENT_READ));
            }
            segments[i]->attrib = good ^ 2;
            CHECK(ia32_segment_valid(&n, data[i], IA32_SEGMENT_READ));
            CHECK(!ia32_segment_valid(&n, data[i], IA32_SEGMENT_WRITE));
            segments[i]->attrib = (good & ~15u) | 9;
            CHECK(!ia32_segment_valid(&n, data[i], IA32_SEGMENT_READ));
            segments[i]->attrib |= 2;
            CHECK(ia32_segment_valid(&n, data[i], IA32_SEGMENT_READ));
            CHECK(!ia32_segment_valid(&n, data[i], IA32_SEGMENT_WRITE));
            segments[i]->attrib = good | 4;
            CHECK(ia32_segment_valid(&n, data[i], IA32_SEGMENT_READ) == (mode != 2));
            segments[i]->attrib = good | 0x400;
            CHECK(ia32_segment_valid(&n, data[i], IA32_SEGMENT_READ) == (mode != 2));
            segments[i]->attrib = good;
            segments[i]->unusable = true;
            CHECK(!ia32_segment_valid(&n, data[i], IA32_SEGMENT_READ));
            segments[i]->unusable = false;
        }
        n.ss.attrib = mode == 2 ? 0xd3 : 0xb3;
        CHECK(!ia32_segment_valid(&n, IA32_SEG_SS, IA32_SEGMENT_READ));
        n.cs.attrib = mode == 2 ? 0xf9 : 0x99;
        CHECK(ia32_segment_valid(&n, IA32_SEG_CS, IA32_SEGMENT_READ) == (mode != 1));
        CHECK(ia32_segment_valid(&n, IA32_SEG_CS, IA32_SEGMENT_WRITE) == (mode != 1));
    }
    struct ia32_segment *system[] = {&n.ldtr, &n.tr, &n.gdtr};
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned a = 0; a < 256; ++a) {
            system[i]->attrib = a;
            CHECK(ia32_segment_valid(&n, IA32_SEG_LDT + i, IA32_SEGMENT_READ) ==
                  ((a & 0x90) == 0x80));
        }
}
static void mxcsr_reserved(void)
{
    /* Vol.1 Fig.6-1 labels FCR[38] reserved; Vol.4 LDMXCSR p.4:517
     * reserves MXCSR[6] and [31:16]. Itanium does not implement DAZ. */
    init();
    s.ar[21] = UINT64_C(1) << 38;
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    CHECK(get(n.fp.bytes + 24, 4) == 0 && get(n.fp.bytes + 28, 4) == 0xffbf);
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_FP, 0, false) == IA32_STATE_OK);
    CHECK(s.ar[21] == (UINT64_C(1) << 38));
    put(n.fp.bytes + 24, 0x40, 4);
    before = s;
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_FP, 0, false) == IA32_STATE_UNREPRESENTABLE);
    CHECK(!memcmp(&s, &before, sizeof(s)));
}
static void extended_denormals(void)
{
    /* Vol.1 Fig.5-6 (ldfe), Fig.5-9 (stfe), Table 5-2: extended zero,
     * denormal and pseudo-denormal all keep exponent zero, sign and all
     * significand bits. This is a bit transfer, not normalization. */
    const uint64_t significands[] = {0, 1, UINT64_C(0x7fffffffffffffff),
                                     UINT64_C(0x8000000000000000), UINT64_MAX};
    for (unsigned sign = 0; sign < 2; ++sign)
        for (unsigned i = 0; i < sizeof(significands) / sizeof(significands[0]); ++i) {
            uint8_t memory[10] = {0}, stored[10];
            put(memory, significands[i], 8);
            put(memory + 8, sign << 15, 2);
            struct ia64_fr f;
            ia32_x87_decode(memory, &f);
            CHECK(f.exponent == 0 && f.sign == sign && f.significand == significands[i]);
            f = (struct ia64_fr){significands[i], 0, sign};
            CHECK(ia32_x87_encode(&f, stored) == IA32_STATE_OK);
            CHECK(get(stored, 8) == significands[i] && get(stored + 8, 2) == sign << 15);
        }
}
static void integer_bits(void)
{
    for (unsigned r = 0; r < 8; ++r)
        for (unsigned bit = 0; bit < 64; ++bit) {
            init();
            s.gr[8 + r] = UINT64_C(1) << bit;
            CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
            CHECK(n.gpr[r] == (bit < 32 ? UINT32_C(1) << bit : 0));
            s.nat[8 + r] = true;
            CHECK(ia32_state_import(&s, &n, IA32_DIRTY_GPR, 0, false) == IA32_STATE_OK);
            CHECK(!s.nat[8 + r]);
            CHECK(s.gr[8 + r] == (bit == 31  ? UINT64_C(0xffffffff80000000)
                                  : bit < 31 ? UINT64_C(1) << bit
                                             : 0));
        }
}
static void floating(void)
{
    /* All 15-bit exponents, both signs, zero/normal/NaN payloads. */
    const uint64_t mant[] = {0, 1, UINT64_C(0x8000000000000000), UINT64_MAX};
    for (unsigned e = 0; e < 32768; ++e)
        for (unsigned sign = 0; sign < 2; ++sign)
            for (unsigned j = 0; j < 4; ++j) {
                struct ia64_fr f = {mant[j], e == 32767 ? 131071 : e ? e + 49152 : 0, sign};
                struct ia64_fr back;
                uint8_t b[10];
                CHECK(ia32_x87_encode(&f, b) == IA32_STATE_OK);
                CHECK(get(b, 8) == mant[j] && get(b + 8, 2) == (e | sign << 15));
                ia32_x87_decode(b, &back);
                CHECK(back.exponent == f.exponent && back.sign == f.sign &&
                      back.significand == f.significand);
            }
    /* Every 17-bit exponent outside the representable set is diagnostic. */
    for (unsigned e = 0; e < 131072; ++e) {
        struct ia64_fr f = {0, e, false};
        uint8_t b[10];
        bool valid = e == 0 || e == 131071 || (e >= 49153 && e <= 81918);
        CHECK((ia32_x87_encode(&f, b) == IA32_STATE_OK) == valid);
    }
    for (unsigned top = 0; top < 8; ++top)
        for (unsigned tags = 0; tags < 256; ++tags) {
            init();
            s.ar[28] = top << 11;
            for (unsigned i = 0; i < 8; ++i) {
                s.ar[28] |= (uint64_t)((tags >> i) & 1) << (16 + 2 * i);
                s.fr[8 + i] = (struct ia64_fr){UINT64_C(0x8000000000000000) + i, 65535 + i, i & 1};
                s.fr[16 + 2 * i].significand = UINT64_C(0x1122334455667700) + i;
                s.fr[17 + 2 * i].significand = UINT64_C(0xaabbccddeeff0000) + i;
            }
            s.ar[29] = UINT64_C(0x0567abcd12345678);
            s.ar[30] = UINT64_C(0x987687654321);
            CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
            CHECK(n.fp.bytes[4] == (uint8_t)~tags);
            CHECK(get(n.fp.bytes + 2, 2) == top << 11);
            for (unsigned st = 0; st < 8; ++st) {
                unsigned p = (top + st) & 7;
                CHECK(get(n.fp.bytes + 32 + 16 * st, 8) == UINT64_C(0x8000000000000000) + p);
                CHECK(get(n.fp.bytes + 40 + 16 * st, 2) == ((16383 + p) | (p & 1) << 15));
                CHECK(get(n.fp.bytes + 160 + 16 * st, 8) == UINT64_C(0x1122334455667700) + st);
                CHECK(get(n.fp.bytes + 168 + 16 * st, 8) == UINT64_C(0xaabbccddeeff0000) + st);
            }
            CHECK(get(n.fp.bytes + 6, 2) == 0x567 && get(n.fp.bytes + 8, 4) == 0x12345678);
            CHECK(get(n.fp.bytes + 12, 2) == 0xabcd && get(n.fp.bytes + 16, 4) == 0x87654321);
            CHECK(get(n.fp.bytes + 20, 2) == 0x9876);
            CHECK(ia32_state_import(&s, &n, IA32_DIRTY_FP, 0, false) == IA32_STATE_OK);
            uint64_t expected_fsr = top << 11;
            for (unsigned i = 0; i < 8; ++i) {
                expected_fsr |= (uint64_t)((tags >> i) & 1) << (16 + 2 * i);
                CHECK(s.fr[8 + i].exponent == 65535 + i && s.fr[8 + i].sign == (i & 1));
                CHECK(s.fr[8 + i].significand == UINT64_C(0x8000000000000000) + i);
            }
            CHECK(s.ar[28] == expected_fsr);
            for (unsigned i = 16; i < 32; ++i)
                CHECK(s.fr[i].exponent == 65598 && !s.fr[i].sign);
            for (unsigned i = 416; i < 512; ++i)
                CHECK(n.fp.bytes[i] == 0);
        }
    for (unsigned v = 0; v < 65536; ++v) {
        init();
        s.ar[21] = v;
        s.ar[28] = v;
        CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
        CHECK(get(n.fp.bytes, 2) == ((v & 0x1f3f) | 0x40));
        CHECK(get(n.fp.bytes + 2, 2) == ((v & 0x7fff) | ((v & 0x80) ? 0x8000 : 0)));
    }
    for (unsigned bit = 0; bit < 64; ++bit) {
        init();
        s.ar[21] = UINT64_C(1) << bit;
        s.ar[28] = UINT64_C(1) << bit;
        CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
        uint32_t expected = bit >= 39 && bit <= 47   ? 1u << (bit - 32)
                            : bit >= 32 && bit <= 37 ? 1u << (bit - 32)
                                                     : 0;
        CHECK(get(n.fp.bytes + 24, 4) == expected);
        CHECK(get(n.fp.bytes + 28, 4) == 0xffbf);
    }
}
static void tag_words(void)
{
    /* SDM Vol.4 FXSAVE pp.4:513–514, including unsupported encodings. */
    const struct {
        uint16_t exponent;
        uint64_t significand;
        unsigned tag;
    } cases[] = {
        {0, 0, 1},
        {0, 1, 2},
        {0, UINT64_C(0x8000000000000000), 2},
        {1, 0, 2},
        {1, UINT64_C(0x8000000000000000), 0},
        {0x7ffe, UINT64_MAX, 0},
        {0x7fff, UINT64_C(0x8000000000000000), 2},
        {0x7fff, UINT64_C(0xc000000000000000), 2},
        {0x7fff, 1, 2},
    };
    for (unsigned top = 0; top < 8; ++top)
        for (unsigned tags = 0; tags < 256; ++tags)
            for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
                struct ia32_fp_image fp = {0};
                put(fp.bytes + 2, top << 11, 2);
                fp.bytes[4] = tags;
                unsigned expected = 0;
                for (unsigned physical = 0; physical < 8; ++physical) {
                    unsigned st = (physical + 8 - top) & 7;
                    unsigned at = (c + physical) % (sizeof(cases) / sizeof(cases[0]));
                    put(fp.bytes + 32 + 16 * st, cases[at].significand, 8);
                    put(fp.bytes + 40 + 16 * st, cases[at].exponent | (physical & 1) << 15, 2);
                    expected |= ((tags & (1u << physical)) ? cases[at].tag : 3) << (2 * physical);
                }
                CHECK(ia32_x87_tag_word(&fp) == expected);
            }
}
static void mapping(void)
{
    init();
    for (unsigned i = 0; i < 128; ++i) {
        s.gr[i] = UINT64_C(0xfedcba9876543200) + i;
        s.fr[i].significand = i;
    }
    s.gr[16] = UINT64_C(0x4444333322221111);
    s.gr[17] = UINT64_C(0x8888777766665555);
    s.gr[31] = UINT64_C(0x12345678);
    s.gr[24] = UINT64_C(0x8fabcde012345678);
    s.gr[27] = UINT64_C(0x4b12345687654321);
    s.ar[25] = UINT64_C(0x4b100000fffffff0);
    s.ar[26] = UINT64_C(0xcb20000012345678);
    s.ar[0] = UINT64_C(0x1111222233334444);
    s.ar[1] = UINT64_C(0x8b00010087654321);
    s.ar[2] = UINT64_C(0xabcdef0187654321);
    s.ar[27] = UINT64_C(0x55555555aaaaaaaa);
    s.psr |= UINT64_C(2) << 32;
    s.ip = 0x23;
    before = s;
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    for (unsigned i = 0; i < 8; ++i)
        CHECK(n.gpr[i] == (uint32_t)(UINT64_C(0xfedcba9876543208) + i));
    CHECK(n.ds.selector == 0x1111 && n.es.selector == 0x2222 && n.fs.selector == 0x3333 &&
          n.gs.selector == 0x4444);
    CHECK(n.cs.selector == 0x5555 && n.ss.selector == 0x6666 && n.ldtr.selector == 0x7777 &&
          n.tr.selector == 0x8888);
    CHECK(n.cs.base == 0xfffffff0 && n.ss.base == 0x12345678 && n.es.base == 0x12345678);
    CHECK(n.eip.value == 0x33 && n.cpl == 2 && n.cflg == s.ar[27]);
    CHECK(n.kr0 == s.ar[0] && n.kr1 == s.ar[1] && n.kr2 == s.ar[2]);
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_ALL, 0, false) == IA32_STATE_OK);
    for (unsigned i = 8; i < 16; ++i)
        CHECK(s.gr[i] == UINT64_C(0x76543200) + i && !s.nat[i]);
    CHECK(s.gr[25] == before.gr[25] && s.gr[26] == before.gr[26]);
    CHECK(s.gr[16] == before.gr[16] && s.gr[17] == before.gr[17]);
    CHECK(s.gr[24] == before.gr[24] && s.gr[27] == before.gr[27]);
    CHECK(s.ar[25] == before.ar[25] && s.ar[26] == before.ar[26]);
    CHECK(s.ar[40] == before.ar[40] && !memcmp(s.bank_gr, before.bank_gr, sizeof(s.bank_gr)));
    for (unsigned bit = 0; bit < 64; ++bit) {
        init();
        s.ar[24] = UINT64_C(1) << bit;
        CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
        CHECK(n.eflag == (((UINT64_C(1) << bit) & 0x3f7fd5) | 2));
        before = s;
        n.gpr[0] = 0x80000000;
        n.gpr[1] = 0xffffffff;
        CHECK(ia32_state_import(&s, &n, IA32_DIRTY_GPR, 0, false) == IA32_STATE_OK);
        CHECK(s.gr[8] == UINT64_C(0xffffffff80000000) && s.gr[9] == UINT64_MAX);
        CHECK(s.ar[24] == before.ar[24]);
    }
    init();
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    before = s;
    n.ss.base = UINT64_C(1) << 32;
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_ALL, 0, false) == IA32_STATE_UNREPRESENTABLE);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    init();
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    put(n.fp.bytes + 24, 0x40, 4);
    before = s;
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_ALL, 0, false) == IA32_STATE_UNREPRESENTABLE);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    for (unsigned r = 8; r <= 31; ++r) {
        if (r >= 18 && r <= 23)
            continue;
        if (r == 25 || r == 26)
            continue;
        init();
        s.nat[r] = true;
        memset(&n, 0xab, sizeof(n));
        struct ia32_native_state old = n;
        CHECK(ia32_state_export(&s, &n) == IA32_STATE_UNDEFINED);
        CHECK(!memcmp(&n, &old, sizeof(n)));
    }
}
static void defined_bits(void)
{
    /* Field lists, not copies of the implementation masks: Vol.1 Table
     * 6-5, Figs.6-1/2/4/5; Vol.2 Fig.10-3 and Table 10-4. */
    const unsigned ars[] = {21, 24, 27, 28, 29, 30};
    const unsigned fcr[] = {0,  1,  2,  3,  4,  5, /* IM,DM,ZM,OM,UM,PM */
                            8,  9,  10, 11, 12,    /* PC,RC,IC; bits 6/7 are constants */
                            39, 40, 41, 42, 43, 44, 45, 46, 47}; /* SSE masks,RC,FZ; 38 reserved */
    const unsigned eflag[] = {0, 2, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 18, 19, 20, 21};
    const unsigned cflg[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,           /* PE..NE,IO,IF,II */
                             16, 18, 29, 30, 31,                          /* WP,AM,NW,CD,PG */
                             32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42}; /* VME..MMXEX */
    uint64_t masks[6] = {0};
    for (unsigned i = 0; i < sizeof(fcr) / sizeof(fcr[0]); ++i)
        masks[0] |= UINT64_C(1) << fcr[i];
    for (unsigned i = 0; i < sizeof(eflag) / sizeof(eflag[0]); ++i)
        masks[1] |= UINT64_C(1) << eflag[i];
    for (unsigned i = 0; i < sizeof(cflg) / sizeof(cflg[0]); ++i)
        masks[2] |= UINT64_C(1) << cflg[i];
    for (unsigned bit = 0; bit < 64; ++bit) {
        /* FSR: FSW[15:0], eight one-bit empty tags, SSE status[5:0].
         * FIR: offset[31:0], selector[15:0], opcode[10:0].
         * FDR: offset[31:0], selector[15:0]. */
        if (bit < 16 || (bit >= 16 && bit <= 30 && !(bit & 1)) || (bit >= 32 && bit <= 37))
            masks[3] |= UINT64_C(1) << bit;
        if (bit < 59)
            masks[4] |= UINT64_C(1) << bit;
        if (bit < 48)
            masks[5] |= UINT64_C(1) << bit;
    }
    const uint64_t production[] = {IA32_FCR_DEFINED, IA32_EFLAG_DEFINED, IA32_CFLG_DEFINED,
                                   IA32_FSR_DEFINED, IA32_FIR_DEFINED,   IA32_FDR_DEFINED};
    for (unsigned i = 0; i < 6; ++i)
        for (unsigned bit = 0; bit < 64; ++bit)
            CHECK(((production[i] >> bit) & 1) == ((masks[i] >> bit) & 1));
    for (unsigned bit = 0; bit < 64; ++bit) {
        init();
        for (unsigned i = 0; i < 6; ++i)
            s.ar[ars[i]] = UINT64_MAX;
        before = s;
        CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
        n.eflag = bit < 32 ? UINT32_C(1) << bit : 0;
        n.cflg = UINT64_C(1) << bit;
        put(n.fp.bytes, bit < 16 ? UINT64_C(1) << bit : 0, 2);
        put(n.fp.bytes + 2, bit < 16 ? UINT64_C(1) << bit : 0, 2);
        put(n.fp.bytes + 24, bit < 16 && bit != 6 ? UINT64_C(1) << bit : 0, 4);
        CHECK(ia32_state_import(&s, &n, IA32_DIRTY_ALL, 0, false) == IA32_STATE_OK);
        for (unsigned i = 0; i < 6; ++i)
            CHECK((s.ar[ars[i]] & ~masks[i]) == (before.ar[ars[i]] & ~masks[i]));
        CHECK((s.ar[24] & masks[1]) == (n.eflag & masks[1]));
        CHECK((s.ar[27] & masks[2]) == (n.cflg & masks[2]));
        CHECK(!(s.psr & (UINT64_C(1) << 4)) && !(s.psr & (UINT64_C(1) << 5)));
    }
    init();
    s.psr |= UINT64_C(1) << 34;
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_ALL, 0, false) == IA32_STATE_OK);
    CHECK(!(s.psr & 0x10));
    CHECK(ia32_state_leave(&s, &n, 0, false) == IA32_STATE_OK);
    CHECK((s.psr & 0x10) && (s.psr & (UINT64_C(1) << 34)));
    init();
    s.psr |= (UINT64_C(1) << 18) | (UINT64_C(1) << 34);
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_ALL, 0, false) == IA32_STATE_OK);
    CHECK(!(s.psr & (UINT64_C(1) << 4)));
    CHECK(ia32_state_leave(&s, &n, 0, false) == IA32_STATE_OK && !(s.psr & 0x10));
    s.psr |= (UINT64_C(1) << 4) | (UINT64_C(1) << 5);
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_ALL, 0, false) == IA32_STATE_OK);
    CHECK((s.psr & 0x30) == 0x30);
    init();
    for (unsigned i = 0; i < 128; ++i) {
        s.gr[i] = i + 0x123400;
        s.fr[i] = (struct ia64_fr){i, 65535, false};
        s.br[i & 7] = 0x876500 + i;
        s.ar[i] = 0x100 + i;
    }
    before = s;
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    CHECK(ia32_state_import(&s, &n, 0, 0, false) == IA32_STATE_OK);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_ALL, 0, false) == IA32_STATE_OK);
    for (unsigned i = 4; i < 8; ++i)
        CHECK(s.gr[i] == before.gr[i]);
    for (unsigned i = 2; i < 6; ++i)
        CHECK(!memcmp(&s.fr[i], &before.fr[i], sizeof(s.fr[i])));
    for (unsigned i = 0; i < 6; ++i)
        CHECK(s.br[i] == before.br[i]);
    const unsigned preserved[] = {16, 17, 18, 19, 36, 40, 64, 65, 66};
    for (unsigned i = 0; i < sizeof(preserved) / sizeof(preserved[0]); ++i)
        CHECK(s.ar[preserved[i]] == before.ar[preserved[i]]);
    init();
    s.gr[24] = UINT64_C(3) << 60;
    s.gr[30] = UINT64_C(7) << 60;
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_SEGMENTS, (1u << 0) | (1u << 6), false) ==
          IA32_STATE_OK);
    CHECK(s.gr[24] == 0 && s.gr[30] == 0);
    init();
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    before = s;
    CHECK(ia32_state_import(&s, &n, 0, 1, false) == IA32_STATE_UNREPRESENTABLE);
    CHECK(!memcmp(&s, &before, sizeof(s)));
}
static void disabled_fp(void)
{
    /* SDM Vol.2 §10.4: integer execution with DFL=1 cannot consume or
     * modify low FP state, which may still belong to another context. */
    init();
    s.psr |= UINT64_C(1) << 18;
    for (unsigned i = 2; i < 32; ++i)
        s.fr[i] = (struct ia64_fr){UINT64_C(0xfedcba9876543200) + i, 131070, i & 1};
    before = s;
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    CHECK(ia32_state_import(&s, &n, IA32_DIRTY_ALL, 0, false) == IA32_STATE_OK);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    struct ia32_interruption rec;
    CHECK(ia32_state_enter(&s, IA32_ENTRY_BRANCH, 0x123, &n, &rec) == IA32_STATE_OK);
    CHECK(ia32_state_jmpe(&s, &n, 0x100, (ia32_eip_t){0x124}, 0, &rec) == IA32_STATE_OK);
    CHECK(!memcmp(s.fr, before.fr, sizeof(s.fr)) && !(s.psr & 0x10));
}
static void transitions(void)
{
    struct ia32_interruption rec;
    init();
    s.rse_dirty = 1;
    before = s;
    CHECK(ia32_state_enter(&s, IA32_ENTRY_BRANCH, 0x123, &n, &rec) == IA32_STATE_ILLEGAL);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    s.rse_dirty = 0;
    s.psr |= UINT64_C(1) << 22;
    before = s;
    CHECK(ia32_state_enter(&s, IA32_ENTRY_BRANCH, 0x123, &n, &rec) == IA32_STATE_DISABLED);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    init();
    s.cfm = 0x1234;
    s.rse_bof = 37;
    s.ar[16] = 3;
    s.psr |= (UINT64_C(3) << 32) | (UINT64_C(1) << 37) | (UINT64_C(1) << 38) | (UINT64_C(1) << 39) |
             (UINT64_C(1) << 43) | (UINT64_C(1) << 45);
    s.ar[24] = 0x10002;
    s.ar[25] = 0x100;
    s.ar[17] = s.ar[18] = 0x2000;
    CHECK(ia32_state_enter(&s, IA32_ENTRY_BRANCH, 0x123, &n, &rec) == IA32_STATE_OK);
    CHECK(s.ip == 0x123 && s.slot == 0 && n.eip.value == 0x23 && n.cpl == 3);
    CHECK(s.cfm == 0 && s.rse_bof == 37 && s.ar[16] == 3 && s.ar[17] == 0x2000);
    /* Vol.3 br p.3:21, rfi p.3:237; Vol.2 §5.3: da/dd/ia/ed
     * clear before IA-32 starts; only id survives rfi until completion. */
    CHECK((s.psr & (UINT64_C(1) << 34)) && !(s.psr & (UINT64_C(1) << 37)) && s.ar[24] == 0x10002);
    CHECK(!(s.psr & ((UINT64_C(1) << 38) | (UINT64_C(1) << 39) | (UINT64_C(1) << 43) |
                     (UINT64_C(1) << 45))));
    CHECK(!rec.valid);
    n.eflag = 0x10102;
    n.eip.value = 0x23;
    s.psr |= UINT64_C(1) << 26;
    CHECK(ia32_state_jmpe_check(&s, &n) == IA32_STATE_OK);
    CHECK(ia32_state_jmpe(&s, &n, 0x135, (ia32_eip_t){0x29}, 5, &rec) == IA32_STATE_OK);
    CHECK(s.ip == 0x230 && s.gr[1] == 0x129 && !s.nat[1] && !(s.psr & (UINT64_C(1) << 34)));
    CHECK(!(s.ar[24] & 0x10000) && ((s.psr >> 32) & 3) == 3 && s.ar[16] == 3);
    CHECK(rec.valid && rec.ivt == 0x6900 && rec.iip == 0x230 && rec.iipa == 0x123);
    CHECK(rec.isr == 0x1005c && rec.phase == IA32_PHASE_COMPLETED && rec.transition);
    init();
    s.psr |= UINT64_C(1) << 34;
    s.psr |= UINT64_C(1) << 22;
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    put(n.fp.bytes + 2, 0x80, 2);
    before = s;
    CHECK(ia32_state_jmpe_check(&s, &n) == IA32_STATE_DISABLED);
    CHECK(ia32_state_jmpe(&s, &n, 0, (ia32_eip_t){1}, 0, &rec) == IA32_STATE_DISABLED);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    s.psr &= ~(UINT64_C(1) << 22);
    CHECK(ia32_state_jmpe_check(&s, &n) == IA32_STATE_FP_PENDING);
    init();
    s.cr[16] = s.psr | (UINT64_C(1) << 34) | (UINT64_C(1) << 22) | (UINT64_C(1) << 37);
    s.cr[16] |=
        (UINT64_C(1) << 38) | (UINT64_C(1) << 39) | (UINT64_C(1) << 43) | (UINT64_C(1) << 45);
    s.cr[19] = 0x123;
    s.cr[23] = UINT64_C(1) << 63 | 0x1234;
    s.cfm = 64;
    s.rse_dirty = 64;
    s.rse_missing = 8;
    s.rse_cfle = true;
    CHECK(ia32_state_enter(&s, IA32_ENTRY_RFI, 0, &n, &rec) == IA32_STATE_OK);
    CHECK(s.ip == 0x123 && !s.cfm && !s.rse_dirty && !s.rse_missing && !s.rse_cfle);
    CHECK(s.psr & (UINT64_C(1) << 37));
    CHECK(!(s.psr & ((UINT64_C(1) << 38) | (UINT64_C(1) << 39) | (UINT64_C(1) << 43) |
                     (UINT64_C(1) << 45))));
    CHECK(s.cr[19] == 0x123 && s.cr[23] == (UINT64_C(1) << 63 | 0x1234));
    init();
    s.psr |= (UINT64_C(1) << 26) | (UINT64_C(1) << 40);
    CHECK(ia32_state_enter(&s, IA32_ENTRY_BRANCH, UINT64_C(0x1234567800000123), &n, &rec) ==
          IA32_STATE_OK);
    CHECK(rec.valid && rec.ivt == 0x5f00 && rec.isr == 12 &&
          rec.iip == UINT64_C(0x1234567800000123));
    init();
    s.psr |= UINT64_C(1) << 40;
    CHECK(ia32_state_enter(&s, IA32_ENTRY_BRANCH, 0x123, &n, &rec) == IA32_STATE_OK);
    CHECK(rec.valid && rec.ivt == 0x6000 && rec.isr == 8);
    init();
    s.gr[16] = 0xaaaa;
    s.bank_gr[0] = UINT64_C(0x4444333322221111);
    s.gr[27] = 0xbbbb;
    s.bank_gr[11] = 0xcccc;
    s.nat[16] = true;
    s.cr[16] = s.psr | (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    s.cr[19] = 0x321;
    s.ar[17] = 0x2000;
    s.ar[18] = 0x1ff0;
    s.ar[19] = UINT64_MAX;
    CHECK(ia32_state_enter(&s, IA32_ENTRY_RFI, 0, &n, &rec) == IA32_STATE_OK);
    CHECK(n.ds.selector == 0x1111 && n.es.selector == 0x2222 && n.ds.base == 0xcccc);
    CHECK(s.gr[16] == UINT64_C(0x4444333322221111) && s.bank_gr[0] == 0xaaaa);
    CHECK(!s.nat[16] && s.bank_nat[0] && s.ar[17] == s.ar[18] && s.ar[19] == 0);
    init();
    s.psr |= UINT64_C(1) << 1;
    CHECK(ia32_state_enter(&s, IA32_ENTRY_BRANCH, 0x123, &n, &rec) == IA32_STATE_OK);
    CHECK(s.psr & (UINT64_C(1) << 1));
    CHECK(ia32_state_jmpe(&s, &n, 0x200, (ia32_eip_t){0x124}, 0, &rec) == IA32_STATE_OK);
    CHECK(s.psr & (UINT64_C(1) << 1));
    init();
    s.cpu_model = IA64_CPU_MONTECITO;
    before = s;
    CHECK(ia32_state_enter(&s, IA32_ENTRY_BRANCH, 0, &n, &rec) == IA32_STATE_DISABLED);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    for (unsigned bit = 0; bit < 4; ++bit) {
        init();
        if (bit == 3)
            s.psr |= UINT64_C(1) << 35;
        else
            s.psr &= ~(UINT64_C(1) << (bit == 0 ? 13 : bit == 1 ? 17 : 36));
        before = s;
        CHECK(ia32_state_enter(&s, IA32_ENTRY_BRANCH, 0, &n, &rec) == IA32_STATE_UNDEFINED);
        CHECK(!memcmp(&s, &before, sizeof(s)));
    }
}
static void bank_restoration(void)
{
    /* Vol.3 rfi; Vol.2 Table 3-2 BN, Vol.1 §6.2.2.1 mapped NaTs.
     * Both bank changes must inspect the bank selected by IPSR. */
    struct ia32_interruption rec;
    for (unsigned bn = 0; bn < 2; ++bn) {
        init();
        s.psr |= (uint64_t)bn << 44;
        s.gr[16] = 0xaaaa;
        s.bank_gr[0] = 0x1111;
        s.gr[27] = 0xbbbb;
        s.bank_gr[11] = 0xcccc;
        s.cr[16] = (s.psr ^ (UINT64_C(1) << 44)) | UINT64_C(1) << 34;
        s.cr[19] = 0x123;
        s.bank_nat[11] = true;
        before = s;
        struct ia32_native_state old = n;
        CHECK(ia32_state_enter(&s, IA32_ENTRY_RFI, 0, &n, &rec) == IA32_STATE_UNDEFINED);
        CHECK(!memcmp(&s, &before, sizeof(s)) && !memcmp(&n, &old, sizeof(n)));
        s.bank_nat[11] = false;
        s.nat[27] = true;
        CHECK(ia32_state_enter(&s, IA32_ENTRY_RFI, 0, &n, &rec) == IA32_STATE_OK);
        CHECK(n.ds.selector == 0x1111 && n.ds.base == 0xcccc);
        CHECK(s.gr[16] == 0x1111 && s.bank_gr[0] == 0xaaaa);
        CHECK(!s.nat[27] && s.bank_nat[11]);
        CHECK(((s.psr >> 44) & 1) == !bn);
    }
}
static void native_completion(void)
{
    /* Vol.2 Table 3-2, §5.3 and Vol.3 rfi p.3:237: id survives
     * faults/refills/partial REP, but clears on a completed instruction.
     * EIP equality cannot distinguish a self-branch from no progress. */
    struct ia32_interruption rec;
    for (unsigned leave = 0; leave < 2; ++leave)
        for (unsigned completed = 0; completed < 2; ++completed) {
            init();
            s.cr[16] = s.psr | UINT64_C(1) << 34 | UINT64_C(1) << 37;
            s.cr[19] = 0x123;
            CHECK(ia32_state_enter(&s, IA32_ENTRY_RFI, 0, &n, &rec) == IA32_STATE_OK);
            CHECK(s.psr & (UINT64_C(1) << 37));
            before = s;
            n.cpl = 1;
            enum ia32_state_status status =
                leave ? ia32_state_leave(&s, &n, 0, completed)
                      : ia32_state_import(&s, &n, IA32_DIRTY_ALL, 0, completed);
            CHECK(status == IA32_STATE_UNREPRESENTABLE && !memcmp(&s, &before, sizeof(s)));
            n.cpl = 0;
            n.eflag |= 0x10000; /* Native partial REP may set RF. */
            status = leave ? ia32_state_leave(&s, &n, 0, completed)
                           : ia32_state_import(&s, &n, IA32_DIRTY_ALL, 0, completed);
            CHECK(status == IA32_STATE_OK && s.ip == 0x123);
            CHECK(!!(s.psr & (UINT64_C(1) << 37)) == !completed);
            CHECK(s.ar[24] & 0x10000);
        }
    init();
    s.psr |= UINT64_C(1) << 34 | UINT64_C(1) << 37;
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    CHECK(ia32_state_jmpe(&s, &n, 0, (ia32_eip_t){1}, 0, &rec) == IA32_STATE_OK);
    CHECK(!(s.psr & (UINT64_C(1) << 37)));
}
struct operand_reader {
    unsigned reads;
    enum ia32_state_status status;
};
static enum ia32_state_status read_operand(void *opaque, uint32_t *operand)
{
    struct operand_reader *reader = opaque;
    ++reader->reads;
    CHECK(!memcmp(&s, &before, sizeof(s)));
    *operand = 0x35;
    return reader->status;
}
static void jmpe_memory(void)
{
    /* Vol.4 JMPE pp.4:249-250: DI, then pending numeric exceptions,
     * then operand access, then CS-base addition modulo 32 bits,
     * low-nibble clearing and zero extension. */
    struct ia32_interruption rec;
    struct operand_reader reader = {0, IA32_STATE_UNDEFINED};
    init();
    s.psr |= UINT64_C(1) << 34 | UINT64_C(1) << 22;
    s.ar[25] = 0xfffffff7;
    s.ip = 0xfffffffb;
    CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
    put(n.fp.bytes + 2, 0x80, 2);
    before = s;
    CHECK(ia32_state_jmpe_memory(&s, &n, read_operand, &reader, (ia32_eip_t){0x13}, 0, &rec) ==
          IA32_STATE_DISABLED);
    CHECK(!reader.reads && !rec.valid && !memcmp(&s, &before, sizeof(s)));
    s.psr &= ~(UINT64_C(1) << 22);
    before = s;
    CHECK(ia32_state_jmpe_memory(&s, &n, read_operand, &reader, (ia32_eip_t){0x13}, 0, &rec) ==
          IA32_STATE_FP_PENDING);
    CHECK(!reader.reads && !rec.valid && !memcmp(&s, &before, sizeof(s)));
    put(n.fp.bytes + 2, 0, 2);
    CHECK(ia32_state_jmpe_memory(&s, &n, read_operand, &reader, (ia32_eip_t){0x13}, 0, &rec) ==
          IA32_STATE_UNDEFINED);
    CHECK(reader.reads == 1 && !rec.valid && !memcmp(&s, &before, sizeof(s)));
    reader.status = IA32_STATE_OK;
    CHECK(ia32_state_jmpe_memory(&s, &n, read_operand, &reader, (ia32_eip_t){0x13}, 0, &rec) ==
          IA32_STATE_OK);
    CHECK(reader.reads == 2 && s.ip == 0x20 && s.gr[1] == 0xa);
}
static void jmpe_debug_records(void)
{
    /* Vol.2 §9.2 p.2:216: IIPA is defined for a Debug trap only if
     * there was a taken branch, including a JMPE taken branch trap. */
    struct ia32_interruption rec;
    for (unsigned traps = 1; traps < 8; ++traps) {
        init();
        s.psr |= UINT64_C(1) << 34;
        if (traps & 1)
            s.psr |= UINT64_C(1) << 26;
        if (traps & 2)
            s.psr |= UINT64_C(1) << 40;
        s.ip = 0x123;
        CHECK(ia32_state_export(&s, &n) == IA32_STATE_OK);
        CHECK(ia32_state_jmpe(&s, &n, 0x235, (ia32_eip_t){0x129}, (traps & 4) ? 1 : 0, &rec) ==
              IA32_STATE_OK);
        CHECK(rec.valid && rec.iipa_valid == !!(traps & 1));
        CHECK(rec.iip == 0x230 && rec.iipa == 0x123);
    }
}
static void records(void)
{
    /* Vol.2 §9.2 pp.2:215-217: Debug fault has x=1/code=0;
     * Debug traps have x=0; Breakpoint/Overflow are completed traps.
     * Only Segment Not Present/Stack/GP faults carry error codes
     * (pp.2:221-223). IIPA is defined for Debug traps only when tb=1
     * (p.2:216, not a blanket rule for other exceptions). */
    struct ia32_record_input in = {.kind = IA32_RECORD_EXCEPTION,
                                   .phase = IA32_PHASE_BEFORE,
                                   .source = {0x123},
                                   .next = {0x129},
                                   .next_valid = true};
    struct ia32_interruption rec;
    const unsigned vectors[] = {0, 5, 7, 11, 12, 13, 16, 17, 19};
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        in.vector = vectors[i];
        in.code = (in.vector >= 11 && in.vector <= 13) ? 0x1234 : 0;
        in.ifa_valid = in.vector == 17;
        in.ifa = 0x987;
        CHECK(ia32_make_record(&in, &rec) == IA32_STATE_OK);
        CHECK(rec.valid && rec.ivt == 0x6900 && rec.iip == 0x123 && rec.iipa == 0x123);
        CHECK(rec.isr == ((uint64_t)in.vector << 16 | in.code));
        CHECK(rec.ifa_valid == (in.vector == 17));
        if (in.vector < 11 || in.vector > 13) {
            in.code = 1;
            CHECK(ia32_make_record(&in, &rec) == IA32_STATE_UNREPRESENTABLE && !rec.valid);
            in.code = 0;
        }
    }
    in.vector = 1;
    in.code = 0;
    in.execute = true;
    in.ifa_valid = false;
    CHECK(ia32_make_record(&in, &rec) == IA32_STATE_OK && rec.isr == UINT64_C(0x100010000));
    in.code = 8;
    CHECK(ia32_make_record(&in, &rec) == IA32_STATE_UNREPRESENTABLE && !rec.valid);
    in.code = 0;
    in.phase = IA32_PHASE_COMPLETED;
    CHECK(ia32_make_record(&in, &rec) == IA32_STATE_UNREPRESENTABLE && !rec.valid);
    in.execute = false;
    in.phase = IA32_PHASE_COMPLETED;
    in.code = 0x1c;
    CHECK(ia32_make_record(&in, &rec) == IA32_STATE_OK && rec.iip == 0x129 && rec.iipa_valid);
    in.code = 8;
    CHECK(ia32_make_record(&in, &rec) == IA32_STATE_OK && !rec.iipa_valid);
    for (unsigned vector = 0; vector < 256; ++vector) {
        in.kind = IA32_RECORD_SOFTWARE_INT;
        in.vector = vector;
        in.code = 0x18;
        CHECK(ia32_make_record(&in, &rec) == IA32_STATE_OK);
        CHECK(rec.ivt == 0x6b00 && rec.isr == ((uint64_t)vector << 16 | 0x18));
        CHECK(rec.iip == 0x129 && rec.iipa == 0x123 && rec.iipa_valid);
    }
    in.kind = IA32_RECORD_INSTRUCTION;
    in.phase = IA32_PHASE_BEFORE;
    in.vector = 0;
    in.code = 0x5046;
    in.iim = UINT64_C(0x8877665544332211);
    CHECK(ia32_make_record(&in, &rec) == IA32_STATE_OK && rec.ivt == 0x6a00 && rec.isr == 0x5046);
    CHECK(rec.iim_valid && rec.iim == in.iim && rec.iip == 0x123);
    in.kind = IA32_RECORD_GATE;
    in.phase = IA32_PHASE_COMPLETED;
    in.code = 0x4018;
    in.ifa_valid = true;
    in.ifa = 0x1234;
    CHECK(ia32_make_record(&in, &rec) == IA32_STATE_OK && rec.isr == 0x14018 && rec.ifa == 0x1234);
    in.kind = IA32_RECORD_SYSTEM_FLAG;
    in.code = 0xc008;
    in.ifa_valid = false;
    in.iim = 0x202;
    CHECK(ia32_make_record(&in, &rec) == IA32_STATE_OK && rec.isr == 0x2c008 && rec.iim == 0x202);
    in.phase = IA32_PHASE_BEFORE;
    in.code = 0xc000;
    CHECK(ia32_make_record(&in, &rec) == IA32_STATE_OK && rec.iip == 0x123);
    in.kind = IA32_RECORD_LOCK;
    in.code = 0;
    in.ifa_valid = true;
    in.read = in.write = true;
    CHECK(ia32_make_record(&in, &rec) == IA32_STATE_OK && rec.isr == UINT64_C(0x600040000));
    in.phase = IA32_PHASE_UNKNOWN;
    CHECK(ia32_make_record(&in, &rec) == IA32_STATE_UNREPRESENTABLE && !rec.valid);
    in.kind = IA32_RECORD_SOFTWARE_INT;
    in.phase = IA32_PHASE_COMPLETED;
    in.next_valid = false;
    CHECK(ia32_make_record(&in, &rec) == IA32_STATE_UNREPRESENTABLE && !rec.valid);
    in = (struct ia32_record_input){.kind = IA32_RECORD_EXCEPTION,
                                    .phase = IA32_PHASE_COMPLETED,
                                    .source = {0xfffffff0},
                                    .next = {3},
                                    .next_valid = true};
    for (unsigned vector = 3; vector <= 4; ++vector) {
        in.vector = vector;
        in.code = 0xfc;
        CHECK(ia32_make_record(&in, &rec) == IA32_STATE_OK);
        CHECK(rec.ivt == 0x6900 && rec.iip == 3 && rec.iipa == 0xfffffff0 &&
              rec.isr == ((uint64_t)vector << 16 | 0xfc));
        in.phase = IA32_PHASE_BEFORE;
        CHECK(ia32_make_record(&in, &rec) == IA32_STATE_UNREPRESENTABLE && !rec.valid);
        in.phase = IA32_PHASE_COMPLETED;
    }
    in.phase = IA32_PHASE_PARTIAL;
    in.vector = 13;
    in.code = 0;
    CHECK(ia32_make_record(&in, &rec) == IA32_STATE_OK && rec.iip == 0xfffffff0 &&
          rec.phase == IA32_PHASE_PARTIAL);
    const unsigned invalid[] = {2, 6, 8, 9, 10, 14, 18, 255};
    in.phase = IA32_PHASE_BEFORE;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        in.vector = invalid[i];
        CHECK(ia32_make_record(&in, &rec) == IA32_STATE_UNREPRESENTABLE && !rec.valid);
    }
}
int main(void)
{
    descriptors();
    segment_boundaries();
    segment_integrity();
    mxcsr_reserved();
    extended_denormals();
    integer_bits();
    floating();
    tag_words();
    mapping();
    defined_bits();
    disabled_fp();
    transitions();
    bank_restoration();
    native_completion();
    jmpe_memory();
    jmpe_debug_records();
    records();
    printf("ia32 state: %lu checks passed\n", checks);
    return 0;
}
