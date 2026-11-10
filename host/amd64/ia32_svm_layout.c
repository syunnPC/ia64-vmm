/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AMD APM Vol.2 sections 15.5-15.12, Appendix B/C. No VBIOS policy. */
#include "host/amd64/ia32_svm.h"
#include "host/amd64/ia32_native_mode.h"
#include "ia64vmm/string.h"

static uint64_t get(const uint8_t *p, unsigned off, unsigned size)
{
    uint64_t n = 0;
    memcpy(&n, p + off, size);
    return n;
}
static void put(uint8_t *p, unsigned off, uint64_t n, unsigned size)
{
    memcpy(p + off, &n, size);
}
static void segment_put(uint8_t *p, unsigned off, const struct ia32_segment *s)
{
    put(p, off, s->selector, 2);
    put(p, off + 2, s->unusable ? 0 : s->attrib, 2);
    put(p, off + 4, s->limit, 4);
    put(p, off + 8, s->base, 8);
}
void amd64_svm_ia32_controls(uint8_t *p, const struct svm_vmcb_params *v)
{
    memset(p, 0, 4096);
    put(p, VMCB_INTERCEPT_CR, UINT32_MAX, 4);
    put(p, VMCB_INTERCEPT_DR, UINT32_MAX, 4);
    put(p, VMCB_INTERCEPT_EXC, UINT32_C(0x400f7dfb), 4);
    /* No virtual interrupts, selective CR0 bypass or PAUSE filtering. */
    put(p, VMCB_INTERCEPT_MISC1, UINT32_MAX - (1u << 4) - (1u << 5) - (1u << 23), 4);
    put(p, VMCB_INTERCEPT_MISC2, 0x3fff, 4);
    put(p, VMCB_IOPM_BASE, v->iopm_pa, 8);
    put(p, VMCB_MSRPM_BASE, v->msrpm_pa, 8);
    put(p, VMCB_ASID, v->asid, 4);
    put(p, VMCB_INT_CTL, SVM_V_INTR_MASKING, 4);
    put(p, VMCB_NP_CONTROL, SVM_NP_ENABLE, 8);
    put(p, VMCB_N_CR3, v->n_cr3, 8);
    /* Unpaged guests use gPAT[0]=WB. Nested leaves select the host PAT. */
    put(p, VMCB_GPAT, UINT64_C(0x0606060606060606), 8);
}
static bool invalid(const char **why, const char *detail)
{
    *why = detail;
    return false;
}
bool amd64_svm_ia32_state_valid(const struct ia32_native_state *n, const char **why)
{
    if (n->cpl > 3)
        return invalid(why, "native SVM CPL state is unqualified");
    if ((n->eflag & ~UINT32_C(0x003f7fd7)) || !(n->eflag & 2))
        return invalid(why, "native SVM EFLAGS has reserved bits");
    if (n->cs.attrib & 0x200)
        return invalid(why, "native SVM CS mode is unqualified");
    const struct ia32_segment *segments[] = {&n->es, &n->cs,   &n->ss, &n->ds,   &n->fs,
                                             &n->gs, &n->ldtr, &n->tr, &n->gdtr, &n->idtr};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(segments); ++i)
        if (segments[i]->base > UINT32_MAX || (segments[i]->attrib & 0xf000))
            return invalid(why, "native SVM cached segment encoding is unqualified");
    if (n->interrupt_shadow > 1 || n->interruptibility || n->pending_debug)
        return invalid(why, "native SVM restart shadow is unrepresentable");
    if ((n->dr6 | n->dr7) >> 32)
        return invalid(why, "native SVM debug state exceeds legacy width");
    for (unsigned i = 0; i < 4; ++i)
        if (n->dr[i] > UINT32_MAX)
            return invalid(why, "native SVM debug address exceeds legacy width");
    if (get(n->fp.bytes, 24, 4) & ~UINT64_C(0xffbf))
        return invalid(why, "native SVM MXCSR has reserved or DAZ bits");
    return true;
}
void amd64_svm_ia32_put(uint8_t *p, struct amd64_svm_ia32_gprs *g,
                        const struct ia32_native_state *n)
{
    memset(g, 0, sizeof(*g));
    g->rbx = n->gpr[IA32_EBX];
    g->rcx = n->gpr[IA32_ECX];
    g->rdx = n->gpr[IA32_EDX];
    g->rsi = n->gpr[IA32_ESI];
    g->rdi = n->gpr[IA32_EDI];
    g->rbp = n->gpr[IA32_EBP];
    memcpy(g->dr, n->dr, sizeof(g->dr));
    put(p, VMCB_RAX, n->gpr[IA32_EAX], 8);
    put(p, VMCB_RSP, n->gpr[IA32_ESP], 8);
    put(p, VMCB_RIP, n->eip.value, 8);
    put(p, VMCB_RFLAGS, amd64_ia32_private_eflag(n), 8);
    /* Private protected/unpaged mode; all virtual CR reads/writes exit. */
    /* CFLG.NE is ignored in the Itanium System Environment. Native x87
     * exceptions must remain synchronous instead of using legacy FERR. */
    put(p, VMCB_CR0, 0x31 | (n->cflg & UINT64_C(0x4000e)), 8);
    put(p, VMCB_CR3, 0, 8);
    put(p, VMCB_CR4, 0x440 | ((n->cflg >> 32) & 0x200), 8);
    put(p, VMCB_CR2, 0, 8);
    put(p, VMCB_EFER, SVM_EFER_SVME, 8);
    /* APM Vol.3 SYSENTER/SYSEXIT: null linkage faults before effects.
     * All MSR writes exit; VMLOAD must never inherit host linkage. */
    put(p, VMCB_SYSENTER_CS, 0, 8);
    put(p, VMCB_SYSENTER_ESP, 0, 8);
    put(p, VMCB_SYSENTER_EIP, 0, 8);
    put(p, VMCB_CPL, n->cpl, 1);
    put(p, VMCB_DR6, n->dr6, 8);
    put(p, VMCB_DR7, n->dr7, 8);
    for (unsigned i = 0; i < 6; ++i) {
        struct ia32_segment private = amd64_ia32_private_segment(n, i);
        segment_put(p, VMCB_ES + 16 * i, &private);
    }
    /* VMLOAD requires valid system caches even when the IA-64 state has
     * no loaded TSS/LDT. Their semantic values remain in software: task
     * switches and every descriptor-register instruction exit. */
    const struct ia32_segment dummy_ldtr = {.attrib = 0x82, .limit = 0xffff};
    const struct ia32_segment dummy_tr = {.attrib = 0x8b, .limit = 0x67};
    segment_put(p, VMCB_LDTR, &dummy_ldtr);
    segment_put(p, VMCB_TR, &dummy_tr);
    put(p, VMCB_GDTR + 4, n->gdtr.limit > 0xffff ? 0xffff : n->gdtr.limit, 4);
    put(p, VMCB_GDTR + 8, n->gdtr.base, 8);
    /* No native event injection or guest x86 IDT delivery is permitted. */
    memset(p + VMCB_IDTR, 0, 16);
    put(p, VMCB_INT_STATE, n->interrupt_shadow, 8);
    put(p, VMCB_EVENTINJ, 0, 8);
    put(p, VMCB_CLEAN, 0, 8);
}
void amd64_svm_ia32_get(const uint8_t *p, const struct amd64_svm_ia32_gprs *g,
                        struct ia32_native_state *n)
{
    n->gpr[IA32_EAX] = get(p, VMCB_RAX, 8);
    n->gpr[IA32_ESP] = get(p, VMCB_RSP, 8);
    n->gpr[IA32_EBX] = g->rbx;
    n->gpr[IA32_ECX] = g->rcx;
    n->gpr[IA32_EDX] = g->rdx;
    n->gpr[IA32_ESI] = g->rsi;
    n->gpr[IA32_EDI] = g->rdi;
    n->gpr[IA32_EBP] = g->rbp;
    n->eip.value = get(p, VMCB_RIP, 8);
    n->eflag = amd64_ia32_semantic_eflag(n, get(p, VMCB_RFLAGS, 8));
    n->dr6 = get(p, VMCB_DR6, 8);
    n->dr7 = get(p, VMCB_DR7, 8);
    memcpy(n->dr, g->dr, sizeof(n->dr));
    n->interrupt_shadow = get(p, VMCB_INT_STATE, 8);
}
static struct ia32_event event(uint64_t raw)
{
    if (!(raw & (UINT64_C(1) << 31)))
        return (struct ia32_event){0};
    return (struct ia32_event){.valid = true,
                               .error_valid = (raw & (1u << 11)) != 0,
                               .vector = raw,
                               .type = (raw >> 8) & 7,
                               .error = (raw & (1u << 11)) ? raw >> 32 : 0};
}
struct ia32_native_exit amd64_svm_ia32_decode(const uint8_t *p, bool nrip, bool decode_assists)
{
    uint64_t code = get(p, VMCB_EXITCODE, 8), a = get(p, VMCB_EXITINFO1, 8),
             b = get(p, VMCB_EXITINFO2, 8);
    struct ia32_native_exit e = {.backend = IA32_BACKEND_SVM,
                                 .raw_reason = code,
                                 .reason = IA32_EXIT_OTHER,
                                 .phase = IA32_PHASE_UNKNOWN};
    if (code == SVM_EXIT_INVALID) {
        e.reason = IA32_EXIT_ENTRY_FAILURE;
        e.detail = "SVM VMRUN rejected native state";
        return e;
    }
    e.position_valid = true;
    e.eip.value = get(p, VMCB_RIP, 8);
    e.linear_ip.value = get(p, VMCB_CS + 8, 8) + e.eip.value;
    e.hardware_eflag = get(p, VMCB_RFLAGS, 8);
    e.vectoring = event(get(p, VMCB_EXITINTINFO, 8));
    bool instruction = false;
    bool cr = code == 0 || code == 2 || code == 3 || code == 4 || code == 8 || code == 0x10 ||
              code == 0x12 || code == 0x13 || code == 0x14 || code == 0x18;
    bool dr = (code >= 0x20 && code <= 0x27) || (code >= 0x30 && code <= 0x37);
    if (cr || dr || (code >= 0x65 && code <= 0x8e) || (code >= 0xa0 && code <= 0xa4)) {
        e.reason = IA32_EXIT_SYSTEM;
        e.phase = IA32_PHASE_BEFORE;
        instruction = true;
        /* Task-switch intercepts need instruction/partial-state analysis. */
        if (code == 0x7d || code == 0x7e) {
            e.phase = IA32_PHASE_UNKNOWN;
            e.qualification = a;
            e.info2 = b;
            instruction = false;
        }
        if ((cr || dr) && decode_assists && (a >> 63))
            e.qualification = a;
        if (code == 0x79 && decode_assists)
            e.qualification = a; /* INVLPG linear address */
        if (code == 0x75 && decode_assists)
            e.qualification = a; /* INTn vector */
    }
    switch (code) {
    case 0x60:
        e.reason = IA32_EXIT_HOST_IRQ;
        break;
    case 0x61:
        e.reason = IA32_EXIT_HOST_NMI;
        break;
    case 0x6e:
    case 0x6f:
    case 0x87:
    case 0x8e:
        e.reason = IA32_EXIT_CLOCK;
        break;
    case 0x72:
        e.reason = IA32_EXIT_CPUID;
        break;
    case 0x7b:
        e.reason = IA32_EXIT_IO;
        e.qualification = a;
        e.info2 = b;
        e.next_eip.value = b;
        e.next_eip_valid = b <= UINT32_MAX;
        if (a & (1u << 2))
            e.phase = IA32_PHASE_UNKNOWN;
        break;
    case 0x7c:
        e.qualification = a;
        break;
    case 0x7f:
        e.reason = IA32_EXIT_SHUTDOWN;
        e.phase = IA32_PHASE_UNKNOWN;
        instruction = false;
        break;
    case 0x400:
        e.reason = IA32_EXIT_NESTED_FAULT;
        e.qualification = a;
        e.info2 = b;
        e.access.token_valid = true;
        e.access.token.value = b;
        e.access.kind = (a & (1u << 4)) ? IA32_ACCESS_FETCH
                        : (a & 2)       ? IA32_ACCESS_WRITE
                                        : IA32_ACCESS_READ;
        if (a & (UINT64_C(1) << 33))
            e.access.kind = IA32_ACCESS_PRIVATE_WALK;
        break;
    default:
        break;
    }
    if (code >= 0x40 && code < 0x60) {
        unsigned vector = code - 0x40;
        e.reason = IA32_EXIT_EXCEPTION;
        e.exception = (struct ia32_event){.valid = true, .vector = vector, .type = 3};
        /* Only these error/auxiliary fields are defined by the intercept. */
        if (vector == 11 || vector == 12 || vector == 13 || vector == 14 || vector == 17 ||
            vector == 21 || vector == 30) {
            e.qualification = a;
            e.exception.error_valid = true;
            e.exception.error = a;
        }
        if (vector == 14)
            e.info2 = b;
        /* BP/OF intercepts precede trap delivery. Segment/page faults
         * can retain partial instruction or task-switch effects. #DB
         * may be a fault or a REP trap; DR6 also retains earlier causes.
         * #MF precedes the current waiting instruction; #XM retains
         * the exception status written to MXCSR. */
        if (vector == 0 || vector == 3 || vector == 4 || vector == 5 || vector == 6 ||
            vector == 7 || vector == 16 || vector == 17 || vector == 19)
            e.phase = IA32_PHASE_BEFORE;
        instruction = vector == 3 || vector == 4 || vector == 5;
        if (vector == 8 || vector == 10)
            e.position_valid = false;
    }
    if (!e.next_eip_valid && nrip && instruction) {
        uint64_t next = get(p, VMCB_NRIP, 8);
        if (next <= UINT32_MAX) {
            e.next_eip.value = next;
            e.next_eip_valid = true;
        }
    }
    if (e.vectoring.valid)
        e.phase = IA32_PHASE_UNKNOWN;
    return e;
}
