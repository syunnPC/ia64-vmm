/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VMX hosted checks of the VMX container's data structures.
 * VMCS encodings,
 * control bits, EPT bits and exit-qualification bits are written here as
 * literals from the Intel SDM (order 325462-093US): Vol.3D Appendix B
 * (encodings), Appendix A (capability MSRs), Vol.3C Tables 27-2, 27-5..27-9,
 * 27-14, 27-17, 27-20, 30-5, 30-7, 31-2..31-7, independently of
 * host/amd64/vmx.h. The VM-entry checker below is written from §29.2 and
 * §29.3.1 alone. Every check runs once on the product's output and once on
 * a broken copy that it must reject.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/amd64/vmx.h"

static unsigned failures;
#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #e);                           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* vmx_layout.c formats its refusals with the VMM's vmm_snprintf (core/log.c
 * on the VMM); the C library's has the same meaning for these formats. */
int vmm_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* --- capability fixtures (Appendix A) ------------------------------------- */

/* A Sandy Bridge-like processor: IA32_VMX_BASIC with revision 10h, 1024-byte
 * VMCS, WB, INS/OUTS information and true controls (bit 55); the default1
 * classes forced in the non-true MSRs; secondary controls up to bit 7; EPT
 * with a 4-level walk, UC and WB structures, 2 MiB leaves, INVEPT single
 * and all-context. */
struct msrs {
    uint64_t v[0x500];
    uint32_t ecx1;
    uint64_t cr0, cr4;
};
static void sandy_bridge(struct msrs *m, bool true_controls)
{
    memset(m, 0, sizeof(*m));
    m->ecx1 = 1u << 5;
    m->v[0x03a] = 0x5;
    m->v[0x480] = true_controls ? 0x00da040000000010ull : 0x005a040000000010ull;
    m->v[0x481] = 0x0000007f00000016ull;
    m->v[0x482] = 0xfff9fffe0401e172ull;
    m->v[0x483] = 0x007fffff00036dffull;
    m->v[0x484] = 0x0000ffff000011ffull;
    m->v[0x485] = 0x00000000100401e5ull;
    m->v[0x486] = 0x80000021ull;
    m->v[0x487] = 0xffffffffull;
    m->v[0x488] = 0x2000ull;
    m->v[0x489] = 0x767ffull;
    m->v[0x48b] = 0x000000ff00000000ull;
    m->v[0x48c] = 0x00000f0106114141ull;
    m->v[0x48d] = 0x0000007f00000016ull;
    m->v[0x48e] = 0xfff9fffe04006172ull; /* CR3-load/store exiting may be 0 */
    m->v[0x48f] = 0x007fffff00036dfbull; /* "save debug controls" may be 0 */
    m->v[0x490] = 0x0000ffff000011fbull; /* "load debug controls" may be 0 */
    m->cr0 = 0x80050033ull;              /* PG AM WP NE ET MP PE */
    m->cr4 = 0x6a0ull;                   /* OSXMMEXCPT OSFXSR PGE PAE */
}
static uint64_t fixture_rdmsr(void *ctx, uint32_t msr)
{
    const struct msrs *m = ctx;
    if (msr >= COUNT(m->v)) {
        fprintf(stderr, "unexpected RDMSR %x\n", msr);
        ++failures;
        return 0;
    }
    return m->v[msr];
}
static bool caps_of(struct msrs *m, struct vmx_caps *c, char *why)
{
    return vmx_capabilities(m->ecx1, 39, m->cr0, m->cr4, fixture_rdmsr, m, c, why, 200);
}

/* A control word is valid when it has every bit the capability MSR forces
 * (allowed-0, bits 31:0) and none it forbids (allowed-1, bits 63:32), the
 * bits the container plans as 1, and none of the bits it plans as 0. */
struct word_rule {
    uint32_t ones, zeros;
};
static const struct word_rule pin_rule = {0x00000009u, 0x00000060u}; /* 0, 3 / 5, 6 */
static const struct word_rule proc_rule = {0x82000080u,
                                           0x18f0100cu}; /* 7, 25, 31 / 2, 3, 12, 21-23, 27, 28 */
static const struct word_rule proc2_rule = {0x000000c2u, 0x0000002cu}; /* 1, 6, 7 / 2, 3, 5 */
static const struct word_rule exit_rule = {0x00200204u, 0x001c8000u};  /* 2, 9, 21 / 15, 18-20 */
static const struct word_rule entry_rule = {0x00008004u, 0x00004600u}; /* 2, 15 / 9, 10, 14 */
static bool word_ok(uint32_t word, uint64_t msr, const struct word_rule *r)
{
    uint32_t allowed0 = (uint32_t)msr, allowed1 = (uint32_t)(msr >> 32);
    return (word & allowed0) == allowed0 && !(word & ~allowed1) && (word & r->ones) == r->ones &&
           !(word & r->zeros);
}
static bool words_ok(const struct vmx_caps *c, const struct msrs *m, bool true_controls)
{
    const uint64_t *v = m->v;
    return word_ok(c->pin, v[true_controls ? 0x48d : 0x481], &pin_rule) &&
           word_ok(c->proc, v[true_controls ? 0x48e : 0x482], &proc_rule) &&
           word_ok(c->proc2, v[0x48b], &proc2_rule) &&
           word_ok(c->exit, v[true_controls ? 0x48f : 0x483], &exit_rule) &&
           word_ok(c->entry, v[true_controls ? 0x490 : 0x484], &entry_rule);
}
/* A refusal is right when create was refused and the reason names it. */
static bool refused_for(bool accepted, const char *why, const char *expected)
{
    return !accepted && strstr(why, expected) != NULL;
}

