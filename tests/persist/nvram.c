/* SPDX-License-Identifier: GPL-2.0-or-later */
/* EFI variable slots. A manifest per firmware format
 * lists its slot pairs; the newest valid record of the running host (its
 * SMBIOS identity) is loaded; one write at the end of the run goes to the
 * host's pair, else a free pair, else the least recently written pair,
 * into the slot that does not hold the pair's newest record (blank header,
 * window, flush, header, flush); nothing on panic or when unchanged; every
 * media check refuses the slots. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "persist/persist.h"
#include "persist/protocol.h"
#include "core/log_internal.h"
#define SECTORS 131072u
#define SLOT 256u
#define WINDOW 65536u
#define MANIFEST TEST_MANIFEST
#define DIRECTORY TEST_DIRECTORY
static unsigned char disk[SECTORS * 512], m[512], window[WINDOW];
#include "tests/persist/fat32-fixture.h"
static unsigned char *slot_entry[32][2], *manifest_entry;
static struct boot_info bi;
static unsigned pairs, format, ops, fail_at, reads, fail_read;
static bool panic;
static uint64_t relocated_lba = UINT64_MAX;
static char logged[16384];
static struct {
    char kind;
    uint64_t lba;
    unsigned n;
    bool zero;
} trace[256];

bool log_in_panic(void)
{
    return panic;
}
void vmm_log(enum log_level level, const char *fmt, ...)
{
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    size_t at = strlen(logged);
    if (at < sizeof(logged) - 256) {
        vsnprintf(logged + at, sizeof(logged) - at - 1, fmt, ap);
        strcat(logged, "\n");
    }
    va_end(ap);
}
int vmm_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
static uint64_t slot_lba(unsigned p, unsigned s)
{
    return TEST_DATA + 100 + (p * 2 + s) * SLOT;
}
static bool in_slots(uint64_t lba, unsigned n)
{
    for (unsigned p = 0; p < pairs; ++p)
        for (unsigned s = 0; s < 2; ++s)
            if (lba >= slot_lba(p, s) && lba + n <= slot_lba(p, s) + SLOT)
                return true;
    return lba == relocated_lba && n == 1;
}
static bool read_block(uint64_t lba, unsigned n, void *out)
{
    assert(n && n <= 32 && lba + n <= SECTORS);
    if (++reads == fail_read)
        return false;
    memcpy(out, disk + lba * 512, n * 512);
    return true;
}
static bool write_block(uint64_t lba, unsigned n, const void *in)
{
    assert(n && n <= 32 && in_slots(lba, n));
    if (ops < 256) {
        trace[ops].kind = 'W';
        trace[ops].lba = lba;
        trace[ops].n = n;
        trace[ops].zero = true;
        for (unsigned i = 0; i < n * 512; ++i)
            if (((const unsigned char *)in)[i])
                trace[ops].zero = false;
    }
    if (++ops == fail_at)
        return false;
    memcpy(disk + lba * 512, in, n * 512);
    return true;
}
static bool flush(void)
{
    if (ops < 256)
        trace[ops].kind = 'F';
    return ++ops != fail_at;
}
static struct plog_block block = {SECTORS, read_block, write_block, flush};

static void put64(void *out, uint64_t v)
{
    memcpy(out, &v, 8);
}
static void put32(void *out, uint32_t v)
{
    memcpy(out, &v, 4);
}
static uint64_t get64(const void *in)
{
    uint64_t v;
    memcpy(&v, in, 8);
    return v;
}
static uint32_t get32(const void *in)
{
    uint32_t v;
    memcpy(&v, in, 4);
    return v;
}
static void seal(void)
{
    put32(m + 508, plog_crc(m, 508));
    memcpy(disk + MANIFEST * 512, m, 512);
}
static void setup(unsigned count, unsigned fmt)
{
    pairs = count;
    format = fmt;
    memset(disk, 0, sizeof(disk));
    memset(m, 0, sizeof(m));
    memset(&bi, 0, sizeof(bi));
    memcpy(m, "NVRAMMAP", 8);
    put32(m + 8, 2);
    put32(m + 12, 512);
    memcpy(m + 16, "unique image key", 16);
    put32(m + 32, fmt);
    put32(m + 36, count);
    test_volume(SECTORS);
    manifest_entry = test_file(fmt ? "nvram-new.bin" : "nvram-ref.bin", MANIFEST, 512);
    for (unsigned p = 0; p < count; ++p)
        for (unsigned s = 0; s < 2; ++s) {
            char name[32];
            snprintf(name, sizeof(name), "nvram-%s-%02u%c.bin", fmt ? "new" : "ref", p, 'a' + s);
            slot_entry[p][s] = test_file(name, slot_lba(p, s), SLOT * 512);
        }
    seal();
    bi.module_count = 1;
    strcpy(bi.modules[0].name, "nvram");
    bi.modules[0].hva = m;
    bi.modules[0].size = 512;
    ops = fail_at = reads = fail_read = 0;
    relocated_lba = UINT64_MAX;
    panic = false;
    logged[0] = 0;
}
static void host(unsigned kind, const char *text)
{
    memset(&bi.identity, 0, sizeof(bi.identity));
    bi.identity.kind = kind;
    bi.identity.length = kind == BOOT_IDENTITY_UUID      ? 16
                         : kind == BOOT_IDENTITY_STRINGS ? (unsigned)strlen(text) + 1
                                                         : 0;
    memcpy(bi.identity.bytes, text, bi.identity.length);
}
static void content(unsigned char *out, unsigned seed)
{
    for (unsigned i = 0; i < WINDOW; ++i)
        out[i] = (unsigned char)(seed * 131 + i * 7 + (i >> 9));
}
/* The record format: header sector, then the 64 KiB window. */
static void record(unsigned p, unsigned s, uint64_t gen, uint64_t seq,
                   const struct boot_identity *id, const unsigned char *data)
{
    unsigned char *h = disk + slot_lba(p, s) * 512;
    memset(h, 0, 512);
    memcpy(h, "NVRAMRC1", 8);
    put32(h + 8, 1);
    put32(h + 12, format);
    put64(h + 16, gen);
    put64(h + 24, WINDOW);
    memcpy(h + 32, m + 16, 16);
    memcpy(h + 512, data, WINDOW);
    put32(h + 48, plog_crc(data, WINDOW));
    put32(h + 52, id->kind);
    put64(h + 56, seq);
    put32(h + 64, id->length);
    put32(h + 68, p);
    memcpy(h + 72, id->bytes, id->length);
    put32(h + 508, plog_crc(h, 508));
}
static void reseal_header(unsigned p, unsigned s)
{
    unsigned char *h = disk + slot_lba(p, s) * 512;
    put32(h + 508, plog_crc(h, 508));
}
/* A VMM run's start: verification and the first boot's load. */
static void start(void)
{
    logged[0] = 0;
    ops = 0;
    nvram_init(&bi, &block);
    memset(window, 0, sizeof(window));
    nvram_load(format, window);
}
static void run_end(const unsigned char *data)
{
    memcpy(window, data, WINDOW);
    nvram_stage(window);
    ops = 0;
    nvram_save();
}
static bool zero(const unsigned char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (b[i])
            return false;
    return true;
}
static void check_record(unsigned p, unsigned s, uint64_t gen, uint64_t seq,
                         const struct boot_identity *id, const unsigned char *data)
{
    const unsigned char *h = disk + slot_lba(p, s) * 512;
    assert(!memcmp(h, "NVRAMRC1", 8) && get32(h + 8) == 1 && get32(h + 12) == format);
    assert(get64(h + 16) == gen && get64(h + 24) == WINDOW && !memcmp(h + 32, m + 16, 16));
    assert(get32(h + 48) == plog_crc(data, WINDOW) && get32(h + 508) == plog_crc(h, 508));
    assert(get32(h + 52) == id->kind && get64(h + 56) == seq && get32(h + 64) == id->length &&
           get32(h + 68) == p);
    assert(!memcmp(h + 72, id->bytes, id->length));
    assert(!memcmp(h + 512, data, WINDOW));
}
static void refused(void)
{
    start();
    assert(zero(window, WINDOW));
    window[5] = 1;
    nvram_stage(window);
    ops = 0;
    nvram_save();
    assert(!ops);
}

