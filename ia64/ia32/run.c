/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/ia32/run.h"
#include "ia64/ia32/memory.h"
#include "ia64/ia32/extended.h"
#include "ia64/ia32/string.h"
#include "ia64/ia32/scalar.h"
#include "ia64/ia32/segment.h"
#include "ia64/interrupt.h"
#include "ia64/mmu.h"
#include "ia64/debug.h"
#include "engine/cache/cache.h"
#include "engine/cache/validity.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"
#include "ia64vmm/log.h"

struct ia32_vcpu_private {
    const struct ia32_backend_ops *ops;
    struct ia32_container *container;
    struct ia32_capabilities caps;
    struct ia32_native_state state;
    struct ia32_generations generations;
    struct vcpu_isa_snapshot snapshot;
    unsigned cpu, snapshot_lock;
    unsigned operand_debug;
    uint64_t instructions, ticks;
    bool instructions_valid, pending, unresolved;
    unsigned pending_debug;
    struct ia32_authorization authorization;
    struct ia32_decoded decoded;
    uint8_t instruction[15];
    uint32_t code_token;
    uint8_t code[4096] __attribute__((aligned(4096)));
    uint8_t scratch[IA32_MEMORY_PAGES][4096] __attribute__((aligned(4096)));
};
#define PRIVATE_PAGES ((sizeof(struct ia32_vcpu_private) + 4095) / 4096)
static struct ia64_result unsupported(const char *detail)
{
    return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, 0, detail);
}
static struct vcpu_position position(const struct vcpu *v)
{
    bool ia32 = (v->s.psr & IA64_PSR_IS) != 0;
    struct vcpu_position p = {.isa = ia32 ? VCPU_ISA_IA32 : VCPU_ISA_IA64,
                              .ip = ia32 ? (uint32_t)v->s.ip : v->s.ip,
                              .slot = ia32 ? 0 : v->s.slot};
    if (ia32) {
        p.eip_valid = true;
        p.eip.value = (uint32_t)v->s.ip - (uint32_t)v->s.ar[AR_CSD];
    }
    return p;
}
static struct ia64_result state_error(enum ia32_state_status status)
{
    switch (status) {
    case IA32_STATE_OK:
        return ia64_complete();
    case IA32_STATE_ILLEGAL:
        return ia64_fault(IA64_FAULT_ILLEGAL, 0);
    case IA32_STATE_PRIVILEGED:
        return ia64_fault(IA64_FAULT_PRIVILEGED, 0);
    case IA32_STATE_RESERVED:
        return ia64_fault(IA64_FAULT_RESERVED, 0);
    case IA32_STATE_DISABLED:
        return ia64_fault(IA64_FAULT_DISABLED_ISA, 0);
    default:
        return unsupported("IA-32 architectural state cannot be represented");
    }
}
struct ia64_result ia32_enter(struct vcpu *v, enum ia32_entry_kind kind, uint64_t target)
{
    struct ia32_native_state n;
    struct ia32_interruption completion;
    enum ia32_state_status status = ia32_state_enter(&v->s, kind, target, &n, &completion);
    if (status != IA32_STATE_OK)
        return state_error(status);
    v->code_entry_generation = 0;
    ia64_mmu_cache_reset(v);
    if (completion.valid) {
        status = ia32_state_leave(&v->s, &n, 0, false);
        if (status != IA32_STATE_OK)
            return state_error(status);
        ia32_deliver(v, &completion);
    }
    return (struct ia64_result){.kind = IA64_STEP_BRANCH, .info = v->s.ip, .next_slot = 0};
}
static bool attach(struct vcpu *v, unsigned cpu, struct ia32_setup_error *e)
{
    if (v->native) {
        if (v->native->cpu == cpu)
            return true;
        *e = (struct ia32_setup_error){.reason = IA32_EXIT_ENTRY_FAILURE,
                                       .host_cpu = cpu,
                                       .detail = "IA-32 executor CPU mismatch"};
        return false;
    }
    struct ia32_vcpu_private *p = hal_alloc_pages(PRIVATE_PAGES, HAL_PAGE_ZERO);
    if (!p) {
        *e = (struct ia32_setup_error){.reason = IA32_EXIT_ENTRY_FAILURE,
                                       .host_cpu = cpu,
                                       .detail = "IA-32 executor allocation failed"};
        return false;
    }
    p->cpu = cpu;
    p->instructions_valid = true;
    p->generations = (struct ia32_generations){1, 1, 1, 1};
    v->native = p;
    return true;
}
static bool reset(struct vcpu *v, struct ia32_setup_error *e)
{
    struct ia32_vcpu_private *p = v->native;
    if (!p)
        return true;
    if (p->container && !p->ops->reset(p->container, e))
        return false;
    p->snapshot = (struct vcpu_isa_snapshot){0};
    p->instructions = p->ticks = 0;
    p->pending = p->unresolved = false;
    p->instructions_valid = true;
    return true;
}
static void detach(struct vcpu *v)
{
    struct ia32_vcpu_private *p = v->native;
    if (!p)
        return;
    if (p->container)
        p->ops->destroy(p->container);
    hal_free_pages(p, PRIVATE_PAGES);
    v->native = NULL;
}
static bool backend(struct ia32_vcpu_private *p, struct ia32_setup_error *e)
{
    if (p->container)
        return true;
    p->ops = hal_ia32_backend();
    if (!p->ops) {
        *e = (struct ia32_setup_error){.reason = IA32_EXIT_UNIMPLEMENTED,
                                       .detail =
                                           "IA-32 hardware virtualization backend unavailable"};
        return false;
    }
    if (!p->ops->probe(p->cpu, &p->caps, e))
        return false;
    return p->ops->create(p->cpu, &p->container, e);
}
static struct ia64_result fetch(struct vcpu *v, const struct ia32_native_state *n, uint8_t *bytes,
                                struct ia32_decoded *d, struct ia32_interruption *record)
{
    if (!ia32_segment_valid(n, IA32_SEG_CS, IA32_SEGMENT_FETCH))
        return ia32_exception(n, 13, 0, record);
    for (unsigned i = 0; i < 15; ++i) {
        uint64_t off = (uint64_t)n->eip.value + i;
        if (off > n->cs.limit)
            return ia32_exception(n, 13, 0, record);
        struct ia64_result r = ia32_memory_fetch(
            v, (ia32_linear_t){(uint32_t)n->cs.base + (uint32_t)off}, bytes + i, 1);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        enum ia32_decode_status status = ia32_decode(bytes, i + 1, n, d);
        if (status == IA32_DECODE_OK)
            return ia64_complete();
        if (status == IA32_DECODE_ILLEGAL) {
            if (d->overlong)
                return ia32_exception(n, 13, 0, record);
            d->kind = IA32_INSN_INTERCEPT;
            return ia64_complete();
        }
        if (status == IA32_DECODE_UNSUPPORTED)
            return unsupported("IA-32 instruction decoder coverage");
    }
    return ia32_exception(n, 13, 0, record);
}
static unsigned debug_matches(const struct ia64_state *s, uint32_t address, unsigned size,
                              unsigned rights)
{
    if (!(s->psr & IA64_PSR_DB))
        return 0;
    bool exec = rights & IA64_ACCESS_EXECUTE;
    if (s->psr & (exec ? IA64_PSR_ID : IA64_PSR_DD))
        return 0;
    const uint64_t *regs = exec ? s->ibr : s->dbr;
    unsigned matches = 0, cpl = (s->psr >> 32) & 3;
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t ctrl = regs[2 * i + 1];
        if (!(ctrl & (UINT64_C(1) << (56 + cpl))) ||
            !(((rights & (IA64_ACCESS_READ | IA64_ACCESS_EXECUTE)) && (ctrl >> 63)) ||
              ((rights & IA64_ACCESS_WRITE) && (ctrl & (UINT64_C(1) << 62)))))
            continue;
        uint64_t mask = (ctrl & UINT64_C(0x00ffffffffffffff)) | UINT64_C(0xff00000000000000);
        for (unsigned j = 0; j < size; ++j)
            if (!(((uint32_t)(address + j) ^ regs[2 * i]) & mask))
                matches |= 1u << i;
    }
    return matches;
}
void ia32_note_operand(struct vcpu *v, uint32_t address, unsigned size, bool write)
{
    if (v->native)
        v->native->operand_debug |=
            debug_matches(&v->s, address, size, write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ);
}
unsigned ia32_operand_breakpoints(const struct vcpu *v)
{
    return v->native ? v->native->operand_debug : 0;
}
static struct ia64_result range(struct vcpu *v, const struct ia32_native_state *n,
                                struct ia32_access *a, unsigned segment, uint32_t offset,
                                unsigned width, enum ia32_access_kind kind,
                                struct ia32_interruption *record)
{
    const struct ia32_segment *s = ia32_segment(n, segment);
    if (!s || !width || kind == IA32_ACCESS_UNKNOWN)
        return unsupported("IA-32 memory access classification");
    unsigned type = s->attrib & 15;
    bool write = kind == IA32_ACCESS_WRITE || kind == IA32_ACCESS_RMW;
    uint64_t last = (uint64_t)offset + width - 1;
    bool expand = !(type & 8) && (type & 4);
    bool bound = expand ? offset > s->limit && last <= ((s->attrib & 0x400) ? UINT32_MAX : 0xffff)
                        : last <= s->limit;
    if (!bound || !ia32_segment_valid(n, segment, write ? IA32_SEGMENT_WRITE : IA32_SEGMENT_READ))
        return ia32_exception(n, segment == IA32_SEG_SS ? 12 : 13, 0, record);
    *a = (struct ia32_access){.kind = kind,
                              .linear = {(uint32_t)s->base + offset},
                              .linear_valid = true,
                              .width_valid = true,
                              .width = width};
    (void)v;
    return ia64_complete();
}
static struct ia64_result authorize(struct vcpu *v, const struct ia32_native_state *n,
                                    const struct ia32_decoded *d, struct ia32_authorization *out,
                                    struct ia32_interruption *record, unsigned *debug)
{
    struct ia32_access a[10] = {{.kind = IA32_ACCESS_FETCH,
                                 .linear = {(uint32_t)n->cs.base + n->eip.value},
                                 .linear_valid = true,
                                 .width = d->length,
                                 .width_valid = true}};
    unsigned count = 1, op = d->opcode, width = d->operand_bits / 8;
    struct ia64_result r;
#define ADD(seg, off, bytes, access)                                                               \
    do {                                                                                           \
        if (count == IA64VMM_ARRAY_SIZE(a))                                                        \
            return unsupported("IA-32 operand count");                                             \
        r = range(v, n, &a[count++], seg, off, bytes, access, record);                             \
        if (r.kind != IA64_STEP_COMPLETE || record->valid)                                         \
            return r;                                                                              \
    } while (0)
    if (d->flags & IA32_INSN_STRING) {
        uint32_t mask = d->address_bits == 16 ? 0xffff : UINT32_MAX;
        if (!(d->flags & IA32_INSN_REP) || (n->gpr[IA32_ECX] & mask)) {
            if (op == 0xa4 || op == 0xa5 || op == 0xa6 || op == 0xa7 || op == 0xac || op == 0xad)
                ADD(d->segment, n->gpr[IA32_ESI] & mask, d->access_width, IA32_ACCESS_READ);
            if (op != 0xac && op != 0xad)
                ADD(IA32_SEG_ES, n->gpr[IA32_EDI] & mask, d->access_width,
                    op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf ? IA32_ACCESS_READ
                                                                         : IA32_ACCESS_WRITE);
        }
    } else if ((d->flags & IA32_INSN_MEMORY) && d->operand_memory) {
        uint32_t off = d->offset;
        ADD(d->segment, off, d->access_width, d->access_kind);
    } else if ((d->flags & IA32_INSN_MEMORY) && !(d->flags & IA32_INSN_STACK))
        return unsupported("IA-32 implicit memory access classification");
    if (d->flags & IA32_INSN_STACK) {
        uint32_t sp = n->gpr[IA32_ESP];
        bool pop = (op >= 0x58 && op <= 0x5f) || op == 0x61 || op == 0x8f || op == 0xc2 ||
                   op == 0xc3 || op == 0xc9;
        unsigned bytes = op == 0x60 || op == 0x61 ? 8 * width : width;
        if (op == 0xc9)
            sp = n->gpr[IA32_EBP];
        if (!pop)
            sp -= bytes;
        for (unsigned i = 0; i < bytes; i += width) {
            /* POPA skips the saved SP slot without reading it. */
            if (op == 0x61 && i == 3 * width)
                continue;
            uint32_t offset = sp + i;
            if (!(n->ss.attrib & 0x400))
                offset = (uint16_t)offset;
            ADD(IA32_SEG_SS, offset, width, pop ? IA32_ACCESS_READ : IA32_ACCESS_WRITE);
        }
    }
#undef ADD
    r = ia32_memory_authorize_ranges(v, a, count, out);
    if (r.kind != IA64_STEP_COMPLETE)
        return r;
    *debug = 0;
    for (unsigned i = 1; i < count; ++i) {
        bool write = a[i].kind == IA32_ACCESS_WRITE || a[i].kind == IA32_ACCESS_RMW;
        unsigned rights = a[i].kind == IA32_ACCESS_WRITE ? IA64_ACCESS_WRITE
                          : write                        ? IA64_ACCESS_READ | IA64_ACCESS_WRITE
                                                         : IA64_ACCESS_READ;
        *debug |= debug_matches(&v->s, a[i].linear.value, a[i].width, rights);
        unsigned align = a[i].width == 10 ? ((v->s.psr & (UINT64_C(1) << 3)) ? 16 : 8)
                         : a[i].width > 8 ? 1
                                          : a[i].width;
        if ((align & (align - 1)) == 0 && (a[i].linear.value & (align - 1)) &&
            ((v->s.psr & (UINT64_C(1) << 3)) ||
             (n->cpl == 3 && (n->eflag & 0x40000) && (n->cflg & 0x40000)))) {
            struct ia32_record_input in = {.kind = IA32_RECORD_EXCEPTION,
                                           .phase = IA32_PHASE_BEFORE,
                                           .source = {(uint32_t)n->cs.base + n->eip.value},
                                           .vector = 17,
                                           .ifa_valid = true,
                                           .ifa = a[i].linear.value};
            ia32_make_record(&in, record);
            return ia64_complete();
        }
        bool locked =
            (d->flags & IA32_INSN_LOCK) || (d->operand_memory && (op == 0x86 || op == 0x87));
        if (locked && (v->s.cr[CR_DCR] & (UINT64_C(1) << 2))) {
            struct ia64_translation t;
            r = ia64_translate(v, a[i].linear.value, IA64_REF_DATA, rights, n->cpl, false, &t);
            if (r.kind != IA64_STEP_COMPLETE)
                return r;
            if (t.ma != IA64_MA_WB || (a[i].linear.value & (a[i].width - 1))) {
                struct ia32_record_input in = {.kind = IA32_RECORD_LOCK,
                                               .phase = IA32_PHASE_BEFORE,
                                               .source = {(uint32_t)n->cs.base + n->eip.value},
                                               .ifa_valid = true,
                                               .ifa = a[i].linear.value,
                                               .read = true,
                                               .write = true};
                ia32_make_record(&in, record);
                return ia64_complete();
            }
        }
    }
    return ia64_complete();
}
/* Only the decoded instruction can run. A private instruction page keeps
 * separate IA-64 I/D translations independent, even on NPT without execute-only
 * pages. Data leaves are installed only after their complete ranges pass checks. */
