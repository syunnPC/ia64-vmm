/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Completed-store log of the lower tier and the reference;
 * see storelog.h. */
#include <string.h>
#include "tests/tier1/storelog.h"
#include "engine/cache/cache.h"
#include "engine/tier1/tier1.h"
#include "ia64/tlb.h"
#include "ia64vmm/guest_mem.h"

#define MAX_RANGES 8u
#define PA_LIMIT (UINT64_C(1) << (IA64_IMPL_PA_MSB + 1))

static struct storelog_range ranges[MAX_RANGES];
static unsigned range_count;
static struct gm_region proxy;
static const struct gm_region *ram_region;

void storelog_set_ranges(const struct storelog_range *r, unsigned n)
{
    T1H_CHECK(n <= MAX_RANGES);
    memcpy(ranges, r, n * sizeof(*r));
    range_count = n;
}

void storelog_default_ranges(void)
{
    static const struct storelog_range data[] = {
        {T1H_INDEX, T1H_INDEX + 0x2000},
        {T1H_STATUS, T1H_STATUS + 0x1000},
        {T1H_OUT, T1H_OUT + T1H_OUT_PAGES * UINT64_C(4096)},
        {T1H_TABLE, T1H_TABLE + T1H_TABLE_PAGES * UINT64_C(4096)},
    };
    storelog_set_ranges(data, IA64VMM_ARRAY_SIZE(data));
}

static bool in_ranges(gpa_t gpa, unsigned size)
{
    for (unsigned i = 0; i < range_count; ++i)
        if (gpa < ranges[i].hi && gpa + size > ranges[i].lo)
            return true;
    return false;
}

unsigned storelog_store_width(Ia64Opcode opcode)
{
    switch (opcode) {
    case IA64_OP_ST1:
    case IA64_OP_ST1REL:
        return 1;
    case IA64_OP_ST2:
    case IA64_OP_ST2REL:
        return 2;
    case IA64_OP_ST4:
    case IA64_OP_ST4REL:
        return 4;
    case IA64_OP_ST8:
    case IA64_OP_ST8REL:
    case IA64_OP_ST8SPILL:
        return 8;
    case IA64_OP_STFS:
        return 4;
    case IA64_OP_STFD:
    case IA64_OP_STF8:
        return 8;
    default:
        return 0;
    }
}

bool storelog_decode(const struct vcpu *v, uint64_t ip, unsigned slot, Ia64Instruction *out)
{
    gpa_t pa;
    if (slot > 2)
        return false;
    if (v->s.psr & IA64_PSR_IT) {
        /* Only an existing I-TLB entry: a VHPT walk would insert a TC entry
         * and change the state under comparison. */
        const struct ia64_tlb_entry *e = ia64_tlb_find(&v->s, ip, 0);
        if (!e || !(e->pte & 1))
            return false;
        pa = (e->pte & (PA_LIMIT - 4096) & ~ia64_low_mask(e->page_shift)) |
             (ip & ia64_low_mask(e->page_shift));
    } else {
        pa = ip & ~(UINT64_C(1) << 63);
    }
    uint8_t bytes[16];
    if (!gm_read(pa & ~UINT64_C(15), bytes, 16))
        return false;
    uint64_t low, high;
    memcpy(&low, bytes, 8);
    memcpy(&high, bytes + 8, 8);
    struct ia64_bundle bundle;
    ia64_decode_bundle(low, high, pa & ~UINT64_C(15), &bundle);
    *out = bundle.insn[slot];
    return out->valid;
}

void storelog_append_pre(struct t1h_logs *logs, uint64_t gva, uint64_t value, unsigned width,
                         uint64_t pre, unsigned kind)
{
    if (logs->store_n == T1H_STORE_MAX) {
        t1h_error(logs, "store log overflow");
        return;
    }
    logs->store_pre[logs->store_n] = pre;
    logs->store_pre_kind[logs->store_n] = (uint8_t)kind;
    logs->stores[logs->store_n++] =
        (struct compare_store_record){gva, value & ia64_low_mask(width * 8), (uint8_t)width};
}

void storelog_append(struct t1h_logs *logs, uint64_t gva, uint64_t value, unsigned width)
{
    storelog_append_pre(logs, gva, value, width, 0, T1H_PRE_NONE);
}

void storelog_t1_store(const struct t1_store_record *r, void *arg)
{
    storelog_append_pre(arg, r->gva, r->value, r->width, r->pre,
                        r->has_pre ? T1H_PRE_UNDO : T1H_PRE_NONE);
}