static void test_capabilities(void)
{
    struct msrs m;
    struct vmx_caps c;
    char why[200];
    for (int t = 0; t < 2; ++t) {
        sandy_bridge(&m, t);
        why[0] = 0;
        bool ok = caps_of(&m, &c, why);
        if (!ok)
            fprintf(stderr, "capabilities refused: %s\n", why);
        CHECK(ok && c.true_controls == (bool)t && c.revision == 0x10 && c.vmcs_size == 1024);
        CHECK(words_ok(&c, &m, t));
        CHECK(c.ept_2m && !c.ept_1g && c.invept_single && c.maxphyaddr == 39);
        /* CR3-load/store exiting (primary 15, 16): forced without true
         * controls, absent with them. */
        CHECK(t ? !(c.proc & 0x18000u) : (c.proc & 0x18000u) == 0x18000u);
        /* Exit and entry keep "save/load debug controls" (bit 2) either way. */
        CHECK((c.exit & 4) && (c.entry & 4));
        /* Broken: "use MSR bitmaps" (28) or acknowledge-on-exit (15) in the
         * words must fail the independent rule. */
        struct vmx_caps bad = c;
        bad.proc |= 1u << 28;
        CHECK(!words_ok(&bad, &m, t));
        bad = c;
        bad.exit |= 1u << 15;
        CHECK(!words_ok(&bad, &m, t));
        bad = c;
        bad.proc2 &= ~0x80u;
        CHECK(!words_ok(&bad, &m, t));
    }
    /* Refusals, one fixture change each, in the order vmx_capabilities reads them. */
    static const struct {
        uint32_t msr;
        uint64_t clear, set;
        const char *expected;
    } refusals[] = {
        {0x03a, 0x1, 0, "IA32_FEATURE_CONTROL unlocked"},
        {0x03a, 0x4, 0, "VMX disabled by firmware"},
        {0x480, 0x000c000000000000ull, 0, "VMCS memory type"},
        {0x480, 0, 0x0001000000000000ull, "bit 48"},
        {0x480, 0, 0x0000100000000000ull, "VMCS size"},
        {0x48e, 0x8000000000000000ull, 0, "no secondary processor-based controls"},
        {0x48b, 0x0000000200000000ull, 0, "no EPT"},
        {0x48b, 0x0000008000000000ull, 0, "no unrestricted guest"},
        {0x48b, 0x0000004000000000ull, 0, "no WBINVD exiting"},
        {0x48c, 0x40, 0, "no 4-level walk"},
        {0x48c, 0x4000, 0, "no write-back EPT structures"},
        {0x48c, 0x100000, 0, "no INVEPT"},
        {0x48c, 0x6000000, 0, "no INVEPT"},
        {0x48d, 0x0000000100000000ull, 0, "pin-based control bit 0 cannot be 1"},
        {0x48d, 0x0000000800000000ull, 0, "pin-based control bit 3 cannot be 1"},
        {0x48e, 0x0000008000000000ull, 0, "primary processor-based control bit 7 cannot be 1"},
        {0x48e, 0x0200000000000000ull, 0, "primary processor-based control bit 25 cannot be 1"},
        {0x48e, 0, 0x10000000ull, "primary processor-based control bit 28 cannot be 0"},
        {0x48f, 0x0020000000000000ull, 0, "VM-exit control bit 21 cannot be 1"},
        {0x48f, 0x0000020000000000ull, 0, "VM-exit control bit 9 cannot be 1"},
        {0x48f, 0, 0x8000ull, "VM-exit control bit 15 cannot be 0"},
        {0x490, 0x0000800000000000ull, 0, "VM-entry control bit 15 cannot be 1"},
        {0x490, 0, 0x200ull, "VM-entry control bit 9 cannot be 0"},
    };
    for (unsigned i = 0; i < COUNT(refusals); ++i) {
        sandy_bridge(&m, true);
        m.v[refusals[i].msr] = (m.v[refusals[i].msr] & ~refusals[i].clear) | refusals[i].set;
        why[0] = 0;
        bool ok = caps_of(&m, &c, why);
        if (!refused_for(ok, why, refusals[i].expected))
            fprintf(stderr, "refusal %u: expected \"%s\", got %s \"%s\"\n", i, refusals[i].expected,
                    ok ? "acceptance" : "refusal", why);
        CHECK(refused_for(ok, why, refusals[i].expected));
    }
    /* Without true controls the non-true MSRs decide: the same refusal for
     * a missing load IA32_EFER exit control. */
    sandy_bridge(&m, false);
    m.v[0x483] &= ~0x0020000000000000ull;
    CHECK(refused_for(caps_of(&m, &c, why), why, "VM-exit control bit 21 cannot be 1"));
    /* CPUID, then host CR0/CR4 against the fixed MSRs (CR4.VMXE excepted). */
    sandy_bridge(&m, true);
    m.ecx1 = 0;
    CHECK(refused_for(caps_of(&m, &c, why), why, "CPUID.01H:ECX.VMX is clear"));
    sandy_bridge(&m, true);
    m.cr0 &= ~0x20ull;
    CHECK(refused_for(caps_of(&m, &c, why), why,
                      "host CR0 80050013 violates IA32_VMX_CR0_FIXED0/1 (bit 5)"));
    sandy_bridge(&m, true);
    m.cr4 |= 0x80000ull;
    CHECK(refused_for(caps_of(&m, &c, why), why,
                      "host CR4 806a0 violates IA32_VMX_CR4_FIXED0/1 (bit 19)"));
    sandy_bridge(&m, true);
    CHECK(caps_of(&m, &c, why)); /* CR4 without VMXE is the normal case */
    /* Broken checks of the checker itself: an acceptance of a fixture
     * without unrestricted guest is not a refusal. */
    CHECK(!refused_for(true, "", "no unrestricted guest"));
    CHECK(!refused_for(false, "no EPT", "no unrestricted guest"));
}

/* --- the VMCS after create and one put_regs -------------------------------- */

struct vmcs {
    uint32_t enc[256];
    uint64_t val[256];
    unsigned n;
    bool duplicate;
};
static void vmcs_put(struct vmcs *v, uint32_t enc, uint64_t val)
{
    for (unsigned i = 0; i < v->n; ++i)
        if (v->enc[i] == enc) {
            v->val[i] = val;
            v->duplicate = true;
            return;
        }
    v->enc[v->n] = enc;
    v->val[v->n++] = val;
}
static bool vmcs_has(const struct vmcs *v, uint32_t enc, uint64_t *val)
{
    for (unsigned i = 0; i < v->n; ++i)
        if (v->enc[i] == enc) {
            *val = v->val[i];
            return true;
        }
    return false;
}
/* A field never written reads as 0 here; vmcs_problem checks presence. */
static uint64_t F(const struct vmcs *v, uint32_t enc)
{
    uint64_t x = 0;
    vmcs_has(v, enc, &x);
    return x;
}
static uint64_t vmcs_read(void *ctx, uint32_t enc)
{
    return F(ctx, enc);
}
static void vmcs_set(struct vmcs *v, uint32_t enc, uint64_t val)
{
    for (unsigned i = 0; i < v->n; ++i)
        if (v->enc[i] == enc) {
            v->val[i] = val;
            return;
        }
    v->enc[v->n] = enc;
    v->val[v->n++] = val;
}

static void real_mode_call(struct x86rm_regs *r)
{
    /* The registers platform/vbios.c loads for the POST call. */
    memset(r, 0, sizeof(*r));
    r->cs = (struct x86rm_segment){0xf000, 0x9b, 0xffff, 0xf0000};
    r->ds = r->ss = r->fs = r->gs = (struct x86rm_segment){0, 0x93, 0xffff, 0};
    r->es = (struct x86rm_segment){0xf000, 0x93, 0xffff, 0xf0000};
    r->gdtr = (struct x86rm_segment){0, 0, 0xffff, 0};
    r->idtr = (struct x86rm_segment){0, 0, 0x3ff, 0};
    r->rip = 0xe000;
    r->rsp = 0x7c00;
    r->rflags = 0x202;
    r->cr0 = 0x10;
    r->rax = 0x0010;
    r->rdx = 0xffff;
}

static const struct vmx_host test_host = {
    .cr0 = 0x80050033,
    .cr3 = 0x1000000,
    .cr4 = 0x26a0,
    .efer = 0xd01,
    .fs_base = 0,
    .gs_base = 0xffff800000100000ull,
    .tr_base = 0xffff800000200040ull,
    .gdtr_base = 0xffff800000200000ull,
    .idtr_base = 0xffff800000300000ull,
    .sysenter_esp = 0,
    .sysenter_eip = 0,
    .rip = 0xffffc00000001000ull,
    .sysenter_cs = 0,
    .es = 16,
    .cs = 8,
    .ss = 16,
    .ds = 16,
    .fs = 0,
    .gs = 0,
    .tr = 24,
};
#define IO_A 0x1000000ull
#define IO_B 0x1001000ull
#define EPT_ROOT 0x2000000ull

static bool build_vmcs(const struct vmx_caps *c, const struct x86rm_regs *r, struct vmcs *v)
{
    memset(v, 0, sizeof(*v));
    const struct vmx_vmcs_params p = {c, &test_host, IO_A, IO_B, EPT_ROOT | EPTP_WB_4LEVEL};
    struct vmx_field f[VMX_MAX_FIELDS];
    unsigned n = vmx_vmcs_fields(&p, f, VMX_MAX_FIELDS);
    if (!n)
        return false;
    for (unsigned i = 0; i < n; ++i)
        vmcs_put(v, f[i].encoding, f[i].value);
    bool create_dup = v->duplicate;
    n = vmx_put_regs(c, r, f, VMX_MAX_FIELDS);
    if (!n)
        return false;
    for (unsigned i = 0; i < n; ++i)
        vmcs_set(v, f[i].encoding, f[i].value);
    return !create_dup && vmx_vmcs_fields(&p, f, 10) == 0; /* too small a buffer: 0 */
}

static bool seg_is(const struct vmcs *v, unsigned k, uint64_t sel, uint64_t limit, uint64_t ar,
                   uint64_t base)
{
    /* k: ES 0, CS 1, SS 2, DS 3, FS 4, GS 5, LDTR 6, TR 7 (B.1-B.4). */
    return F(v, 0x0800 + 2 * k) == sel && F(v, 0x4800 + 2 * k) == limit &&
           F(v, 0x4814 + 2 * k) == ar && F(v, 0x6806 + 2 * k) == base;
}