static struct ia64_result install(struct ia32_vcpu_private *p, const struct ia32_authorization *a)
{
    struct ia32_setup_error e = {0};
    if (!p->ops->unmap(p->container, (ia32_token_t){0}, UINT64_C(1) << 32, &e))
        return unsupported(e.detail);
    ++p->generations.mapping;
    if (!p->ops->invalidate(p->container, &p->generations, &e))
        return unsupported(e.detail);
    p->code_token = 0xfffff000;
    for (unsigned i = 0; i < a->count; ++i) {
        if (!a->page[i].side)
            continue;
        if (a->page[i].mapping.token.value == p->code_token) {
            p->code_token -= 4096;
            i = (unsigned)-1;
        }
    }
    for (unsigned i = 0; i < a->count; ++i) {
        if (!a->page[i].side)
            continue;
        struct ia32_mapping m = a->page[i].mapping;
        void *backing = a->page[i].indirect ? p->scratch[i] : gm_direct_ptr(a->page[i].gpa, 4096);
        if (!backing || !hal_ia32_backing_hpa(backing, &m.hpa))
            return unsupported("IA-32 backing has no host physical address");
        /* WRITE-only IA-64 operands use a one-instruction lease. The decoder
         * proved there is no read reference; EPT/NPT still require READ. */
        if (m.rights & IA32_MAP_WRITE)
            m.rights |= IA32_MAP_READ;
        if (a->page[i].indirect)
            m.memory_type = IA32_MEMORY_WB;
        m.generations = p->generations;
        if (!p->ops->map(p->container, &m, &e))
            return unsupported(e.detail);
    }
    struct ia32_mapping code = {.token = {p->code_token},
                                .rights = IA32_MAP_READ | IA32_MAP_EXEC,
                                .memory_type = IA32_MEMORY_WB,
                                .generations = p->generations};
    if (!hal_ia32_backing_hpa(p->code, &code.hpa) || !p->ops->map(p->container, &code, &e) ||
        !p->ops->invalidate(p->container, &p->generations, &e))
        return unsupported(e.detail ? e.detail : "IA-32 instruction buffer mapping");
    return ia64_complete();
}
static int data_page(const struct ia32_authorization *a, uint32_t address)
{
    for (unsigned i = 0; i < a->count; ++i)
        if (a->page[i].side && a->page[i].mapping.token.value == (address & ~4095u))
            return i;
    return -1;
}
/* Transfer only architecturally named bytes, never an entire device page.
 * Read results remain cached across a pre-instruction host interrupt. */