void storelog_t1_store_ranged(const struct t1_store_record *r, void *arg)
{
    struct vcpu *v = arg;
    struct t1h_logs *logs = t1h_logs_of(v);
    if (!logs)
        return;
    gpa_t pa;
    if (v->s.psr & IA64_PSR_DT) {
        const struct ia64_tlb_entry *e = ia64_tlb_find(&v->s, r->gva, 1);
        if (!e || !(e->pte & 1)) {
            t1h_error(logs, "Tier1 store to %#llx: no D-TLB entry left to classify it",
                      (unsigned long long)r->gva);
            return;
        }
        pa = (e->pte & (PA_LIMIT - 4096) & ~ia64_low_mask(e->page_shift)) |
             (r->gva & ia64_low_mask(e->page_shift));
    } else {
        pa = r->gva & ~(UINT64_C(1) << 63);
    }
    if (in_ranges(pa, r->width))
        storelog_append_pre(logs, r->gva, r->value, r->width, r->pre,
                            r->has_pre ? T1H_PRE_UNDO : T1H_PRE_NONE);
    else
        ++logs->stores_outside;
}

void storelog_predict(struct vcpu *v)
{
    struct t1h_logs *logs = t1h_logs_of(v);
    if (!logs || !logs->stores_on)
        return;
    Ia64Instruction insn;
    logs->boundary_valid = true;
    logs->boundary_ip = v->s.ip;
    logs->boundary_slot = v->s.slot;
    logs->predicted_valid = false;
    if (!storelog_decode(v, v->s.ip, v->s.slot, &insn))
        return;
    unsigned width = storelog_store_width(insn.opcode);
    if (!width || !ia64_pr_read(&v->s, insn.qp))
        return;
    logs->predicted_valid = true;
    logs->predicted_gva = ia64_gr_read(&v->s, insn.operands.memory.base);
    logs->predicted_width = width;
}

void storelog_step_done(struct vcpu *v)
{
    struct t1h_logs *logs = t1h_logs_of(v);
    if (logs)
        logs->boundary_valid = false;
}

/* ---- the proxy --------------------------------------------------------- */

static uint64_t proxy_read(void *opaque, struct vcpu *v, uint64_t offset, unsigned size)
{
    (void)opaque;
    (void)v;
    /* Same aligned, width-specific transaction as ia64_physical_read. */
    const volatile uint8_t *p = ram_region->hva + offset;
    switch (size) {
    case 1:
        return *p;
    case 2:
        return *(const volatile uint16_t *)p;
    case 4:
        return *(const volatile uint32_t *)p;
    default:
        return *(const volatile uint64_t *)p;
    }
}

static void proxy_write(void *opaque, struct vcpu *v, uint64_t offset, uint64_t value,
                        unsigned size)
{
    (void)opaque;
    gpa_t gpa = ram_region->base + offset;
    volatile uint8_t *p = ram_region->hva + offset;
    /* The bytes the write replaces (the reference side of the undo-log
     * test, ver_tests.c). */
    uint64_t pre;
    switch (size) {
    case 1:
        pre = *p;
        break;
    case 2:
        pre = *(volatile uint16_t *)p;
        break;
    case 4:
        pre = *(volatile uint32_t *)p;
        break;
    default:
        pre = *(volatile uint64_t *)p;
        break;
    }
    code_notify_cpu_write(gpa, size);
    switch (size) {
    case 1:
        *p = (uint8_t)value;
        break;
    case 2:
        *(volatile uint16_t *)p = (uint16_t)value;
        break;
    case 4:
        *(volatile uint32_t *)p = (uint32_t)value;
        break;
    default:
        *(volatile uint64_t *)p = value;
        break;
    }

    struct t1h_logs *logs = t1h_logs_of(v);
    if (!logs)
        return;
    if (!in_ranges(gpa, size)) {
        ++logs->stores_outside;
        return;
    }
    Ia64Instruction insn;
    if (!storelog_decode(v, v->s.ip, v->s.slot, &insn)) {
        t1h_error(logs, "store to %#llx at %#llx:%u: instruction not decodable",
                  (unsigned long long)gpa, (unsigned long long)v->s.ip, v->s.slot);
        return;
    }
    unsigned width = storelog_store_width(insn.opcode);
    if (width != size) {
        t1h_error(logs, "%u-byte write to %#llx at %#llx:%u by %s (not a %u-byte store)", size,
                  (unsigned long long)gpa, (unsigned long long)v->s.ip, v->s.slot,
                  ia64_opcode_info(insn.opcode)->name, size);
        return;
    }
    uint64_t gva = ia64_gr_read(&v->s, insn.operands.memory.base);
    if (logs->boundary_valid && v->s.ip == logs->boundary_ip && v->s.slot == logs->boundary_slot) {
        if (!logs->predicted_valid || logs->predicted_gva != gva || logs->predicted_width != size) {
            t1h_error(logs, "store at %#llx:%u: predicted GVA %#llx/%u, completed %#llx/%u",
                      (unsigned long long)v->s.ip, v->s.slot,
                      (unsigned long long)logs->predicted_gva, logs->predicted_width,
                      (unsigned long long)gva, size);
            return;
        }
        gva = logs->predicted_gva;
        ++logs->paired_stepped;
    } else {
        ++logs->paired_callback;
    }
    storelog_append_pre(logs, gva, value, size, pre, T1H_PRE_MEMORY);
}

