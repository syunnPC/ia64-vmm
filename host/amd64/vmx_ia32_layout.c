/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/vmx_ia32.h"
#include "host/amd64/ia32_native_mode.h"
#include "ia64vmm/string.h"

static bool unsupported(struct ia32_setup_error *error, const char *detail)
{
    if (error)
        *error = (struct ia32_setup_error){.reason = IA32_EXIT_UNIMPLEMENTED, .detail = detail};
    return false;
}
static bool controls(uint64_t msr, uint32_t required, uint32_t defaults, uint32_t *out)
{
    uint32_t forced = (uint32_t)msr, allowed = (uint32_t)(msr >> 32);
    if ((required & ~allowed) || (forced & ~(required | defaults)))
        return false;
    *out = required | forced;
    return true;
}
bool vmx_ia32_controls(struct vmx_caps *c, struct ia32_capabilities *out,
                       struct ia32_setup_error *error)
{
    /* Primary: HLT, INVLPG, MWAIT, RDPMC, RDTSC, CR3 load/store,
     * MOV-DR, all-port I/O bitmaps, MONITOR and secondary controls.
     * With MSR bitmaps disabled every RDMSR/WRMSR exits. */
    const uint32_t primary = UINT32_C(0xa2819e80);
    const uint32_t secondary = UINT32_C(0x000000ce);
    if (!controls(c->pin_msr, VMX_PIN_EXTINT | VMX_PIN_NMI, VMX_PIN_DEFAULT1, &c->pin) ||
        !controls(c->proc_msr, primary, VMX_PROC_DEFAULT1, &c->proc) ||
        !controls(c->proc2_msr, secondary, 0, &c->proc2) ||
        !controls(c->exit_msr, VMX_EXIT_DEFAULT1 | VMX_EXIT_HOST_64 | VMX_EXIT_LOAD_EFER,
                  VMX_EXIT_DEFAULT1, &c->exit) ||
        !controls(c->entry_msr, VMX_ENTRY_DEFAULT1 | VMX_ENTRY_LOAD_EFER, VMX_ENTRY_DEFAULT1,
                  &c->entry))
        return unsupported(error, "VMX native interception controls unavailable");
    memset(out, 0, sizeof(*out));
    out->backend = IA32_BACKEND_VMX;
    out->nested_paging = out->unrestricted_guest = out->nested_invalidate = true;
    out->bounded_exit = out->legacy_fp = out->descriptor_read_exit = true;
    out->mtf = ((c->proc_msr >> 32) & (UINT32_C(1) << 27)) != 0;
    out->vmx_ins_outs_info = ((c->basic >> 54) & 1) != 0;
    out->ept_execute_only = (c->ept_vpid & 1) != 0;
    out->invept_types = (uint32_t)((c->ept_vpid >> 24) & 6);
    return true;
}
static const struct ia32_segment *segment(const struct ia32_native_state *s, unsigned i)
{
    const struct ia32_segment *all[] = {&s->es, &s->cs, &s->ss,   &s->ds,
                                        &s->fs, &s->gs, &s->ldtr, &s->tr};
    return all[i];
}
bool vmx_ia32_validate(const struct ia32_native_state *s, struct ia32_setup_error *error)
{
    if (s->cpl > 3)
        return unsupported(error, "VMX native CPL is unrepresentable");
    if (!(s->eflag & 2) || (s->eflag & ~UINT32_C(0x003f7fd7)))
        return unsupported(error, "VMX native EFLAGS has reserved bits");
    if (s->gdtr.base > UINT32_MAX || s->idtr.base > UINT32_MAX)
        return unsupported(error, "VMX native descriptor table outside IA-32 range");
    for (unsigned i = 0; i < 8; ++i) {
        const struct ia32_segment *seg = segment(s, i);
        if (seg->base > UINT32_MAX || (seg->attrib & ~UINT16_C(0xdff)))
            return unsupported(error, "VMX native segment has unrepresentable base or attributes");
    }
    if (s->interrupt_shadow)
        return unsupported(error, "VMX native foreign restart state");
    for (unsigned i = 0; i < 4; ++i)
        if (s->dr[i] > UINT32_MAX)
            return unsupported(error, "VMX native debug address outside IA-32 range");
    if (s->dr6 > UINT32_MAX || s->dr7 > UINT32_MAX)
        return unsupported(error, "VMX native debug control outside IA-32 range");
    return true;
}
static uint64_t native_cr0(const struct ia32_native_state *s)
{
    /* CFLG.NE is ignored in the Itanium system environment. */
    return CR0_PE | CR0_ET | UINT64_C(0x20) | (s->cflg & UINT64_C(0x4000e));
}
static uint64_t native_cr4(const struct vmx_caps *c, const struct ia32_native_state *s)
{
    return c->cr4_fixed0 | UINT64_C(0x400) | ((s->cflg >> 32) & UINT64_C(0x200));
}
bool vmx_ia32_control_state(const struct vmx_caps *c, const struct ia32_native_state *s,
                            struct ia32_setup_error *error)
{
    uint64_t cr0 = native_cr0(s), cr4 = native_cr4(c, s);
    if (((c->cr0_fixed0 & ~cr0) & ~(CR0_PE | CR0_PG)) || (cr0 & ~c->cr0_fixed1) ||
        (cr4 & ~c->cr4_fixed1) ||
        (c->cr4_fixed0 & ~(CR4_VMXE | UINT64_C(0x400) | ((s->cflg >> 32) & UINT64_C(0x200)))))
        return unsupported(error, "VMX fixed controls cannot represent virtual CFLG");
    return true;
}
struct state_fields {
    struct vmx_field *out;
    unsigned n, max;
    bool overflow;
};
static void put(struct state_fields *f, uint32_t encoding, uint64_t value)
{
    if (f->n == f->max) {
        f->overflow = true;
        return;
    }
    f->out[f->n++] = (struct vmx_field){encoding, value};
}
unsigned vmx_ia32_put_state(const struct vmx_caps *c, const struct ia32_native_state *s,
                            struct vmx_field *out, unsigned max)
{
    struct state_fields f = {out, 0, max, false};
    for (unsigned i = 0; i < 8; ++i) {
        struct ia32_segment private = i < 6 ? amd64_ia32_private_segment(s, i)
                                      : i == 6
                                          ? (struct ia32_segment){.unusable = true}
                                          : (struct ia32_segment){.limit = 0x67, .attrib = 0x8b};
        const struct ia32_segment *seg = &private;
        uint32_t ar = (seg->attrib & 0xffu) | ((uint32_t)(seg->attrib & 0xf00u) << 4);
        if (seg->unusable)
            ar |= 1u << 16;
        put(&f, VMCS_GUEST_ES_SEL + 2 * i, seg->selector);
        put(&f, VMCS_GUEST_ES_LIMIT + 2 * i, seg->limit);
        put(&f, VMCS_GUEST_ES_AR + 2 * i, ar);
        put(&f, VMCS_GUEST_ES_BASE + 2 * i, seg->base);
    }
    put(&f, VMCS_GUEST_GDTR_LIMIT, s->gdtr.limit > 0xffff ? 0xffff : s->gdtr.limit);
    put(&f, VMCS_GUEST_GDTR_BASE, s->gdtr.base);
    put(&f, VMCS_GUEST_IDTR_LIMIT, 0);
    put(&f, VMCS_GUEST_IDTR_BASE, 0);
    put(&f, VMCS_GUEST_RSP, s->gpr[IA32_ESP]);
    put(&f, VMCS_GUEST_RIP, s->eip.value);
    put(&f, VMCS_GUEST_RFLAGS, amd64_ia32_private_eflag(s));
    /* Unpaged protected mode. CFLG/KRs are never host control registers.
     * Unrestricted guest relaxes only PE/PG fixed-bit requirements. */
    uint64_t cr0 = native_cr0(s);
    uint64_t cr4 = native_cr4(c, s);
    put(&f, VMCS_GUEST_CR0, cr0);
    put(&f, VMCS_GUEST_CR3, 0);
    put(&f, VMCS_GUEST_CR4, cr4);
    put(&f, VMCS_CR0_MASK, UINT64_MAX);
    put(&f, VMCS_CR4_MASK, UINT64_MAX);
    put(&f, VMCS_CR0_SHADOW, cr0);
    put(&f, VMCS_CR4_SHADOW, cr4 & ~CR4_VMXE);
    put(&f, VMCS_GUEST_DR7, s->dr7);
    put(&f, VMCS_GUEST_INTERRUPTIBILITY, s->interruptibility);
    put(&f, VMCS_GUEST_PENDING_DEBUG, s->pending_debug);
    return f.overflow ? 0 : f.n;
}
void vmx_ia32_get_state(vmx_vmread_fn read, void *ctx, struct ia32_native_state *s)
{
    /* Segment and descriptor-table operations are caller assists. Private
     * entry normalization must never replace the semantic cached state. */
    s->gpr[IA32_ESP] = (uint32_t)read(ctx, VMCS_GUEST_RSP);
    s->eip.value = (uint32_t)read(ctx, VMCS_GUEST_RIP);
    s->eflag = amd64_ia32_semantic_eflag(s, (uint32_t)read(ctx, VMCS_GUEST_RFLAGS));
    s->dr7 = read(ctx, VMCS_GUEST_DR7);
    s->interruptibility = (uint32_t)read(ctx, VMCS_GUEST_INTERRUPTIBILITY);
    s->pending_debug = read(ctx, VMCS_GUEST_PENDING_DEBUG);
}
static struct ia32_event event(uint32_t info, uint32_t error)
{
    if (!(info & (1u << 31)))
        return (struct ia32_event){0};
    return (struct ia32_event){.valid = true,
                               .error_valid = (info & (1u << 11)) != 0,
                               .vector = info & 0xff,
                               .type = (info >> 8) & 7,
                               .error = (info & (1u << 11)) ? error : 0};
}
struct ia32_native_exit vmx_ia32_decode_exit(const struct vmx_ia32_exit_fields *f,
                                             const struct ia32_native_state *s, bool ins_outs_info)
{
    struct ia32_native_exit e = {.backend = IA32_BACKEND_VMX,
                                 .phase = IA32_PHASE_UNKNOWN,
                                 .raw_reason = f->reason,
                                 .hardware_eflag = f->hardware_eflag,
                                 .interruptibility = f->interruptibility,
                                 .eip = s->eip,
                                 .position_valid = true};
    e.linear_ip.value = (uint32_t)(s->cs.base + s->eip.value);
    if (f->reason & VMX_EXIT_ENTRY_FAILURE) {
        e.reason = IA32_EXIT_ENTRY_FAILURE;
        e.phase = IA32_PHASE_BEFORE;
        return e;
    }
    e.vectoring = event(f->vectoring_info, f->vectoring_error);
    bool instruction = false;
    switch (f->reason & 0xffffu) {
    case 0:
        e.exception = event(f->intr_info, f->intr_error);
        if (e.exception.valid && e.exception.vector == 1 && e.exception.type == 3)
            e.qualification = f->qualification;
        e.reason =
            e.exception.valid && e.exception.type == 2 ? IA32_EXIT_HOST_NMI : IA32_EXIT_EXCEPTION;
        instruction = e.exception.valid &&
                      ((e.exception.type == 5 && e.exception.vector == 1) ||
                       (e.exception.type == 6 &&
                        (e.exception.vector == 3 || e.exception.vector == 4)));
        if (instruction)
            e.phase = IA32_PHASE_BEFORE;
        if (e.exception.valid && e.exception.type == 3) {
            /* SDM Vol.3A 7.5/7.15: these fault before the current
             * instruction's effects. #XM still updates MXCSR status.
             * Segment/page faults can retain task-switch or partial
             * multiaccess effects; #DB can be a fault or a REP trap. */
            switch (e.exception.vector) {
            case 0:
            case 5:
            case 6:
            case 7:
            case 16:
            case 17:
            case 19:
                e.phase = IA32_PHASE_BEFORE;
                break;
            case 8:
                e.position_valid = false;
                break;
            default:
                break;
            }
        }
        break;
    case 1:
        e.reason = IA32_EXIT_HOST_IRQ;
        break;
    case 2:
        e.reason = IA32_EXIT_SHUTDOWN;
        break;
    case 10:
        e.reason = IA32_EXIT_CPUID;
        instruction = true;
        break;
    case 16:
    case 51:
        e.reason = IA32_EXIT_CLOCK;
        instruction = true;
        break;
    case 30:
        e.reason = IA32_EXIT_IO;
        e.qualification = f->qualification;
        instruction = true;
        e.access.rep = (f->qualification & (1u << 5)) != 0;
        e.instruction_info_valid = ins_outs_info && (f->qualification & (1u << 4));
        break;
    case 37:
        e.reason = IA32_EXIT_STEP;
        break;
    case 48:
        e.reason = IA32_EXIT_NESTED_FAULT;
        e.qualification = f->qualification;
        e.info2 = f->token;
        e.access.token_valid = true;
        e.access.token.value = f->token;
        e.access.linear_valid = (f->qualification & (1u << 7)) != 0;
        if (e.access.linear_valid)
            e.access.linear.value = (uint32_t)f->linear;
        e.access.kind = (f->qualification & 4)   ? IA32_ACCESS_FETCH
                        : (f->qualification & 2) ? IA32_ACCESS_WRITE
                        : (f->qualification & 1) ? IA32_ACCESS_READ
                                                 : IA32_ACCESS_UNKNOWN;
        break;
    case 49:
        e.reason = IA32_EXIT_ENTRY_FAILURE;
        e.detail = "EPT misconfiguration";
        break;
    case 9:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 31:
    case 32:
    case 36:
    case 39:
    case 40:
    case 46:
    case 47:
    case 50:
    case 53:
    case 54:
    case 55:
        e.reason = IA32_EXIT_SYSTEM;
        if ((f->reason & 0xffffu) == 9 || (f->reason & 0xffffu) == 14 ||
            (f->reason & 0xffffu) == 28 || (f->reason & 0xffffu) == 29)
            e.qualification = f->qualification;
        instruction = true;
        e.instruction_info_valid = (f->reason & 0xffffu) == 46 || (f->reason & 0xffffu) == 47;
        break;
    default:
        e.reason = IA32_EXIT_OTHER;
        break;
    }
    if (e.instruction_info_valid)
        e.instruction_info = f->instruction_info;
    if (instruction && e.reason != IA32_EXIT_EXCEPTION) {
        e.phase = IA32_PHASE_BEFORE;
        if (f->length >= 1 && f->length <= 15) {
            e.length_valid = e.next_eip_valid = true;
            e.length = (uint8_t)f->length;
            e.next_eip.value =
                (s->eip.value + f->length) & ((s->cs.attrib & 0x400) ? UINT32_MAX : UINT16_MAX);
        }
    } else if (instruction && f->length >= 1 && f->length <= 15) {
        e.length_valid = true;
        e.length = (uint8_t)f->length;
    }
    if (e.vectoring.valid)
        e.phase = IA32_PHASE_UNKNOWN;
    return e;
}