/* NULL when the VMCS is the real-mode container, else what differs. */
static const char *vmcs_problem(const struct vmcs *v)
{
    static const uint32_t required[] = {
        0x4000, 0x4002, 0x401e, 0x400c, 0x4012, 0x4004, 0x4006, 0x4008, 0x400a, 0x400e,
        0x4010, 0x4014, 0x4016, 0x2000, 0x2002, 0x201a, 0x6000, 0x6002, 0x6004, 0x6006,
        0x2800, 0x2802, 0x2806, 0x681a, 0x4824, 0x4826, 0x6822, 0x482a, 0x6824, 0x6826,
        0x6800, 0x6802, 0x6804, 0x681c, 0x681e, 0x6820, 0x4810, 0x4812, 0x6816, 0x6818,
        0x0c00, 0x0c02, 0x0c04, 0x0c06, 0x0c08, 0x0c0a, 0x0c0c, 0x2c02, 0x4c00, 0x6c00,
        0x6c02, 0x6c04, 0x6c06, 0x6c08, 0x6c0a, 0x6c0c, 0x6c0e, 0x6c10, 0x6c12, 0x6c16,
    };
    uint64_t x;
    for (unsigned i = 0; i < COUNT(required); ++i)
        if (!vmcs_has(v, required[i], &x))
            return "a field is missing";
    for (unsigned k = 0; k < 8; ++k)
        if (!vmcs_has(v, 0x0800 + 2 * k, &x) || !vmcs_has(v, 0x4800 + 2 * k, &x) ||
            !vmcs_has(v, 0x4814 + 2 * k, &x) || !vmcs_has(v, 0x6806 + 2 * k, &x))
            return "a segment field is missing";
    if (vmcs_has(v, 0x6c14, &x))
        return "HOST_RSP is vmx_vmrun.S's";
    /* Table 27-5: external-interrupt exiting 0, NMI exiting 3. */
    if ((F(v, 0x4000) & 0x9) != 0x9)
        return "pin-based 0/3";
    /* Table 27-6: HLT 7, I/O bitmaps 25, secondary 31; MSR bitmaps 28 0. */
    if ((F(v, 0x4002) & 0x82000080u) != 0x82000080u || (F(v, 0x4002) & (1u << 28)))
        return "primary";
    /* Table 27-7: EPT 1, WBINVD 6, unrestricted 7; RDTSCP 3, VPID 5 0. */
    if ((F(v, 0x401e) & 0xc2) != 0xc2 || (F(v, 0x401e) & 0x28))
        return "secondary";
    /* Table 27-14: host address-space size 9, load EFER 21; ack 15 0. */
    if ((F(v, 0x400c) & 0x200200u) != 0x200200u || (F(v, 0x400c) & 0x8000u))
        return "VM-exit";
    /* Table 27-17: load EFER 15; IA-32e mode guest 9 0. */
    if (!(F(v, 0x4012) & 0x8000u) || (F(v, 0x4012) & 0x200u))
        return "VM-entry";
    if (F(v, 0x4004) != 0xffffffffu)
        return "exception bitmap";
    if (F(v, 0x4006) || F(v, 0x4008) || F(v, 0x400a) || F(v, 0x400e) || F(v, 0x4010) ||
        F(v, 0x4014) || F(v, 0x4016))
        return "PF mask/match, CR3 targets, MSR counts, injection";
    if (F(v, 0x2000) != IO_A || F(v, 0x2002) != IO_B)
        return "I/O bitmap addresses";
    /* Table 27-9: WB (6) in 2:0, walk length 4 - 1 in 5:3, A/D 6 off. */
    if (F(v, 0x201a) != (EPT_ROOT | 0x1e))
        return "EPTP";
    if (F(v, 0x2800) != UINT64_MAX)
        return "VMCS link pointer";
    if (F(v, 0x2806) || F(v, 0x2802))
        return "guest EFER/DEBUGCTL";
    /* CR0: ET and NE (fixed) set, PE, PG, CD, NW clear; the mask owns NE,
     * CD and NW but not PE/PG; the shadow shows ET only. */
    uint64_t cr0 = F(v, 0x6800), mask0 = F(v, 0x6000);
    if ((cr0 & 0xe0000031ull) != 0x30)
        return "guest CR0";
    if ((mask0 & 0x60000020ull) != 0x60000020ull || (mask0 & 0x80000001ull))
        return "CR0 guest/host mask";
    if (F(v, 0x6004) != 0x10)
        return "CR0 read shadow";
    /* CR4: VMXE (13) set, owned by the host, read as 0. */
    if (F(v, 0x6804) != 0x2000 || !(F(v, 0x6002) & 0x2000) || F(v, 0x6006))
        return "guest CR4 / mask / shadow";
    if (F(v, 0x6802))
        return "guest CR3";
    if (!seg_is(v, 1, 0xf000, 0xffff, 0x9b, 0xf0000))
        return "CS";
    if (!seg_is(v, 0, 0xf000, 0xffff, 0x93, 0xf0000))
        return "ES";
    for (unsigned k = 2; k <= 5; ++k)
        if (!seg_is(v, k, 0, 0xffff, 0x93, 0))
            return "SS/DS/FS/GS";
    if (!seg_is(v, 6, 0, 0xffff, 0x82, 0) || !seg_is(v, 7, 0, 0xffff, 0x83, 0))
        return "LDTR/TR";
    if (F(v, 0x4810) != 0xffff || F(v, 0x6816) || F(v, 0x4812) != 0x3ff || F(v, 0x6818))
        return "GDTR/IDTR";
    if (F(v, 0x681e) != 0xe000 || F(v, 0x681c) != 0x7c00 || F(v, 0x6820) != 0x202)
        return "RIP/RSP/RFLAGS";
    if (F(v, 0x4824) || F(v, 0x4826) || F(v, 0x6822))
        return "interruptibility/activity/pending debug";
    if (F(v, 0x681a) != 0x400 || F(v, 0x482a) || F(v, 0x6824) || F(v, 0x6826))
        return "DR7/SYSENTER";
    /* Host state as read (§27.5), CR4 with VMXE. */
    const uint64_t host[][2] = {
        {0x0c00, 16},
        {0x0c02, 8},
        {0x0c04, 16},
        {0x0c06, 16},
        {0x0c08, 0},
        {0x0c0a, 0},
        {0x0c0c, 24},
        {0x2c02, 0xd01},
        {0x4c00, 0},
        {0x6c00, 0x80050033},
        {0x6c02, 0x1000000},
        {0x6c04, 0x26a0},
        {0x6c06, 0},
        {0x6c08, 0xffff800000100000ull},
        {0x6c0a, 0xffff800000200040ull},
        {0x6c0c, 0xffff800000200000ull},
        {0x6c0e, 0xffff800000300000ull},
        {0x6c10, 0},
        {0x6c12, 0},
        {0x6c16, 0xffffc00000001000ull},
    };
    for (unsigned i = 0; i < COUNT(host); ++i)
        if (F(v, (uint32_t)host[i][0]) != host[i][1])
            return "host state";
    return NULL;
}

/* --- independent VM-entry checker (§29.2, §29.3.1) ------------------------- */

static bool canonical(uint64_t a)
{
    uint64_t top = a >> 47;
    return top == 0 || top == 0x1ffff;
}
static bool g_consistent(uint64_t limit, uint64_t ar)
{
    /* G (AR bit 15): 0 if any of limit 11:0 is 0, 1 if any of 31:20 is 1. */
    bool g = (ar >> 15) & 1;
    if ((limit & 0xfff) != 0xfff && g)
        return false;
    if ((limit & 0xfff00000u) && !g)
        return false;
    return true;
}
static bool control_ok(uint64_t word, uint64_t msr)
{
    uint32_t allowed0 = (uint32_t)msr, allowed1 = (uint32_t)(msr >> 32);
    return (word & allowed0) == allowed0 && !(word & ~(uint64_t)allowed1);
}

/* NULL when VM entry would load this guest state on the processor the MSRs
 * describe (true controls; MAXPHYADDR 39; host in IA-32e mode), else the
 * first check that fails. Only the checks that can apply to the container
 * are written: no IA-32e guest, no injection, no SMM, no shadow VMCS. */
