/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/interrupt.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64/tlb.h"
#include "host/amd64/udiv128.h"
#include "ia64vmm/interrupts.h"
#include "ia64vmm/platform.h"
#include "ia64/fault-count.h"
#include "ia64/fp.h"

void vcpu_kick(struct vcpu *v)
{
    /* Separate from requested_exit so a wake cannot overwrite HALT, DEBUG
     * or a fatal exit, nor interrupt a lockstep region's replay. */
    __atomic_store_n(&v->irq_kick, 1, __ATOMIC_RELEASE);
}

static unsigned highest(const uint64_t bits[4])
{
    if (bits[0] & 4)
        return 2;
    if (bits[0] & 1)
        return 0;
    for (unsigned i = 4; i--;) {
        uint64_t word = bits[i];
        if (!i)
            word &= ~UINT64_C(0xffff);
        if (word)
            return i * 64 + 63 - (unsigned)__builtin_clzll(word);
    }
    return 15;
}
static unsigned pending(const struct vcpu *v)
{
    uint64_t bits[4], any = 0;
    for (unsigned i = 0; i < 4; ++i)
        any |= bits[i] = __atomic_load_n(&v->s.cr[CR_IRR0 + i], __ATOMIC_ACQUIRE);
    /* No request at all: the usual case at every engine boundary. */
    if (!any)
        return 15;
    unsigned vector = highest(bits), service = highest(v->in_service);
    uint64_t tpr = v->s.cr[CR_TPR];
    /* SDM Vol.2 Table 5-8: NMI > ExtINT > numeric vectors. In-service
     * priority compares vectors, whereas TPR.mic compares classes. */
    if (vector == 15 || service == 2 || vector == service)
        return 15;
    if (vector == 2)
        return 2;
    if (tpr & (1u << 16))
        return 15;
    if (service == 0)
        return 15;
    if (vector == 0)
        return 0;
    if ((service != 15 && vector <= service) || (vector >> 4) <= ((tpr >> 4) & 15))
        return 15;
    return vector;
}
bool vcpu_accept_interrupt(struct vcpu *v, unsigned vector)
{
    if (!v || (vector != 0 && vector != 2 && (vector < 16 || vector > 255)))
        return false;
    __atomic_fetch_or(&v->s.cr[CR_IRR0 + vector / 64], UINT64_C(1) << (vector % 64),
                      __ATOMIC_RELEASE);
    return true;
}
bool vcpu_accept_pal_interrupt(struct vcpu *v, unsigned mode, unsigned vector)
{
    if (!v || (mode != IA64_IRQ_INIT && (mode != IA64_IRQ_PMI || vector > 15)))
        return false;
    /* No entry, no delivery; never a silent drop. */
    if (mode == IA64_IRQ_INIT ? !v->cfg.pale_init : !v->pal_pale_pmi) {
        if (mode == IA64_IRQ_INIT)
            LOGE("vCPU%u INIT: the firmware has no PALE_INIT", v->cfg.index);
        else
            LOGE("vCPU%u PMI vector %u: no PALE_PMI registered", v->cfg.index, vector);
        vcpu_request_exit(v, VCPU_EXIT_UNIMPLEMENTED);
        return false;
    }
    if (mode == IA64_IRQ_INIT)
        v->pal_init_pending = true;
    else
        v->pal_pmi_pending |= (uint16_t)(1u << vector);
    return true;
}
bool vcpu_pal_interrupt_pending(const struct vcpu *v)
{
    return v->pal_init_pending || v->pal_mca_pending || v->pal_pmi_pending;
}
uint64_t vcpu_interrupt_lid(const struct vcpu *v)
{
    return v->s.cr[CR_LID];
}
bool vcpu_raise_local_interrupt(struct vcpu *v, enum vcpu_local_irq source)
{
    unsigned cr;
    switch (source) {
    case VCPU_LOCAL_ITV:
        cr = CR_ITV;
        break;
    case VCPU_LOCAL_PMV:
        cr = CR_PMV;
        break;
    case VCPU_LOCAL_CMCV:
        cr = CR_CMCV;
        break;
    case VCPU_LOCAL_LINT0:
        cr = CR_LRR0;
        break;
    case VCPU_LOCAL_LINT1:
        cr = CR_LRR1;
        break;
    default:
        return false;
    }
    uint64_t entry = v->s.cr[cr];
    if (entry & (1u << 16))
        return false;
    unsigned vector = entry & 255, mode = cr >= CR_LRR0 ? (entry >> 8) & 7 : IA64_IRQ_INT;
    if (mode == IA64_IRQ_NMI)
        vector = 2;
    else if (mode == IA64_IRQ_EXTINT)
        vector = 0;
    else if (mode == IA64_IRQ_INIT || mode == IA64_IRQ_PMI) {
        vcpu_request_exit(v, VCPU_EXIT_UNIMPLEMENTED);
        return false;
    } else if (mode != IA64_IRQ_INT)
        return false;
    return vcpu_accept_interrupt(v, vector);
}
/* SDM Vol.2 Table 5-6: machine check, INIT, PMI, then external interrupts.
 * PSR.mc holds MCA and INIT (§11.3.1, §11.4.1); PSR.ic = 0 masks PMI and
 * PSR.i does not (§11.5.1). */
