/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included by vmx.c to share VMCS instructions and host-state transport. */
#include "host/amd64/ia32_native.h"
#include "host/amd64/ia32_native_mode.h"
#include "host/amd64/vmx_ia32.h"

struct vmx_ia32_container {
    struct amd64_virtualization_cpu *owner;
    struct amd64_virtualization_tag tag;
    struct amd64_ia32_map map;
    struct amd64_ia32_fp_owner *fp;
    struct amd64_xstate_pair xstate;
    struct vmx_caps caps;
    struct ia32_capabilities capabilities;
    struct ia32_native_state state;
    struct vmx_gprs gprs;
    struct vmx_field fields[VMX_MAX_FIELDS];
    struct ia32_generations generations;
    uint8_t *vmcs, *io_a, *io_b;
    uint64_t eptp;
    unsigned cpu;
    bool active, launched, imported, configured, tsc_aux;
};
#define VMX_IA32_PAGES ((sizeof(struct vmx_ia32_container) + 4095) / 4096)
_Static_assert(offsetof(struct ia32_native_state, dr6) ==
                   offsetof(struct ia32_native_state, dr) + 32,
               "native VMX assembly debug registers");
int amd64_vmx_ia32_vmrun(struct vmx_gprs *, int launched, const struct amd64_xstate_pair *,
                         uint64_t *debug);

