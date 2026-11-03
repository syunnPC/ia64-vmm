/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform/vbios_trace.h"
#include "ia64/state.h"
#include "persist/persist.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"
#include "ia64vmm/hal.h"

/* Guest display diagnostics: observe completed accesses, keeping the real card's
 * transaction widths and ordering. No VGA registers are emulated. */
static bool enabled;
static struct {
    uint64_t base, size;
} memory[7];
/* Each executor writes only its own counters. A LOCK after every pixel
 * would drain WC framebuffer stores and change the display's timing. */
struct memory_counts {
    _Alignas(64) uint64_t reads[7];
    uint64_t writes[7];
    uint8_t pad[16];
};
_Static_assert(sizeof(struct memory_counts) == 128, "per-CPU counters occupy whole cache lines");
static struct memory_counts *counts;
static struct {
    uint16_t base;
    uint64_t size;
} io[8];
static uint64_t port_reads, port_writes;
struct packet {
    unsigned count;
    bool output;
    uint32_t sequence, value[14];
};
static struct packet packets[IA64VMM_MAX_CPUS + 1];
struct event {
    unsigned cpu;
    bool output;
    uint32_t sequence, value[14];
};
static struct event events[8], first_events[8];
struct mode_info {
    uint16_t mode;
    uint32_t detail[4];
};
static struct mode_info modes[256];
static unsigned mode_used, mode_replace, selected_cpu;
static uint16_t selected_bx, selected_ax;
static bool selected_seen, selected_done;
static uint64_t event_count, discarded;
static uint32_t issued_sequence;
/* Calls without an output packet: the guest's calls together, since its
 * thread may return on another vCPU, and the container's. */
static unsigned guest_pending, container_pending;

