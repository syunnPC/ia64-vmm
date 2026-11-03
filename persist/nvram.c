/* SPDX-License-Identifier: GPL-2.0-or-later */
/* EFI variable slots on the VMM medium: per firmware
 * format a manifest-defined number of slot pairs; the record of the
 * running host, recognised by its SMBIOS identity, is loaded before the
 * first guest boot and written once when the guest run ends. */
#include "persist/persist.h"
#include "persist/fat32.h"
#include "persist/protocol.h"
#include "core/log_internal.h"
#include "ia64vmm/platform.h"
#include "ia64vmm/string.h"

#define WINDOW IA64VMM_NVRAM_SIZE
#define SLOT_SECTORS 256u /* header sector, window, unused rest of 128 KiB */
#define CHUNK 32u         /* sectors per backend transfer */

enum { SLOTS_ON, SLOTS_OFF, SLOTS_FRESH };
static struct {
    const struct plog_block *block;
    const uint8_t *m;
    struct fat32 fs;
    struct fat32_file files[64];
    struct plog_block guarded;
    struct boot_identity id;
    unsigned format, pairs, mode;
    /* Where the run-end write goes and why. */
    unsigned target_pair, target_slot;
    uint64_t target_generation, sequence;
    const char *why;
    bool verified, enabled, failed;
} n;
/* loaded: the record the run started from; staged: the last completed update. */
static uint8_t loaded[WINDOW], staged[WINDOW], header[512];
static const char *const format_name[] = {"unsupported", "native"};
static uint64_t u64(const uint8_t *b)
{
    uint64_t v;
    memcpy(&v, b, 8);
    return v;
}
static uint32_t u32(const uint8_t *b)
{
    uint32_t v;
    memcpy(&v, b, 4);
    return v;
}
static void put64(uint8_t *b, uint64_t v)
{
    memcpy(b, &v, 8);
}
static void put32(uint8_t *b, uint32_t v)
{
    memcpy(b, &v, 4);
}

static bool read(uint64_t lba, unsigned count, void *out)
{
    bool ok = !log_in_panic() && n.block->read(lba, count, out);
    if (!ok)
        n.failed = true;
    return ok;
}
static bool write(uint64_t lba, unsigned count, const void *in)
{
    bool ok = !log_in_panic() && n.block->write(lba, count, in);
    if (!ok)
        n.failed = true;
    return ok;
}
static bool flush(void)
{
    bool ok = !log_in_panic() && n.block->flush();
    if (!ok)
        n.failed = true;
    return ok;
}

static const struct boot_module *module(const struct boot_info *bi, const char *name)
{
    for (unsigned i = 0; i < bi->module_count; ++i)
        if (!strcmp(bi->modules[i].name, name))
            return &bi->modules[i];
    return NULL;
}
/* nvram_slots=off|fresh: the last token wins. */
static unsigned slots_mode(const char *line)
{
    unsigned mode = SLOTS_ON;
    while (line && *line) {
        while (*line == ' ' || *line == '\t')
            ++line;
        const char *end = line;
        while (*end && *end != ' ' && *end != '\t')
            ++end;
        if (end - line == 15 && !strncmp(line, "nvram_slots=off", 15))
            mode = SLOTS_OFF;
        else if (end - line == 17 && !strncmp(line, "nvram_slots=fresh", 17))
            mode = SLOTS_FRESH;
        else if (end - line == 14 && !strncmp(line, "nvram_slots=on", 14))
            mode = SLOTS_ON;
        line = end;
    }
    return mode;
}
bool nvram_requested(const struct boot_info *bi)
{
    return module(bi, "nvram") && slots_mode(bi->cmdline) != SLOTS_OFF;
}