enum pal_kind { PAL_NONE, PAL_MCA, PAL_INIT, PAL_PMI };
static enum pal_kind pal_deliverable(const struct vcpu *v)
{
    if (!(v->s.psr & IA64_PSR_MC)) {
        if (v->pal_mca_pending)
            return PAL_MCA;
        if (v->pal_init_pending)
            return PAL_INIT;
    }
    return v->pal_pmi_pending && (v->s.psr & IA64_PSR_IC) ? PAL_PMI : PAL_NONE;
}
/* A Tier0 lockstep shadow replays the entry; the log names it once. */
extern bool tier0_is_shadow(const struct vcpu *) __attribute__((weak));
/* SDM Vol.2 §11.3.2, §11.4.2, §11.5.2: with PSR.ic = 1, IPSR <- PSR (ri the
 * slot), IIP <- IP and IFS.v <- 0; PSR keeps mc, mfl, mfh and pk, INIT and
 * MCA set mc, and every other bit is 0, which selects bank 0. Execution
 * continues at the physical entrypoint. */
static void pal_enter(struct vcpu *v, enum pal_kind kind, unsigned cause)
{
    struct ia64_state *s = &v->s;
    v->code_entry_generation = 0;
    if (kind != PAL_PMI)
        ia64_pal_capture_state(v);
    s->rse_cfle = false;
    bool collect = s->psr & IA64_PSR_IC;
    if (!tier0_is_shadow || !tier0_is_shadow(v))
        LOGI("vCPU%u %s at ip=%llx slot=%u psr=%llx", v->cfg.index,
             kind == PAL_MCA    ? "machine check"
             : kind == PAL_INIT ? "INIT"
                                : "PMI",
             (unsigned long long)s->ip, s->slot, (unsigned long long)s->psr);
    if (collect) {
        s->cr[CR_IPSR] = (s->psr & ~(UINT64_C(3) << 41)) | ((uint64_t)s->slot << 41);
        s->cr[CR_IIP] = s->ip;
        s->cr[CR_IFS] = 0;
    }
    uint64_t psr = s->psr & (IA64_PSR_MC | IA64_PSR_MFL | IA64_PSR_MFH | IA64_PSR_PK);
    uint64_t entry;
    if (kind == PAL_PMI) {
        unsigned vector = 31 - (unsigned)__builtin_clz(v->pal_pmi_pending);
        v->pal_pmi_pending &= (uint16_t)~(1u << vector);
        v->pal_pmi_vector = vector;
        entry = v->pal_pale_pmi;
    } else {
        psr |= IA64_PSR_MC;
        if (kind == PAL_MCA)
            v->pal_mca_pending = 0;
        else
            v->pal_init_pending = false;
        v->pal_event = (kind == PAL_MCA ? IA64VMM_PAL_EVENT_MCA : IA64VMM_PAL_EVENT_INIT) | cause |
                       (collect ? IA64VMM_PAL_EVENT_COLLECTED : 0);
        entry = kind == PAL_MCA ? v->cfg.pale_check : v->cfg.pale_init;
    }
    ia64_psr_write(s, psr);
    s->ip = entry & ~UINT64_C(15);
    s->slot = 0;
}
unsigned ia64_ivr_read(struct vcpu *v)
{
    ia64_timer_poll(v);
    unsigned vector = pending(v);
    if (vector != 15) {
        uint64_t bit = UINT64_C(1) << (vector % 64);
        __atomic_fetch_and(&v->s.cr[CR_IRR0 + vector / 64], ~bit, __ATOMIC_ACQ_REL);
        v->in_service[vector / 64] |= bit;
    }
    return vector;
}
void ia64_eoi(struct vcpu *v)
{
    unsigned vector = highest(v->in_service);
    if (vector != 15)
        v->in_service[vector / 64] &= ~(UINT64_C(1) << (vector % 64));
    extern void platform_intx_local_eoi(struct vcpu *, unsigned) __attribute__((weak));
    if (vector != 15 && platform_intx_local_eoi)
        platform_intx_local_eoi(v, vector);
}
void ia64_timer_program(struct vcpu *v)
{
    v->timer_last = ia64_itc_read(v);
    v->timer_armed = true;
    v->timer_fired = false;
    v->timer_cache.valid = false;
}
bool ia64_timer_poll_noop(const struct vcpu *v)
{
    if (!v->timer_armed || v->timer_fired)
        return true;
    const struct ia64_timer_cache *c = &v->timer_cache;
    return !v->cfg.itc_icount && !v->clock_hook && c->valid && hal_counter() - c->host < c->span;
}
void ia64_timer_poll(struct vcpu *v)
{
    if (!v->timer_armed || v->timer_fired)
        return;
    struct ia64_timer_cache *c = &v->timer_cache;
    bool host_clock = !v->cfg.itc_icount && !v->clock_hook;
    uint64_t host = host_clock ? hal_counter() : 0;
    /* ITM/ITC writes, reset and restore invalidate the cache. Frequency
     * and clock origin are fixed for the run; timer_last changes only on
     * those boundaries or below, where the old binding is discarded. */
    if (host_clock && c->valid && host - c->host < c->span)
        return;
    c->valid = false;
    uint64_t hz = host_clock ? hal_counter_hz() : 0, phase = 0;
    uint64_t now = host_clock ? ia64_itc_read_host(v, host, hz, &phase) : ia64_itc_read(v);
    /* Equality is sampled by crossing it, including unsigned ITC wrap. An
     * ITM programmed in the past waits for wrap rather than firing now. */
    if (now - v->timer_last >= v->s.cr[CR_ITM] - v->timer_last) {
        v->timer_fired = true;
        if (!(v->s.cr[CR_ITV] & (1u << 16)))
            vcpu_raise_external(v, v->s.cr[CR_ITV] & 255);
    }
    v->timer_last = now;
    if (host_clock && !v->timer_fired) {
        uint64_t delta = v->s.cr[CR_ITM] - now;
        /* The first host tick that can cross ITM is ceil((delta * hz -
         * phase) / itc_hz). Stop caching before elapsed-counter wrap. */
        __uint128_t product = (__uint128_t)delta * hz - phase;
        uint64_t span, rest;
        if (amd64_udiv128(product >> 64, (uint64_t)product, v->cfg.itc_hz, &span, &rest) &&
            span <= UINT64_MAX / 2) {
            span += rest != 0;
            if (span && span <= UINT64_MAX / 2 && span <= UINT64_MAX - (host - v->clock_origin))
                *c = (struct ia64_timer_cache){.host = host, .span = span, .valid = true};
        }
    }
}
uint64_t vcpu_next_timer_deadline(const struct vcpu *v)
{
    if (!v->timer_armed || v->timer_fired || v->cfg.itc_icount || (v->s.cr[CR_ITV] & (1u << 16)))
        return UINT64_MAX;
    uint64_t now = hal_counter(), hz = hal_counter_hz(), phase = 0;
    uint64_t itc = v->clock_hook ? ia64_itc_read(v) : ia64_itc_read_host(v, now, hz, &phase);
    uint64_t delta = v->s.cr[CR_ITM] - itc;
    /* A past sample can still have timer work because the match crossed
     * since the last poll. Keep the idle deadline at the current counter. */
    if (itc - v->timer_last >= v->s.cr[CR_ITM] - v->timer_last)
        return now;
    /* Remove the current fractional tick before rounding up. The resulting
     * absolute deadline is independent of the host sample's tick phase. */
    __uint128_t product = (__uint128_t)delta * hz - phase;
    /* The elapsed host counter wraps at clock_origin, changing the ITC
     * conversion discontinuously. Wake there to poll before converting again. */
    uint64_t bound = !v->clock_hook && now < v->clock_origin ? v->clock_origin : UINT64_MAX;
    uint64_t quotient, remainder;
    if (!amd64_udiv128(product >> 64, (uint64_t)product, v->cfg.itc_hz, &quotient, &remainder))
        return bound;
    if (remainder) {
        if (quotient == UINT64_MAX)
            return bound;
        ++quotient;
    }
    if (quotient > UINT64_MAX - now)
        return bound;
    uint64_t deadline = now + quotient;
    return deadline < bound ? deadline : bound;
}
static void deliver_ei(struct vcpu *v, uint64_t vector, uint64_t isr, unsigned ei)
{
    struct ia64_state *s = &v->s;
    /* Every interruption ends v's code-validity stay. */
    v->code_entry_generation = 0;
    if (s->rse_cfle)
        isr |= UINT64_C(1) << 38;
    s->rse_cfle = false;
    bool collect = s->psr & IA64_PSR_IC;
    if (collect) {
        s->cr[CR_IPSR] = (s->psr & ~(UINT64_C(3) << 41)) | ((uint64_t)s->slot << 41);
        s->cr[CR_IIP] = s->ip;
        s->cr[CR_IIPA] = v->last_successful_bundle;
        s->cr[CR_IFS] = 0; /* cover saves IFM; until then the frame stays current. */
    }
    if (vector != 0x1400) /* SDM Vol.2 §5.5: Data Nested TLB preserves ISR. */
        s->cr[CR_ISR] = isr | ((uint64_t)ei << 41) | (collect ? 0 : UINT64_C(1) << 39);
    /* SDM Vol.2 §5.5: translations and machine-check mask persist, user
     * profiling bits persist, DCR supplies BE/PP, and bank 0 becomes visible. */
    uint64_t psr = s->psr & (UINT64_C(0x30) | (UINT64_C(1) << 2) | (UINT64_C(1) << 15) |
                             IA64_PSR_DT | IA64_PSR_RT | (UINT64_C(1) << 35) | IA64_PSR_IT);
    psr |= s->cr[CR_DCR] & IA64_PSR_BE;
    if (s->cr[CR_DCR] & 1)
        psr |= UINT64_C(1) << 21;
    ia64_psr_write(s, psr);
    s->ip = (s->cr[CR_IVA] & ~UINT64_C(0x7fff)) | vector;
    s->slot = 0;
}
static void deliver(struct vcpu *v, uint64_t vector, uint64_t isr)
{
    deliver_ei(v, vector, isr, v->s.slot);
}
bool ia64_external_deliver(struct vcpu *v)
{
    enum pal_kind kind = pal_deliverable(v);
    if (kind != PAL_NONE) {
        pal_enter(v, kind, kind == PAL_MCA ? v->pal_mca_pending : 0);
        return true;
    }
    if (!((v->s.psr & IA64_PSR_I) && pending(v) != 15))
        return false;
    extern void platform_irq_ivt(struct vcpu *, unsigned) __attribute__((weak));
    if (platform_irq_ivt)
        platform_irq_ivt(v, pending(v));
    deliver(v, 0x3000, 0);
    return true;
}
bool ia64_external_pending(const struct vcpu *v)
{
    return pal_deliverable(v) != PAL_NONE || ((v->s.psr & IA64_PSR_I) && pending(v) != 15);
}
bool vcpu_halt_wake_pending(const struct vcpu *v)
{
    /* SDM Vol.2 §§11.6, 5.8.3.3: HALT_LIGHT ignores PSR.i and in-service
     * priority. Test every source so a masked request cannot hide another
     * eligible one. */
    if (vcpu_pal_interrupt_pending(v))
        return true;
    uint64_t bits[4];
    for (unsigned i = 0; i < 4; ++i)
        bits[i] = __atomic_load_n(&v->s.cr[CR_IRR0 + i], __ATOMIC_ACQUIRE);
    uint64_t tpr = v->s.cr[CR_TPR];
    if (bits[0] & 4)
        return true; /* NMI */
    if (tpr & (UINT64_C(1) << 16))
        return false; /* TPR.mmi */
    if (bits[0] & 1)
        return true; /* ExtINT */
    bits[0] &= ~UINT64_C(0xffff);
    for (unsigned i = 4; i--;)
        if (bits[i])
            return ((i * 64 + 63 - (unsigned)__builtin_clzll(bits[i])) >> 4) > ((tpr >> 4) & 15);
    return false;
}
/* ia64/fault-count.h */
static uint64_t faults_delivered[IA64VMM_MAX_CPUS];
uint64_t ia64_faults_delivered(const struct vcpu *v)
{
    return v->cfg.index < IA64VMM_MAX_CPUS
               ? __atomic_load_n(&faults_delivered[v->cfg.index], __ATOMIC_RELAXED)
               : 0;
}
static unsigned stop_fault, log_faults;
void ia64_set_stop_fault(unsigned kind)
{
    stop_fault = kind;
}
void ia64_set_log_faults(unsigned count)
{
    log_faults = count;
}
static bool routine_fault(enum ia64_fault_kind kind, uint64_t psr)
{
    switch (kind) {
    case IA64_FAULT_ALT_DTLB:
    case IA64_FAULT_ALT_ITLB:
    case IA64_FAULT_DTLB:
    case IA64_FAULT_ITLB:
    case IA64_FAULT_VHPT:
    case IA64_FAULT_NESTED_DTLB:
    case IA64_FAULT_PAGE_NOT_PRESENT:
    case IA64_FAULT_ACCESS_BIT:
    case IA64_FAULT_DIRTY_BIT:
        return true;
    /* Loaders run with PSR.it=0 and lean on the firmware assist; once an
     * OS translates instructions an alignment fault is worth logging. */
    case IA64_FAULT_UNALIGNED:
        return !(psr & IA64_PSR_IT);
    default:
        return false;
    }
}
/* Fault loops do not consume retirement budgets. Bound consecutive deliveries
 * without retirement with a diagnostic exit; lockstep shadows share the
 * per-CPU history outside architectural state. */