static const char *entry_problem(const struct vmcs *v, const struct msrs *m)
{
    uint64_t pin = F(v, 0x4000), proc = F(v, 0x4002), exit = F(v, 0x400c), entry = F(v, 0x4012);
    uint64_t proc2 = proc & (1u << 31) ? F(v, 0x401e) : 0;
    /* §29.2.1.1-§29.2.1.3: reserved control bits as the true MSRs say. */
    if (!control_ok(pin, m->v[0x48d]) || !control_ok(proc, m->v[0x48e]) ||
        !control_ok(exit, m->v[0x48f]) || !control_ok(entry, m->v[0x490]))
        return "reserved control bits";
    if ((proc & (1u << 31)) && !control_ok(proc2, m->v[0x48b]))
        return "reserved secondary control bits";
    if (F(v, 0x400a) > 4)
        return "CR3-target count";
    if ((proc & (1u << 25)) && ((F(v, 0x2000) | F(v, 0x2002)) & 0xfff))
        return "I/O bitmap alignment";
    if (!(pin & (1u << 3)) && (pin & (1u << 5)))
        return "virtual NMIs without NMI exiting";
    bool ept = proc2 & 2, unrestricted = proc2 & 0x80;
    if (ept) {
        uint64_t eptp = F(v, 0x201a), cap = m->v[0x48c];
        unsigned type = eptp & 7;
        if (!((type == 0 && (cap & (1u << 8))) || (type == 6 && (cap & (1u << 14)))))
            return "EPTP memory type";
        if (((eptp >> 3) & 7) != 3 || !(cap & (1u << 6)))
            return "EPTP walk length";
        if ((eptp & 0x40) && !(cap & (1u << 21)))
            return "EPTP accessed/dirty";
        if (eptp & 0xf80)
            return "EPTP reserved bits";
        if (eptp >> 39)
            return "EPTP beyond MAXPHYADDR";
    }
    if (unrestricted && !ept)
        return "unrestricted guest without EPT";
    if (F(v, 0x4016) >> 31)
        return "injection";
    if (entry & (3u << 10))
        return "entry to SMM / dual-monitor";
    /* §29.2.2-§29.2.4: the host is in IA-32e mode. */
    uint64_t hcr0 = F(v, 0x6c00), hcr4 = F(v, 0x6c04), hefer = F(v, 0x2c02);
    if ((m->v[0x486] & ~hcr0) || (hcr0 & ~m->v[0x487]))
        return "host CR0 fixed bits";
    if ((m->v[0x488] & ~hcr4) || (hcr4 & ~m->v[0x489]))
        return "host CR4 fixed bits";
    if (!(exit & (1u << 9)) || (entry & (1u << 9)))
        return "address-space size";
    if (!(hcr4 & 0x20) || !canonical(F(v, 0x6c16)))
        return "host PAE / RIP";
    if ((exit & (1u << 21)) && (((hefer >> 10) & 1) != 1 || ((hefer >> 8) & 1) != 1))
        return "host EFER LMA/LME";
    for (uint32_t e = 0x0c00; e <= 0x0c0c; e += 2)
        if (F(v, e) & 7)
            return "host selector RPL/TI";
    if (!F(v, 0x0c02) || !F(v, 0x0c0c))
        return "host CS/TR null";
    if (!canonical(F(v, 0x6c06)) || !canonical(F(v, 0x6c08)) || !canonical(F(v, 0x6c0a)) ||
        !canonical(F(v, 0x6c0c)) || !canonical(F(v, 0x6c0e)))
        return "host base not canonical";
    /* §29.3.1.1: CR0 fixed bits except NW/CD always and PE/PG under
     * unrestricted guest; PG => PE; CR4 fixed bits; EFER. */
    uint64_t cr0 = F(v, 0x6800), cr4 = F(v, 0x6804);
    uint64_t exempt0 = 0x60000000ull | (unrestricted ? 0x80000001ull : 0);
    if (((m->v[0x486] & ~cr0) | (cr0 & ~m->v[0x487])) & ~exempt0)
        return "guest CR0 fixed bits";
    if ((cr0 & 0x80000000ull) && !(cr0 & 1))
        return "guest CR0.PG without PE";
    if ((m->v[0x488] & ~cr4) | (cr4 & ~m->v[0x489]))
        return "guest CR4 fixed bits";
    if (cr4 & (1u << 17))
        return "guest CR4.PCIDE outside IA-32e mode";
    if ((entry & 4) && (F(v, 0x681a) >> 32))
        return "guest DR7";
    if (!canonical(F(v, 0x6824)) || !canonical(F(v, 0x6826)))
        return "guest SYSENTER";
    if (entry & (1u << 15)) {
        uint64_t efer = F(v, 0x2806);
        if (efer & ~0xd01ull)
            return "guest EFER reserved bits";
        if (((efer >> 10) & 1) != ((entry >> 9) & 1))
            return "guest EFER.LMA";
    }
    /* §29.3.1.2, virtual-8086 excluded below (RFLAGS.VM = 0). */
    bool pe = cr0 & 1;
    uint64_t rflags = F(v, 0x6820);
    uint64_t cs_ar = F(v, 0x4816), ss_ar = F(v, 0x4818);
    unsigned cs_type = cs_ar & 15, cs_dpl = (cs_ar >> 5) & 3, ss_dpl = (ss_ar >> 5) & 3;
    if (F(v, 0x080e) & 4)
        return "TR selector TI";
    if (!(F(v, 0x4820) & 0x10000) && (F(v, 0x080c) & 4))
        return "LDTR selector TI";
    if (!unrestricted && (F(v, 0x0804) & 3) != (F(v, 0x0802) & 3))
        return "SS RPL";
    if (!canonical(F(v, 0x6814)) || !canonical(F(v, 0x680e)) || !canonical(F(v, 0x6810)))
        return "TR/FS/GS base";
    if (!(F(v, 0x4820) & 0x10000) && !canonical(F(v, 0x6812)))
        return "LDTR base";
    if (F(v, 0x6808) >> 32)
        return "CS base";
    for (unsigned k = 0; k <= 3; ++k)
        if (k != 1 && !(F(v, 0x4814 + 2 * k) & 0x10000) && (F(v, 0x6806 + 2 * k) >> 32))
            return "SS/DS/ES base";
    if (!(cs_type == 9 || cs_type == 11 || cs_type == 13 || cs_type == 15 ||
          (unrestricted && cs_type == 3)))
        return "CS type";
    if (cs_type == 3 && cs_dpl != 0)
        return "CS DPL (type 3)";
    if ((cs_type == 9 || cs_type == 11) && cs_dpl != ss_dpl)
        return "CS DPL != SS DPL";
    if ((cs_type == 13 || cs_type == 15) && cs_dpl > ss_dpl)
        return "CS DPL > SS DPL";
    if (!(ss_ar & 0x10000) && (ss_ar & 15) != 3 && (ss_ar & 15) != 7)
        return "SS type";
    if (!unrestricted && ss_dpl != (F(v, 0x0804) & 3))
        return "SS DPL != RPL";
    if ((cs_type == 3 || !pe) && ss_dpl != 0)
        return "SS DPL with CR0.PE = 0";
    for (unsigned k = 0; k <= 5; ++k) {
        uint64_t ar = F(v, 0x4814 + 2 * k), limit = F(v, 0x4800 + 2 * k);
        bool usable = !(ar & 0x10000);
        if (k == 1)
            usable = true; /* the CS checks apply whatever bit 16 says */
        if (!usable)
            continue;
        if (k == 0 || k >= 3) {
            if (!(ar & 1))
                return "data segment not accessed";
            if ((ar & 8) && !(ar & 2))
                return "code segment in a data register not readable";
            if (!unrestricted && (ar & 15) <= 11 && ((ar >> 5) & 3) < (F(v, 0x0800 + 2 * k) & 3))
                return "DPL < RPL";
        }
        if (!(ar & 0x10))
            return "segment S";
        if (!(ar & 0x80))
            return "segment P";
        if (ar & 0xf00)
            return "segment AR 11:8";
        if (!g_consistent(limit, ar))
            return "segment G";
        if (ar & 0xfffe0000ull)
            return "segment AR 31:17";
    }
    uint64_t tr_ar = F(v, 0x4822);
    if ((tr_ar & 15) != 3 && (tr_ar & 15) != 11)
        return "TR type";
    if ((tr_ar & 0x10) || !(tr_ar & 0x80) || (tr_ar & 0xf00) || (tr_ar & 0x10000) ||
        (tr_ar & 0xfffe0000ull))
        return "TR S/P/reserved/unusable";
    if (!g_consistent(F(v, 0x480e), tr_ar))
        return "TR G";
    uint64_t ldtr_ar = F(v, 0x4820);
    if (!(ldtr_ar & 0x10000)) {
        if ((ldtr_ar & 15) != 2 || (ldtr_ar & 0x10) || !(ldtr_ar & 0x80) || (ldtr_ar & 0xf00) ||
            (ldtr_ar & 0xfffe0000ull))
            return "LDTR type/S/P/reserved";
        if (!g_consistent(F(v, 0x480c), ldtr_ar))
            return "LDTR G";
    }
    /* §29.3.1.3 */
    if ((F(v, 0x4810) | F(v, 0x4812)) >> 16)
        return "GDTR/IDTR limit";
    if (!canonical(F(v, 0x6816)) || !canonical(F(v, 0x6818)))
        return "GDTR/IDTR base";
    /* §29.3.1.4 */
    if (F(v, 0x681e) >> 32)
        return "RIP 63:32";
    if ((rflags & ~0x3fffffull) || (rflags & 0x8028) || !(rflags & 2))
        return "RFLAGS reserved bits";
    if ((rflags & (1u << 17)) && !pe)
        return "RFLAGS.VM with CR0.PE = 0";
    if (rflags & (1u << 17))
        return "virtual-8086 is not the container's";
    /* §29.3.1.5 */
    uint64_t activity = F(v, 0x4826), intr = F(v, 0x4824), pending = F(v, 0x6822);
    if (activity > 3)
        return "activity state";
    if (activity == 1 && ss_dpl)
        return "HLT with SS DPL != 0";
    if ((intr & 3) && activity)
        return "blocking outside the active state";
    if (intr & ~0x1full)
        return "interruptibility reserved bits";
    if ((intr & 3) == 3)
        return "blocking by STI and MOV SS";
    if ((intr & 1) && !(rflags & 0x200))
        return "blocking by STI with IF = 0";
    if (intr & 4)
        return "blocking by SMI outside SMM";
    if (pending & ~0x1500full)
        return "pending debug reserved bits";
    if ((intr & 3) || activity == 1) {
        bool tf = rflags & 0x100, btf = F(v, 0x2802) & 2, bs = pending & 0x4000;
        if (bs != (tf && !btf))
            return "pending debug BS";
    }
    /* The link pointer must be all-ones: any other value names memory
     * that would have to hold a VMCS of this revision (§29.3.1.5). */
    if (F(v, 0x2800) != UINT64_MAX)
        return "VMCS link pointer";
    return NULL;
}