void nvram_identity_text(const struct boot_identity *id, char *out, size_t size)
{
    if (id->kind == BOOT_IDENTITY_UUID) {
        const uint8_t *u = id->bytes;
        /* SMBIOS 2.6+ §7.2.1: the first three fields are little-endian. */
        vmm_snprintf(
            out, size,
            "SMBIOS UUID %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            u[3], u[2], u[1], u[0], u[5], u[4], u[7], u[6], u[8], u[9], u[10], u[11], u[12], u[13],
            u[14], u[15]);
    } else if (id->kind == BOOT_IDENTITY_STRINGS) {
        const char *s[6];
        unsigned at = 0;
        for (unsigned k = 0; k < 6; ++k) {
            s[k] = at < id->length ? (const char *)id->bytes + at : "";
            while (at < id->length && id->bytes[at])
                ++at;
            ++at;
        }
        vmm_snprintf(out, size, "SMBIOS strings '%s' '%s' '%s' / '%s' '%s' '%s' (no UUID)", s[0],
                     s[1], s[2], s[3], s[4], s[5]);
    } else
        vmm_snprintf(out, size, "none (no SMBIOS UUID or strings)");
}

static bool manifest_valid(const uint8_t *m, uint64_t size)
{
    if (size != 512 || memcmp(m, "NVRAMMAP", 8) || u32(m + 8) != 2 || u32(m + 12) != 512 ||
        u32(m + 32) != 1 || !u32(m + 36) || u32(m + 36) > 32 || plog_crc(m, 508) != u32(m + 508))
        return false;
    unsigned token = 0;
    for (unsigned i = 16; i < 32; ++i)
        token |= m[i];
    if (!token)
        return false;
    for (unsigned i = 40; i < 508; ++i)
        if (m[i])
            return false;
    return true;
}

void nvram_init(const struct boot_info *bi, const struct plog_block *block)
{
    memset(&n, 0, sizeof(n));
    const struct boot_module *mod = module(bi, "nvram");
    if (!mod)
        return;
    n.mode = slots_mode(bi->cmdline);
    if (n.mode == SLOTS_OFF) {
        LOGI("NVRAM slots: nvram_slots=off: not loaded or written this run");
        return;
    }
    if (!block) {
        LOGI("NVRAM slots: no persistence backend; variables persist within this VMM run only");
        return;
    }
    const uint8_t *m = mod->hva;
    if (!manifest_valid(m, mod->size)) {
        LOGE("NVRAM slots: invalid nvram manifest; variables persist within this VMM run only");
        return;
    }
    n.block = block;
    n.m = m;
    n.format = u32(m + 32);
    n.pairs = u32(m + 36);
    memcpy(&n.id, &bi->identity, sizeof(n.id));
    n.guarded = (struct plog_block){block->sectors, read, write, flush};
    struct fat32_file manifest;
    const char *manifest_name = "nvram-new.bin";
    if (!persist_mount(&n.fs, &n.guarded, manifest_name, m, &manifest))
        goto mismatch;
    for (unsigned pair = 0; pair < n.pairs; ++pair) {
        for (unsigned slot = 0; slot < 2; ++slot) {
            char name[32];
            vmm_snprintf(name, sizeof(name), "nvram-%s-%02u%c.bin", "new", pair, 'a' + slot);
            unsigned index = pair * 2 + slot;
            struct fat32_file *file = n.files + index;
            if (!fat32_lookup(&n.fs, name, file) || file->size != SLOT_SECTORS * 512 ||
                (file->attributes & 0x01) || !fat32_disjoint(&manifest, file))
                goto mismatch;
            for (unsigned j = 0; j < index; ++j)
                if (!fat32_disjoint(n.files + j, file))
                    goto mismatch;
        }
    }
    n.verified = true;
    return;
mismatch:
    n.failed = true;
    LOGE(
        "NVRAM slots: media identity/filesystem mismatch; variables persist within this VMM run only");
}

