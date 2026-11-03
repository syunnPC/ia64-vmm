/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/amd64.h"
#include "host/amd64/boot_diag.h"
#include "host/amd64/time_math.h"
#include "host/fb/fb_console.h"
#include "core/log_internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

static unsigned pause_seconds;
static const struct boot_info *layout;
static const char *stage = "boot initialization";
void amd64_stage(const char *name)
{
    stage = name;
}
void amd64_checkpoint_init(const struct boot_info *bi)
{
    layout = bi;
    bool seen = false;
    const char *p = bi->cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            ++p;
        const char *end = p;
        while (*end && *end != ' ' && *end != '\t')
            ++end;
        if (end - p >= 10 && !strncmp(p, "pause_sec=", 10)) {
            if (seen)
                hal_panic("duplicate pause_sec option");
            seen = true;
            pause_seconds = 0;
            const char *q = p + 10;
            if (q == end)
                hal_panic("pause_sec needs decimal seconds (0..3600)");
            for (; q < end; ++q) {
                if (*q < '0' || *q > '9' || pause_seconds > (3600u - (unsigned)(*q - '0')) / 10)
                    hal_panic("pause_sec outside 0..3600 seconds");
                pause_seconds = pause_seconds * 10 + (unsigned)(*q - '0');
            }
        }
        p = end;
    }
}
void hal_checkpoint(const char *name)
{
    amd64_checkpoint(name);
}
void amd64_checkpoint(const char *name)
{
    LOGI("checkpoint=%s pause_sec=%u", name, pause_seconds);
    log_drain();
    if (!pause_seconds || !hal_counter_hz())
        return;
    uint64_t now = hal_counter();
    uint64_t wait = amd64_scale_ticks(pause_seconds, hal_counter_hz(), 1, false);
    uint64_t until = wait > UINT64_MAX - now ? UINT64_MAX : now + wait;
    while (hal_counter() < until) {
        hal_cpu_poll();
        log_drain();
        hal_idle_until(until);
    }
    LOGI("checkpoint=%s resumed", name);
}

static bool contains(const char *s, const char *word)
{
    size_t n = strlen(word);
    for (; *s; ++s)
        if (!strncmp(s, word, n))
            return true;
    return false;
}

/* Snapshot the ring before printing anything. Never depend on warm-reset
 * retention, and never chase a vCPU pointer after its executor has stopped. */
