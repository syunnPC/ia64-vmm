/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Literal expectations: Intel SDM Vol.3D Appendices A-C and Vol.3C
 * VM-entry segment checks and VM-exit information. */
#include <stdio.h>
#include <string.h>
#include "host/amd64/vmx_ia32.h"

static unsigned failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x);                                \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static struct ia32_native_state state(void)
{
    struct ia32_native_state s = {0};
    s.cs = (struct ia32_segment){
        .base = 0x1000, .limit = 0xffffffff, .selector = 0x1b, .attrib = 0xcfb};
    s.ss = s.ds = s.es = s.fs = s.gs =
        (struct ia32_segment){.limit = 0xffffffff, .selector = 0x23, .attrib = 0xcf3};
    s.ldtr.unusable = true;
    s.tr = (struct ia32_segment){.base = 0x2000, .limit = 0x67, .selector = 0x28, .attrib = 0x8b};
    s.gdtr.base = 0x3000;
    s.gdtr.limit = 0xff;
    s.gdtr.attrib = 0xc93;
    s.idtr.base = 0x4000;
    s.idtr.limit = 0xff;
    s.cpl = 3;
    s.cflg = (UINT64_C(3) << 41) | 0x21;
    s.eflag = 0x202;
    s.eip.value = 0xfffffff0;
    s.gpr[4] = 0x1234;
    s.dr7 = 0x400;
    s.interruptibility = 2;
    s.pending_debug = 0x4000;
    return s;
}
static uint64_t value(const struct vmx_field *f, unsigned n, uint32_t encoding)
{
    for (unsigned i = 0; i < n; ++i)
        if (f[i].encoding == encoding)
            return f[i].value;
    ++failures;
    return 0;
}
struct vmread_fixture {
    const struct vmx_field *f;
    unsigned n;
};
static uint64_t read_field(void *p, uint32_t encoding)
{
    struct vmread_fixture *fixture = p;
    return value(fixture->f, fixture->n, encoding);
}
static void test_controls(void)
{
    struct vmx_caps c = {.proc_msr = UINT64_C(0xffffffff00000000),
                         .proc2_msr = UINT64_C(0xff00000000),
                         .pin_msr = UINT64_C(0x7f00000000),
                         .exit_msr = UINT64_C(0x7fffff00000000),
                         .entry_msr = UINT64_C(0xffff00000000),
                         .basic = UINT64_C(1) << 54,
                         .ept_vpid = (UINT64_C(1) << 25) | (UINT64_C(1) << 26) | 1};
    struct ia32_capabilities out;
    struct ia32_setup_error error;
    CHECK(vmx_ia32_controls(&c, &out, &error));
    /* All MSRs exit (no MSR bitmap), every I/O port exits, clocks,
     * debug, INVLPG, descriptor operations and CR3 are intercepted. */
    CHECK((c.proc & 0x22819e80u) == 0x22819e80u);
    CHECK(!(c.proc & ((1u << 27) | (1u << 28))));
    CHECK((c.proc2 & 0xceu) == 0xceu && !(c.proc2 & (1u << 5)));
    CHECK((c.pin & 9) == 9 && !(c.exit & (1u << 15)));
    CHECK(out.backend == IA32_BACKEND_VMX && out.mtf && out.descriptor_read_exit);
    CHECK(out.vmx_ins_outs_info && out.invept_types == 6 && out.ept_execute_only);
    c.proc_msr &= ~(UINT64_C(1) << (32 + 27));
    CHECK(vmx_ia32_controls(&c, &out, &error) && !out.mtf);
    c.proc2_msr &= ~(UINT64_C(1) << (32 + 2));
    CHECK(!vmx_ia32_controls(&c, &out, &error));
}
static void test_state(void)
{
    struct ia32_native_state s = state(), out = s;
    struct vmx_caps c = {.cr0_fixed0 = 0x80000021,
                         .cr0_fixed1 = 0xffffffff,
                         .cr4_fixed0 = 0x2000,
                         .cr4_fixed1 = 0xffffffff};
    struct ia32_setup_error error;
    struct vmx_field f[112];
    CHECK(vmx_ia32_validate(&s, &error));
    CHECK(vmx_ia32_control_state(&c, &s, &error));
    s.cflg &= ~UINT64_C(0x20);
    CHECK(vmx_ia32_control_state(&c, &s, &error));
    s.cflg |= 0x20;
    unsigned n = vmx_ia32_put_state(&c, &s, f, 112);
    CHECK(n && !vmx_ia32_put_state(&c, &s, f, 1));
    CHECK(value(f, n, 0x480e) == 0x67 && value(f, n, 0x080e) == 0);
    CHECK(value(f, n, 0x4822) == 0x8b && value(f, n, 0x4820) == 0x10000);
    CHECK(value(f, n, 0x4816) == 0xc0fb && value(f, n, 0x6808) == 0x1000);
    CHECK(value(f, n, 0x6800) == 0x31 && value(f, n, 0x6802) == 0);
    CHECK(value(f, n, 0x6804) == 0x2600);
    CHECK(value(f, n, 0x6820) == 0x202 && value(f, n, 0x4824) == 2);
    CHECK(value(f, n, 0x6822) == 0x4000);
    s.cflg = 0x4000e;
    CHECK(vmx_ia32_put_state(&c, &s, f, 112) == n);
    CHECK(value(f, n, 0x6800) == 0x4003f && value(f, n, 0x6804) == 0x2400);
    s = state();
    CHECK(vmx_ia32_put_state(&c, &s, f, 112) == n);
    struct vmread_fixture fixture = {f, n};
    vmx_ia32_get_state(read_field, &fixture, &out);
    CHECK(!memcmp(&s.es, &out.es, sizeof(s.es)));
    CHECK(!memcmp(&s.cs, &out.cs, sizeof(s.cs)));
    CHECK(!memcmp(&s.ldtr, &out.ldtr, sizeof(s.ldtr)));
    CHECK(!memcmp(&s.tr, &out.tr, sizeof(s.tr)));
    CHECK(out.interruptibility == 2 && out.pending_debug == 0x4000);
    CHECK(out.cpl == 3 && out.eip.value == s.eip.value);
    CHECK(!memcmp(&out.gdtr, &s.gdtr, sizeof(s.gdtr)));
    s.cpl = 0;
    CHECK(vmx_ia32_validate(&s, &error));
    CHECK(vmx_ia32_put_state(&c, &s, f, 112) == n);
    CHECK((value(f, n, 0x4818) & 0x60) == 0);
    CHECK((value(f, n, 0x0802) & 3) == 0 && (value(f, n, 0x0804) & 3) == 0);
    out = s;
    vmx_ia32_get_state(read_field, &fixture, &out);
    CHECK(!memcmp(&out.ss, &s.ss, sizeof(s.ss)) && out.cpl == 0);
    s = state();
    s.ds.attrib &= ~1u;
    CHECK(vmx_ia32_validate(&s, &error));
    CHECK(vmx_ia32_put_state(&c, &s, f, 112) == n);
    CHECK(value(f, n, 0x481a) & 1);
    out = s;
    vmx_ia32_get_state(read_field, &fixture, &out);
    CHECK(!memcmp(&out.ds, &s.ds, sizeof(s.ds)));
    s = state();
    s.cs.base = UINT64_C(1) << 32;
    CHECK(!vmx_ia32_validate(&s, &error));
    s = state();
    s.eflag |= 1u << 17;
    s.cflg |= 1;
    s.cs = s.ss = s.es = s.ds = s.fs = s.gs =
        (struct ia32_segment){.base = 0x12340, .limit = 0xffff, .selector = 0x1234, .attrib = 0xf3};
    s.gdtr.limit = UINT32_MAX;
    CHECK(vmx_ia32_validate(&s, &error));
    CHECK(vmx_ia32_put_state(&c, &s, f, 112) == n);
    CHECK(!(value(f, n, 0x6820) & (1u << 17)));
    CHECK(value(f, n, 0x4816) == 0xfb && value(f, n, 0x4818) == 0xf3);
    CHECK(value(f, n, 0x6808) == 0x12340 && value(f, n, 0x4802) == 0xffff);
    CHECK(value(f, n, 0x4810) == 0xffff);
    out = s;
    vmx_ia32_get_state(read_field, &fixture, &out);
    CHECK(out.eflag == s.eflag && out.cpl == s.cpl && out.gdtr.limit == UINT32_MAX);
    CHECK(!memcmp(&out.cs, &s.cs, sizeof(s.cs)) && !memcmp(&out.ss, &s.ss, sizeof(s.ss)));
}
static void test_exits(void)
{
    struct ia32_native_state s = state();
    struct vmx_ia32_exit_fields f = {
        .reason = 10, .length = 2, .hardware_eflag = 0x202, .qualification = 0xdead};
    struct ia32_native_exit e = vmx_ia32_decode_exit(&f, &s, false);
    CHECK(e.reason == IA32_EXIT_CPUID && e.phase == IA32_PHASE_BEFORE);
    CHECK(e.length_valid && e.next_eip_valid && e.next_eip.value == 0xfffffff2);
    CHECK(!e.access.token_valid && !e.info2 && !e.exception.valid && !e.qualification);
    f.reason = 48;
    f.qualification = 0x182;
    f.token = 0x12345;
    f.linear = 0x8765;
    e = vmx_ia32_decode_exit(&f, &s, false);
    CHECK(e.reason == IA32_EXIT_NESTED_FAULT && e.phase == IA32_PHASE_UNKNOWN);
    CHECK(e.access.token_valid && e.access.token.value == 0x12345 && !e.access.gpa_valid);
    CHECK(e.access.linear_valid && e.access.linear.value == 0x8765);
    CHECK(e.access.kind == IA32_ACCESS_WRITE && !e.length_valid && !e.next_eip_valid);
    f.reason = 0;
    f.intr_info = 0x80000b0d;
    f.intr_error = 0x18;
    f.vectoring_info = 0x80000b0e;
    f.vectoring_error = 0x44;
    e = vmx_ia32_decode_exit(&f, &s, false);
    CHECK(e.reason == IA32_EXIT_EXCEPTION && e.exception.valid && e.exception.error_valid);
    CHECK(e.exception.vector == 13 && e.exception.error == 0x18);
    CHECK(e.vectoring.valid && e.vectoring.vector == 14 && e.vectoring.error == 0x44);
    f.reason = 37;
    f.intr_info = f.vectoring_info = 0;
    e = vmx_ia32_decode_exit(&f, &s, false);
    CHECK(e.reason == IA32_EXIT_STEP && e.phase == IA32_PHASE_UNKNOWN && !e.length_valid);
    f.reason = 0x80000021;
    e = vmx_ia32_decode_exit(&f, &s, false);
    CHECK(e.reason == IA32_EXIT_ENTRY_FAILURE && e.phase == IA32_PHASE_BEFORE);
    f.reason = 30;
    f.qualification = 0x30;
    f.instruction_info = 0xaabb;
    e = vmx_ia32_decode_exit(&f, &s, false);
    CHECK(!e.instruction_info_valid);
    e = vmx_ia32_decode_exit(&f, &s, true);
    CHECK(e.instruction_info_valid && e.instruction_info == 0xaabb);
}
static void test_segment_geometry(void)
{
    /* Itanium Vol.1 Table 6-4 permits readable code through data/SS
     * caches. Conforming code keeps an expand-up private data cache. */
    const unsigned references[] = {0, 2, 3, 4, 5};
    const struct vmx_caps c = {.cr0_fixed0 = 0x80000021,
                               .cr0_fixed1 = UINT32_MAX,
                               .cr4_fixed0 = 0x2000,
                               .cr4_fixed1 = UINT32_MAX};
    for (unsigned r = 0; r < sizeof(references) / sizeof(references[0]); ++r)
        for (unsigned expand = 0; expand < 2; ++expand) {
            struct ia32_native_state s = state();
            struct ia32_segment *segments[] = {&s.es, &s.cs, &s.ss, &s.ds, &s.fs, &s.gs};
            unsigned i = references[r];
            segments[i]->base = 0x12000;
            segments[i]->limit = 0xffff;
            segments[i]->attrib = expand ? (i == 2 ? 0xcf7 : 0xc97)
                                        : (i == 2 ? 0xcff : 0xc9f);
            struct ia32_native_state before = s, out = s;
            struct ia32_setup_error error;
            struct vmx_field f[112];
            CHECK(vmx_ia32_validate(&s, &error));
            unsigned n = vmx_ia32_put_state(&c, &s, f, 112);
            CHECK(n && value(f, n, 0x4814 + 2 * i) == (expand ? 0xc0f7 : 0xc0f3));
            CHECK(value(f, n, 0x4800 + 2 * i) == 0xffff);
            CHECK(value(f, n, 0x6806 + 2 * i) == 0x12000);
            CHECK(!memcmp(&s, &before, sizeof(s)));
            struct vmread_fixture fixture = {f, n};
            vmx_ia32_get_state(read_field, &fixture, &out);
            CHECK(!memcmp(&out, &before, sizeof(out)));
        }
}

