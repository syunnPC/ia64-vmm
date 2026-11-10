/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AMD APM Vol.2 rev.3.44 sections 11.4.4, 15.5-15.12 and Appendix B/C. */
#include <stdio.h>
#include <string.h>
#include "host/amd64/ia32_svm.h"
#include "tests/host/test.h"
static uint8_t vmcb[4096];
static uint64_t readn(unsigned offset, unsigned width)
{
    uint64_t value = 0;
    memcpy(&value, vmcb + offset, width);
    return value;
}
static void writen(unsigned offset, uint64_t value, unsigned width)
{
    memcpy(vmcb + offset, &value, width);
}
static void controls(void)
{
    const struct svm_vmcb_params p = {0x1000, 0x4000, 0x6000, 2};
    amd64_svm_ia32_controls(vmcb, &p);
    CHECK(readn(0, 4) == UINT32_MAX && readn(4, 4) == UINT32_MAX);
    /* Vectors 2 and 9 are reserved, not implemented intercepts. */
    CHECK(!(readn(8, 4) & ((1u << 2) | (1u << 9))));
    CHECK(readn(8, 4) & (1u << 1)); /* Private stepping #DB always exits. */
    CHECK((readn(8, 4) & ((1u << 6) | (1u << 14) | (1u << 19))) ==
          ((1u << 6) | (1u << 14) | (1u << 19)));
    CHECK(readn(12, 4) == UINT32_MAX - (1u << 4) - (1u << 5) - (1u << 23));
    CHECK((readn(16, 4) & 0x3fff) == 0x3fff && !(readn(16, 4) >> 14));
    CHECK(readn(0x40, 8) == p.iopm_pa && readn(0x48, 8) == p.msrpm_pa);
    CHECK(readn(0x58, 4) == 2 && readn(0x60, 4) == (1u << 24));
    CHECK(readn(0x90, 8) == 1 && readn(0xb0, 8) == p.n_cr3);
    CHECK(!readn(0xa8, 8) && !readn(0xb8, 8) && !readn(0xc0, 8));
}
static struct ia32_native_state good_state(void)
{
    struct ia32_native_state n = {.cpl = 3,
                                  .eflag = 0x202,
                                  .cflg = (UINT64_C(1) << 41) | 1,
                                  .kr0 = 0x12340000,
                                  .kr1 = 0x23450000,
                                  .kr2 = 0xabcdef0123456789,
                                  .eip = {0x73},
                                  .interrupt_shadow = 1,
                                  .dr6 = 0xffff0ff0,
                                  .dr7 = 0x400};
    n.cs = (struct ia32_segment){
        .base = 0x120000, .limit = 0xfffff, .selector = 0x23, .attrib = 0xcfb};
    n.ss = n.ds = n.es = n.fs = n.gs =
        (struct ia32_segment){.limit = 0xffffffff, .selector = 0x2b, .attrib = 0xcf3};
    n.ldtr = (struct ia32_segment){.limit = 0xffff, .attrib = 0x82};
    n.tr = (struct ia32_segment){.limit = 0x67, .attrib = 0x8b};
    n.gdtr = (struct ia32_segment){.base = 0x456000, .limit = 0xffff, .attrib = 0xc93};
    n.fp.bytes[0] = 0x7f;
    n.fp.bytes[1] = 3;
    n.fp.bytes[24] = 0x80;
    n.fp.bytes[25] = 0x1f;
    for (unsigned i = 0; i < 8; ++i)
        n.gpr[i] = 0x80001000 + i;
    for (unsigned i = 0; i < 4; ++i)
        n.dr[i] = 0x12340000 + i * 4;
    return n;
}
static void state(void)
{
    struct ia32_native_state n = good_state(), out = n;
    struct amd64_svm_ia32_gprs g;
    const char *why;
    CHECK(amd64_svm_ia32_state_valid(&n, &why));
    /* APM Vol.3 SYSENTER/SYSEXIT: a null SYSENTER_CS forces #GP
     * before transfer. VMLOAD/VMSAVE owns these private linkage fields. */
    writen(0x628, 0x23, 8);
    writen(0x630, 0x12345678, 8);
    writen(0x638, 0x87654321, 8);
    amd64_svm_ia32_put(vmcb, &g, &n);
    CHECK(!readn(0x628, 8) && !readn(0x630, 8) && !readn(0x638, 8));
    CHECK(readn(0x558, 8) == 0x31 && readn(0x550, 8) == 0);
    CHECK(readn(0x548, 8) == 0x640 && readn(0x4d0, 8) == 0x1000);
    CHECK(readn(0x570, 8) == n.eflag && readn(0x578, 8) == n.eip.value);
    CHECK(readn(0x400, 2) == n.es.selector && readn(0x418, 8) == n.cs.base);
    CHECK(readn(0x4cb, 1) == 3 && readn(0x68, 8) == 1);
    CHECK(g.rbx == n.gpr[IA32_EBX] && g.rcx == n.gpr[IA32_ECX] && g.dr[3] == n.dr[3]);
    CHECK(!g.r8 && !g.r15 && !readn(0x480, 8));
    amd64_svm_ia32_get(vmcb, &g, &out);
    CHECK(!memcmp(&out, &n, sizeof(n)));
    /* APM 15.5.1 observes CS D/L/P/R; its effective limit is byte granular.
     * A stopped hardware roundtrip must retain the architectural cache G. */
    writen(0x412, (n.cs.attrib & 0x682) | 0x11d, 2);
    writen(0x422, n.ss.attrib & ~0x961u, 2);
    writen(0x432, n.ds.attrib & ~0x901u, 2);
    /* GDTR selector and attributes are reserved, never architectural state. */
    writen(0x460, UINT32_MAX, 4);
    amd64_svm_ia32_get(vmcb, &g, &out);
    CHECK(!memcmp(&out, &n, sizeof(n)));
    n.cflg |= UINT64_C(0x4002e) | (UINT64_C(1) << 42);
    amd64_svm_ia32_put(vmcb, &g, &n);
    CHECK(readn(0x558, 8) == 0x4003f && readn(0x548, 8) == 0x640);
    n.ldtr = (struct ia32_segment){0};
    n.tr = (struct ia32_segment){0};
    n.fs.unusable = true;
    n.fs.selector = 0;
    CHECK(amd64_svm_ia32_state_valid(&n, &why));
    out = n;
    amd64_svm_ia32_put(vmcb, &g, &n);
    CHECK(readn(0x472, 2) == 0x82 && readn(0x492, 2) == 0x8b);
    CHECK(!readn(0x442, 2));
    amd64_svm_ia32_get(vmcb, &g, &out);
    CHECK(!memcmp(&out, &n, sizeof(n)));
    struct ia32_native_state bad = n;
    bad.cpl = 4;
    CHECK(!amd64_svm_ia32_state_valid(&bad, &why));
    bad = n;
    bad.cs.attrib |= 0x200;
    CHECK(!amd64_svm_ia32_state_valid(&bad, &why));
    bad = n;
    bad.eflag |= 1u << 17;
    bad.cpl = 3;
    bad.cflg |= 1;
    bad.cs = bad.ss = bad.ds = bad.es = bad.fs = bad.gs =
        (struct ia32_segment){.base = 0x12340, .limit = 0xffff, .selector = 0x1234, .attrib = 0xf3};
    bad.gdtr.limit = UINT32_MAX;
    CHECK(amd64_svm_ia32_state_valid(&bad, &why));
    out = bad;
    amd64_svm_ia32_put(vmcb, &g, &bad);
    CHECK(!(readn(0x570, 8) & (1u << 17)));
    CHECK(readn(0x4cb, 1) == 3 && readn(0x412, 2) == 0xfb);
    CHECK(readn(0x414, 4) == 0xffff && readn(0x418, 8) == 0x12340);
    CHECK(readn(0x464, 4) == 0xffff);
    amd64_svm_ia32_get(vmcb, &g, &out);
    CHECK(!memcmp(&out, &bad, sizeof(bad)));
    bad = n;
    bad.ds.base = UINT64_C(1) << 32;
    CHECK(!amd64_svm_ia32_state_valid(&bad, &why));
    bad = n;
    bad.interrupt_shadow = 2;
    CHECK(!amd64_svm_ia32_state_valid(&bad, &why));
    bad = n;
    bad.pending_debug = 1;
    CHECK(!amd64_svm_ia32_state_valid(&bad, &why));
    bad = n;
    bad.fp.bytes[26] = 1;
    CHECK(!amd64_svm_ia32_state_valid(&bad, &why));
}
static void exits(void)
{
    struct ia32_native_exit e;
    memset(vmcb, 0, sizeof(vmcb));
    writen(0x578, 0x7b, 8);
    writen(0x418, 0x123000, 8);
    writen(0x570, 0x202, 8);
    writen(0x70, 0x72, 8);
    writen(0xc8, 0x7d, 8);
    e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.reason == IA32_EXIT_CPUID && e.phase == IA32_PHASE_BEFORE);
    CHECK(e.eip.value == 0x7b && e.linear_ip.value == 0x12307b && e.position_valid);
    CHECK(e.next_eip_valid && e.next_eip.value == 0x7d && !e.length_valid);
    e = amd64_svm_ia32_decode(vmcb, false, false);
    CHECK(!e.next_eip_valid);
    writen(0x70, 0x400, 8);
    writen(0x78, 0x100000003, 8);
    writen(0x80, 0x1234567b, 8);
    e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.reason == IA32_EXIT_NESTED_FAULT && e.phase == IA32_PHASE_UNKNOWN);
    CHECK(e.access.token_valid && e.access.token.value == 0x1234567b && !e.access.linear_valid);
    CHECK(e.access.kind == IA32_ACCESS_WRITE && !e.next_eip_valid);
    writen(0x78, 0x100000010, 8);
    e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.access.kind == IA32_ACCESS_FETCH);
    writen(0x78, UINT64_C(1) << 33, 8);
    e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.access.kind == IA32_ACCESS_PRIVATE_WALK);
    writen(0x70, 0x46, 8);
    writen(0x78, 0xfeed, 8);
    writen(0x80, 0xbeef, 8);
    e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.reason == IA32_EXIT_EXCEPTION && e.exception.valid && e.exception.vector == 6);
    CHECK(e.phase == IA32_PHASE_BEFORE && !e.exception.error_valid && !e.qualification && !e.info2);
    writen(0x70, 0x4e, 8);
    e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.exception.error_valid && e.exception.error == 0xfeed && e.info2 == 0xbeef);
    writen(0x70, 0x41, 8);
    e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.phase == IA32_PHASE_UNKNOWN);
    writen(0x70, 0x7b, 8);
    writen(0x80, 0x80, 8);
    writen(0xc8, 0x99, 8);
    e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.reason == IA32_EXIT_IO && e.next_eip.value == 0x80);
    writen(0x70, 0x7d, 8);
    e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.phase == IA32_PHASE_UNKNOWN && !e.next_eip_valid);
    writen(0x70, 0x7e, 8);
    e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.phase == IA32_PHASE_UNKNOWN && !e.next_eip_valid);
    writen(0x70, 0, 8);
    writen(0x78, UINT64_C(1) << 63 | 3, 8);
    e = amd64_svm_ia32_decode(vmcb, true, false);
    CHECK(!e.qualification);
    e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.qualification == (UINT64_C(1) << 63 | 3));
    writen(0x88, UINT64_C(0xabcdef0180000b0d), 8);
    e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.vectoring.valid && e.vectoring.vector == 13 && e.vectoring.type == 3);
    CHECK(e.vectoring.error_valid && e.vectoring.error == 0xabcdef01);
    writen(0x70, UINT64_MAX, 8);
    e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.reason == IA32_EXIT_ENTRY_FAILURE && !e.position_valid && !e.next_eip_valid);
}
static void segment_geometry(void)
{
    /* Itanium Vol.1 Table 6-4 permits readable code through data/SS
     * caches. A code type's conforming bit is not data expand-down. */
    const unsigned references[] = {0, 2, 3, 4, 5};
    for (unsigned r = 0; r < sizeof(references) / sizeof(references[0]); ++r)
        for (unsigned expand = 0; expand < 2; ++expand) {
            struct ia32_native_state n = good_state();
            struct ia32_segment *segments[] = {&n.es, &n.cs, &n.ss, &n.ds, &n.fs, &n.gs};
            unsigned i = references[r];
            segments[i]->base = 0x12000;
            segments[i]->limit = 0xffff;
            segments[i]->attrib = expand ? (i == 2 ? 0xcf7 : 0xc97)
                                        : (i == 2 ? 0xcff : 0xc9f);
            struct ia32_native_state before = n, out = n;
            struct amd64_svm_ia32_gprs g;
            const char *why;
            CHECK(amd64_svm_ia32_state_valid(&n, &why));
            amd64_svm_ia32_put(vmcb, &g, &n);
            CHECK(readn(0x402 + 16 * i, 2) == (expand ? 0xcf7 : 0xcf3));
            CHECK(readn(0x404 + 16 * i, 4) == 0xffff);
            CHECK(readn(0x408 + 16 * i, 8) == 0x12000);
            CHECK(!memcmp(&n, &before, sizeof(n)));
            amd64_svm_ia32_get(vmcb, &g, &out);
            CHECK(!memcmp(&out, &before, sizeof(out)));
        }
}
static void exception_phases(void)
{
    /* APM Vol.2 15.7.1, 15.12: BP/OF save the trapping instruction;
     * DB can be a fault or a trap, including an interrupted REP. */
    const unsigned before[] = {0, 3, 4, 5, 6, 7, 16, 17, 19};
    const unsigned ambiguous[] = {1, 8, 9, 10, 11, 12, 13, 14, 15, 18, 20, 21, 30};
    memset(vmcb, 0, sizeof(vmcb));
    writen(VMCB_RIP, 0x123, 8);
    writen(VMCB_NRIP, 0x125, 8);
    for (unsigned i = 0; i < sizeof(before) / sizeof(before[0]); ++i) {
        writen(VMCB_EXITCODE, 0x40 + before[i], 8);
        struct ia32_native_exit e = amd64_svm_ia32_decode(vmcb, true, true);
        CHECK(e.reason == IA32_EXIT_EXCEPTION && e.exception.vector == before[i]);
        CHECK(e.phase == IA32_PHASE_BEFORE && e.position_valid);
        CHECK(e.next_eip_valid == (before[i] >= 3 && before[i] <= 5));
        writen(VMCB_EXITINTINFO, UINT64_C(0x8000030d), 8);
        e = amd64_svm_ia32_decode(vmcb, true, true);
        CHECK(e.phase == IA32_PHASE_UNKNOWN && e.vectoring.valid);
        writen(VMCB_EXITINTINFO, 0, 8);
    }
    for (unsigned i = 0; i < sizeof(ambiguous) / sizeof(ambiguous[0]); ++i) {
        writen(VMCB_EXITCODE, 0x40 + ambiguous[i], 8);
        struct ia32_native_exit e = amd64_svm_ia32_decode(vmcb, true, true);
        CHECK(e.phase == IA32_PHASE_UNKNOWN && !e.next_eip_valid);
        if (ambiguous[i] == 8 || ambiguous[i] == 10)
            CHECK(!e.position_valid);
    }
    /* Sticky DR6 alone cannot identify which fault/trap caused this exit. */
    writen(VMCB_EXITCODE, 0x41, 8);
    writen(VMCB_DR6, UINT64_C(0xffff6ff1), 8);
    struct ia32_native_exit e = amd64_svm_ia32_decode(vmcb, true, true);
    CHECK(e.phase == IA32_PHASE_UNKNOWN);
}
static void native_features(void)
{
    /* CPUID.1:ECX: SSE3 bit 0 and POPCNT bit 23 are independent. */
    CHECK(!amd64_ia32_native_features(0));
    CHECK(amd64_ia32_native_features(1) == IA32_NATIVE_SSE3);
    CHECK(amd64_ia32_native_features(1u << 23) == IA32_NATIVE_POPCNT);
    CHECK(amd64_ia32_native_features(UINT32_MAX) == (IA32_NATIVE_SSE3 | IA32_NATIVE_POPCNT));
    CHECK(!amd64_ia32_native_features(UINT32_MAX & ~(1u | (1u << 23))));
}
int main(void)
{
    controls();
    state();
    segment_geometry();
    exits();
    exception_phases();
    native_features();
    puts("native SVM layout: controls, state admission/roundtrip and precise exit validity passed");
    return 0;
}
