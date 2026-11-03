/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/state.h"
#include "ia64/interrupt.h"
#include "ia64/pmu.h"
#include "ia64/mmu.h"
#include "ia64/fault-count.h"
#include "ia64/mmu_log.h"
#include "host/amd64/udiv128.h"
#include "ia64/smp.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"
#include "ia64vmm/platform.h"
#include "ia64vmm/log.h"
#include "ia64vmm/guest_mem.h"
#include "engine/cache/cache.h"

unsigned ia64_gr_index(const struct ia64_state *s, unsigned reg)
{
    unsigned sor = ((s->cfm >> 14) & 15) * 8;
    if (reg < 32)
        return reg;
    unsigned offset = reg - 32;
    if (offset < sor)
        offset = (offset + ((s->cfm >> 18) & 127)) % sor;
    return 32 + (s->rse_bof + offset) % IA64_N_STACKED_PHYS;
}
unsigned ia64_fr_index(const struct ia64_state *s, unsigned reg)
{
    return reg < 32 ? reg : 32 + (reg - 32 + ((s->cfm >> 25) & 127)) % 96;
}
unsigned ia64_pr_index(const struct ia64_state *s, unsigned reg)
{
    return reg < 16 ? reg : 16 + (reg - 16 + ((s->cfm >> 32) & 63)) % 48;
}
uint64_t ia64_gr_read(const struct ia64_state *s, unsigned reg)
{
    return reg ? s->gr[ia64_gr_index(s, reg)] : 0;
}
bool ia64_nat_read(const struct ia64_state *s, unsigned reg)
{
    return reg && s->nat[ia64_gr_index(s, reg)];
}
void ia64_gr_write(struct ia64_state *s, unsigned reg, uint64_t value, bool nat)
{
    if (reg) {
        unsigned at = ia64_gr_index(s, reg);
        s->gr[at] = value;
        s->nat[at] = nat;
    }
}
bool ia64_pr_read(const struct ia64_state *s, unsigned reg)
{
    return !reg || ((s->pr >> ia64_pr_index(s, reg)) & 1);
}
void ia64_pr_write(struct ia64_state *s, unsigned reg, bool value)
{
    if (reg) {
        uint64_t mask = UINT64_C(1) << ia64_pr_index(s, reg);
        s->pr = (s->pr & ~mask) | (value ? mask : 0);
    }
    s->pr |= 1;
}
uint64_t ia64_pr_pack(const struct ia64_state *s)
{
    /* SDM Vol.3 mov pr: the bulk move ignores CFM.rrb.pr. */
    return s->pr | 1;
}
void ia64_pr_unpack(struct ia64_state *s, uint64_t value, uint64_t mask)
{
    mask &= ~UINT64_C(1);
    s->pr = (s->pr & ~mask) | (value & mask) | 1;
}
void ia64_rotate(struct ia64_state *s)
{
    unsigned sor = ((s->cfm >> 14) & 15) * 8;
    unsigned gr = (s->cfm >> 18) & 127;
    unsigned fr = (s->cfm >> 25) & 127;
    unsigned pr = (s->cfm >> 32) & 63;
    gr = sor ? (gr ? gr - 1 : sor - 1) : 0;
    fr = fr ? fr - 1 : 95;
    pr = pr ? pr - 1 : 47;
    s->cfm = (s->cfm & ~((UINT64_C(127) << 18) | (UINT64_C(127) << 25) | (UINT64_C(63) << 32))) |
             ((uint64_t)gr << 18) | ((uint64_t)fr << 25) | ((uint64_t)pr << 32);
}

static uint64_t fractional_ticks(uint64_t remainder, uint64_t frequency, uint64_t divisor,
                                 uint64_t *phase)
{
    if (!frequency || remainder <= UINT64_MAX / frequency) {
        if (phase)
            *phase = remainder * frequency % divisor;
        return remainder * frequency / divisor;
    }
    /* Here remainder < divisor, so high < divisor and the 128/64 quotient
     * fits in 64 bits; the guard cannot fail. */
    __uint128_t product = (__uint128_t)remainder * frequency;
    uint64_t quotient, rest;
    if (!amd64_udiv128(product >> 64, (uint64_t)product, divisor, &quotient, &rest))
        return UINT64_MAX;
    if (phase)
        *phase = rest;
    return quotient;
}
uint64_t ia64_itc_read_host(const struct vcpu *v, uint64_t host, uint64_t hz, uint64_t *phase)
{
    uint64_t ticks = host - v->clock_origin;
    return v->clock_offset + (ticks / hz) * v->cfg.itc_hz +
           fractional_ticks(ticks % hz, v->cfg.itc_hz, hz, phase);
}
/* Engine-internal ITC value: never passes through the lockstep clock hook,
 * which records only guest-visible reads. */