static void test_absent_caches(void)
{
    struct ia32_native_state s = state(), out;
    struct vmx_caps c = {.cr0_fixed0 = 0x80000021,
                         .cr0_fixed1 = UINT32_MAX,
                         .cr4_fixed0 = 0x2000,
                         .cr4_fixed1 = UINT32_MAX};
    s.tr = (struct ia32_segment){.base = 0x4567, .limit = 0x1234, .unusable = true};
    s.ldtr = (struct ia32_segment){.base = 0x9876, .limit = 0x5678};
    s.ds = (struct ia32_segment){.base = 0x123400, .limit = 0x4567, .unusable = true};
    struct ia32_setup_error error;
    CHECK(vmx_ia32_validate(&s, &error));
    struct vmx_field f[112];
    unsigned n = vmx_ia32_put_state(&c, &s, f, 112);
    CHECK(n && value(f, n, 0x4822) == 0x8b && value(f, n, 0x480e) == 0x67);
    CHECK(value(f, n, 0x6814) == 0 && value(f, n, 0x080e) == 0);
    CHECK(value(f, n, 0x4820) == 0x10000 && value(f, n, 0x481a) == 0x10000);
    out = s;
    /* VMX may disregard cache fields for an unusable segment. */
    for (unsigned i = 0; i < n; ++i)
        if (f[i].encoding == 0x680c || f[i].encoding == 0x4806 || f[i].encoding == 0x6812 ||
            f[i].encoding == 0x480c)
            f[i].value = 0;
    struct vmread_fixture fixture = {f, n};
    vmx_ia32_get_state(read_field, &fixture, &out);
    CHECK(!memcmp(&s.tr, &out.tr, sizeof(s.tr)));
    CHECK(!memcmp(&s.ldtr, &out.ldtr, sizeof(s.ldtr)));
    CHECK(!memcmp(&s.ds, &out.ds, sizeof(s.ds)));
    for (unsigned i = 0; i < n; ++i)
        if (f[i].encoding == 0x0806)
            f[i].value = 3;
    vmx_ia32_get_state(read_field, &fixture, &out);
    CHECK(!memcmp(&out.ds, &s.ds, sizeof(s.ds)));
    /* Segment loads are caller assists; private cache changes do not
     * replace any semantic cache or its original ignored fields. */
    for (unsigned i = 0; i < n; ++i)
        if (f[i].encoding == 0x481a)
            f[i].value = 0xc0f3;
    vmx_ia32_get_state(read_field, &fixture, &out);
    CHECK(!memcmp(&out.ds, &s.ds, sizeof(s.ds)));
    s.tr.unusable = false;
    s.tr.attrib = 3;
    CHECK(vmx_ia32_validate(&s, &error));
    n = vmx_ia32_put_state(&c, &s, f, 112);
    CHECK(value(f, n, 0x4822) == 0x8b && value(f, n, 0x6814) == 0);
    out = s;
    fixture.n = n;
    vmx_ia32_get_state(read_field, &fixture, &out);
    CHECK(!memcmp(&s.tr, &out.tr, sizeof(s.tr)));
}
static void test_exception_phases(void)
{
    /* SDM Vol.3A 7.5, 7.15 and Vol.3C 30.2.2: synchronous faults
     * preserve the current instruction; delivery and partial accesses do not. */
    const unsigned before[] = {0, 5, 6, 7, 16, 17, 19};
    const unsigned ambiguous[] = {1, 3, 4, 8, 9, 10, 11, 12, 13, 14, 15, 18, 20, 21, 30};
    struct ia32_native_state s = state();
    struct vmx_ia32_exit_fields f = {.reason = 0, .length = 7};
    for (unsigned i = 0; i < sizeof(before) / sizeof(before[0]); ++i) {
        f.intr_info = UINT32_C(0x80000300) | before[i];
        struct ia32_native_exit e = vmx_ia32_decode_exit(&f, &s, false);
        CHECK(e.reason == IA32_EXIT_EXCEPTION && e.exception.vector == before[i]);
        CHECK(e.phase == IA32_PHASE_BEFORE && e.position_valid);
        CHECK(!e.length_valid && !e.next_eip_valid);
        f.vectoring_info = UINT32_C(0x80000603);
        e = vmx_ia32_decode_exit(&f, &s, false);
        CHECK(e.phase == IA32_PHASE_UNKNOWN && e.vectoring.valid);
        f.vectoring_info = 0;
    }
    for (unsigned i = 0; i < sizeof(ambiguous) / sizeof(ambiguous[0]); ++i) {
        f.intr_info = UINT32_C(0x80000300) | ambiguous[i];
        struct ia32_native_exit e = vmx_ia32_decode_exit(&f, &s, false);
        CHECK(e.phase == IA32_PHASE_UNKNOWN && !e.length_valid && !e.next_eip_valid);
        if (ambiguous[i] == 8)
            CHECK(!e.position_valid);
    }
    /* VMX intercepts INT1/INT3/INTO before delivering their trap. */
    const uint32_t software[] = {0x80000501, 0x80000603, 0x80000604};
    for (unsigned i = 0; i < sizeof(software) / sizeof(software[0]); ++i) {
        f.intr_info = software[i];
        struct ia32_native_exit e = vmx_ia32_decode_exit(&f, &s, false);
        CHECK(e.phase == IA32_PHASE_BEFORE && e.length_valid && e.length == 7);
        CHECK(!e.next_eip_valid);
    }
    f.intr_info = UINT32_C(0x80000413);
    struct ia32_native_exit e = vmx_ia32_decode_exit(&f, &s, false);
    CHECK(e.phase == IA32_PHASE_UNKNOWN && !e.length_valid);
    f.intr_info = UINT32_C(0x80000202);
    e = vmx_ia32_decode_exit(&f, &s, false);
    CHECK(e.reason == IA32_EXIT_HOST_NMI && e.phase == IA32_PHASE_UNKNOWN);
    f.intr_info = 0;
    e = vmx_ia32_decode_exit(&f, &s, false);
    CHECK(e.phase == IA32_PHASE_UNKNOWN && !e.exception.valid && !e.length_valid);
    /* Qualification identifies #DB causes but does not prove full REP completion. */
    f.intr_info = UINT32_C(0x80000301);
    const uint64_t debug[] = {0, 1, UINT64_C(1) << 13, UINT64_C(1) << 14};
    for (unsigned i = 0; i < sizeof(debug) / sizeof(debug[0]); ++i) {
        f.qualification = debug[i];
        e = vmx_ia32_decode_exit(&f, &s, false);
        CHECK(e.phase == IA32_PHASE_UNKNOWN && e.qualification == debug[i]);
    }
}
int main(void)
{
    test_controls();
    test_state();
    test_segment_geometry();
    test_exits();
    test_absent_caches();
    test_exception_phases();
    if (failures)
        return 1;
    puts(
        "test-vmx-ia32: native controls, exact cached state, restart state and raw exit fields passed");
    return 0;
}