/* One change to a built VMCS: value = (value & ~clear) ^ flip, or = set. */
struct mutation {
    uint32_t enc;
    uint64_t clear, flip;
    bool assign;
    uint64_t set;
    const char *what;
    bool entry_refuses;
};
static const struct mutation vmcs_mutations[] = {
    {0x6800, 0, 0x1, false, 0, "CR0.PE set", false},
    {0x6800, 0, 0x80000000ull, false, 0, "CR0.PG set", true},
    {0x6800, 0, 0x20, false, 0, "CR0.NE cleared", true},
    {0x6000, 0, 0x40000000ull, false, 0, "CD without its mask bit", false},
    {0x4000, 0, 0x1, false, 0, "external-interrupt exiting cleared", false},
    {0x4000, 0, 0x8, false, 0, "NMI exiting cleared", false},
    {0x4002, 0, 0x80, false, 0, "HLT exiting cleared", false},
    {0x401e, 0, 0x80, false, 0, "unrestricted guest cleared", true},
    {0x401e, 0, 0x2, false, 0, "EPT cleared", true},
    {0x4002, 0, 0x10000000ull, false, 0, "use MSR bitmaps set", false},
    {0x400c, 0, 0x8000, false, 0, "acknowledge interrupt on exit set", false},
    {0x2800, 0, 0, true, 0, "VMCS link pointer 0", true},
    {0x4816, 0, 0x400, false, 0, "CS AR with D/B at bit 10", true},
    {0x4818, 0, 0x60, false, 0, "SS DPL 3", true},
    {0x4822, 0, 0x10000, false, 0, "TR unusable", true},
    {0x201a, 0, 0, true, EPT_ROOT | 0x16, "EPTP walk length 3", true},
    {0x201a, 0, 0, true, EPT_ROOT | 0x18, "EPTP type UC", false},
    {0x6006, 0, 0, true, 0x2000, "VMXE visible in the CR4 shadow", false},
};
static void mutate(struct vmcs *v, const struct mutation *m)
{
    uint64_t x = F(v, m->enc);
    vmcs_set(v, m->enc, m->assign ? m->set : (x & ~m->clear) ^ m->flip);
}

static void test_vmcs(void)
{
    struct msrs m;
    struct vmx_caps c;
    char why[200];
    for (int t = 0; t < 2; ++t) {
        sandy_bridge(&m, t);
        CHECK(caps_of(&m, &c, why));
        struct x86rm_regs r, back;
        real_mode_call(&r);
        struct vmcs v;
        CHECK(build_vmcs(&c, &r, &v));
        const char *problem = vmcs_problem(&v);
        if (problem)
            fprintf(stderr, "vmcs: %s\n", problem);
        CHECK(!problem);
        const char *entry = t ? entry_problem(&v, &m) : NULL;
        if (entry)
            fprintf(stderr, "VM entry would fail: %s\n", entry);
        CHECK(!entry);
        /* The registers read back as the platform wrote them (CR0 through
         * the shadow: ET only, NE hidden). */
        memset(&back, 0x5a, sizeof(back));
        vmx_get_regs(&c, vmcs_read, &v, &back);
        back.rax = r.rax;
        back.rbx = r.rbx;
        back.rcx = r.rcx;
        back.rdx = r.rdx;
        back.rsi = r.rsi;
        back.rdi = r.rdi;
        back.rbp = r.rbp;
        CHECK(!memcmp(&back, &r, sizeof(r)));
        for (unsigned i = 0; i < COUNT(vmcs_mutations); ++i) {
            struct vmcs bad = v;
            mutate(&bad, &vmcs_mutations[i]);
            if (!vmcs_problem(&bad)) {
                fprintf(stderr, "vmcs broken variant not caught: %s\n", vmcs_mutations[i].what);
                ++failures;
            }
            if (t && vmcs_mutations[i].entry_refuses && !entry_problem(&bad, &m)) {
                fprintf(stderr, "VM-entry checker misses: %s\n", vmcs_mutations[i].what);
                ++failures;
            }
        }
    }
}