static bool same_identity(const uint8_t *h)
{
    return u32(h + 52) == n.id.kind && u32(h + 64) == n.id.length &&
           n.id.length <= BOOT_IDENTITY_MAX && !memcmp(h + 72, n.id.bytes, n.id.length);
}
/* Header sector: magic, version, format, the pair's generation, window
 * length, image token, window CRC32, identity kind, stick-wide sequence,
 * identity length, pair index, identity bytes; CRC32 at 508. Reads the
 * slot's header into header and, when it is valid, its window into staged;
 * the generation, 0 when the record is not valid. */
static uint64_t record(unsigned pair, unsigned slot, bool *io_error)
{
    const struct fat32_file *file = n.files + pair * 2 + slot;
    *io_error = !fat32_read(&n.fs, file, 0, header, 512);
    if (*io_error || memcmp(header, "NVRAMRC1", 8) || u32(header + 8) != 1 ||
        u32(header + 12) != n.format || !u64(header + 16) || u64(header + 24) != WINDOW ||
        memcmp(header + 32, n.m + 16, 16) || !u64(header + 56) ||
        u32(header + 64) > BOOT_IDENTITY_MAX || u32(header + 68) != pair ||
        plog_crc(header, 508) != u32(header + 508))
        return 0;
    for (unsigned at = 0; at < WINDOW / 512; at += CHUNK)
        if (!fat32_read(&n.fs, file, (1 + at) * 512, staged + at * 512, CHUNK * 512)) {
            *io_error = true;
            return 0;
        }
    return plog_crc(staged, WINDOW) == u32(header + 48) ? u64(header + 16) : 0;
}

void nvram_load(unsigned format, uint8_t *window)
{
    memcpy(loaded, window, WINDOW);
    memcpy(staged, window, WINDOW);
    if (!n.verified || n.failed)
        return;
    if (format != n.format) {
        n.failed = true;
        LOGE("NVRAM slots: the manifest is for the %s firmware, this run uses the %s firmware",
             format_name[n.format], format_name[format & 1]);
        return;
    }
    /* Every pair: its newest valid record (the slot it keeps), and the
     * newest valid record of this host anywhere. The write goes to this
     * host's pair (newest record its own), else a free pair, else the pair
     * whose newest record is the oldest. */
    uint64_t best_sequence = 0, own_sequence = 0, lru_sequence = UINT64_MAX;
    int own = -1, free_pair = -1, lru = -1;
    unsigned best_pair = 0, best_slot = 0;
    uint64_t best_generation = 0;
    struct {
        unsigned slot;
        uint64_t generation;
    } newest_of_own = {0, 0}, newest_of_lru = {0, 0};
    for (unsigned p = 0; p < n.pairs; ++p) {
        uint64_t newest_generation = 0, newest_sequence = 0;
        unsigned newest_slot = 0;
        bool newest_matches = false;
        for (unsigned s = 0; s < 2; ++s) {
            bool io_error;
            uint64_t generation = record(p, s, &io_error);
            if (io_error) {
                n.failed = true;
                LOGE("NVRAM slots: read failed; variables persist within this VMM run only");
                memcpy(staged, window, WINDOW);
                return;
            }
            if (!generation)
                continue;
            uint64_t sequence = u64(header + 56);
            if (sequence > n.sequence)
                n.sequence = sequence;
            bool matches = same_identity(header);
            if (generation > newest_generation) {
                newest_generation = generation;
                newest_sequence = sequence;
                newest_slot = s;
                newest_matches = matches;
            }
            if (matches && sequence > best_sequence && n.mode != SLOTS_FRESH) {
                best_sequence = sequence;
                best_pair = p;
                best_slot = s;
                best_generation = generation;
                memcpy(loaded, staged, WINDOW);
            }
        }
        if (!newest_generation) {
            if (free_pair < 0)
                free_pair = (int)p;
            continue;
        }
        if (newest_matches && newest_sequence > own_sequence) {
            own_sequence = newest_sequence;
            own = (int)p;
            newest_of_own.slot = newest_slot;
            newest_of_own.generation = newest_generation;
        }
        if (newest_sequence < lru_sequence) {
            lru_sequence = newest_sequence;
            lru = (int)p;
            newest_of_lru.slot = newest_slot;
            newest_of_lru.generation = newest_generation;
        }
    }
    if (n.sequence == UINT64_MAX) {
        n.failed = true;
        LOGE("NVRAM slots: sequence exhausted");
        return;
    }
    if (own >= 0) {
        n.target_pair = (unsigned)own;
        n.target_slot = newest_of_own.slot ^ 1;
        n.target_generation = newest_of_own.generation + 1;
        n.why = "this host's pair";
    } else if (free_pair >= 0) {
        n.target_pair = (unsigned)free_pair;
        n.target_slot = 0;
        n.target_generation = 1;
        n.why = "a free pair";
    } else {
        n.target_pair = (unsigned)lru;
        n.target_slot = newest_of_lru.slot ^ 1;
        n.target_generation = newest_of_lru.generation + 1;
        n.why = "the least recently written pair";
    }
    n.enabled = true;
    char identity[160];
    nvram_identity_text(&n.id, identity, sizeof(identity));
    if (!best_sequence) {
        memcpy(loaded, window, WINDOW);
        memcpy(staged, window, WINDOW);
        LOGI("NVRAM %s slots: %s for this host (%s); the firmware formats the window",
             format_name[n.format],
             n.mode == SLOTS_FRESH ? "nvram_slots=fresh, no record loaded" : "no record", identity);
        return;
    }
    memcpy(window, loaded, WINDOW);
    memcpy(staged, loaded, WINDOW);
    LOGI("NVRAM %s pair %u slot %c: generation %llu loaded (this host: %s)", format_name[n.format],
         best_pair, 'a' + best_slot, (unsigned long long)best_generation, identity);
}