void amd64_final_summary(int code, bool screen)
{
    if (!strcmp(stage, "controller discovery")) {
        LOGI("final stage=%s exit_code=%d; inventory retained", stage, code);
        return;
    }
    const struct log_ring *r = log_get_ring();
    /* The last GUEST_LINES guest lines, and the last STORAGE_LINES guest
     * lines naming a storage path; controller/device lines may precede the
     * final boot-medium diagnostic by more than the ordinary tail. */
    enum { GUEST_LINES = 10, STORAGE_LINES = 16, PCI_LINES = 24 };
    static const char *const storage_words[] = {
        "Ata",        "AHCI",     "Ahci",      "SATA",   "Sata",     "ATAPI",    "Atapi",
        "Scsi",       "BlockIo",  "Block I/O", "CDROM",  "Bds",      "BDS",      "Partition",
        "boot media", "bootable", "Port ",     "port [", "COMRESET", "Mode Fail"};
    static char guests[GUEST_LINES][161], storage[STORAGE_LINES][161], pci[PCI_LINES][161];
    char reason[241] = "", ip[161] = "", line[241];
    unsigned n = 0, found = 0, stored = 0, pcis = 0;
    uint64_t end = r->write_pos, start = end > LOG_RING_SIZE ? end - LOG_RING_SIZE : 0;
    for (uint64_t at = start; at < end; ++at) {
        char c = r->data[at % LOG_RING_SIZE];
        if (c != '\n') {
            if (n < sizeof(line) - 1)
                line[n++] = c;
            continue;
        }
        line[n] = 0;
        n = 0;
        if (!strncmp(line, "[guest] ", 8)) {
            size_t len = strlen(line + 8);
            if (len > 160)
                len = 160;
            char *slot = guests[found++ % GUEST_LINES];
            memcpy(slot, line + 8, len);
            slot[len] = 0;
            /* The firmware's PCI bus scan: which functions its drivers can see. */
            if (contains(line + 8, "PciBus")) {
                slot = pci[pcis++ % PCI_LINES];
                memcpy(slot, line + 8, len);
                slot[len] = 0;
            }
            for (unsigned w = 0; w < IA64VMM_ARRAY_SIZE(storage_words); ++w)
                if (contains(line + 8, storage_words[w])) {
                    slot = storage[stored++ % STORAGE_LINES];
                    memcpy(slot, line + 8, len);
                    slot[len] = 0;
                    break;
                }
        }
        if (!strncmp(line, "[error] guest", 13) || !strncmp(line, "[info] EFI ResetSystem", 22) ||
            !strncmp(line, "[error] panic:", 14)) {
            memcpy(reason, line, strlen(line) + 1);
        }
        /* Both vcpu_dump and successful ResetSystem carry the actual (guest)
         * IP. Only a whole token counts: a host exception's "RIP=" is not a
         * guest IP and its line is reprinted whole below. */
        for (unsigned i = 0; line[i]; ++i)
            if ((!strncmp(line + i, "IP=", 3) || !strncmp(line + i, "ip=", 3)) &&
                (!i || line[i - 1] == ' ')) {
                size_t len = strlen(line + i);
                if (len > 160)
                    len = 160;
                memcpy(ip, line + i, len);
                ip[len] = 0;
                break;
            }
    }
    /* The firmware's "vga: " transition trace (register values
     * before/after) belongs on the summary screen. 30 lines
     * keep the whole summary within a 768-line (48-row) panel. */
    static char trace[30][LOG_MATCH_COLUMNS + 1];
    size_t trace_kept = 0,
           trace_total = log_ring_matching(r, "[guest] vga: ", trace, 30, &trace_kept);
    /* stop_fault=K / log_faults=N: the faulting instruction; a nested
     * General Exception IIP may name its handler rather than the cause. */
    static char stops[2][LOG_MATCH_COLUMNS + 1], faults[8][LOG_MATCH_COLUMNS + 1];
    size_t stops_kept = 0, faults_kept = 0;
    log_ring_matching(r, "[warn] stop_fault: ", stops, 2, &stops_kept);
    size_t faults_total = log_ring_matching(r, "[warn] log_faults: ", faults, 8, &faults_kept);
    /* A fatal host exception (host/amd64/cpu.c): vector, error code,
     * registers and stack, which the screen clear below would otherwise
     * take away on a machine without serial. */
    static char host_exception[14][LOG_MATCH_COLUMNS + 1];
    size_t host_kept = 0;
    log_ring_matching(r, "[error] host exc: ", host_exception, 14, &host_kept);
    if (screen) {
        fb_console_reclaim();
        fb_console_clear();
    }
    LOGI("final stage=%s exit_code=%d", stage, code);
    static const char *const codes[] = {"EFI_SUCCESS", "failure",       "unimplemented",
                                        "bad access",  "setup failure", "timeout"};
    LOGI("result=%s", code >= 0 && code < 6 ? codes[code] : "unknown");
    if (code && !layout && amd64_boot_diag_available()) {
        /* Fit the cause and retained boot state on an 80x30 console.
         * The full exception register dump remains in the RAM/serial log. */
        if (*reason)
            LOGI("reason: %s", reason);
        for (size_t i = 0; i < host_kept; ++i)
            if (i < 3 || contains(host_exception[i], " rfl="))
                LOGI("%s", host_exception[i] + 8);
        amd64_boot_diag_dump();
        if (screen)
            amd64_checkpoint("boot-exit");
        return;
    }
    LOGI("TSC=%llu Hz source=%s", (unsigned long long)hal_counter_hz(), amd64_time_source());
    if (layout) {
        LOGI("host CPUs=%u management=CPU%u BootInfo ranges=%u/%u", layout->host_cpu_count,
             layout->management_cpu, layout->mem_range_count, BOOT_MAX_MEM_RANGES);
        for (unsigned v = 0; v < layout->vcpu_count; ++v)
            for (unsigned i = 0; i < layout->host_cpu_count; ++i) {
                const struct boot_cpu_info *cpu = &layout->cpus[i];
                if (cpu->vcpu_index == v)
                    LOGI("vCPU%u=CPU%u APIC=%u package=%u core=%u type=%02x", v, i, cpu->apic_id,
                         cpu->package_id, cpu->core_id, cpu->core_type);
            }
        /* One line for the helpers: the summary has to fit one screen. */
        char helpers[200];
        size_t used = 0;
        helpers[0] = 0;
        for (unsigned h = 0; h < hal_helper_count() && used + 16 < sizeof(helpers); ++h)
            used += (size_t)vmm_snprintf(helpers + used, sizeof(helpers) - used, " CPU%u/r%u",
                                         hal_helper_cpu(h), hal_helper_rank(h));
        if (hal_helper_count())
            LOGI("helpers=%u:%s", hal_helper_count(), helpers);
    }
    if (*reason)
        LOGI("reason: %s", reason);
    for (size_t i = 0; i < host_kept; ++i)
        LOGI("%s", host_exception[i] + 8);
    if (*ip)
        LOGI("last %s", ip);
    if (pcis) {
        LOGI("PciBus lines (last %u of %u):", pcis < PCI_LINES ? pcis : PCI_LINES, pcis);
        for (unsigned i = pcis < PCI_LINES ? 0 : pcis - PCI_LINES; i < pcis; ++i)
            LOGI("  %s", pci[i % PCI_LINES]);
    }
    if (stored) {
        LOGI("Storage lines (last %u of %u):", stored < STORAGE_LINES ? stored : STORAGE_LINES,
             stored);
        for (unsigned i = stored < STORAGE_LINES ? 0 : stored - STORAGE_LINES; i < stored; ++i)
            LOGI("  %s", storage[i % STORAGE_LINES]);
    }
    if (found) {
        LOGI("Last guest lines:");
        for (unsigned i = found < GUEST_LINES ? 0 : found - GUEST_LINES; i < found; ++i)
            LOGI("  %s", guests[i % GUEST_LINES]);
    }
    if (trace_total) {
        LOGI("VGA trace (last %zu of %zu lines):", trace_kept, trace_total);
        for (size_t i = 0; i < trace_kept; ++i)
            LOGI(" %s", trace[i] + 8);
    }
    /* Last, so they stay on a 48-row (1024x768) screen after the scroll. */
    for (size_t i = 0; i < stops_kept; ++i)
        LOGI("%s", stops[i] + 7);
    if (faults_kept) {
        LOGI("Fault lines (last %zu of %zu):", faults_kept, faults_total);
        for (size_t i = 0; i < faults_kept; ++i)
            LOGI("  %s", faults[i] + 19);
    }
    if (screen)
        amd64_checkpoint("guest-exit");
}
bool amd64_pause_enabled(void)
{
    return pause_seconds != 0;
}