static void test_entry_checker(void)
{
    struct msrs m;
    struct vmx_caps c;
    char why[200];
    sandy_bridge(&m, true);
    CHECK(caps_of(&m, &c, why));
    /* Unreal mode: a ROM that loaded a flat 4 GiB FS in protected mode and
     * returned (APM attrib G at bit 11, VMX AR bit 15); and a 32-bit CS. */
    struct x86rm_regs r;
    real_mode_call(&r);
    r.fs = (struct x86rm_segment){0x0010, 0x893, 0xffffffffu, 0};
    r.gs = (struct x86rm_segment){0, 0, 0, 0}; /* null in protected mode: unusable */
    struct vmcs v;
    CHECK(build_vmcs(&c, &r, &v));
    CHECK(F(&v, 0x481c) == 0x8093 && F(&v, 0x4808) == 0xffffffffu && (F(&v, 0x481e) & 0x10000));
    const char *problem = entry_problem(&v, &m);
    if (problem)
        fprintf(stderr, "unreal mode: %s\n", problem);
    CHECK(!problem);
    /* Broken: FS's G left at bit 11 (AR bits 11:8 reserved) is refused, as
     * is a 4 GiB limit with G = 0. */
    struct vmcs bad = v;
    vmcs_set(&bad, 0x481c, 0x893);
    CHECK(entry_problem(&bad, &m) != NULL);
    bad = v;
    vmcs_set(&bad, 0x481c, 0x93);
    CHECK(entry_problem(&bad, &m) != NULL);
    /* STI blocking with IF = 0, and TR type 1 (a 16-bit available TSS). */
    real_mode_call(&r);
    CHECK(build_vmcs(&c, &r, &v));
    bad = v;
    vmcs_set(&bad, 0x4824, 1);
    vmcs_set(&bad, 0x6820, 0x2);
    CHECK(entry_problem(&bad, &m) != NULL);
    vmcs_set(&bad, 0x6820, 0x202);
    CHECK(entry_problem(&bad, &m) == NULL); /* the same blocking with IF = 1 is legal */
    bad = v;
    vmcs_set(&bad, 0x4822, 0x81);
    CHECK(entry_problem(&bad, &m) != NULL);
}

/* --- access rights (Table 27-2) -------------------------------------------- */

static uint32_t ar_expected(uint16_t attrib, bool cs)
{
    /* Bits 7:0 unchanged; APM 8 AVL, 9 L, 10 D/B, 11 G -> 12, 13, 14, 15;
     * not present and not CS -> unusable (16). */
    uint32_t ar = (attrib & 0xff) | ((attrib >> 8) & 1) << 12 | ((attrib >> 9) & 1) << 13 |
                  ((attrib >> 10) & 1) << 14 | ((attrib >> 11) & 1) << 15;
    if (!cs && !(attrib & 0x80))
        ar |= 1u << 16;
    return ar;
}
static bool ar_matches(uint32_t (*from)(uint16_t, bool), uint16_t (*to)(uint32_t))
{
    for (unsigned a = 0; a < 4096; ++a)
        for (int cs = 0; cs < 2; ++cs) {
            uint32_t ar = from((uint16_t)a, cs);
            if (ar != ar_expected((uint16_t)a, cs))
                return false;
            uint16_t back = to(ar);
            if (back != ((ar & 0x10000) ? 0 : a))
                return false;
        }
    return !(from(0, true) & 0x10000); /* CS is never unusable */
}
/* Broken copy: AVL/L/DB/G left at 11:8. */
static uint32_t ar_unshifted(uint16_t attrib, bool cs)
{
    uint32_t ar = attrib & 0xfff;
    if (!cs && !(attrib & 0x80))
        ar |= 1u << 16;
    return ar;
}
static uint16_t ar_unshifted_back(uint32_t ar)
{
    return ar & 0x10000 ? 0 : (uint16_t)(ar & 0xfff);
}
static void test_access_rights(void)
{
    CHECK(ar_matches(vmx_ar_from_attrib, vmx_ar_to_attrib));
    CHECK(!ar_matches(ar_unshifted, ar_unshifted_back));
    CHECK(vmx_ar_from_attrib(0x49b, true) == 0x409b); /* a 32-bit code segment */
    CHECK(vmx_ar_from_attrib(0x13, false) == 0x10013 && vmx_ar_to_attrib(0x10013) == 0);
}

/* --- I/O bitmaps (§27.6.4) ------------------------------------------------- */

static bool passed(unsigned port)
{
    return (port >= 0x3b0 && port <= 0x3df) || port == 0x80 || (port >= 0x7ff8 && port <= 0x8007) ||
           (port >= 0x5000 && port < 0x5040);
}
static bool bitmaps_match(const uint8_t *a, const uint8_t *b)
{
    /* Bit p of A (0000-7FFFh) or bit p - 8000h of B: 1 = exit. */
    for (unsigned port = 0; port < 65536; ++port) {
        const uint8_t *map = port < 0x8000 ? a : b;
        unsigned bit = port & 0x7fff;
        bool exits = (map[bit / 8] >> (bit & 7)) & 1;
        if (exits != !passed(port))
            return false;
    }
    return true;
}
static void test_io_bitmaps(void)
{
    static uint8_t pass[8192], a[4096], b[4096];
    for (unsigned p = 0; p < 65536; ++p)
        if (passed(p))
            pass[p / 8] |= 1u << (p & 7);
    vmx_io_bitmaps(pass, a, b);
    CHECK(bitmaps_match(a, b));
    /* The A/B boundary: 7FFFh is A's last bit, 8000h B's first. */
    CHECK(!((a[4095] >> 7) & 1) && !(b[0] & 1));
    a[0x1ce / 8] &= (uint8_t)~(1u << (0x1ce & 7)); /* broken: DISPI index passed */
    CHECK(!bitmaps_match(a, b));
    vmx_io_bitmaps(pass, a, b);
    static uint8_t b_from_a[4096];
    memcpy(b_from_a, a, sizeof(b_from_a)); /* broken: B built from A's half */
    CHECK(!bitmaps_match(a, b_from_a));
    static uint8_t na[4096], nb[4096];
    memcpy(na, pass, 4096);
    memcpy(nb, pass + 4096, 4096); /* broken: polarity not inverted */
    CHECK(!bitmaps_match(na, nb));
}

/* --- EPT (Tables 31-2..31-7) ----------------------------------------------- */

struct pool {
    void *pages[64];
    unsigned used, limit;
};
static void *pool_alloc(void *ctx)
{
    struct pool *p = ctx;
    if (p->used == p->limit)
        return NULL;
    void *page = aligned_alloc(4096, 4096);
    memset(page, 0, 4096);
    p->pages[p->used++] = page;
    return page;
}
static uint64_t pool_pa(void *ctx, const void *va)
{
    (void)ctx;
    return (uintptr_t)va;
}
static void *pool_va(void *ctx, uint64_t pa)
{
    (void)ctx;
    return (void *)(uintptr_t)pa;
}
static void pool_free(struct pool *p)
{
    for (unsigned i = 0; i < p->used; ++i)
        free(p->pages[i]);
    p->used = 0;
}

struct run {
    uint64_t gpa, size, hpa, flags;
};
struct runs {
    struct run r[64];
    unsigned n;
    uint64_t leaves[4];
    bool misconfigured;
};
static void collect(void *arg, uint64_t gpa, uint64_t size, uint64_t entry)
{
    struct runs *s = arg;
    /* Leaf bits: R 0, W 1, X 2, memory type 5:3, ignore PAT 6; address
     * 51:12 (Tables 31-3, 31-5, 31-7). */
    uint64_t hpa = entry & 0x000ffffffffff000ull & ~(size - 1), flags = entry & 0x7f;
    unsigned type = (entry >> 3) & 7;
    /* §31.3.3.1: W without R, and types 2, 3, 7, are misconfigurations. */
    if (((entry & 3) == 2) || type == 2 || type == 3 || type == 7)
        s->misconfigured = true;
    ++s->leaves[size == 4096 ? 1 : size == (1ull << 21) ? 2 : 3];
    struct run *last = s->n ? &s->r[s->n - 1] : NULL;
    if (last && last->gpa + last->size == gpa && last->hpa + last->size == hpa &&
        last->flags == flags) {
        last->size += size;
        return;
    }
    if (s->n < 64)
        s->r[s->n++] = (struct run){gpa, size, hpa, flags};
}

static uint8_t low[0xa0000] __attribute__((aligned(4096)));
static uint8_t shadow[0x10000] __attribute__((aligned(4096)));
static uint8_t stub[0x10000] __attribute__((aligned(4096)));

/* The gen6 host-image map: RAM, VGA window, shadow,
 * stubs, the IGD's GTTMMADR (4 MiB at F0000000h) and GMADR (256 MiB at
 * E0000000h) and its 8 KiB OpRegion; a 2 GiB window above 4 GiB. */