void nvram_stage(const uint8_t *window)
{
    memcpy(staged, window, WINDOW);
}
bool nvram_pending(void)
{
    return memcmp(staged, loaded, WINDOW) != 0;
}

void nvram_save(void)
{
    if (!n.enabled || n.failed || n.fs.failed || log_in_panic())
        return;
    if (!nvram_pending()) {
        LOGI("NVRAM unchanged; nothing written");
        return;
    }
    if (n.target_generation == 0) {
        LOGE("NVRAM generation exhausted; nothing written");
        return;
    }
    const struct fat32_file *file = n.files + n.target_pair * 2 + n.target_slot;
    /* A torn write leaves the pair's other record valid. */
    memset(header, 0, sizeof(header));
    if (!fat32_write(&n.fs, file, 0, header, 512))
        goto failed;
    for (unsigned at = 0; at < WINDOW / 512; at += CHUNK)
        if (!fat32_write(&n.fs, file, (1 + at) * 512, staged + at * 512, CHUNK * 512))
            goto failed;
    if (!flush())
        goto failed;
    memcpy(header, "NVRAMRC1", 8);
    put32(header + 8, 1);
    put32(header + 12, n.format);
    put64(header + 16, n.target_generation);
    put64(header + 24, WINDOW);
    memcpy(header + 32, n.m + 16, 16);
    put32(header + 48, plog_crc(staged, WINDOW));
    put32(header + 52, n.id.kind);
    put64(header + 56, n.sequence + 1);
    put32(header + 64, n.id.length);
    put32(header + 68, n.target_pair);
    memcpy(header + 72, n.id.bytes, n.id.length);
    put32(header + 508, plog_crc(header, 508));
    if (!fat32_write(&n.fs, file, 0, header, 512) || !flush())
        goto failed;
    memcpy(loaded, staged, WINDOW);
    LOGI("NVRAM %s pair %u slot %c: generation %llu written (%s)", format_name[n.format],
         n.target_pair, 'a' + n.target_slot, (unsigned long long)n.target_generation, n.why);
    n.enabled = false;
    return;
failed:
    n.failed = true;
    LOGE(
        "NVRAM write/flush failed; the next run loads the pair's previous record, or this one if its header "
        "reached the medium");
}