static bool native_error(struct ia32_setup_error *error, enum ia32_exit_reason reason,
                         const char *detail)
{
    if (error)
        *error = (struct ia32_setup_error){
            .reason = reason, .host_cpu = hal_cpu_index(), .detail = detail};
    return false;
}
static bool native_probe(unsigned cpu, struct vmx_caps *c, struct ia32_capabilities *caps,
                         struct ia32_setup_error *error)
{
    if (cpu != hal_cpu_index())
        return native_error(error, IA32_EXIT_UNIMPLEMENTED, "VMX probe requires the pinned CPU");
    const char *why;
    if (!probe(c, &why))
        return native_error(error, IA32_EXIT_UNIMPLEMENTED, why);
    if (!vmx_ia32_controls(c, caps, error)) {
        if (error)
            error->host_cpu = cpu;
        return false;
    }
    struct amd64_ia32_fp_requirements fp;
    if (!amd64_ia32_fp_requirements(&fp, error))
        return false;
    uint32_t a, b, ecx, d;
    amd64_cpuid(1, 0, &a, &b, &ecx, &d);
    caps->native_features = amd64_ia32_native_features(ecx);
    caps->host_cpu = cpu;
    return true;
}
static bool native_public_probe(unsigned cpu, struct ia32_capabilities *caps,
                                struct ia32_setup_error *error)
{
    struct vmx_caps c;
    if (!caps)
        return native_error(error, IA32_EXIT_ENTRY_FAILURE, "VMX missing capability output");
    memset(caps, 0, sizeof(*caps));
    return native_probe(cpu, &c, caps, error);
}
static bool native_here(struct vmx_ia32_container *v, struct ia32_setup_error *error)
{
    if (!v || v->cpu != hal_cpu_index())
        return native_error(error, IA32_EXIT_ENTRY_FAILURE,
                            "VMX container requires its pinned CPU");
    return true;
}
static bool native_current(struct vmx_ia32_container *v, struct ia32_setup_error *error)
{
    if (!native_here(v, error))
        return false;
    if (!vmx_ptrld(physical(v->vmcs)))
        return native_error(error, IA32_EXIT_ENTRY_FAILURE, "native VMPTRLD failed");
    v->active = true;
    return true;
}
static bool native_clear(struct vmx_ia32_container *v)
{
    if (v->active && !vmx_clear(physical(v->vmcs)))
        return false;
    v->active = v->launched = false;
    return true;
}
static void native_free(struct vmx_ia32_container *v)
{
    amd64_ia32_map_drop(&v->map);
    if (v->fp)
        amd64_ia32_fp_destroy(v->fp);
    if (v->io_a)
        hal_free_pages(v->io_a, 1);
    if (v->io_b)
        hal_free_pages(v->io_b, 1);
    if (v->vmcs)
        hal_free_pages(v->vmcs, 1);
    if (v->owner) {
        amd64_virtualization_tag_release(v->owner, AMD64_VIRT_IA32, &v->tag);
        amd64_virtualization_release(v->owner, AMD64_VIRT_IA32);
    }
    hal_free_pages(v, VMX_IA32_PAGES);
}
static bool native_setup_vmcs(struct vmx_ia32_container *v, struct ia32_setup_error *error)
{
    v->configured = false;
    if (!native_current(v, error))
        return false;
    struct vmx_host host;
    host_state(&host);
    const struct vmx_vmcs_params p = {&v->caps, &host, physical(v->io_a), physical(v->io_b),
                                      v->eptp};
    unsigned n = vmx_vmcs_fields(&p, v->fields, VMX_MAX_FIELDS);
    uint32_t failed;
    if (!n || !write_fields(v->fields, n, &failed))
        return native_error(error, IA32_EXIT_ENTRY_FAILURE, "native VMCS setup VMWRITE failed");
    v->configured = true;
    return true;
}
static bool native_create(unsigned cpu, struct ia32_container **out, struct ia32_setup_error *error)
{
    if (!out)
        return native_error(error, IA32_EXIT_ENTRY_FAILURE, "VMX missing container output");
    *out = NULL;
    struct vmx_caps c;
    struct ia32_capabilities caps;
    if (!native_probe(cpu, &c, &caps, error))
        return false;
    struct vmx_ia32_container *v = hal_alloc_pages(VMX_IA32_PAGES, HAL_PAGE_ZERO);
    if (!v)
        return native_error(error, IA32_EXIT_ENTRY_FAILURE, "VMX container allocation failed");
    v->cpu = cpu;
    v->caps = c;
    v->capabilities = caps;
    v->tsc_aux = vmx_native_tsc_aux_supported();
    if (!amd64_virtualization_acquire(cpu, IA32_BACKEND_VMX, AMD64_VIRT_IA32, &v->owner, error) ||
        !amd64_virtualization_tag_acquire(v->owner, AMD64_VIRT_IA32, &v->tag, error))
        goto fail;
    v->vmcs = hal_alloc_pages(1, HAL_PAGE_ZERO);
    v->io_a = hal_alloc_pages(1, HAL_PAGE_ZERO);
    v->io_b = hal_alloc_pages(1, HAL_PAGE_ZERO);
    if (!v->vmcs || !v->io_a || !v->io_b) {
        native_error(error, IA32_EXIT_ENTRY_FAILURE, "VMX control-page allocation failed");
        goto fail;
    }
    if (!amd64_ia32_fp_create(&v->fp, error) ||
        !amd64_ia32_map_init(&v->map, IA32_BACKEND_VMX, caps.ept_execute_only, c.maxphyaddr, error))
        goto fail;
    v->xstate = (struct amd64_xstate_pair){v->fp, &v->state.fp, 1};
    memset(v->io_a, 0xff, 4096);
    memset(v->io_b, 0xff, 4096);
    v->eptp = v->map.root | EPTP_WB_4LEVEL;
    memcpy(v->vmcs, &c.revision, sizeof(c.revision));
    if (!vmx_clear(physical(v->vmcs))) {
        native_error(error, IA32_EXIT_ENTRY_FAILURE, "native initial VMCLEAR failed");
        goto fail;
    }
    if (!native_setup_vmcs(v, error))
        goto fail;
    *out = (struct ia32_container *)v;
    return true;
fail:
    if (!native_clear(v))
        hal_panic("VMX: native VMCLEAR failed during unwind");
    native_free(v);
    return false;
}
static void native_merge(struct ia32_native_state *s, const struct ia32_native_state *in,
                         uint64_t dirty)
{
    if (dirty & IA32_DIRTY_GPR)
        memcpy(s->gpr, in->gpr, sizeof(s->gpr));
    if (dirty & IA32_DIRTY_IP)
        s->eip = in->eip;
    if (dirty & IA32_DIRTY_FLAGS)
        s->eflag = in->eflag;
    if (dirty & IA32_DIRTY_CONTROL) {
        s->cflg = in->cflg;
        s->kr0 = in->kr0;
        s->kr1 = in->kr1;
        s->kr2 = in->kr2;
    }
    if (dirty & IA32_DIRTY_SEGMENTS) {
        s->es = in->es;
        s->cs = in->cs;
        s->ss = in->ss;
        s->ds = in->ds;
        s->fs = in->fs;
        s->gs = in->gs;
        s->ldtr = in->ldtr;
        s->tr = in->tr;
        s->gdtr = in->gdtr;
        s->idtr = in->idtr;
        s->cpl = in->cpl;
    }
    if (dirty & IA32_DIRTY_DEBUG) {
        memcpy(s->dr, in->dr, sizeof(s->dr));
        s->dr6 = in->dr6;
        s->dr7 = in->dr7;
    }
    if (dirty & IA32_DIRTY_FP)
        s->fp = in->fp;
    if (dirty & IA32_DIRTY_SHADOW) {
        s->interruptibility = in->interruptibility;
        s->pending_debug = in->pending_debug;
        s->interrupt_shadow = in->interrupt_shadow;
    }
}
static bool native_put(struct vmx_ia32_container *v, struct ia32_setup_error *error)
{
    if (!native_current(v, error))
        return false;
    unsigned n = vmx_ia32_put_state(&v->caps, &v->state, v->fields, VMX_MAX_FIELDS);
    uint32_t failed;
    if (!n || !write_fields(v->fields, n, &failed)) {
        v->imported = false;
        return native_error(error, IA32_EXIT_ENTRY_FAILURE, "native state VMWRITE failed");
    }
    v->gprs.rax = v->state.gpr[IA32_EAX];
    v->gprs.rbx = v->state.gpr[IA32_EBX];
    v->gprs.rcx = v->state.gpr[IA32_ECX];
    v->gprs.rdx = v->state.gpr[IA32_EDX];
    v->gprs.rsi = v->state.gpr[IA32_ESI];
    v->gprs.rdi = v->state.gpr[IA32_EDI];
    v->gprs.rbp = v->state.gpr[IA32_EBP];
    return true;
}
static bool native_import(struct ia32_container *container, const struct ia32_native_state *s,
                          uint64_t dirty, struct ia32_setup_error *error)
{
    struct vmx_ia32_container *v = (void *)container;
    if (!native_here(v, error) || !s)
        return false;
    if (!v->configured)
        return native_error(error, IA32_EXIT_ENTRY_FAILURE, "native VMX container requires reset");
    if (dirty & ~IA32_DIRTY_ALL || (!v->imported && dirty != IA32_DIRTY_ALL))
        return native_error(error, IA32_EXIT_UNIMPLEMENTED,
                            "native VMX import requires valid dirty groups");
    struct ia32_native_state candidate = v->state;
    native_merge(&candidate, s, dirty);
    if (!vmx_ia32_validate(&candidate, error) ||
        !vmx_ia32_control_state(&v->caps, &candidate, error) ||
        !amd64_ia32_fp_prepare(v->fp, &candidate.fp, error))
        return false;
    v->state = candidate;
    if (!native_put(v, error))
        return false;
    v->imported = true;
    return true;
}
static bool native_export(struct ia32_container *container, struct ia32_native_state *s,
                          struct ia32_setup_error *error)
{
    struct vmx_ia32_container *v = (void *)container;
    if (!native_here(v, error) || !s)
        return false;
    if (!v->imported)
        return native_error(error, IA32_EXIT_ENTRY_FAILURE,
                            "native VMX state has not been imported");
    *s = v->state;
    return true;
}
static bool native_fence(struct vmx_ia32_container *v, struct ia32_setup_error *error)
{
    if (!native_here(v, error))
        return false;
    if (!amd64_virtualization_tag_invalidate(v->owner, AMD64_VIRT_IA32, &v->tag))
        return native_error(error, IA32_EXIT_ENTRY_FAILURE,
                            "native VMX invalidation could not be recorded");
    return true;
}
static bool native_map(struct ia32_container *container, const struct ia32_mapping *m,
                       struct ia32_setup_error *error)
{
    struct vmx_ia32_container *v = (void *)container;
    if (!native_here(v, error) || !v->configured)
        return native_error(error, IA32_EXIT_ENTRY_FAILURE, "native VMX container requires reset");
    if (!native_fence(v, error) || !amd64_ia32_map_set(&v->map, m, error))
        return false;
    v->generations = m->generations;
    return true;
}
static bool native_unmap(struct ia32_container *container, ia32_token_t token, uint64_t bytes,
                         struct ia32_setup_error *error)
{
    struct vmx_ia32_container *v = (void *)container;
    if (!native_here(v, error) || !v->configured)
        return native_error(error, IA32_EXIT_ENTRY_FAILURE, "native VMX container requires reset");
    return native_fence(v, error) && amd64_ia32_map_unset(&v->map, token, bytes, error);
}
static bool native_invalidate(struct ia32_container *container, const struct ia32_generations *g,
                              struct ia32_setup_error *error)
{
    struct vmx_ia32_container *v = (void *)container;
    if (!native_fence(v, error))
        return false;
    if (g)
        v->generations = *g;
    return true;
}
static struct ia32_native_exit native_enter(struct ia32_container *container,
                                            struct ia32_native_state *s,
                                            const struct ia32_run_limit *limit,
                                            struct ia32_run_usage *usage)
{
    struct vmx_ia32_container *v = (void *)container;
    struct ia32_setup_error error = {0};
    struct ia32_native_exit e = {
        .backend = IA32_BACKEND_VMX, .reason = IA32_EXIT_ENTRY_FAILURE, .phase = IA32_PHASE_BEFORE};
    if (usage)
        memset(usage, 0, sizeof(*usage));
    if (!native_here(v, &error) || !s)
        goto failed;
    if (!v->imported || !v->configured) {
        error.detail = "native VMX entry without imported state";
        goto failed;
    }
    *s = v->state;
    e.eip = s->eip;
    e.linear_ip.value = (uint32_t)(s->cs.base + s->eip.value);
    e.position_valid = true;
    e.generations = v->generations;
    if (!limit || !limit->deadline || limit->deadline == UINT64_MAX) {
        error.detail = "native VMX entry requires a finite deadline";
        goto failed;
    }
    if (limit->instructions || (limit->step && !v->capabilities.mtf)) {
        e.reason = IA32_EXIT_UNIMPLEMENTED;
        error.detail = limit->instructions ? "VMX has no completed-instruction counter"
                                           : "VMX monitor trap flag is unavailable";
        goto failed;
    }
    if (hal_counter() >= limit->deadline) {
        e.reason = IA32_EXIT_DEADLINE;
        return e;
    }
    if (!native_put(v, &error) || !amd64_ia32_fp_prepare(v->fp, &v->state.fp, &error))
        goto failed;
    uint32_t primary = v->caps.proc | (limit->step ? UINT32_C(1) << 27 : 0);
    if (!vmx_write(VMCS_PROC_CONTROLS, primary) || !vmx_write(VMCS_ENTRY_INTR_INFO, 0)) {
        error.detail = "native VMX entry controls VMWRITE failed";
        goto failed;
    }
    uint8_t ignored;
    if (!amd64_virtualization_tag_prepare(v->owner, AMD64_VIRT_IA32, &v->tag, v->eptp, &ignored)) {
        error.detail = "native VMX pending invalidation failed";
        goto failed;
    }
    uint64_t flags = vmx_native_irq_save();
    if (!amd64_ia32_fp_host_admit(v->fp, &error)) {
        vmx_native_irq_restore(flags, false, false);
        e.reason = IA32_EXIT_UNIMPLEMENTED;
        goto failed;
    }
    uint64_t start = hal_counter();
    if (start >= limit->deadline) {
        vmx_native_irq_restore(flags, false, false);
        e.reason = IA32_EXIT_DEADLINE;
        return e;
    }
    /* Arm with host interrupts blocked so an early timer cannot be
     * consumed before VM entry. External-interrupt exiting ignores guest IF. */
    hal_timer_arm(limit->deadline);
    struct amd64_ia32_segment_view segment_view = amd64_ia32_segment_view(&v->state);
    uint64_t tsc_aux = vmx_native_tsc_aux_enter(v->tsc_aux);
    int status = amd64_vmx_ia32_vmrun(&v->gprs, v->launched, &v->xstate, v->state.dr);
    vmx_native_tsc_aux_leave(v->tsc_aux, tsc_aux);
    uint64_t end = hal_counter();
    hal_timer_cancel();
    if (usage)
        usage->residence_ticks = end - start;
    bool nmi = false;
    if (!status) {
        struct vmx_ia32_exit_fields f = {
            .reason = (uint32_t)vmx_read(VMCS_EXIT_REASON),
            .intr_info = (uint32_t)vmx_read(VMCS_EXIT_INTR_INFO),
            .intr_error = (uint32_t)vmx_read(VMCS_EXIT_INTR_ERROR),
            .vectoring_info = (uint32_t)vmx_read(VMCS_IDT_VECTORING_INFO),
            .vectoring_error = (uint32_t)vmx_read(0x440a),
            .length = (uint32_t)vmx_read(VMCS_EXIT_INSTR_LENGTH),
            .instruction_info = (uint32_t)vmx_read(0x440e),
            .hardware_eflag = (uint32_t)vmx_read(VMCS_GUEST_RFLAGS),
            .interruptibility = (uint32_t)vmx_read(VMCS_GUEST_INTERRUPTIBILITY),
            .qualification = vmx_read(VMCS_EXIT_QUALIFICATION),
            .token = vmx_read(VMCS_GUEST_PHYSICAL),
            .linear = vmx_read(0x640a)};
        if (!(f.reason & VMX_EXIT_ENTRY_FAILURE)) {
            v->launched = true;
            vmx_ia32_get_state(vmread_fn, NULL, &v->state);
            v->state.gpr[IA32_EAX] = (uint32_t)v->gprs.rax;
            v->state.gpr[IA32_EBX] = (uint32_t)v->gprs.rbx;
            v->state.gpr[IA32_ECX] = (uint32_t)v->gprs.rcx;
            v->state.gpr[IA32_EDX] = (uint32_t)v->gprs.rdx;
            v->state.gpr[IA32_ESI] = (uint32_t)v->gprs.rsi;
            v->state.gpr[IA32_EDI] = (uint32_t)v->gprs.rdi;
            v->state.gpr[IA32_EBP] = (uint32_t)v->gprs.rbp;
        }
        amd64_ia32_fp_complete(v->fp, &v->state.fp);
        amd64_ia32_restore_fp_selectors(&v->state, segment_view);
        e = vmx_ia32_decode_exit(&f, &v->state, v->capabilities.vmx_ins_outs_info);
        if (!(f.reason & VMX_EXIT_ENTRY_FAILURE))
            e.dirty = IA32_DIRTY_ALL;
        e.generations = v->generations;
        if (e.reason == IA32_EXIT_HOST_IRQ && end >= limit->deadline)
            e.reason = IA32_EXIT_DEADLINE;
        amd64_ia32_step_host_phase(&e, limit->step);
        nmi = e.reason == IA32_EXIT_HOST_NMI;
    } else {
        amd64_ia32_fp_complete(v->fp, &v->state.fp);
        amd64_ia32_restore_fp_selectors(&v->state, segment_view);
        e.raw_reason = status == 1 ? UINT64_MAX : vmx_read(VMCS_INSTRUCTION_ERROR);
        e.detail = status == 1 ? "native VM entry VMfailInvalid" : "native VM entry VMfailValid";
    }
    *s = v->state;
    /* FP and host debug state have been restored before any host delivery. */
    vmx_native_irq_restore(flags, true, nmi);
    return e;
failed:
    if (error.detail &&
        (error.reason == IA32_EXIT_UNIMPLEMENTED || error.reason == IA32_EXIT_ENTRY_FAILURE))
        e.reason = error.reason;
    e.detail = error.detail;
    return e;
}
static bool native_reset(struct ia32_container *container, struct ia32_setup_error *error)
{
    struct vmx_ia32_container *v = (void *)container;
    if (!native_fence(v, error))
        return false;
    if (!native_clear(v))
        return native_error(error, IA32_EXIT_ENTRY_FAILURE, "native reset VMCLEAR failed");
    v->configured = false;
    v->imported = false;
    amd64_virtualization_tag_release(v->owner, AMD64_VIRT_IA32, &v->tag);
    memset(&v->tag, 0, sizeof(v->tag));
    memset(&v->state, 0, sizeof(v->state));
    memset(&v->gprs, 0, sizeof(v->gprs));
    memset(&v->generations, 0, sizeof(v->generations));
    amd64_ia32_map_drop(&v->map);
    if (!amd64_virtualization_tag_acquire(v->owner, AMD64_VIRT_IA32, &v->tag, error))
        return false;
    if (!amd64_ia32_map_init(&v->map, IA32_BACKEND_VMX, v->capabilities.ept_execute_only,
                             v->caps.maxphyaddr, error))
        return false;
    v->eptp = v->map.root | EPTP_WB_4LEVEL;
    return native_setup_vmcs(v, error);
}
static void native_destroy(struct ia32_container *container)
{
    struct vmx_ia32_container *v = (void *)container;
    if (!v || v->cpu != hal_cpu_index())
        return;
    if (!native_clear(v))
        hal_panic("VMX: native destroy VMCLEAR failed");
    native_free(v);
}
const struct ia32_backend_ops amd64_vmx_ia32_ops = {
    .probe = native_public_probe,
    .create = native_create,
    .import = native_import,
    .export = native_export,
    .map = native_map,
    .unmap = native_unmap,
    .enter = native_enter,
    .invalidate = native_invalidate,
    .reset = native_reset,
    .destroy = native_destroy,
};