static const struct gm_mmio_ops proxy_ops = {proxy_read, proxy_write};

/* The GR loads (SDM Vol.3 ld: plain, speculative, advanced, check, fill). */
static unsigned load_width(Ia64Opcode opcode)
{
    switch (opcode) {
    case IA64_OP_LD1:
    case IA64_OP_LD1S:
    case IA64_OP_LD1A:
    case IA64_OP_LD1SA:
    case IA64_OP_LD1C_CLR:
    case IA64_OP_LD1C_NC:
        return 1;
    case IA64_OP_LD2:
    case IA64_OP_LD2S:
    case IA64_OP_LD2A:
    case IA64_OP_LD2SA:
    case IA64_OP_LD2C_CLR:
    case IA64_OP_LD2C_NC:
        return 2;
    case IA64_OP_LD4:
    case IA64_OP_LD4S:
    case IA64_OP_LD4A:
    case IA64_OP_LD4SA:
    case IA64_OP_LD4C_CLR:
    case IA64_OP_LD4C_NC:
        return 4;
    case IA64_OP_LD8:
    case IA64_OP_LD8S:
    case IA64_OP_LD8A:
    case IA64_OP_LD8SA:
    case IA64_OP_LD8C_CLR:
    case IA64_OP_LD8C_NC:
    case IA64_OP_LD8FILL:
        return 8;
    default:
        return 0;
    }
}

static const struct gm_region *hook(struct vcpu *v, gpa_t gpa, unsigned size)
{
    /* The same bounds ia64_physical_region applies without a hook. */
    const struct gm_region *r = gm_lookup(gpa);
    if (!r || size > r->size - (gpa - r->base))
        return NULL;
    if (r != ram_region)
        return r;
    /* Preserve RAM semantics for loads (ld.s and ld.a of an emulated page
     * defer or read zero), and outside the ranges for atomics and RSE
     * helpers. Native records describe guest integer stores only. */
    Ia64Instruction insn;
    bool decoded = storelog_decode(v, v->s.ip, v->s.slot, &insn);
    if (decoded && load_width(insn.opcode) == size)
        return r;
    if (!in_ranges(gpa, size) && (!decoded || storelog_store_width(insn.opcode) != size))
        return r;
    if (size == 1 || size == 2 || size == 4 || size == 8)
        return &proxy;
    if (!in_ranges(gpa, size))
        return r;
    /* 16-byte and FP-spill accesses would bypass the log. */
    struct t1h_logs *logs = t1h_logs_of(v);
    if (logs)
        t1h_error(logs, "unlogged %u-byte access to data page %#llx", size,
                  (unsigned long long)gpa);
    return r;
}

void storelog_attach(struct vcpu *v)
{
    struct t1h_logs *logs = t1h_logs_of(v);
    T1H_CHECK(logs != NULL);
    if (!ram_region) {
        ram_region = gm_lookup(T1H_RAM_BASE);
        T1H_CHECK(ram_region && ram_region->kind == GM_RAM);
        proxy = *ram_region;
        proxy.kind = GM_EMULATED;
        proxy.ops = &proxy_ops;
        proxy.name = "t1-store-log";
    }
    if (!range_count)
        storelog_default_ranges();
    logs->stores_on = true;
    v->memory_hook = hook;
    /* Keeps Tier0's inline accesses off: they would bypass the proxy. */
    v->physical_region = NULL;
}

void storelog_detach(struct vcpu *v)
{
    struct t1h_logs *logs = t1h_logs_of(v);
    if (logs)
        logs->stores_on = false;
    v->memory_hook = NULL;
    v->physical_region = NULL;
}