bool vbios_guest_trace_enabled(void)
{
    return enabled;
}
bool vbios_guest_trace_init(const char *cmdline)
{
    enabled = false;
    const char *value;
    size_t n;
    if (platform_cmdline_value(cmdline, "vbios_trace_guest", &value, &n)) {
        if (n != 1 || (*value != '0' && *value != '1')) {
            LOGE("vbios_trace_guest takes 0 or 1");
            return false;
        }
        enabled = *value == '1';
    }
    if (enabled && !counts) {
        counts =
            hal_alloc_pages(IA64VMM_ALIGN_UP(sizeof(*counts) * IA64VMM_MAX_CPUS, 4096) / 4096, 0);
        if (!counts) {
            LOGE("display trace counter allocation failed");
            enabled = false;
            return false;
        }
    }
    if (counts)
        memset(counts, 0, sizeof(*counts) * IA64VMM_MAX_CPUS);
    memset(memory, 0, sizeof(memory));
    memset(io, 0, sizeof(io));
    guest_pending = container_pending = 0;
    memset(packets, 0, sizeof(packets));
    memset(events, 0, sizeof(events));
    memset(first_events, 0, sizeof(first_events));
    memset(modes, 0, sizeof(modes));
    mode_used = mode_replace = 0;
    selected_seen = selected_done = false;
    event_count = discarded = port_reads = port_writes = 0;
    issued_sequence = 0;
    return true;
}
void vbios_guest_trace_card(const struct pci_device *card)
{
    if (!enabled)
        return;
    memory[6].base = 0xa0000;
    memory[6].size = 0x20000;
    io[6].base = 0x3b0;
    io[6].size = 0x30;
    if (card->vendor == 0x1234 && card->device == 0x1111) {
        io[7].base = 0x1ce;
        io[7].size = 3;
    }
    for (unsigned i = 0; i < 6; ++i) {
        const struct pci_bar *b = &card->bars[i];
        if (b->io) {
            io[i].base = (uint16_t)b->base;
            io[i].size = b->size;
        } else {
            memory[i].base = b->base;
            memory[i].size = b->size;
        }
    }
}
void platform_display_memory_access(struct vcpu *v, uint64_t address, unsigned size, bool write,
                                    uint64_t value)
{
    if (!enabled || !v || v->cfg.index >= IA64VMM_MAX_CPUS)
        return;
    unsigned cpu = v->cfg.index;
    unsigned slot;
    uint64_t ip = vcpu_instruction_pointer(v, &slot);
    if (!(ip >> 61))
        return; /* same OS-phase filter as port_trace */
    for (unsigned i = 0; i < 7; ++i) {
        if (address < memory[i].base || address - memory[i].base >= memory[i].size ||
            size > memory[i].size - (address - memory[i].base))
            continue;
        uint64_t *count = write ? &counts[cpu].writes[i] : &counts[cpu].reads[i];
        if (!(*count)++)
            LOGI("display memory %s%u cpu%u first %s pa=%llx size=%u value=%llx ip=%llx:%u",
                 i == 6 ? "VGA" : "BAR", i == 6 ? 0 : i, cpu, write ? "write" : "read",
                 (unsigned long long)address, size, (unsigned long long)value,
                 (unsigned long long)ip, slot);
        return;
    }
}
static const struct mode_info *find_mode(uint16_t mode)
{
    for (unsigned i = 0; i < mode_used; ++i)
        if (modes[i].mode == mode)
            return &modes[i];
    return NULL;
}
static void remember_event(const struct event *e)
{
    const uint32_t *r = e->value;
    if (!e->output && (uint16_t)r[0] == 0x4f02) {
        selected_seen = true;
        selected_done = false;
        selected_cpu = e->cpu;
        selected_bx = (uint16_t)r[1];
    }
    if (!e->output)
        return;
    unsigned function = r[10] >> 16;
    /* The guest's call may return on another vCPU; the container's cannot
     * complete a guest call or the reverse. */
    if (function == 0x4f02 && selected_seen &&
        (e->cpu == IA64VMM_MAX_CPUS) == (selected_cpu == IA64VMM_MAX_CPUS) &&
        (r[1] & 0x37ff) == (selected_bx & 0x37ff)) {
        selected_ax = (uint16_t)r[0];
        selected_done = true;
    }
    if (function != 0x4f01 || (uint16_t)r[0] != 0x004f)
        return;
    uint16_t mode = (uint16_t)(r[2] & 0x37ff);
    unsigned i;
    for (i = 0; i < mode_used; ++i)
        if (modes[i].mode == mode)
            break;
    if (i == mode_used) {
        if (mode_used < IA64VMM_ARRAY_SIZE(modes))
            ++mode_used;
        else
            i = mode_replace++ % IA64VMM_ARRAY_SIZE(modes);
    }
    modes[i].mode = mode;
    memcpy(modes[i].detail, r + 10, sizeof(modes[i].detail));
}
static void print_mode(uint16_t mode, const uint32_t *d)
{
    LOGI("VBE %04x %ux%ux%u pitch=%u attr=%04x fb=%08x", mode, d[1] >> 16, d[2] & 0xffff,
         (d[2] >> 16) & 0xff, d[1] & 0xffff, d[0] & 0xffff, d[3]);
}
static void print_event(const struct event *e)
{
    const uint32_t *r = e->value;
    LOGI("INT10 %s%u %s AX=%04x BX=%04x CX=%04x DX=%04x F=%04x seq=%u",
         e->cpu == IA64VMM_MAX_CPUS ? "container" : "cpu", e->cpu == IA64VMM_MAX_CPUS ? 0 : e->cpu,
         e->output ? "out" : "in ", r[0] & 0xffff, r[1] & 0xffff, r[2] & 0xffff, r[3] & 0xffff,
         r[9] & 0xffff, e->sequence);
    if (e->output && r[10] >> 16 == 0x4f01 && (uint16_t)r[0] == 0x004f)
        print_mode((uint16_t)(r[2] & 0x37ff), r + 10);
    else
        LOGI("  ES:DI=%04x:%04x DS=%04x SI=%04x BP=%04x", r[7] & 0xffff, r[5] & 0xffff,
             r[8] & 0xffff, r[4] & 0xffff, r[6] & 0xffff);
}
bool vbios_guest_trace_sequence(struct vcpu *v, uint16_t port, unsigned size, uint32_t *value)
{
    if (!enabled || port != 0x80 || size != 4)
        return false;
    unsigned cpu = v ? v->cfg.index : IA64VMM_MAX_CPUS;
    if ((v && cpu >= IA64VMM_MAX_CPUS) || packets[cpu].count != 1)
        return false;
    *value = __atomic_add_fetch(&issued_sequence, 1, __ATOMIC_RELAXED);
    return true;
}
bool vbios_guest_trace_port(struct vcpu *v, uint16_t port, unsigned size, bool write,
                            uint32_t value)
{
    if (!enabled)
        return false;
    unsigned cpu = v ? v->cfg.index : IA64VMM_MAX_CPUS;
    if (v && cpu >= IA64VMM_MAX_CPUS)
        return false;
    struct packet *p = &packets[cpu];
    if (port == 0x80 && size == 4 && write) {
        /* Header magic is reserved in the diagnostic stream. A new
         * header always resynchronizes after truncation or migration. */
        if (value == 0x56424930 || value == 0x56424931) {
            if (p->count)
                ++discarded;
            p->count = 1;
            p->output = value == 0x56424931;
            memset(p->value, 0, sizeof(p->value));
        } else if (!p->count) {
            return false;
        } else if (p->count == 1) {
            p->sequence = value;
            ++p->count;
        } else {
            p->value[p->count - 2] = value;
            unsigned words = p->output ? 14 : 10;
            if (++p->count == words + 2) {
                struct event e = {.cpu = cpu, .output = p->output, .sequence = p->sequence};
                memcpy(e.value, p->value, sizeof(e.value));
                uint64_t seq = event_count++;
                if (seq < IA64VMM_ARRAY_SIZE(first_events))
                    first_events[seq] = e;
                events[seq % IA64VMM_ARRAY_SIZE(events)] = e;
                unsigned *pending = cpu == IA64VMM_MAX_CPUS ? &container_pending : &guest_pending;
                if (!e.output)
                    ++*pending;
                else if (*pending)
                    --*pending;
                remember_event(&e);
                print_event(&e);
                /* Full registers stay in the RAM/media log. Screen pages
                 * below use the 16-bit VGA/VBE ABI to fit 80 columns. */
                LOGI(
                    "INT10 EAX=%08x EBX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x EBP=%08x ES=%04x DS=%04x",
                    e.value[0], e.value[1], e.value[2], e.value[3], e.value[4], e.value[5],
                    e.value[6], e.value[7] & 0xffff, e.value[8] & 0xffff);
                p->count = 0;
            }
        }
        return true;
    }
    if (v) {
        unsigned slot;
        uint64_t ip = vcpu_instruction_pointer(v, &slot);
        if (ip >> 61)
            for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(io); ++i)
                if (port >= io[i].base && (uint64_t)port - io[i].base < io[i].size &&
                    size <= io[i].size - (port - io[i].base)) {
                    __atomic_fetch_add(write ? &port_writes : &port_reads, 1, __ATOMIC_RELAXED);
                    break;
                }
    }
    return false;
}
void vbios_guest_trace_summary(void)
{
    if (!enabled)
        return;
    LOGI("PLOG media log: %s", plog_status());
    LOGI("VBIOS display summary: OS VGA/BAR ports R=%llu W=%llu", (unsigned long long)port_reads,
         (unsigned long long)port_writes);
    for (unsigned i = 0; i < 7; ++i)
        if (memory[i].size) {
            uint64_t reads = 0, writes = 0;
            for (unsigned cpu = 0; cpu < IA64VMM_MAX_CPUS; ++cpu) {
                reads += counts[cpu].reads[i];
                writes += counts[cpu].writes[i];
            }
            LOGI("%s%u %llx+%llx R=%llu W=%llu", i == 6 ? "VGA" : "BAR", i == 6 ? 0 : i,
                 (unsigned long long)memory[i].base, (unsigned long long)memory[i].size,
                 (unsigned long long)reads, (unsigned long long)writes);
        }
    hal_checkpoint("vbios-trace-access");
    if (event_count > IA64VMM_ARRAY_SIZE(first_events)) {
        LOGI("VBIOS INT10: first eight of %llu packets (in/out; F bit0=CF)",
             (unsigned long long)event_count);
        for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(first_events); ++i)
            print_event(&first_events[i]);
        hal_checkpoint("vbios-trace-int10-first");
    }
    LOGI("VBIOS INT10: %llu packets; last eight (in/out; F bit0=CF)",
         (unsigned long long)event_count);
    uint64_t first =
        event_count > IA64VMM_ARRAY_SIZE(events) ? event_count - IA64VMM_ARRAY_SIZE(events) : 0;
    if (event_count > IA64VMM_ARRAY_SIZE(first_events) && first < IA64VMM_ARRAY_SIZE(first_events))
        first = IA64VMM_ARRAY_SIZE(first_events);
    for (uint64_t i = first; i < event_count; ++i)
        print_event(&events[i % IA64VMM_ARRAY_SIZE(events)]);
    if (selected_seen) {
        if (selected_done)
            LOGI("VBE selected BX=%04x AX=%04x", selected_bx, selected_ax);
        else
            LOGI("VBE selected BX=%04x result pending", selected_bx);
        const struct mode_info *m = find_mode(selected_bx & 0x37ff);
        if (m)
            print_mode(m->mode, m->detail);
        else
            LOGI("VBE selected mode info not retained");
    }
    if (mode_replace)
        LOGI("VBE mode-info cache: %u replacements", mode_replace);
    LOGI("INT10 trace discarded=%llu", (unsigned long long)discarded);
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(packets); ++i)
        if (packets[i].count)
            LOGW("INT10 %s%u incomplete packet (%u/%u words) seq=%u",
                 i == IA64VMM_MAX_CPUS ? "container" : "cpu", i == IA64VMM_MAX_CPUS ? 0 : i,
                 packets[i].count - 1, packets[i].output ? 15 : 11, packets[i].sequence);
    if (guest_pending)
        LOGW("INT10 guest %u pending call(s); guest may have stopped inside INT10", guest_pending);
    if (container_pending)
        LOGW("INT10 container0 %u pending call(s)", container_pending);
    hal_checkpoint("vbios-trace-int10");
}
