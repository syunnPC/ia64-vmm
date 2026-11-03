/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "persist/persist.h"
#include "persist/fat32.h"
#include "persist/protocol.h"
#include "core/log_internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"

#define SLOT_BYTES (2u * 1024u * 1024u)
static struct {
    const struct plog_block *block;
    uint8_t identity[16];
    struct fat32 fs;
    struct fat32_file files[3];
    struct plog_block guarded;
    uint64_t generation, cursor, next_poll;
    uint64_t total, prefix[2];
    unsigned active, used;
    const char *state;
    bool enabled, busy, failed;
} p;
static uint8_t archive[SLOT_BYTES - 512], work[16384], header[512];
static uint64_t u64(const uint8_t *b)
{
    uint64_t n;
    memcpy(&n, b, 8);
    return n;
}
static uint32_t u32(const uint8_t *b)
{
    uint32_t n;
    memcpy(&n, b, 4);
    return n;
}
static void put64(uint8_t *b, uint64_t n)
{
    memcpy(b, &n, 8);
}
static void put32(uint8_t *b, uint32_t n)
{
    memcpy(b, &n, 4);
}
bool plog_requested(const char *line)
{
    if (!line)
        return false;
    while (*line) {
        while (*line == ' ' || *line == '\t')
            ++line;
        const char *end = line;
        while (*end && *end != ' ' && *end != '\t')
            ++end;
        if (end - line >= 10 && !strncmp(line, "media_log=", 10))
            return end - line == 11 && line[10] == '1';
        line = end;
    }
    return false;
}
static bool read(uint64_t lba, unsigned count, void *out)
{
    bool ok = !log_in_panic() && p.block->read(lba, count, out);
    if (!ok)
        p.failed = true;
    return ok;
}
static bool write(uint64_t lba, unsigned count, const void *in)
{
    bool ok = !log_in_panic() && p.block->write(lba, count, in);
    if (!ok)
        p.failed = true;
    return ok;
}
static bool flush(void)
{
    bool ok = !log_in_panic() && p.block->flush();
    if (!ok)
        p.failed = true;
    return ok;
}
static uint64_t valid_generation(unsigned slot)
{
    if (!fat32_read(&p.fs, &p.files[slot], 0, header, 512) || memcmp(header, "PLOGREC1", 8) ||
        (u32(header + 8) != 1 && u32(header + 8) != 2) || u32(header + 12) != (slot == 2) ||
        !u64(header + 16) || memcmp(header + 32, p.identity, 16) ||
        u64(header + 24) > sizeof(archive) || plog_crc(header, 508) != u32(header + 508))
        return 0;
    uint64_t gen = u64(header + 16);
    unsigned length = (unsigned)u64(header + 24), crc = u32(header + 48);
    unsigned start = u32(header + 8) == 2 ? u32(header + 52) : 0;
    if (start >= sizeof(archive))
        return 0;
    for (unsigned at = 0; at < length;) {
        unsigned pos = (start + at) % sizeof(archive), skip = pos % 512;
        unsigned n = length - at;
        if (n > sizeof(work) - skip)
            n = sizeof(work) - skip;
        if (n > sizeof(archive) - pos)
            n = sizeof(archive) - pos;
        if (!fat32_read(&p.fs, &p.files[slot], 512 + pos - skip, work,
                        ((skip + n + 511) / 512) * 512))
            return 0;
        memcpy(archive + at, work + skip, n);
        at += n;
    }
    return plog_crc(archive, length) == crc ? gen : 0;
}
static bool commit(bool crash)
{
    if (p.generation == UINT64_MAX) {
        LOGE("PLOG generation exhausted");
        return false;
    }
    unsigned target = crash ? 2 : p.active ^ 1;
    memset(header, 0, 512);
    if (!fat32_write(&p.fs, &p.files[target], 0, header, 512))
        return false;
    /* Each copy catches up only the sectors changed since its last commit.
     * Circular payloads retain the newest text without shifting full slots.
     * An inactive copy older than one full turn needs one full refresh. */
    unsigned start = crash ? 0 : (unsigned)((p.total - p.used) % sizeof(archive));
    uint64_t end = crash ? p.used : p.total;
    uint64_t at = crash ? 0 : p.prefix[target];
    if (!crash && at < p.total - p.used)
        at = p.total - p.used;
    at &= ~UINT64_C(511);
    while (at < end) {
        unsigned pos = (unsigned)(at % sizeof(archive));
        uint64_t remaining = end - at;
        unsigned n = remaining > sizeof(work) ? sizeof(work) : (unsigned)remaining;
        n = (n + 511) & ~511u;
        if (n > sizeof(archive) - pos)
            n = sizeof(archive) - pos;
        /* Unused bytes in a partial sector belong to neither committed
         * record until its header names them; copying them is harmless. */
        if (!fat32_write(&p.fs, &p.files[target], 512 + pos, archive + pos, n))
            return false;
        at += n;
    }
    if (!flush())
        return false;
    memcpy(header, "PLOGREC1", 8);
    put32(header + 8, crash ? 1 : 2);
    put32(header + 12, crash);
    put64(header + 16, p.generation + 1);
    put64(header + 24, p.used);
    memcpy(header + 32, p.identity, 16);
    unsigned first = p.used < sizeof(archive) - start ? p.used : sizeof(archive) - start;
    put32(header + 48, plog_crc_pair(archive + start, first, archive, p.used - first));
    put32(header + 52, start);
    put32(header + 508, plog_crc(header, 508));
    if (!fat32_write(&p.fs, &p.files[target], 0, header, 512) || !flush())
        return false;
    if (!crash) {
        p.active = target;
        p.prefix[target] = p.total;
    }
    ++p.generation;
    return true;
}
void plog_init(const struct boot_info *bi, const struct plog_block *block)
{
    memset(&p, 0, sizeof(p));
    p.active = 1;
    p.state = "not requested";
    if (!plog_requested(bi->cmdline))
        return;
    p.failed = true;
    p.state = "unavailable";
    if (!block) {
        LOGE("PLOG unavailable; RAM log retained");
        return;
    }
    p.block = block;
    p.state = "refused";
    const uint8_t *m = NULL;
    for (unsigned i = 0; i < bi->module_count; ++i)
        if (!strcmp(bi->modules[i].name, "persistence") && bi->modules[i].size == 512)
            m = bi->modules[i].hva;
    if (!m || memcmp(m, "PLOGMAP1", 8) || u32(m + 8) != 3 || u32(m + 12) != 512 ||
        plog_crc(m, 508) != u32(m + 508)) {
        LOGE("PLOG missing/invalid PersistenceManifest; writes refused");
        return;
    }
    unsigned token = 0;
    for (unsigned i = 16; i < 32; ++i)
        token |= m[i];
    if (!token)
        goto mismatch;
    for (unsigned i = 32; i < 508; ++i)
        if (m[i])
            goto mismatch;
    p.guarded = (struct plog_block){block->sectors, read, write, flush};
    struct fat32_file manifest;
    const char *const names[] = {"log-a.bin", "log-b.bin", "crash.bin"};
    if (!persist_mount(&p.fs, &p.guarded, "persistence-manifest.bin", m, &manifest) ||
        !persist_files(&p.fs, &manifest, names, p.files, 3, SLOT_BYTES))
        goto mismatch;
    memcpy(p.identity, m + 16, 16);
    p.failed = false;
    uint64_t a = valid_generation(0);
    if (p.failed || p.fs.failed)
        goto mismatch;
    uint64_t b = valid_generation(1);
    if (p.failed || p.fs.failed)
        goto mismatch;
    uint64_t crash = valid_generation(2);
    if (p.failed || p.fs.failed)
        goto mismatch;
    if (a || b) {
        p.active = b > a ? 1 : 0;
        p.generation = a > b ? a : b;
    }
    if (crash > p.generation)
        p.generation = crash;
    p.enabled = true;
    p.state = "verified";
    uint64_t cursor = 0;
    p.used = (unsigned)log_crash_read(&cursor, archive, sizeof(archive));
    if (p.used && !commit(true)) {
        p.failed = true;
        LOGE("PLOG crash record save failed; RAM retained");
        return;
    }
    if (p.used)
        LOGI("PLOG saved crash record generation=%llu", (unsigned long long)p.generation);
    p.used = 0;
    p.next_poll = hal_counter() + hal_counter_hz() * 30;
    LOGI("PLOG media identity verified; FAT32 log files enabled generation=%llu",
         (unsigned long long)p.generation);
    return;
mismatch:
    p.failed = true;
    LOGE("PLOG media identity/filesystem mismatch; writes refused; RAM retained");
}
/* RAM and disk share the same circular byte positions (record v2).
 * The archive retains exactly the newest SLOT_BYTES - 512 bytes. */