static const struct x86rm_mapping product_map[] = {
    {0, 0xa0000, low, X86RM_MAP_WRITE},
    {0xa0000, 0x20000, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC},
    {0xc0000, 0x10000, shadow, X86RM_MAP_WRITE},
    {0xf0000, 0x10000, stub, 0},
    {0xdae9d000, 0x2000, NULL, X86RM_MAP_WRITE},
    {0xe0000000, 0x10000000, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC},
    {0xf0000000, 0x400000, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC},
    {0x100000000ull, 0x80000000ull, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC},
};
/* What the map must turn into, written out: R|X always, W where writable,
 * type 6 (WB) or 0 (UC) with ignore PAT (bit 6). */
static bool ept_matches(const struct runs *got)
{
    const uint64_t ro = 0x01 | 0x04 | 0x30 | 0x40, rw = ro | 0x02, uc = 0x01 | 0x02 | 0x04 | 0x40;
    const struct run expected[] = {
        {0, 0xa0000, (uintptr_t)low, rw},
        {0xa0000, 0x20000, 0xa0000, uc},
        {0xc0000, 0x10000, (uintptr_t)shadow, rw},
        {0xf0000, 0x10000, (uintptr_t)stub, ro},
        {0xdae9d000, 0x2000, 0xdae9d000, rw},
        {0xe0000000, 0x10400000, 0xe0000000, uc}, /* GMADR, then GTTMMADR adjacent: one run */
        {0x100000000ull, 0x80000000ull, 0x100000000ull, uc},
    };
    if (got->misconfigured || got->n != COUNT(expected))
        return false;
    for (unsigned i = 0; i < got->n; ++i)
        if (memcmp(&got->r[i], &expected[i], sizeof(expected[i])))
            return false;
    return true;
}
static uint64_t *leaf_slot(uint64_t root, uint64_t gpa)
{
    uint64_t *t = pool_va(NULL, root);
    for (unsigned level = 4; level > 1; --level) {
        uint64_t entry = t[(gpa >> (12 + 9 * (level - 1))) & 511];
        if (!(entry & 7) || (entry & 0x80))
            return NULL;
        t = pool_va(NULL, entry & 0x000ffffffffff000ull);
    }
    return &t[(gpa >> 12) & 511];
}
static bool still_matches(uint64_t root, const struct vmx_ept_ops *ops)
{
    struct runs got = {0};
    vmx_ept_enumerate(root, ops, collect, &got);
    return ept_matches(&got);
}
/* Non-leaf entries: R|W|X and bits 7:3 zero (Tables 31-2, 31-4, 31-6). */
static bool tables_ok(const uint64_t *t, unsigned level)
{
    for (unsigned i = 0; i < 512; ++i) {
        uint64_t e = t[i];
        if (!(e & 7) || level == 1 || (e & 0x80))
            continue;
        if ((e & 0xff) != 7 || !tables_ok(pool_va(NULL, e & 0x000ffffffffff000ull), level - 1))
            return false;
    }
    return true;
}

static void test_ept(void)
{
    struct pool pool = {.limit = 64};
    struct vmx_ept_ops ops = {pool_alloc, pool_pa, pool_va, &pool, true, true, 48};
    uint64_t root = 0;
    CHECK(vmx_ept_build(product_map, COUNT(product_map), &ops, &root));
    struct runs got = {0};
    vmx_ept_enumerate(root, &ops, collect, &got);
    CHECK(ept_matches(&got) && tables_ok(pool_va(NULL, root), 4));
    /* 4 KiB for the first MiB and the OpRegion, 2 MiB for the two IGD BARs
     * (128 + 2), 1 GiB for the 4 GiB window (2). */
    CHECK(got.leaves[1] == 0xa0 + 0x20 + 0x10 + 0x10 + 2 && got.leaves[2] == 130 &&
          got.leaves[3] == 2);
    /* Broken variants on the built table. */
    uint64_t *slot = leaf_slot(root, 0xd0000), *vga = leaf_slot(root, 0xb8000),
             *code = leaf_slot(root, 0xff000);
    CHECK(slot && !*slot && vga && code);
    if (slot && vga && code) {
        static const struct {
            uint64_t *where;
            uint64_t value;
            const char *what;
        } broken[] = {
            {NULL, 0, "an extra leaf at D0000h"},
            {NULL, 0, "the VGA window WB"},
            {NULL, 0, "the stub segment writable"},
            {NULL, 0, "ignore PAT dropped"},
            {NULL, 0, "X dropped"},
            {NULL, 0, "W without R"},
        };
        uint64_t values[6] = {0xd0000 | 0x77, (*vga & ~0x38ull) | 0x30,
                              *code | 2,      *code & ~0x40ull,
                              *code & ~4ull,  (*vga & ~1ull)};
        uint64_t *where[6] = {slot, vga, code, code, code, vga};
        for (unsigned i = 0; i < COUNT(broken); ++i) {
            uint64_t saved = *where[i];
            *where[i] = values[i];
            if (still_matches(root, &ops)) {
                fprintf(stderr, "EPT broken variant not caught: %s\n", broken[i].what);
                ++failures;
            }
            *where[i] = saved;
        }
    }
    CHECK(still_matches(root, &ops));
    pool_free(&pool);
    /* Without 1 GiB leaves the window takes 2 MiB ones; without 2 MiB
     * leaves everything is 4 KiB (the pool then runs out: refused). */
    ops.pages_1g = false;
    CHECK(vmx_ept_build(product_map, COUNT(product_map), &ops, &root));
    struct runs small = {0};
    vmx_ept_enumerate(root, &ops, collect, &small);
    CHECK(ept_matches(&small) && small.leaves[3] == 0 && small.leaves[2] == 130 + 1024);
    pool_free(&pool);
    ops.pages_2m = false;
    const struct x86rm_mapping bar0[] = {
        {0xf0000000, 0x400000, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC}};
    CHECK(vmx_ept_build(bar0, 1, &ops, &root));
    struct runs four = {0};
    vmx_ept_enumerate(root, &ops, collect, &four);
    CHECK(four.leaves[1] == 1024 && four.leaves[2] == 0);
    pool_free(&pool);
    ops.pages_2m = true;
    /* Refusals: overlap, misalignment, at or above 2^MAXPHYADDR (39 on the
     * gen6 IGD), pool exhaustion. */
    const struct x86rm_mapping overlap[] = {{0, 0x2000, low, X86RM_MAP_WRITE},
                                            {0x1000, 0x1000, NULL, 0}};
    CHECK(!vmx_ept_build(overlap, 2, &ops, &root));
    pool_free(&pool);
    const struct x86rm_mapping large_overlap[] = {{0xf0000000, 0x200000, NULL, 0},
                                                  {0xf0100000, 0x1000, NULL, 0}};
    CHECK(!vmx_ept_build(large_overlap, 2, &ops, &root));
    pool_free(&pool);
    const struct x86rm_mapping odd[] = {{0x800, 0x1000, NULL, 0}};
    CHECK(!vmx_ept_build(odd, 1, &ops, &root));
    pool_free(&pool);
    ops.maxphyaddr = 39;
    const struct x86rm_mapping far[] = {{1ull << 39, 0x1000, NULL, 0}};
    CHECK(!vmx_ept_build(far, 1, &ops, &root));
    pool_free(&pool);
    const struct x86rm_mapping straddle[] = {{(1ull << 39) - 0x1000, 0x2000, NULL, 0}};
    CHECK(!vmx_ept_build(straddle, 1, &ops, &root));
    pool_free(&pool);
    const struct x86rm_mapping last[] = {{(1ull << 39) - 0x1000, 0x1000, NULL, 0}};
    CHECK(vmx_ept_build(last, 1, &ops, &root));
    pool_free(&pool);
    ops.maxphyaddr = 48;
    pool.limit = 3;
    CHECK(!vmx_ept_build(product_map, 4, &ops, &root));
    pool_free(&pool);
}

/* --- exit decoding (Appendix C, Tables 27-19, 27-20, 30-5, 30-7) ----------- */