#define STALL_DELIVERIES (UINT32_C(1) << 20)
struct delivery_stall {
    uint64_t retired;
    uint32_t deliveries;
};
static struct delivery_stall stalls[IA64VMM_MAX_CPUS];
static void count_delivery(struct vcpu *v, struct ia64_result r)
{
    if (v->cfg.index >= IA64VMM_MAX_CPUS)
        return;
    struct delivery_stall *h = &stalls[v->cfg.index];
    if (h->retired != v->retired) {
        h->retired = v->retired;
        h->deliveries = 0;
    }
    if (++h->deliveries < STALL_DELIVERIES)
        return;
    h->deliveries = 0;
    LOGE("vCPU%u: %u interruption deliveries without a retired instruction (last %s at "
         "ip=%llx slot=%u, IVA=%llx, PSR=%llx): stopping the vCPU",
         v->cfg.index, STALL_DELIVERIES, ia64_fault_name(r.fault), (unsigned long long)v->s.ip,
         v->s.slot, (unsigned long long)v->s.cr[CR_IVA], (unsigned long long)v->s.psr);
    vcpu_request_exit(v, VCPU_EXIT_BAD_ACCESS);
}
void ia64_deliver_fault(struct vcpu *v, const Ia64Instruction *insn, struct ia64_result r)
{
    struct ia64_state *s = &v->s;
    count_delivery(v, r);
    if (r.fault == IA64_FAULT_MACHINE_CHECK) {
        pal_enter(v, PAL_MCA, (unsigned)r.info);
        return;
    }
    if (v->cfg.index < IA64VMM_MAX_CPUS)
        __atomic_fetch_add(&faults_delivered[v->cfg.index], 1, __ATOMIC_RELAXED);
    if (log_faults && !routine_fault(r.fault, s->psr)) {
        --log_faults;
        LOGW("log_faults: %s at %llx:%u raw=%llx info=%llx psr=%llx", ia64_fault_name(r.fault),
             (unsigned long long)insn->address, insn->slot, (unsigned long long)insn->raw,
             (unsigned long long)r.info, (unsigned long long)s->psr);
    }
    if (stop_fault && r.fault == stop_fault && !routine_fault(r.fault, s->psr)) {
        LOGW("stop_fault: %s at %llx:%u raw=%llx info=%llx", ia64_fault_name(r.fault),
             (unsigned long long)insn->address, insn->slot, (unsigned long long)insn->raw,
             (unsigned long long)r.info);
        /* The operands of itc/itr/rfi/mov-to-CR faults; with PSR.ic=0 the
         * interruption CRs still hold the previous interruption's values. */
        LOGW(
            "stop_fault: psr=%llx ipsr=%llx iip=%llx ifs=%llx ifa=%llx itir=%llx iva=%llx pta=%llx",
            (unsigned long long)s->psr, (unsigned long long)s->cr[CR_IPSR],
            (unsigned long long)s->cr[CR_IIP], (unsigned long long)s->cr[CR_IFS],
            (unsigned long long)s->cr[CR_IFA], (unsigned long long)s->cr[CR_ITIR],
            (unsigned long long)s->cr[CR_IVA], (unsigned long long)s->cr[CR_PTA]);
        stop_fault = 0;
        vcpu_request_exit(v, VCPU_EXIT_DEBUG);
    }
    uint64_t vector = 0x5400, isr = 0;
    switch (r.fault) {
    case IA64_FAULT_DISABLED_ISA:
        isr = 0x40;
        break;
    case IA64_FAULT_VIRTUALIZATION:
        vector = 0x6100;
        break;
    case IA64_FAULT_NAT:
        vector = 0x5600;
        isr = 0x10;
        break;
    case IA64_FAULT_RESERVED:
        isr = 0x30;
        break;
    case IA64_FAULT_PRIVILEGED:
        isr = 0x10;
        break;
    case IA64_FAULT_PRIVILEGED_REG:
        isr = 0x20;
        break;
    case IA64_FAULT_DEBUG:
        vector = 0x5900;
        break;
    case IA64_FAULT_LOWER_PRIVILEGE:
        vector = 0x5e00;
        isr = r.info;
        break;
    case IA64_FAULT_TAKEN_BRANCH:
        vector = 0x5f00;
        isr = r.info;
        break;
    case IA64_FAULT_SINGLE_STEP:
        vector = 0x6000;
        isr = r.info;
        break;
    case IA64_FAULT_BREAK:
        vector = 0x2c00;
        break;
    case IA64_FAULT_UNALIGNED:
        vector = 0x5a00;
        break;
    case IA64_FAULT_UNSUPPORTED_DATA:
        vector = 0x5b00;
        break;
    case IA64_FAULT_UNIMPL_DATA:
        isr = 0x30;
        break;
    case IA64_FAULT_ALT_DTLB:
        vector = 0x1000;
        break;
    case IA64_FAULT_NESTED_DTLB:
        vector = 0x1400;
        break;
    case IA64_FAULT_PAGE_NOT_PRESENT:
        vector = 0x5000;
        break;
    case IA64_FAULT_DATA_NAT:
        vector = 0x5600;
        isr = 0x20;
        break;
    case IA64_FAULT_KEY_MISS:
        vector = 0x1c00;
        break; /* Data Key Miss */
    case IA64_FAULT_ALT_ITLB:
        vector = 0x0c00;
        break;
    case IA64_FAULT_ITLB:
        vector = 0x0400;
        break;
    case IA64_FAULT_DTLB:
        vector = 0x0800;
        break;
    case IA64_FAULT_VHPT:
        vector = 0;
        break;
    case IA64_FAULT_KEY_PERMISSION:
        vector = 0x5100;
        break;
    case IA64_FAULT_ACCESS_RIGHTS:
        vector = 0x5300;
        break;
    case IA64_FAULT_ACCESS_BIT:
        vector = 0x2800;
        break;
    case IA64_FAULT_DIRTY_BIT:
        vector = 0x2000;
        break;
    case IA64_FAULT_INST_NAT:
        vector = 0x5600;
        isr = 0x20;
        break;
    case IA64_FAULT_UNIMPL_INST:
        vector = 0x5e00;
        isr = 0x20;
        break;
    /* FP results carry ISR.code in info (ia64/fp.h). */
    case IA64_FAULT_FP_DISABLED:
        vector = 0x5500;
        isr = r.info;
        break;
    case IA64_FAULT_FP:
        vector = 0x5c00;
        isr = r.info;
        break;
    case IA64_FAULT_FP_TRAP:
        vector = 0x5d00;
        isr = r.info;
        break;
    default:
        break;
    }
    bool fetch =
        (r.translation_fault || r.fault == IA64_FAULT_DEBUG) && (r.fault_isr & (UINT64_C(1) << 32));
    /* SDM Rev 2.3 Vol.2 Table 5-7: 0x1800 Instruction Key Miss, 0x1c00 Data
     * Key Miss, 0x2000 Dirty-Bit, 0x2400 Instruction / 0x2800 Data Access-Bit. */
    if (fetch && r.fault == IA64_FAULT_KEY_MISS)
        vector = 0x1800;
    if (fetch && r.fault == IA64_FAULT_ACCESS_RIGHTS)
        vector = 0x5200;
    if (fetch && r.fault == IA64_FAULT_ACCESS_BIT)
        vector = 0x2400;
    bool data =
        !fetch && (r.translation_fault || r.fault == IA64_FAULT_DEBUG ||
                   r.fault == IA64_FAULT_UNALIGNED || r.fault == IA64_FAULT_UNSUPPORTED_DATA ||
                   r.fault == IA64_FAULT_UNIMPL_DATA || r.fault == IA64_FAULT_ALT_DTLB ||
                   r.fault == IA64_FAULT_PAGE_NOT_PRESENT || r.fault == IA64_FAULT_DATA_NAT ||
                   r.fault == IA64_FAULT_NESTED_DTLB || r.fault == IA64_FAULT_KEY_MISS ||
                   (r.fault == IA64_FAULT_NAT &&
                    (ia64_opcode_info(insn->opcode)->family == IA64_FAMILY_MEMORY ||
                     insn->opcode == IA64_OP_TPA || insn->opcode == IA64_OP_TAK)));
    isr |= r.fault_isr;
    if (data && !r.translation_fault && !(r.fault_isr & (UINT64_C(1) << 37))) {
        bool store = ia64_opcode_is_store(insn->opcode);
        isr |= UINT64_C(1) << (store ? 33 : 34);
        if (insn->opcode == IA64_OP_FC || insn->opcode == IA64_OP_FC_I ||
            insn->opcode == IA64_OP_LFETCH_FAULT) {
            isr |= UINT64_C(1) << 35;
            isr |= insn->opcode == IA64_OP_LFETCH_FAULT ? 4 : 1;
        }
        if ((insn->opcode >= IA64_OP_XCHG1 && insn->opcode <= IA64_OP_FETCHADD8) ||
            insn->opcode == IA64_OP_CMP8XCHG16)
            isr |= (UINT64_C(1) << 33) | (UINT64_C(1) << 34);
        if (insn->fp_load_speculative ||
            (insn->opcode >= IA64_OP_LD1S && insn->opcode <= IA64_OP_LD8S) ||
            (insn->opcode >= IA64_OP_LD1SA && insn->opcode <= IA64_OP_LD8SA))
            isr |= UINT64_C(1) << 36;
    }
    if (data && (insn->opcode == IA64_OP_FC || insn->opcode == IA64_OP_FC_I ||
                 insn->opcode == IA64_OP_LFETCH_FAULT))
        isr |= (UINT64_C(1) << 35) | (insn->opcode == IA64_OP_LFETCH_FAULT ? 4 : 1);
    /* SDM Vol.2 §5.1.3, Table 5-1: non-access codes also qualify Register
     * NaT Consumption faults; tpa/tak have neither R nor W set. */
    bool probe = insn->opcode == IA64_OP_PROBE_R || insn->opcode == IA64_OP_PROBE_W ||
                 insn->opcode == IA64_OP_PROBE_RW;
    if (probe || insn->opcode == IA64_OP_TPA || insn->opcode == IA64_OP_TAK) {
        isr &= ~((UINT64_C(1) << 33) | (UINT64_C(1) << 34));
        isr |= UINT64_C(1) << 35;
        isr |= probe ? insn->probe_fault ? 5 : 2 : insn->opcode == IA64_OP_TAK ? 3 : 0;
        if (probe) {
            if (insn->opcode != IA64_OP_PROBE_W)
                isr |= UINT64_C(1) << 34;
            if (insn->opcode != IA64_OP_PROBE_R)
                isr |= UINT64_C(1) << 33;
        }
    }
    if (s->psr & IA64_PSR_IC) {
        if (data || fetch)
            s->cr[CR_IFA] = r.info;
        if ((r.translation_fault && r.fault != IA64_FAULT_UNIMPL_DATA &&
             r.fault != IA64_FAULT_UNIMPL_INST) ||
            r.fault == IA64_FAULT_ALT_DTLB || r.fault == IA64_FAULT_PAGE_NOT_PRESENT ||
            r.fault == IA64_FAULT_KEY_MISS) {
            uint64_t hash = ia64_thash(s, r.info);
            uint64_t default_address = r.fault == IA64_FAULT_VHPT ? hash : r.info;
            s->cr[CR_ITIR] = s->rr[default_address >> 61] & UINT64_C(0xfffffffc);
            if (r.fault == IA64_FAULT_VHPT || r.fault == IA64_FAULT_DTLB ||
                r.fault == IA64_FAULT_ITLB)
                s->cr[CR_IHA] = hash;
        }
        if (r.fault == IA64_FAULT_BREAK)
            s->cr[CR_IIM] = r.info;
    }
    /* ISR.ed is the instruction page's TLB.ed, independent of DCR.
     * Fetch and mandatory RSE faults have ed=0 (Vol.2 Table 8-2). */
    bool ed = r.fault == IA64_FAULT_ALT_DTLB || r.fault == IA64_FAULT_DTLB ||
              r.fault == IA64_FAULT_VHPT || r.fault == IA64_FAULT_PAGE_NOT_PRESENT ||
              r.fault == IA64_FAULT_KEY_MISS || r.fault == IA64_FAULT_KEY_PERMISSION ||
              r.fault == IA64_FAULT_ACCESS_RIGHTS || r.fault == IA64_FAULT_ACCESS_BIT ||
              r.fault == IA64_FAULT_DIRTY_BIT || r.fault == IA64_FAULT_UNALIGNED ||
              r.fault == IA64_FAULT_UNSUPPORTED_DATA || r.fault == IA64_FAULT_DEBUG;
    if (ed && data && !(r.fault_isr & (UINT64_C(1) << 37)) && (s->psr & IA64_PSR_IT)) {
        const struct ia64_tlb_entry *code = ia64_tlb_lookup(v, s->ip, 0);
        if (code && (code->pte & (UINT64_C(1) << 52)))
            isr |= UINT64_C(1) << 43;
    }
    /* The interpreter already advanced past a trapping instruction. */
    if (r.fault == IA64_FAULT_FP_TRAP || r.fault == IA64_FAULT_LOWER_PRIVILEGE ||
        r.fault == IA64_FAULT_TAKEN_BRANCH || r.fault == IA64_FAULT_SINGLE_STEP)
        deliver_ei(v, vector, isr,
                   insn->unit == IA64_UNIT_X || insn->unit == IA64_UNIT_L ? 2 : insn->slot);
    else
        deliver(v, vector, isr);
}