uint64_t ia64_itc_read_unhooked(const struct vcpu *v)
{
    if (v->cfg.itc_icount)
        return v->clock_offset + v->retired;
    uint64_t host = hal_counter(), hz = hal_counter_hz();
    return ia64_itc_read_host(v, host, hz, NULL);
}
uint64_t ia64_itc_read(const struct vcpu *v)
{
    uint64_t value = ia64_itc_read_unhooked(v);
    return v->clock_hook ? v->clock_hook(v, value) : value;
}
void ia64_itc_write(struct vcpu *v, uint64_t value)
{
    ia64_timer_poll(v);
    v->clock_offset += value - ia64_itc_read(v);
    v->timer_last = value;
    v->timer_cache.valid = false;
}

struct vcpu *vcpu_create(const struct vcpu_config *cfg)
{
    if (!cfg || !ia64_cpu_model_valid(cfg->cpu_model) || !hal_counter_hz() ||
        cfg->index >= IA64VMM_MAX_CPUS || cfg->cores_per_socket > IA64VMM_MAX_CPUS ||
        cfg->cpu_count > IA64VMM_MAX_CPUS || (cfg->cpu_count && cfg->index >= cfg->cpu_count) ||
        (cfg->lid && cfg->lid != IA64VMM_CPU_LID(cfg->index)))
        return NULL;
    /* An instruction-count ITC gives each vCPU
     * its own time base, which no VM of more than one vCPU may have. */
    if (cfg->itc_icount && ia64_smp_active()) {
        LOGE("vCPU%u: itc_icount with more than one vCPU", cfg->index);
        return NULL;
    }
    size_t pages = (sizeof(struct vcpu) + 4095) / 4096;
    struct vcpu *v = hal_alloc_pages(pages, HAL_PAGE_ZERO);
    if (v) {
        v->cfg = *cfg;
        if (v->cfg.cpu_count)
            v->cfg.cores_per_socket = ia64_model_socket_width(cfg->cpu_model, cfg->cpu_count);
        else if (!v->cfg.cores_per_socket)
            v->cfg.cores_per_socket = 1;
        else if (v->cfg.cores_per_socket > ia64_model_socket_width(cfg->cpu_model, 2))
            v->cfg.cores_per_socket = ia64_model_socket_width(cfg->cpu_model, 2);
        if (!v->cfg.itc_hz)
            v->cfg.itc_hz = UINT64_C(400000000);
        /* Each vCPU's MMU sequence starts in its own 2^32 range, so caches
         * keyed by the vCPU's address and sequence never take a vCPU for an
         * earlier one at that address. */
        static uint64_t sequence_base;
        v->mmu_seq = v->mmu_iseq = v->mmu_dseq =
            __atomic_add_fetch(&sequence_base, UINT64_C(1) << 32, __ATOMIC_RELAXED);
        vcpu_reset(v, NULL);
    }
    return v;
}
void vcpu_reset(struct vcpu *v, const struct ia64_entry_regs *regs)
{
    memset(&v->s, 0, sizeof(v->s));
    v->s.version = IA64_STATE_VERSION;
    v->s.cpu_model = v->cfg.cpu_model;
    v->s.pr = 1;
    ia64_pmu_reset(v);
    v->s.fr[1] = (struct ia64_fr){.exponent = 0xffff, .significand = UINT64_C(1) << 63};
    v->s.ar[AR_FPSR] = UINT64_C(0x0009804c0270033f);
    v->s.ar[AR_RSC] = 3;
    v->s.cr[CR_DCR] = (1u << 8) | (1u << 9);
    /* The firmware tables use id=index and eid=0 (platform.h). */
    v->s.cr[CR_LID] = IA64VMM_CPU_LID(v->cfg.index);
    v->s.cr[CR_IVR] = 15;
    v->s.cr[CR_ITV] = v->s.cr[CR_PMV] = v->s.cr[CR_CMCV] = 1u << 16;
    v->s.cr[CR_LRR0] = v->s.cr[CR_LRR1] = 1u << 16;
    /* The VM-wide origin, so every vCPU's ITC reads the same
     * value at the same host instant, an AP in reset-hold included. */
    v->clock_origin = ia64_smp_itc_origin();
    v->clock_offset = v->retired = 0;
    v->requested_exit = 0;
    v->irq_kick = 0;
    v->loader_sample_counter = 0;
    v->pal_tr_selector = v->pal_tr_buffer = v->pal_tr_valid = 0;
    v->pal_tr_status = -2;
    v->pal_tr_pending = false;
    v->pal_xr0 = v->pal_pale_pmi = v->pal_sale_pmi = v->pal_event = v->pal_pmi_vector = 0;
    memset(v->pal_min_state, 0, sizeof(v->pal_min_state));
    v->pal_state_address = v->pal_state_pending = 0;
    v->pal_state_valid = false;
    v->pal_pmi_pending = v->pal_mca_pending = 0;
    v->pal_expected = v->pal_init_pending = false;
    v->last_successful_bundle = v->timer_last = 0;
    memset(v->in_service, 0, sizeof(v->in_service));
    v->timer_armed = v->timer_fired = false;
    v->timer_cache.valid = false;
    v->fault = (struct ia64_fault_site){0};
    v->physical_region = NULL;
    v->code_entry_generation = 0;
    v->memory_hook = NULL;
    v->clock_hook = NULL;
    v->check_context = NULL;
    v->trace_used = v->trace_next = 0;
    if (regs) {
        v->s.ip = regs->ip;
        v->s.psr = regs->psr;
        memcpy(v->s.gr + 1, regs->gr + 1, 31 * sizeof(uint64_t));
        v->s.br[0] = regs->br0;
        v->s.ar[AR_BSP] = regs->bsp;
        v->s.ar[AR_BSPSTORE] = regs->bspstore;
        v->s.ar[AR_RSC] = regs->rsc;
        v->s.ar[0] = regs->k0;
    }
    /* TR, TC, RR and PKR were all rewritten; mmu_seq lives outside s and
     * keeps counting. */
    ia64_mmu_logged(v, IA64_MMU_W_RESET, IA64_MMU_ALL, 0, 0);
    ia64_mmu_cache_reset(v);
}
void vcpu_destroy(struct vcpu *v)
{
    if (v) {
        code_cache_vcpu_destroy(v);
        ia64_set_trace(v, 0);
        if (v->check_workspace)
            hal_free_pages(v->check_workspace, v->check_workspace_pages);
        hal_free_pages(v, (sizeof(*v) + 4095) / 4096);
    }
}