static bool append(const void *data, unsigned n)
{
    if (n > UINT64_MAX - p.total)
        return false;
    unsigned pos = (unsigned)(p.total % sizeof(archive));
    unsigned first = n < sizeof(archive) - pos ? n : sizeof(archive) - pos;
    memcpy(archive + pos, data, first);
    memcpy(archive, (const uint8_t *)data + first, n - first);
    p.total += n;
    p.used = p.total < sizeof(archive) ? (unsigned)p.total : sizeof(archive);
    return true;
}
void plog_failure(void)
{
    p.failed = true;
}
void plog_poll(bool final)
{
    if (!p.enabled || p.busy || p.failed || log_in_panic() || hal_cpu_index() != 0)
        return;
    uint64_t now = hal_counter();
    /* One bounded ring copy under its lock; every slow transfer is outside. */
    uint64_t end = log_media_end();
    if (!final && now < p.next_poll && end - p.cursor < 65536)
        return;
    p.busy = true;
    uint64_t cursor = p.cursor, lost;
    unsigned n = (unsigned)log_media_read(
        &cursor, work, end - cursor < sizeof(work) ? end - cursor : sizeof(work), &lost);
    if (!n && !lost) {
        p.next_poll = now + hal_counter_hz() * 30;
        p.busy = false;
        return;
    }
    do {
        if (lost) {
            const char mark[] = "\n[PLOG RAM overrun; bytes lost]\n";
            if (!append(mark, sizeof(mark) - 1))
                goto failed;
        }
        if (!append(work, n))
            goto failed;
        n = (unsigned)log_media_read(
            &cursor, work,
            cursor < end ? (end - cursor < sizeof(work) ? end - cursor : sizeof(work)) : 0, &lost);
    } while (n || lost);
    if (commit(false)) {
        p.cursor = cursor;
        log_media_flushed(cursor);
        p.next_poll = now + hal_counter_hz() * 30;
        LOGI("PLOG committed generation=%llu bytes=%u", (unsigned long long)p.generation, p.used);
    } else {
    failed:
        /* Do not retry ambiguous flush completion with the same generation.
         * The old copy and unchanged RAM cursor survive for the next boot. */
        p.failed = true;
        LOGE("PLOG save/flush failed; RAM retained; media sink stopped");
    }
    p.busy = false;
}

uint64_t plog_deadline(void)
{
    return p.enabled && !p.failed ? p.next_poll : UINT64_MAX;
}

bool plog_save_ok(void)
{
    return !p.failed;
}
const char *plog_status(void)
{
    return p.state ? (p.enabled && p.failed ? "stopped" : p.state) : "not requested";
}