int main(void)
{
    static unsigned char a[WINDOW], b[WINDOW], c[WINDOW], d[WINDOW];
    content(a, 1);
    content(b, 2);
    content(c, 3);
    content(d, 4);
    struct boot_identity A, B, C, D, S;
    host(BOOT_IDENTITY_UUID, "machine A uuid!!");
    A = bi.identity;

    /* No module, or nvram_slots=off: nothing configured, read or written. */
    setup(3, 1);
    bi.module_count = 0;
    assert(!nvram_requested(&bi));
    start();
    assert(!reads && zero(window, WINDOW));
    run_end(a);
    assert(!ops);
    setup(3, 1);
    assert(nvram_requested(&bi));
    strcpy(bi.cmdline, "engine=tier0 nvram_slots=off");
    assert(!nvram_requested(&bi));
    start();
    assert(!reads);
    run_end(a);
    assert(!ops);
    /* Without a backend the window stays this run's only copy. */
    setup(3, 1);
    nvram_init(&bi, NULL);
    memset(window, 0, sizeof(window));
    nvram_load(1, window);
    assert(zero(window, WINDOW));
    run_end(a);
    assert(!ops);

    /* Blank slots: the window starts at zero; unchanged writes nothing;
     * only a commit is staged. */
    setup(3, 1);
    bi.identity = A;
    start();
    assert(zero(window, WINDOW) && !nvram_pending());
    ops = 0;
    nvram_save();
    assert(!ops && strstr(logged, "unchanged"));
    window[100] = 7;
    assert(!nvram_pending());
    nvram_stage(window);
    assert(nvram_pending());
    window[100] = 0;
    nvram_stage(window);
    assert(!nvram_pending());

    /* First write of host A: pair 0 slot a, generation 1, sequence 1;
     * later uncommitted changes are not written. */
    setup(3, 1);
    bi.identity = A;
    start();
    memcpy(window, a, WINDOW);
    nvram_stage(window);
    memset(window, 0x55, WINDOW);
    ops = 0;
    nvram_save();
    check_record(0, 0, 1, 1, &A, a);
    assert(trace[0].kind == 'W' && trace[0].lba == slot_lba(0, 0) && trace[0].n == 1 &&
           trace[0].zero);
    unsigned covered = 0, last_payload = 0;
    for (unsigned i = 1; i < ops; ++i)
        if (trace[i].kind == 'W' && trace[i].lba != slot_lba(0, 0)) {
            covered += trace[i].n;
            last_payload = i;
        }
    assert(covered == 128);
    assert(trace[last_payload + 1].kind == 'F' && trace[last_payload + 2].kind == 'W' &&
           trace[last_payload + 2].lba == slot_lba(0, 0) && trace[last_payload + 2].n == 1 &&
           trace[last_payload + 3].kind == 'F' && ops == last_payload + 4);
    assert(strstr(logged, "pair 0 slot a: generation 1 written (a free pair)"));
    /* One write per run. */
    ops = 0;
    nvram_save();
    assert(!ops);
    memcpy(window, c, WINDOW);
    nvram_stage(window);
    nvram_save();
    assert(!ops);

    /* Host A again loads it and writes the pair's other slot. */
    start();
    assert(!memcmp(window, a, WINDOW) && strstr(logged, "pair 0 slot a: generation 1 loaded"));
    run_end(b);
    check_record(0, 1, 2, 2, &A, b);
    check_record(0, 0, 1, 1, &A, a);
    assert(strstr(logged, "(this host's pair)"));
    /* Unchanged: nothing written. */
    start();
    assert(!memcmp(window, b, WINDOW));
    run_end(b);
    assert(!ops);

    /* Host B (a UUID too): no record, empty window, a free pair; A's pair
     * stays. Then A, then B again, each with its own variables. */
    host(BOOT_IDENTITY_UUID, "machine B uuid!!");
    B = bi.identity;
    start();
    assert(zero(window, WINDOW) && strstr(logged, "no record for this host"));
    run_end(c);
    check_record(1, 0, 1, 3, &B, c);
    check_record(0, 1, 2, 2, &A, b);
    bi.identity = A;
    start();
    assert(!memcmp(window, b, WINDOW));
    run_end(d);
    check_record(0, 0, 3, 4, &A, d);
    check_record(1, 0, 1, 3, &B, c);
    bi.identity = B;
    start();
    assert(!memcmp(window, c, WINDOW));
    run_end(a);
    check_record(1, 1, 2, 5, &B, a);

    /* Identity from SMBIOS strings when the UUID is unset; the same bytes
     * of another kind are another machine. */
    host(BOOT_IDENTITY_STRINGS, "");
    bi.identity.length = 16;
    memcpy(bi.identity.bytes, "Maker\0Model\0SN1", 16);
    S = bi.identity;
    start();
    assert(zero(window, WINDOW));
    run_end(b);
    check_record(2, 0, 1, 6, &S, b);
    start();
    assert(!memcmp(window, b, WINDOW));
    bi.identity = S;
    bi.identity.kind = BOOT_IDENTITY_UUID;
    bi.identity.length = 16;
    memcpy(bi.identity.bytes, "Maker\0Model\0SN1", 16);
    start();
    assert(zero(window, WINDOW));
    /* No SMBIOS identity: such records match each other only. */
    host(BOOT_IDENTITY_NONE, "");
    start();
    assert(zero(window, WINDOW));

    /* N + 1 machines: all three pairs taken (A seq 4/2, B 5, S 6), so
     * machine C takes the least recently written pair (A's, newest
     * sequence 4) and overwrites its older slot; A's newest survives. */
    host(BOOT_IDENTITY_UUID, "machine C uuid!!");
    C = bi.identity;
    start();
    assert(zero(window, WINDOW));
    run_end(c);
    check_record(0, 1, 4, 7, &C, c);
    check_record(0, 0, 3, 4, &A, d);
    assert(strstr(logged, "pair 0 slot b: generation 4 written (the least recently written pair)"));
    /* A still loads its record; its pair now belongs to C, so A takes
     * the least recently written pair (B's, sequence 5). */
    bi.identity = A;
    start();
    assert(!memcmp(window, d, WINDOW));
    run_end(a);
    check_record(1, 0, 3, 8, &A, a);
    check_record(1, 1, 2, 5, &B, a);
    /* D: the least recently written pair is S's (sequence 6). */
    host(BOOT_IDENTITY_UUID, "machine D uuid!!");
    D = bi.identity;
    start();
    run_end(b);
    check_record(2, 1, 2, 9, &D, b);
    check_record(2, 0, 1, 6, &S, b);
    (void)D;

    /* A corrupt record falls back to the pair's other one; a pair whose
     * records are both invalid is free. */
    setup(2, 1);
    bi.identity = A;
    record(0, 0, 4, 10, &A, a);
    record(0, 1, 5, 11, &A, b);
    disk[slot_lba(0, 1) * 512 + 512 + 7] ^= 1;
    start();
    assert(!memcmp(window, a, WINDOW));
    run_end(c);
    check_record(0, 1, 5, 11, &A, c);
    setup(2, 1);
    bi.identity = B;
    record(0, 0, 4, 10, &A, a);
    record(0, 1, 5, 11, &A, b);
    disk[slot_lba(0, 0) * 512 + 100] ^= 1;
    disk[slot_lba(0, 1) * 512 + 512 + 9] ^= 1;
    record(1, 0, 1, 3, &A, c);
    start();
    run_end(d);
    check_record(0, 0, 1, 4, &B, d);
    /* Invalid fields: magic, version, format, length, medium, pair index,
     * identity length, header CRC, generation and sequence 0. */
    for (unsigned broken = 0; broken < 10; ++broken) {
        setup(2, 1);
        bi.identity = A;
        record(0, 0, 4, 10, &A, a);
        record(0, 1, 5, 11, &A, b);
        unsigned char *h = disk + slot_lba(0, 1) * 512;
        switch (broken) {
        case 0:
            h[0] ^= 1;
            break;
        case 1:
            put32(h + 8, 2);
            reseal_header(0, 1);
            break;
        case 2:
            put32(h + 12, 0);
            reseal_header(0, 1);
            break;
        case 3:
            put64(h + 24, WINDOW - 512);
            reseal_header(0, 1);
            break;
        case 4:
            h[40] ^= 1;
            reseal_header(0, 1);
            break;
        case 5:
            put32(h + 68, 1);
            reseal_header(0, 1);
            break;
        case 6:
            put32(h + 64, 385);
            reseal_header(0, 1);
            break;
        case 7:
            h[300] ^= 1;
            break;
        case 8:
            put64(h + 16, 0);
            reseal_header(0, 1);
            break;
        case 9:
            put64(h + 56, 0);
            reseal_header(0, 1);
            break;
        }
        start();
        assert(!memcmp(window, a, WINDOW));
    }

    /* The newest of this host's records by sequence, whichever pair. */
    setup(2, 1);
    bi.identity = A;
    record(0, 0, 1, 9, &A, b);
    record(1, 0, 1, 3, &A, a);
    start();
    assert(!memcmp(window, b, WINDOW));

    /* nvram_slots=fresh: nothing loaded, the write replaces this host's
     * record in its own pair. */
    setup(2, 1);
    bi.identity = A;
    record(0, 0, 4, 10, &A, a);
    strcpy(bi.cmdline, "nvram_slots=fresh");
    assert(nvram_requested(&bi));
    start();
    assert(zero(window, WINDOW));
    run_end(b);
    check_record(0, 1, 5, 11, &A, b);

    /* A native-firmware manifest and a non-default count. */
    setup(1, 1);
    bi.identity = A;
    start();
    run_end(a);
    check_record(0, 0, 1, 1, &A, a);
    start();
    assert(!memcmp(window, a, WINDOW));
    setup(8, 1);
    bi.identity = A;
    start();
    run_end(a);
    check_record(0, 0, 1, 1, &A, a);
    setup(32, 1);
    bi.identity = A;
    record(31, 1, 4, 10, &A, a);
    start();
    assert(!memcmp(window, a, WINDOW));
    run_end(b);
    check_record(31, 0, 5, 11, &A, b);
    /* The manifest's format must be the running firmware's. */
    setup(2, 0);
    bi.identity = A;
    record(0, 0, 4, 10, &A, a);
    nvram_init(&bi, &block);
    memset(window, 0, WINDOW);
    nvram_load(1, window);
    assert(zero(window, WINDOW));
    run_end(b);
    assert(!ops);

    /* A failed write or flush at any step: the run writes nothing more;
     * the next run loads the previous record, or the new one when its
     * header reached the medium before the final flush failed. */
    setup(2, 1);
    bi.identity = A;
    record(0, 0, 4, 10, &A, a);
    start();
    run_end(b);
    unsigned commit_ops = ops;
    for (unsigned fail = 1; fail <= commit_ops; ++fail) {
        setup(2, 1);
        bi.identity = A;
        record(0, 0, 4, 10, &A, a);
        start();
        memcpy(window, b, WINDOW);
        nvram_stage(window);
        fail_at = fail;
        ops = 0;
        nvram_save();
        assert(ops == fail);
        check_record(0, 0, 4, 10, &A, a);
        unsigned previous = ops;
        nvram_save();
        assert(ops == previous);
        fail_at = 0;
        start();
        assert(!memcmp(window, a, WINDOW) || (fail == commit_ops && !memcmp(window, b, WINDOW)));
    }

    /* After a VMM panic nothing is written. */
    setup(2, 1);
    bi.identity = A;
    start();
    memcpy(window, a, WINDOW);
    nvram_stage(window);
    panic = true;
    ops = 0;
    nvram_save();
    assert(!ops);
    panic = false;
    /* Generation exhaustion refuses the write. */
    setup(2, 1);
    bi.identity = A;
    record(0, 0, UINT64_MAX, 10, &A, a);
    start();
    assert(!memcmp(window, a, WINDOW));
    run_end(b);
    assert(!ops);

    /* Identity, partition bounds, FAT chains and cross-linked files. */
    for (unsigned g = 0; g < 17; ++g) {
        setup(2, 1);
        bi.identity = A;
        record(0, 0, 3, 3, &A, a);
        switch (g) {
        case 0:
            m[508] ^= 1;
            memcpy(disk + MANIFEST * 512, m, 512);
            break;
        case 1:
            put32(m + 8, 1);
            seal();
            break;
        case 2:
            memset(m + 16, 0, 16);
            seal();
            break;
        case 3:
            disk[510] ^= 1;
            break;
        case 4:
            test_put32(disk + 446 + 12, SECTORS);
            break;
        case 5:
            disk[MANIFEST * 512 + 10] ^= 1;
            break;
        case 6:
            put32(slot_entry[1][0] + 28, SLOT * 512 - 1);
            break;
        case 7:
            memcpy(slot_entry[1][0] + 20, slot_entry[0][0] + 20, 2);
            memcpy(slot_entry[1][0] + 26, slot_entry[0][0] + 26, 2);
            break;
        case 8:
            memcpy(manifest_entry + 20, slot_entry[0][0] + 20, 2);
            memcpy(manifest_entry + 26, slot_entry[0][0] + 26, 2);
            break;
        case 9:
            disk[(TEST_FAT + TEST_FAT_SIZE) * 512 + test_cluster(slot_lba(1, 0)) * 4] ^= 1;
            break;
        case 10:
            disk[TEST_PARTITION * 512 + 13] = 3;
            break;
        case 11:
            bi.modules[0].size = 1024;
            break;
        case 12:
            put32(m + 36, 0);
            seal();
            break;
        case 13:
            put32(m + 32, 2);
            seal();
            break;
        case 14:
            test_file("log-a.bin", slot_lba(0, 0), SLOT * 512);
            break;
        case 15:
            test_put16(slot_entry[0][0] + 26, 2);
            break;
        case 16:
            put32(m + 36, 33);
            seal();
            break;
        }
        refused();
    }
    /* The same volume on a larger device still loads this host's record. */
    setup(2, 1);
    bi.identity = A;
    record(0, 0, 3, 3, &A, a);
    block.sectors = SECTORS + 100000;
    start();
    assert(!memcmp(window, a, WINDOW));
    run_end(b);
    check_record(0, 1, 4, 4, &A, b);
    block.sectors = SECTORS;
    /* Fragmented files use their FAT chain for both payload I/O directions. */
    setup(2, 1);
    bi.identity = A;
    relocated_lba = TEST_DATA + 50000;
    test_fragment(slot_lba(0, 0), relocated_lba);
    start();
    run_end(a);
    assert(!memcmp(disk + relocated_lba * 512, a, 512));
    assert(!memcmp(disk + (slot_lba(0, 0) + 2) * 512, a + 512, WINDOW - 512));
    start();
    assert(!memcmp(window, a, WINDOW));
    /* A read failure anywhere in verification or load refuses the slots. */
    setup(2, 1);
    bi.identity = A;
    record(0, 0, 3, 3, &A, a);
    record(1, 1, 2, 2, &A, b);
    start();
    unsigned total = reads;
    assert(!memcmp(window, a, WINDOW));
    for (unsigned fail = 1; fail <= total; ++fail) {
        setup(2, 1);
        bi.identity = A;
        record(0, 0, 3, 3, &A, a);
        record(1, 1, 2, 2, &A, b);
        fail_read = fail;
        refused();
    }
    puts("NVRAM slots: passed");
    return 0;
}