void vcpu_request_exit(struct vcpu *v, enum vcpu_exit_reason reason)
{
    if ((unsigned)reason <= VCPU_EXIT_DEBUG)
        __atomic_store_n(&v->requested_exit, (unsigned)reason + 1, __ATOMIC_RELEASE);
}
void vcpu_raise_external(struct vcpu *v, unsigned vector)
{
    if (vector >= 16 && vector <= 255)
        __atomic_fetch_or(&v->s.cr[CR_IRR0 + vector / 64], UINT64_C(1) << (vector % 64),
                          __ATOMIC_RELEASE);
}

uint64_t vcpu_instruction_pointer(const struct vcpu *v, unsigned *slot)
{
    if (slot)
        *slot = v->s.slot;
    return v->s.ip;
}

void vcpu_set_stop_fault(unsigned kind)
{
    ia64_set_stop_fault(kind);
}
void vcpu_set_log_faults(unsigned count)
{
    ia64_set_log_faults(count);
}

bool vcpu_read_virtual(struct vcpu *v, uint64_t va, uint64_t *value)
{
    struct ia64_translation t;
    if (va & 7)
        return false;
    struct ia64_result r = ia64_translate(v, va, IA64_REF_TPA, 0, 0, false, &t);
    return r.kind == IA64_STEP_COMPLETE && gm_read(t.pa, value, 8);
}

void vcpu_progress(const struct vcpu *v, uint64_t *ip, unsigned *slot, uint64_t *psr,
                   uint64_t *retired, const char **fault, uint64_t *fault_ip, uint64_t *fault_info,
                   uint64_t *faults)
{
    *faults = ia64_faults_delivered(v);
    *ip = v->s.ip;
    *slot = v->s.slot;
    *psr = v->s.psr;
    *retired = v->retired;
    *fault = v->fault.kind ? ia64_fault_name(v->fault.kind) : NULL;
    *fault_ip = v->fault.ip;
    *fault_info = v->fault.info;
}

bool vcpu_instruction_physical_address(struct vcpu *v, uint64_t *pa)
{
    struct ia64_translation t;
    /* A diagnostic query must not perform the target frame's mandatory
     * loads (SDM Vol.2 §6.8); the engine does that at its next fetch. */
    bool cfle = v->s.rse_cfle;
    v->s.rse_cfle = false;
    struct ia64_result result = ia64_translate(v, v->s.ip, IA64_REF_FETCH, IA64_ACCESS_EXECUTE,
                                               (v->s.psr >> 32) & 3, false, &t);
    v->s.rse_cfle = cfle;
    if (v->trace_count) {
        uint64_t now = hal_counter(), hz = hal_counter_hz();
        if (hz && now - v->loader_sample_counter >= hz) {
            v->loader_sample_counter = now;
            LOGI(
                "loader sample ip=%llx slot=%u pa=%llx mapped=%u retired=%llu counter=%llu hz=%llu",
                (unsigned long long)v->s.ip, v->s.slot, (unsigned long long)t.pa,
                result.kind == IA64_STEP_COMPLETE && t.granted, (unsigned long long)v->retired,
                (unsigned long long)now, (unsigned long long)hz);
        }
    }
    if (result.kind != IA64_STEP_COMPLETE || !t.granted)
        return false;
    *pa = t.pa;
    return true;
}