static struct ia64_result scratch_transfer(struct vcpu *v, struct ia32_vcpu_private *p, bool write)
{
    const struct ia32_authorization *a = &p->authorization;
    for (unsigned r = 1; r < a->ranges; ++r) {
        const struct ia32_access *access = &a->access[r];
        if (write ? access->kind == IA32_ACCESS_READ : access->kind == IA32_ACCESS_WRITE)
            continue;
        for (unsigned byte = 0; byte < access->width;) {
            uint32_t address = access->linear.value + byte;
            int page = data_page(a, address);
            if (page < 0)
                return unsupported("IA-32 scratch translation missing");
            unsigned left = 4096 - (address & 4095);
            if (left > access->width - byte)
                left = access->width - byte;
            if (!a->page[page].indirect) {
                byte += left;
                continue;
            }
            gpa_t pa = a->page[page].gpa + (address & 4095);
            unsigned width = 1;
            while (width < 8 && width * 2 <= left && !(pa & (width * 2 - 1)) &&
                   ia64_physical_region(v, pa, width * 2))
                width *= 2;
            uint8_t *buffer = p->scratch[page] + (address & 4095);
            uint64_t value = 0;
            if (write)
                for (unsigned i = 0; i < width; ++i)
                    value |= (uint64_t)buffer[i] << (8 * i);
            struct ia64_result result = write ? ia64_physical_write(v, pa, width, value)
                                              : ia64_physical_read(v, pa, width, &value);
            if (result.kind != IA64_STEP_COMPLETE)
                return result;
            if (!write) {
                for (unsigned i = 0; i < width; ++i)
                    buffer[i] = value >> (8 * i);
                p->pending = true;
            }
            byte += width;
        }
    }
    return ia64_complete();
}
static void fp_position(struct ia32_native_state *after, const struct ia32_native_state *before,
                        const struct ia32_decoded *d)
{
    /* SDM Vol.1 8.1.8: x87 pointers name original instruction prefixes and
     * operand offsets. Private code/segment relocation must not escape. */
    if (d->opcode < 0xd8 || d->opcode > 0xdf ||
        (d->opcode == 0xdb && d->modrm >= 0xe0 && d->modrm <= 0xe4))
        return;
    unsigned opcode = ((d->opcode & 7) << 8) | d->modrm;
    after->fp.bytes[6] = opcode;
    after->fp.bytes[7] = opcode >> 8;
    for (unsigned i = 0; i < 4; ++i)
        after->fp.bytes[8 + i] = before->eip.value >> (i * 8);
    after->fp.bytes[12] = before->cs.selector;
    after->fp.bytes[13] = before->cs.selector >> 8;
    if (d->operand_memory) {
        for (unsigned i = 0; i < 4; ++i)
            after->fp.bytes[16 + i] = d->offset >> (i * 8);
        const struct ia32_segment *seg = ia32_segment(before, d->segment);
        after->fp.bytes[20] = seg->selector;
        after->fp.bytes[21] = seg->selector >> 8;
    } else
        memcpy(after->fp.bytes + 16, before->fp.bytes + 16, 6);
}
static void publish(struct vcpu *v, const uint8_t *bytes, const struct ia32_decoded *d,
                    const struct ia32_native_exit *exit)
{
    struct ia32_vcpu_private *p = v->native;
    while (__atomic_exchange_n(&p->snapshot_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    p->snapshot.sequence++;
    p->snapshot.position = position(v);
    p->snapshot.ia64_retired = v->retired;
    p->snapshot.native_state = p->state;
    p->snapshot.native_state_valid = true;
    p->snapshot.native_instructions = p->instructions_valid ? p->instructions : 0;
    p->snapshot.native_instructions_valid = p->instructions_valid;
    p->snapshot.native_residence_ticks = p->ticks;
    p->snapshot.native_exit_valid = exit != NULL;
    if (exit)
        p->snapshot.native_exit = *exit;
    p->snapshot.bytes_valid = d ? d->length : 0;
    p->snapshot.prefix_summary = d ? d->intercept_code : 0;
    if (d && bytes)
        memcpy(p->snapshot.instruction, bytes, d->length);
    __atomic_store_n(&p->snapshot_lock, 0, __ATOMIC_RELEASE);
}
static bool branch_taken(const struct ia32_decoded *d, uint32_t flags, uint32_t ecx)
{
    unsigned op = d->opcode;
    if (!(d->flags & IA32_INSN_BRANCH))
        return false;
    if ((op >= 0x70 && op <= 0x7f) || (op >= 0x0f80 && op <= 0x0f8f)) {
        bool cf = flags & 1, pf = flags & 4, zf = flags & 0x40;
        bool sf = flags & 0x80, of = flags & 0x800;
        bool condition;
        switch ((op & 15) >> 1) {
        case 0:
            condition = of;
            break;
        case 1:
            condition = cf;
            break;
        case 2:
            condition = zf;
            break;
        case 3:
            condition = cf || zf;
            break;
        case 4:
            condition = sf;
            break;
        case 5:
            condition = pf;
            break;
        case 6:
            condition = sf != of;
            break;
        default:
            condition = zf || sf != of;
            break;
        }
        return condition != (bool)(op & 1);
    }
    if (op >= 0xe0 && op <= 0xe3) {
        uint32_t mask = d->address_bits == 16 ? 0xffff : UINT32_MAX;
        if (op == 0xe3)
            return !(ecx & mask);
        return ((ecx - 1) & mask) && (op == 0xe2 || ((flags & 0x40) != 0) == (op == 0xe1));
    }
    return true;
}
bool vcpu_snapshot_isa(const struct vcpu *v, struct vcpu_isa_snapshot *out)
{
    struct ia32_vcpu_private *p = v->native;
    if (!p)
        return false;
    while (__atomic_exchange_n(&p->snapshot_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    bool valid = p->snapshot.sequence != 0;
    if (valid)
        *out = p->snapshot;
    __atomic_store_n(&p->snapshot_lock, 0, __ATOMIC_RELEASE);
    return valid;
}
void vcpu_dump_isa(const struct vcpu *v)
{
    if (!(v->s.psr & IA64_PSR_IS)) {
        vcpu_dump(v);
        return;
    }
    struct ia32_native_state n;
    struct vcpu_position at = position(v);
    LOGE("IA32 IP=%08llx EIP=%08x PSR=%016llx", (unsigned long long)at.ip, at.eip.value,
         (unsigned long long)v->s.psr);
    if (ia32_state_export(&v->s, &n) != IA32_STATE_OK) {
        LOGE("IA32 architectural state cannot be exported");
        return;
    }
    LOGE("EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x EFLAG=%08x",
         n.gpr[0], n.gpr[1], n.gpr[2], n.gpr[3], n.gpr[4], n.gpr[5], n.gpr[6], n.gpr[7], n.eflag);
    struct vcpu_isa_snapshot snap;
    if (vcpu_snapshot_isa(v, &snap) && snap.native_exit_valid)
        LOGE("IA32 backend=%u exit=%u raw=%llx phase=%u: %s", snap.native_exit.backend,
             snap.native_exit.reason, (unsigned long long)snap.native_exit.raw_reason,
             snap.native_exit.phase, snap.native_exit.detail ? snap.native_exit.detail : "");
}
static struct vcpu_run_exit run(struct vcpu *v, const struct vcpu_run_budget *budget)
{
    struct vcpu_run_exit out = {.stop = VCPU_RUN_YIELD,
                                .yield = VCPU_YIELD_NATIVE_BUDGET,
                                .native_usage.instructions_valid = true};
    struct ia32_setup_error e = {0};
    struct ia64_result result = ia64_complete();
    if (!budget || budget->native.deadline == UINT64_MAX) {
        result = unsupported("IA-32 execution requires a finite deadline");
        goto finished;
    }
    if (!attach(v, hal_cpu_index(), &e)) {
        result = unsupported(e.detail);
        goto finished;
    }
    struct ia32_vcpu_private *p = v->native;
    struct ia32_native_state *n = &p->state;
    if (p->unresolved) {
        result = unsupported("IA-32 device operand has unresolved native progress");
        goto finished;
    }
    /* Vol.2 5.5.4: IA/DA/DD are cleared before the first IA-32 instruction. */
    v->s.psr &= ~(IA64_PSR_IA | IA64_PSR_DA | IA64_PSR_DD);
    result = state_error(ia32_state_export(&v->s, n));
    if (result.kind != IA64_STEP_COMPLETE)
        goto finished;
    while (v->s.psr & IA64_PSR_IS) {
        unsigned request = __atomic_exchange_n(&v->requested_exit, 0, __ATOMIC_ACQ_REL);
        if (request) {
            if (request != VCPU_EXIT_BUDGET + 1)
                result = ia64_exit(request - 1, 0, "requested exit");
            out.yield = VCPU_YIELD_KICK;
            break;
        }
        if (hal_counter() >= budget->native.deadline) {
            out.yield = VCPU_YIELD_NATIVE_DEADLINE;
            break;
        }
        if (budget->native.instructions &&
            out.native_usage.instructions >= budget->native.instructions)
            break;
        if (__atomic_exchange_n(&v->irq_kick, 0, __ATOMIC_ACQ_REL)) {
            out.yield = VCPU_YIELD_KICK;
            break;
        }
        if (!p->pending)
            code_quiescent(v);
        ia64_timer_poll(v);
        bool enabled = !(n->cflg & 0x80) || (n->eflag & 0x200);
        if (!p->pending && (enabled || vcpu_pal_interrupt_pending(v)) && ia64_external_pending(v)) {
            result = state_error(ia32_state_leave(&v->s, n, 0, false));
            if (result.kind == IA64_STEP_COMPLETE)
                ia64_external_deliver(v);
            break;
        }
        uint8_t bytes[15] = {0};
        struct ia32_decoded d = {0};
        struct ia32_interruption record = {0};
        unsigned loaded = 0, data_debug = 0;
        p->operand_debug = 0;
        bool handled = false, completed = false, stepped = false;
        uint32_t source = (uint32_t)n->cs.base + n->eip.value, old_eip = n->eip.value;
        uint32_t old_eflag = n->eflag;
        uint32_t old_ecx = n->gpr[IA32_ECX];
        if (p->pending) {
            d = p->decoded;
            memcpy(bytes, p->instruction, sizeof(bytes));
            data_debug = p->pending_debug;
            goto native_resume;
        }
        /* Vol.2 Table 5-6: instruction breakpoints precede code-fetch faults.
         * Keep this order at page boundaries as our Class A model choice. */
        if (debug_matches(&v->s, source, 1, IA64_ACCESS_EXECUTE) && !(n->eflag & 0x10000)) {
            struct ia32_record_input in = {.kind = IA32_RECORD_EXCEPTION,
                                           .phase = IA32_PHASE_BEFORE,
                                           .source = {source},
                                           .vector = 1,
                                           .execute = true};
            ia32_make_record(&in, &record);
            goto instruction_end;
        }
        result = fetch(v, n, bytes, &d, &record);
        if (result.kind != IA64_STEP_COMPLETE || record.valid)
            goto instruction_end;
        if ((v->s.psr & (UINT64_C(1) << 19)) ||
            ((v->s.psr & (UINT64_C(1) << 18)) &&
             (d.flags & (IA32_INSN_FP | IA32_INSN_MMX | IA32_INSN_SSE)))) {
            result = ia64_fault(IA64_FAULT_FP_DISABLED,
                                (v->s.psr & (UINT64_C(1) << 19)) ? 2 : 1);
            goto instruction_end;
        }
        bool fxstate = d.opcode == 0x0fae && d.operand_memory && ((d.modrm >> 3) & 7) < 2;
        if (!fxstate && (((d.flags & (IA32_INSN_MMX | IA32_INSN_SSE)) && (n->cflg & 4)) ||
                         ((d.flags & IA32_INSN_SSE) && !(n->cflg & (UINT64_C(1) << 41)))))
            d.kind = IA32_INSN_INTERCEPT;
        if (d.kind != IA32_INSN_INTERCEPT && d.native_features) {
            if (!backend(p, &e)) {
                result = unsupported(e.detail);
                goto instruction_end;
            }
            if (d.native_features & ~p->caps.native_features)
                d.kind = IA32_INSN_INTERCEPT;
        }
        if (d.kind != IA32_INSN_INTERCEPT) {
            result = ia32_extended_check(v, n, &d, &record);
            if (result.kind != IA64_STEP_COMPLETE || record.valid)
                goto instruction_end;
        }
        result = ia32_assist(v, n, &d, bytes, &handled, &loaded, &record);
        if (result.kind != IA64_STEP_COMPLETE || record.valid || !(v->s.psr & IA64_PSR_IS)) {
            completed = handled && result.kind == IA64_STEP_COMPLETE &&
                        (!record.valid || record.phase == IA32_PHASE_COMPLETED) &&
                        !((d.flags & (IA32_INSN_REP | IA32_INSN_STRING)) ==
                              (IA32_INSN_REP | IA32_INSN_STRING) &&
                          n->eip.value == old_eip);
            goto instruction_end;
        }
        if (handled) {
            completed = !((d.flags & (IA32_INSN_REP | IA32_INSN_STRING)) ==
                              (IA32_INSN_REP | IA32_INSN_STRING) &&
                          n->eip.value == old_eip);
            goto instruction_end;
        }
        result = authorize(v, n, &d, &p->authorization, &record, &data_debug);
        if (result.kind != IA64_STEP_COMPLETE || record.valid)
            goto instruction_end;
        bool indirect = false;
        for (unsigned i = 0; i < p->authorization.count; ++i)
            indirect |= p->authorization.page[i].side && p->authorization.page[i].indirect;
        if (indirect && ia32_scalar_supported(&d)) {
            data_debug = 0;
            result = ia32_scalar(v, n, &d, bytes, &handled, &record);
            if (handled) {
                completed = result.kind == IA64_STEP_COMPLETE && !record.valid;
                if (completed) {
                    n->eip.value += d.length;
                    n->eflag &= ~0x10000u;
                }
                goto instruction_end;
            }
        }
        if (indirect && (d.flags & IA32_INSN_STRING)) {
            result = ia32_string(v, n, &d, &record);
            completed = result.kind == IA64_STEP_COMPLETE &&
                        (!record.valid || record.phase == IA32_PHASE_COMPLETED) &&
                        (!(d.flags & IA32_INSN_REP) || n->eip.value != old_eip);
            goto instruction_end;
        }
        if (!backend(p, &e)) {
            result = unsupported(e.detail);
            goto instruction_end;
        }
        if (!code_validity_active()) {
            result = unsupported("native IA-32 execution requires code validation");
            goto instruction_end;
        }
        p->decoded = d;
        memcpy(p->instruction, bytes, sizeof(bytes));
        p->pending_debug = data_debug;
        memset(p->scratch, 0, sizeof(p->scratch));
    native_resume:
        result = install(p, &p->authorization);
        if (result.kind != IA64_STEP_COMPLETE)
            goto instruction_end;
        /* CFLG.MMXEX=0 masks SSE exceptions in the Itanium environment.
         * It does not turn an unmasked numeric error into host #UD. */
        bool mask_sse =
            !(n->cflg & (UINT64_C(1) << 42)) && (d.flags & IA32_INSN_SSE) && d.opcode != 0x0fae;
        uint8_t mxcsr_low = n->fp.bytes[24] & 0x80, mxcsr_high = n->fp.bytes[25] & 0x1f;
        if (mask_sse) {
            n->fp.bytes[24] |= 0x80;
            n->fp.bytes[25] |= 0x1f;
        }
        struct ia32_native_state execution = *n;
        execution.cs.base = p->code_token - n->eip.value;
        memcpy(p->code, bytes, d.length);
        bool cs_operand = (d.flags & IA32_INSN_MEMORY) && d.segment == IA32_SEG_CS;
        if (cs_operand) {
            execution.ds = n->cs;
            execution.ds.attrib = (n->cs.attrib & 0xc00) | 0x93 | n->cpl << 5;
            for (unsigned i = 0; i < d.prefix_length; ++i)
                if (p->code[i] == 0x2e)
                    p->code[i] = 0x3e;
        }
        if (!p->ops->import(p->container, &execution, IA32_DIRTY_ALL, &e)) {
            if (mask_sse) {
                n->fp.bytes[24] = (n->fp.bytes[24] & ~0x80) | mxcsr_low;
                n->fp.bytes[25] = (n->fp.bytes[25] & ~0x1f) | mxcsr_high;
            }
            result = unsupported(e.detail);
            goto instruction_end;
        }
        struct ia32_run_limit limit = {.deadline = budget->native.deadline,
                                       .step = true,
                                       .svm_step_safe = !(d.flags & IA32_INSN_WRITE_SEGMENT) &&
                                                        d.opcode != 0x9c && d.opcode != 0x9d};
        struct ia32_run_usage usage = {0};
        /* Bracket native writes against decoded-source preparation just as
         * IA-64 RAM stores do. Release on every exit before any retry. */
        code_lock();
        uint8_t checked[15];
        if (!p->pending)
            result = ia32_memory_fetch(v, (ia32_linear_t){source}, checked, d.length);
        if (!p->pending &&
            (result.kind != IA64_STEP_COMPLETE || !ia32_memory_current(v, &p->authorization) ||
             memcmp(bytes, checked, d.length))) {
            code_unlock();
            if (mask_sse) {
                n->fp.bytes[24] = (n->fp.bytes[24] & ~0x80) | mxcsr_low;
                n->fp.bytes[25] = (n->fp.bytes[25] & ~0x1f) | mxcsr_high;
            }
            if (result.kind != IA64_STEP_COMPLETE)
                goto instruction_end;
            continue;
        }
        if (!p->pending)
            result = scratch_transfer(v, p, false);
        if (result.kind != IA64_STEP_COMPLETE) {
            code_unlock();
            if (mask_sse) {
                n->fp.bytes[24] = (n->fp.bytes[24] & ~0x80) | mxcsr_low;
                n->fp.bytes[25] = (n->fp.bytes[25] & ~0x1f) | mxcsr_high;
            }
            goto instruction_end;
        }
        for (unsigned i = 0; i < p->authorization.count; ++i)
            if (!p->authorization.page[i].indirect &&
                (p->authorization.page[i].mapping.rights & IA32_MAP_WRITE))
                code_notify_cpu_write(p->authorization.page[i].gpa, 4096);
        struct ia32_native_exit exit = p->ops->enter(p->container, &execution, &limit, &usage);
        /* An outer AMD XSAVE may zero nonpending x87 error pointers
         * (APM Vol.2 11.5.10). Preserve the semantic environment, then
         * reconstruct it only when the decoded x87 instruction executed. */
        memcpy(execution.fp.bytes + 6, n->fp.bytes + 6, 18);
        /* Raw #NP/#SS/#GP/#PF can follow partial REP or POPA execution.
         * A single prevalidated reference with unchanged registers has no
         * such partial progress; preserve the consumed device operand. */
        if (exit.reason == IA32_EXIT_EXCEPTION && exit.exception.valid &&
            !exit.vectoring.valid && exit.phase == IA32_PHASE_UNKNOWN &&
            exit.exception.vector >= 11 && exit.exception.vector <= 14 &&
            p->authorization.ranges <= 2 && !(d.flags & IA32_INSN_STRING) &&
            exit.position_valid && exit.eip.value == old_eip && execution.eip.value == old_eip &&
            execution.cpl == n->cpl && !memcmp(execution.gpr, n->gpr, sizeof(n->gpr)))
            exit.phase = IA32_PHASE_BEFORE;
        __asm__ volatile("mfence" ::: "memory");
        bool step_effect = exit.reason == IA32_EXIT_STEP && exit.position_valid &&
                           !exit.vectoring.valid && exit.phase != IA32_PHASE_BEFORE &&
                           (exit.phase == IA32_PHASE_COMPLETED
                                ? exit.next_eip_valid && exit.next_eip.value == execution.eip.value
                                : exit.eip.value == execution.eip.value);
        bool debug_effect = exit.reason == IA32_EXIT_EXCEPTION && exit.exception.valid &&
                            exit.exception.vector == 1 && (old_eflag & 0x100) &&
                            exit.phase != IA32_PHASE_BEFORE && !exit.vectoring.valid;
        /* Unmasked IE/OE/UE suppress numeric stores. Unmasked precision
         * still stores the rounded result (Intel SDM Vol.1 8.5.4–8.5.6). */
        bool suppressed = d.opcode >= 0xd8 && d.opcode <= 0xdf && d.operand_memory &&
                          d.access_kind == IA32_ACCESS_WRITE &&
                          (execution.fp.bytes[2] & ~n->fp.bytes[0] & 0x19);
        bool host_exit = exit.reason == IA32_EXIT_HOST_IRQ || exit.reason == IA32_EXIT_HOST_NMI ||
                         exit.reason == IA32_EXIT_DEADLINE || exit.reason == IA32_EXIT_KICK;
        /* Non-control x87 instructions cannot branch. A host exit at the
         * exact next EIP proves their execution even without a step exit. */
        bool fp_advanced = host_exit && exit.position_valid && !exit.vectoring.valid &&
                           exit.phase != IA32_PHASE_BEFORE && d.opcode >= 0xd8 && d.opcode <= 0xdf &&
                           execution.eip.value == old_eip + d.length &&
                           exit.eip.value == execution.eip.value;
        if (step_effect || debug_effect || fp_advanced) {
            fp_position(&execution, n, &d);
            if (suppressed)
                data_debug = 0;
        }
        execution.cs = n->cs;
        if (cs_operand)
            execution.ds = n->ds;
        *n = execution;
        if (exit.position_valid)
            exit.linear_ip.value = (uint32_t)n->cs.base + exit.eip.value;
        if ((step_effect || debug_effect) && !suppressed)
            result = scratch_transfer(v, p, true);
        code_unlock();
        if (p->pending) {
            bool fault = exit.reason == IA32_EXIT_EXCEPTION && exit.exception.valid &&
                         !exit.vectoring.valid && exit.phase == IA32_PHASE_BEFORE;
            if (step_effect || debug_effect || fault)
                p->pending = false;
            else if (exit.phase != IA32_PHASE_BEFORE) {
                /* Replaying a device read after unproven progress is unsafe. */
                result = unsupported("IA-32 device operand has unresolved native progress");
                p->unresolved = true;
                p->instructions_valid = out.native_usage.instructions_valid = false;
            }
        }
        if (mask_sse) {
            n->fp.bytes[24] = (n->fp.bytes[24] & ~0x80) | mxcsr_low;
            n->fp.bytes[25] = (n->fp.bytes[25] & ~0x1f) | mxcsr_high;
        }
        p->ticks += usage.residence_ticks;
        out.native_usage.residence_ticks += usage.residence_ticks;
        out.native_exit = exit;
        out.native_exit_valid = true;
        if (result.kind != IA64_STEP_COMPLETE)
            goto instruction_end;
        if (exit.reason == IA32_EXIT_STEP) {
            if (!exit.position_valid || exit.vectoring.valid || exit.phase == IA32_PHASE_BEFORE ||
                (exit.phase == IA32_PHASE_COMPLETED &&
                 (!exit.next_eip_valid || exit.next_eip.value != n->eip.value)) ||
                (exit.phase == IA32_PHASE_UNKNOWN && exit.eip.value != n->eip.value)) {
                result = unsupported("IA-32 step exit position is inconsistent");
                goto instruction_end;
            }
            stepped = true;
            bool rep = (d.flags & (IA32_INSN_STRING | IA32_INSN_REP)) ==
                       (IA32_INSN_STRING | IA32_INSN_REP);
            completed = !rep || n->eip.value != old_eip;
        } else if (exit.reason == IA32_EXIT_EXCEPTION && exit.exception.valid) {
            if (exit.exception.vector == 6) {
                struct ia32_record_input in = {.kind = IA32_RECORD_INSTRUCTION,
                                               .phase = IA32_PHASE_BEFORE,
                                               .source = {source},
                                               .code = d.intercept_code,
                                               .iim = d.iim};
                ia32_make_record(&in, &record);
            } else if (exit.exception.vector == 1) {
                if (exit.phase == IA32_PHASE_BEFORE) {
                    struct ia32_record_input in = {.kind = IA32_RECORD_EXCEPTION,
                                                   .phase = IA32_PHASE_BEFORE,
                                                   .source = {source},
                                                   .vector = 1,
                                                   .execute = true};
                    ia32_make_record(&in, &record);
                    goto instruction_end;
                }
                if (!(old_eflag & 0x100) || exit.vectoring.valid) {
                    result = unsupported("unclassified IA-32 debug exit");
                    goto instruction_end;
                }
                struct ia32_record_input in = {.kind = IA32_RECORD_EXCEPTION,
                                               .phase = IA32_PHASE_COMPLETED,
                                               .source = {source},
                                               .next = {(uint32_t)n->cs.base + n->eip.value},
                                               .next_valid = true,
                                               .vector = 1,
                                               .code = 8 | (data_debug << 4)};
                ia32_make_record(&in, &record);
                completed = !((d.flags & (IA32_INSN_REP | IA32_INSN_STRING)) ==
                                  (IA32_INSN_REP | IA32_INSN_STRING) &&
                              n->eip.value == old_eip);
            } else
                result =
                    ia32_exception(n, exit.exception.vector,
                                   exit.exception.error_valid ? exit.exception.error : 0, &record);
        } else if (exit.reason == IA32_EXIT_HOST_IRQ || exit.reason == IA32_EXIT_HOST_NMI ||
                   exit.reason == IA32_EXIT_DEADLINE || exit.reason == IA32_EXIT_KICK) {
            /* Unknown completion is materialized, never replayed from an old
             * register snapshot. A host exit is not an ISA transition. */
            result = state_error(ia32_state_import(&v->s, n, IA32_DIRTY_ALL, 0, false));
            if (exit.phase != IA32_PHASE_BEFORE) {
                out.native_usage.instructions_valid = false;
                p->instructions_valid = false;
            }
            out.yield =
                exit.reason == IA32_EXIT_DEADLINE ? VCPU_YIELD_NATIVE_DEADLINE : VCPU_YIELD_KICK;
            publish(v, bytes, &d, &exit);
            break;
        } else
            result = unsupported(
                exit.detail ? exit.detail
                            : "IA-32 native exit requires unsupported access classification");
    instruction_end:
        data_debug |= p->operand_debug;
        if (record.valid && record.phase == IA32_PHASE_COMPLETED)
            record.isr |= data_debug << 4;
        if (completed) {
            ++p->instructions;
            ++out.native_usage.instructions;
            v->last_successful_bundle = source;
        }
        if (v->s.psr & IA64_PSR_IS) {
            enum ia32_state_status status =
                ia32_state_import(&v->s, n, IA32_DIRTY_ALL, loaded, completed);
            if (status != IA32_STATE_OK)
                result = state_error(status);
        }
        if (result.kind == IA64_STEP_COMPLETE && (completed || stepped) && !record.valid &&
            (v->s.psr & IA64_PSR_IS)) {
            unsigned traps = data_debug << 4;
            if ((v->s.psr & IA64_PSR_SS) || (old_eflag & 0x100))
                traps |= 8;
            if ((v->s.psr & IA64_PSR_TB) && branch_taken(&d, old_eflag, old_ecx))
                traps |= 4;
            if (traps) {
                struct ia32_record_input in = {.kind = IA32_RECORD_EXCEPTION,
                                               .phase = IA32_PHASE_COMPLETED,
                                               .source = {source},
                                               .next = {(uint32_t)n->cs.base + n->eip.value},
                                               .next_valid = true,
                                               .vector = 1,
                                               .code = traps};
                ia32_make_record(&in, &record);
            }
        }
        if (record.valid || result.kind == IA64_STEP_FAULT) {
            if (v->s.psr & IA64_PSR_IS) {
                enum ia32_state_status status = ia32_state_leave(&v->s, n, loaded, completed);
                if (status != IA32_STATE_OK)
                    result = state_error(status);
            }
            if (result.kind == IA64_STEP_FAULT) {
                Ia64Instruction insn = {.opcode = IA64_OP_NOP, .address = source};
                v->last_successful_bundle = source;
                ia64_deliver_fault(v, &insn, result);
                result = ia64_complete();
            } else if (result.kind == IA64_STEP_COMPLETE)
                ia32_deliver(v, &record);
        }
        publish(v, bytes, &d, out.native_exit_valid ? &out.native_exit : NULL);
        if (result.kind != IA64_STEP_COMPLETE || !(v->s.psr & IA64_PSR_IS))
            break;
        if (budget->native.step)
            break;
    }
finished:
    if (!out.native_usage.instructions_valid)
        out.native_usage.instructions = 0;
    out.position = position(v);
    if (result.kind != IA64_STEP_COMPLETE) {
        out.stop = VCPU_RUN_EXIT;
        out.reason = result.kind == IA64_STEP_EXIT ? result.exit : VCPU_EXIT_UNIMPLEMENTED;
        out.info = result.info;
        out.detail = result.detail;
    }
    return out;
}
static const struct engine_ia32_ops ops = {
    .attach = attach, .reset = reset, .detach = detach, .run = run};
const struct engine_ia32_ops *engine_ia32_glue(void)
{
    return &ops;
}

struct vcpu_run_exit engine_run_bounded(struct vcpu *v, const struct vcpu_run_budget *budget)
{
    struct vcpu_run_exit out = {.stop = VCPU_RUN_YIELD, .yield = VCPU_YIELD_IA64_BUDGET};
    uint64_t retired = v->retired;
    uint64_t native_instructions = 0, native_ticks = 0;
    bool instructions_valid = true;
    unsigned request = __atomic_exchange_n(&v->requested_exit, 0, __ATOMIC_ACQ_REL);
    if (request) {
        out.yield = VCPU_YIELD_KICK;
        if (request != VCPU_EXIT_BUDGET + 1) {
            out.stop = VCPU_RUN_EXIT;
            out.reason = request - 1;
            out.detail = "requested exit";
        }
        out.position = position(v);
        return out;
    }
    if (!budget || budget->native.deadline == UINT64_MAX) {
        out.stop = VCPU_RUN_EXIT;
        out.reason = VCPU_EXIT_UNIMPLEMENTED;
        out.detail = "bounded execution requires a finite deadline";
        out.position = position(v);
        return out;
    }
    while (budget->ia64_instructions > v->retired - retired) {
        if (hal_counter() >= budget->native.deadline) {
            out.yield = VCPU_YIELD_NATIVE_DEADLINE;
            break;
        }
        if (v->s.psr & IA64_PSR_IS) {
            struct vcpu_run_budget remaining = *budget;
            if (budget->native.instructions)
                remaining.native.instructions -= native_instructions;
            out = run(v, &remaining);
            native_instructions += out.native_usage.instructions;
            instructions_valid &= out.native_usage.instructions_valid;
            native_ticks += out.native_usage.residence_ticks;
            if (out.stop == VCPU_RUN_EXIT || (v->s.psr & IA64_PSR_IS) || budget->native.step ||
                (budget->native.instructions && native_instructions >= budget->native.instructions))
                break;
        } else {
            uint64_t remaining = budget->ia64_instructions - (v->retired - retired);
            /* Bound polling without relying on a guest-enabled timer. */
            uint64_t saved_deadline = v->execution_deadline;
            v->execution_deadline = budget->native.deadline;
            struct vcpu_exit e = engine_run_ia64(v, remaining > 256 ? 256 : remaining);
            v->execution_deadline = saved_deadline;
            if (e.reason != VCPU_EXIT_BUDGET) {
                out.stop = VCPU_RUN_EXIT;
                out.reason = e.reason;
                out.info = e.info;
                out.detail = e.detail;
                break;
            }
            if (__atomic_load_n(&v->irq_kick, __ATOMIC_ACQUIRE)) {
                out.yield = VCPU_YIELD_KICK;
                break;
            }
        }
    }
    out.position = position(v);
    out.ia64_retired = v->retired - retired;
    out.native_usage.instructions = instructions_valid ? native_instructions : 0;
    out.native_usage.instructions_valid = instructions_valid;
    out.native_usage.residence_ticks = native_ticks;
    return out;
}