static struct x86rm_exit decode(uint32_t reason, uint64_t q, uint32_t info, uint32_t error,
                                uint32_t len, uint32_t cs_ar, uint64_t rip, uint64_t gpa, bool *ok)
{
    struct vmx_exit_fields f = {reason, info, error, 0, len, cs_ar, q, gpa, rip};
    struct x86rm_exit e;
    *ok = vmx_decode_exit(&f, &e);
    return e;
}
/* Literal expectations for the I/O qualification, independently decoded. */
static bool io_is(const struct x86rm_exit *e, uint16_t port, uint8_t size, bool in, bool string,
                  bool rep)
{
    return e->reason == X86RM_EXIT_IO && e->port == port && e->size == size && e->in == in &&
           e->string == string && e->rep == rep && e->next_rip_valid;
}

static void test_exits(void)
{
    bool ok;
    struct x86rm_exit e;
    /* 0: NMI (type 2), hardware #GP with error code (type 3, bit 11),
     * software #BP (type 6). */
    e = decode(0, 0, 0x80000202u, 0, 0, 0x93, 0x100, 0, &ok);
    CHECK(ok && e.reason == X86RM_EXIT_NMI && !e.next_rip_valid);
    e = decode(0, 0, 0x80000b0du, 0x18, 0, 0x93, 0x100, 0, &ok);
    CHECK(ok && e.reason == X86RM_EXIT_EXCEPTION && e.vector == 13 && e.info1 == 0x18 &&
          !e.next_rip_valid);
    e = decode(0, 0, 0x80000603u, 0x77, 1, 0x93, 0x100, 0, &ok);
    CHECK(ok && e.reason == X86RM_EXIT_EXCEPTION && e.vector == 3 && e.info1 == 0);
    /* 30: IN word from DX = 1CEh (size 1, IN 3); OUT dword to an immediate
     * port (size 3, operand 6); REP INSB (4, 5); port FFFFh. */
    e = decode(30, (0x1ceull << 16) | 0x8 | 1, 0, 0, 1, 0x93, 0x3000, 0, &ok);
    CHECK(ok && io_is(&e, 0x1ce, 2, true, false, false) && e.next_rip == 0x3001);
    e = decode(30, (0x80ull << 16) | 0x40 | 3, 0, 0, 2, 0x93, 0x3000, 0, &ok);
    CHECK(ok && io_is(&e, 0x80, 4, false, false, false) && e.next_rip == 0x3002);
    e = decode(30, (0x3c9ull << 16) | 0x20 | 0x10 | 0x8, 0, 0, 2, 0x93, 0x3000, 0, &ok);
    CHECK(ok && io_is(&e, 0x3c9, 1, true, true, true));
    e = decode(30, (0xffffull << 16), 0, 0, 1, 0x93, 0x3000, 0, &ok);
    CHECK(ok && io_is(&e, 0xffff, 1, false, false, false));
    /* 48: EPT violation: read, write, fetch; bits 5:3; GPA. */
    e = decode(48, 0x1, 0, 0, 0, 0x93, 0x100, 0xd0000, &ok);
    CHECK(ok && e.reason == X86RM_EXIT_NPF && e.gpa == 0xd0000 && !e.npf_write && !e.npf_fetch &&
          !e.npf_present && !e.next_rip_valid);
    e = decode(48, 0x2 | 0x8 | 0x20, 0, 0, 0, 0x93, 0x100, 0xff000, &ok);
    CHECK(ok && e.npf_write && e.npf_present && e.gpa == 0xff000);
    e = decode(48, 0x4, 0, 0, 0, 0x93, 0x100, 0x200000, &ok);
    CHECK(ok && e.npf_fetch && !e.npf_present);
    /* 49: EPT misconfiguration is a backend error. */
    decode(49, 0, 0, 0, 0, 0x93, 0x100, 0x1000, &ok);
    CHECK(!ok);
    /* 33 with bit 31: entry failure, code and qualification kept. */
    e = decode(0x80000021u, 4, 0, 0, 0, 0x93, 0x100, 0, &ok);
    CHECK(ok && e.reason == X86RM_EXIT_INVALID && e.code == 0x80000021u && e.info1 == 4);
    /* Bit 31 with a basic reason that would otherwise decode (34 MSR load,
     * here reason 30's number) must still be an entry failure. */
    e = decode(0x8000001eu, 0, 0, 0, 1, 0x93, 0x100, 0, &ok);
    CHECK(ok && e.reason == X86RM_EXIT_INVALID);
    const struct {
        uint32_t reason;
        enum x86rm_exit_reason want;
        bool instruction;
    } table[] = {
        {1, X86RM_EXIT_INTR, false},   {2, X86RM_EXIT_SHUTDOWN, false},
        {10, X86RM_EXIT_CPUID, true},  {12, X86RM_EXIT_HLT, true},
        {13, X86RM_EXIT_INVD, true},   {18, X86RM_EXIT_VMMCALL, true},
        {31, X86RM_EXIT_MSR, true},    {32, X86RM_EXIT_MSR, true},
        {54, X86RM_EXIT_WBINVD, true}, {28, X86RM_EXIT_OTHER, false},
        {55, X86RM_EXIT_OTHER, false}, {9, X86RM_EXIT_OTHER, false},
    };
    for (unsigned i = 0; i < COUNT(table); ++i) {
        e = decode(table[i].reason, 0, 0, 0, 3, 0x93, 0x1234, 0, &ok);
        CHECK(ok && e.reason == table[i].want && e.next_rip_valid == table[i].instruction &&
              (!table[i].instruction || e.next_rip == 0x1237) && e.code == table[i].reason);
        if (table[i].reason == 31 || table[i].reason == 32)
            CHECK(e.msr_write == (table[i].reason == 32));
    }
    /* Next RIP: VMCALL (3 bytes) at IP FFFEh wraps to 0001h in a 16-bit
     * CS; with D/B (AR bit 14) set it continues to 10001h. */
    e = decode(18, 0, 0, 0, 3, 0x93, 0xfffe, 0, &ok);
    CHECK(ok && e.next_rip == 0x0001);
    e = decode(18, 0, 0, 0, 3, 0x409b, 0xfffe, 0, &ok);
    CHECK(ok && e.next_rip == 0x10001);
    e = decode(18, 0, 0, 0, 3, 0x409b, 0xfffffffeull, 0, &ok);
    CHECK(ok && e.next_rip == 0x1);
    /* Broken: an unwrapped next RIP and a size without + 1 are not the
     * decoder's answer. */
    e = decode(18, 0, 0, 0, 3, 0x93, 0xfffe, 0, &ok);
    CHECK(e.next_rip != 0xfffe + 3);
    e = decode(30, (0x1ceull << 16) | 1, 0, 0, 1, 0x93, 0x3000, 0, &ok);
    CHECK(e.size != 1);
}

/* --- interrupt shadow across a completed instruction ------------ */

static void test_interruptibility(void)
{
    struct x86rm_regs r;
    real_mode_call(&r);
    /* The exit was at F000:E000 with STI blocking (bit 0) and NMI blocking
     * (bit 3): unchanged CS:RIP keeps both, a completed instruction (RIP
     * moved, or CS moved) drops bits 1:0 only. */
    CHECK(vmx_entry_interruptibility(0x9, &r, 0xf000, 0xf0000, 0xe000) == 0x9);
    CHECK(vmx_entry_interruptibility(0x2, &r, 0xf000, 0xf0000, 0xe000) == 0x2);
    CHECK(vmx_entry_interruptibility(0x9, &r, 0xf000, 0xf0000, 0xdffd) == 0x8);
    CHECK(vmx_entry_interruptibility(0x2, &r, 0xc000, 0xc0000, 0xe000) == 0x0);
    CHECK(vmx_entry_interruptibility(0x1, &r, 0xf000, 0xe0000, 0xe000) == 0x0);
}

int main(void)
{
    test_capabilities();
    test_vmcs();
    test_entry_checker();
    test_access_rights();
    test_io_bitmaps();
    test_ept();
    test_exits();
    test_interruptibility();
    if (failures) {
        fprintf(stderr, "test-vmx: %u failure(s)\n", failures);
        return 1;
    }
    printf(
        "test-vmx: capabilities and control words, VMCS field list, independent VM-entry checks, access "
        "rights, I/O bitmaps, EPT, exit decoding, interrupt shadow (each with its broken variants) passed\n");
    return 0;
}
