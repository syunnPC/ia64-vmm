/* SPDX-License-Identifier: GPL-2.0-or-later */
/* StorVSP protocol definitions: Microsoft OpenVMM storvsp_protocol (MIT).
 * The independent byte-oriented model checks the 64-byte 6.x wire format,
 * SCSI CDB big endian fields, and GPA-direct pages above 4 GiB. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/amd64/storvsc.c"

static _Alignas(4096) uint8_t pool[16384];
static uint8_t response[64];
static uint64_t ticks, response_xid;
static unsigned response_bytes, commands, reads, writes, flushes, allocations;
static unsigned version_count, fault, response_type, sense_mode, init_stage, ua_left;
static bool pending, fallback, optical, high_capacity, writeprotect;
static struct vmbus_offer offers[2];
static unsigned offers_count;
enum {
    BAD_XID = 1,
    SHORT_PACKET,
    BAD_TYPE,
    BAD_SRB,
    BAD_TRANSFER,
    DROP,
    BAD_LENGTH,
    CHECK_SENSE,
    BAD_STATUS,
    BAD_SCSI,
    BAD_OPERATION,
    SHORT_TRANSFER,
    BAD_LUN_LIST
};

void vmm_log(enum log_level level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}
bool log_in_panic(void)
{
    return false;
}
unsigned hal_cpu_index(void)
{
    return 0;
}
uint64_t hal_counter(void)
{
    return ++ticks;
}
uint64_t hal_counter_hz(void)
{
    return 10;
}
void *hal_alloc_pages(size_t n, unsigned flags)
{
    assert(n == 4);
    ++allocations;
    if (flags & HAL_PAGE_ZERO)
        memset(pool, 0, sizeof(pool));
    return pool;
}
bool amd64_memory_physical(uint64_t address, uint64_t *pa)
{
    assert(address >= (uintptr_t)pool && address < (uintptr_t)pool + sizeof(pool));
    unsigned offset = address - (uintptr_t)pool;
    *pa = UINT64_C(0x1234560000) + (3 - offset / 4096) * 4096 + offset % 4096;
    return true;
}
static uint32_t little32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void putlittle32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = v >> (8 * i);
}
static uint64_t big(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    while (n--)
        v = v << 8 | *p++;
    return v;
}
static void putbig(uint8_t *p, uint64_t v, unsigned n)
{
    while (n) {
        p[--n] = v;
        v >>= 8;
    }
}
bool amd64_vmbus_start(void)
{
    return true;
}
unsigned amd64_vmbus_offer_count(void)
{
    return offers_count;
}
const struct vmbus_offer *amd64_vmbus_offer(unsigned n)
{
    return &offers[n];
}
static unsigned channel_token, channel_closes;
struct vmbus_channel *amd64_vmbus_open(const struct vmbus_offer *offer)
{
    assert(offer == &offers[0] || offer == &offers[1]);
    return (struct vmbus_channel *)&channel_token;
}
bool amd64_vmbus_close(struct vmbus_channel *channel)
{
    assert(channel == (struct vmbus_channel *)&channel_token);
    ++channel_closes;
    return true;
}

bool amd64_vmbus_send(struct vmbus_channel *channel, const void *payload, unsigned bytes,
                      uint64_t xid, const uint64_t *pfns, unsigned pfn_count, unsigned data_offset,
                      unsigned data_bytes, bool completion)
{
    assert(channel == (struct vmbus_channel *)&channel_token && completion);
    const uint8_t *p = payload;
    assert(bytes == 64 && little32(p + 4) == 1 && !little32(p + 8));
    assert(!pending && xid);
    assert(data_offset == 0 && pfn_count == (data_bytes + 4095) / 4096);
    for (unsigned i = 0; i < pfn_count; ++i)
        assert(pfns[i] == (UINT64_C(0x1234560000) >> 12) + 3 - i);
    ++commands;
    memcpy(response, p, 64);
    putlittle32(response, 1);
    putlittle32(response + 4, 0);
    response_xid = xid;
    response_bytes = 64;
    response_type = 11;
    pending = true;
    unsigned op = little32(p);
    if (op == 7) {
        assert(init_stage == 0);
        init_stage = 1;
    } else if (op == 9) {
        assert(init_stage == 1);
        unsigned version = p[12] | (unsigned)p[13] << 8;
        assert(version == (version_count++ ? 0x600 : 0x602));
        if (fallback && version == 0x602)
            putlittle32(response + 8, 0xc0000059);
    } else if (op == 10) {
        assert(init_stage == 1);
        init_stage = 2;
        putlittle32(response + 24, 8192);
    } else if (op == 8) {
        assert(init_stage == 2);
        init_stage = 3;
    } else if (op == 3) {
        assert(init_stage == 3);
        assert(p[12] == 52 && p[13] == 0 && p[17] == 0);
        assert(p[21] == 20 && p[22] <= 2 && p[18] < 2);
        assert(little32(p + 24) == data_bytes);
        assert(little32(p + 56) == 30);
        const uint8_t *c = p + 28;
        unsigned lun = p[19], opcode = c[0], actual = data_bytes;
        memset(pool, 0, opcode == 0x2a || opcode == 0x8a ? 0 : data_bytes);
        response[14] = 1;
        response[15] = 0;
        if (opcode == 0xa0) {
            assert(p[20] == 12 && p[22] == 1 && big(c + 6, 4) == data_bytes);
            /* The supported primary controller has only target 0. */
            if (p[18]) {
                response[14] = 0x20;
                actual = 0;
            } else
                actual = 24;
            putbig(pool, actual - 8, 4);
            pool[9] = 3;
            pool[17] = 4;
            if (fault == BAD_LUN_LIST)
                putbig(pool, 4096, 4);
        } else if (opcode == 0x12) {
            assert(p[20] == 6 && c[4] == 36);
            if (p[18] || (lun != 3 && lun != 4)) {
                response[14] = 0x20;
                actual = 0;
            } else {
                pool[0] = lun == 4 && optical ? 5 : 0;
                actual = 36;
            }
        } else if (opcode == 0x25) {
            assert(p[20] == 10 && data_bytes == 8);
            putbig(pool, high_capacity ? UINT32_MAX : 4095, 4);
            putbig(pool + 4, lun == 4 && optical ? 2048 : 512, 4);
        } else if (opcode == 0x1a) {
            assert(c[1] == 8 && c[2] == 0x3f && c[4] == 4 && data_bytes == 4);
            pool[0] = 3;
            pool[2] = writeprotect ? 0x80 : 0;
        } else if (opcode == 0x9e) {
            assert(c[1] == 0x10 && p[20] == 16 && big(c + 10, 4) == 32);
            putbig(pool, UINT64_C(0x100000800), 8);
            putbig(pool + 8, 512, 4);
        } else if (opcode == 0x28 || opcode == 0x88 || opcode == 0x2a || opcode == 0x8a) {
            bool write = opcode == 0x2a || opcode == 0x8a;
            unsigned sector = lun == 4 && optical ? 2048 : 512;
            uint64_t lba = big(c + 2, opcode == 0x88 || opcode == 0x8a ? 8 : 4);
            unsigned blocks = big(c + (p[20] == 16 ? 10 : 7), p[20] == 16 ? 4 : 2);
            assert(blocks && blocks * sector == data_bytes && data_bytes <= 8192);
            assert(p[22] == (write ? 0 : 1));
            assert(little32(p + 52) == (write ? 0x80 : 0x40));
            for (unsigned i = 0; i < data_bytes; ++i) {
                uint8_t expected = (lba * sector + i) * 17 + 3;
                if (write)
                    assert(pool[i] == expected);
                else
                    pool[i] = expected;
            }
            if (write)
                ++writes;
            else
                ++reads;
        } else {
            assert(opcode == 0x35 && !data_bytes && p[22] == 2);
            ++flushes;
        }
        putlittle32(response + 24, actual);
        if (fault == BAD_SRB)
            response[14] = 4;
        if (fault == BAD_TRANSFER)
            putlittle32(response + 24, data_bytes + 1);
        if (fault == SHORT_TRANSFER)
            putlittle32(response + 24, data_bytes - 1);
        if (fault == BAD_LENGTH)
            response[12] = 0;
        if (fault == BAD_SCSI)
            response[15] = 8;
        if (fault == CHECK_SENSE || ua_left) {
            response[14] = 0x84;
            response[15] = 2;
            response[21] = sense_mode == 2 ? 3 : 18;
            memset(response + 28, 0, 20);
            response[28] = sense_mode == 1 ? 0x72 : 0x70;
            response[sense_mode == 1 ? 29 : 30] = 5;
            response[35] = 10;
            if (ua_left) {
                response[30] = 6;
                --ua_left;
            }
        }
    } else
        assert(false);
    if (fault == BAD_XID)
        ++response_xid;
    if (fault == SHORT_PACKET)
        response_bytes = 40;
    if (fault == BAD_TYPE)
        response_type = 6;
    if (fault == DROP)
        pending = false;
    if (fault == BAD_STATUS)
        putlittle32(response + 8, 0xc0000001);
    if (fault == BAD_OPERATION)
        putlittle32(response, 2);
    return true;
}
int amd64_vmbus_receive(struct vmbus_channel *channel, void *payload, unsigned capacity,
                        unsigned *bytes, uint64_t *xid, uint16_t *type)
{
    assert(channel == (struct vmbus_channel *)&channel_token);
    if (!pending)
        return 0;
    assert(capacity >= response_bytes);
    memcpy(payload, response, response_bytes);
    *bytes = response_bytes;
    *xid = response_xid;
    *type = response_type;
    pending = false;
    return 1;
}
static void reset_model(void)
{
    memset(&sv, 0, sizeof(sv));
    memset(offers, 0, sizeof(offers));
    const uint8_t type[16] = {0xd9, 0x63, 0x61, 0xba, 0xa1, 0x04, 0x29, 0x4d,
                              0xb6, 0x05, 0x72, 0xe2, 0xff, 0xb1, 0xdc, 0x7f};
    memcpy(offers[0].type, type, 16);
    memcpy(offers[1].type, type, 16);
    offers[0].instance[0] = 1;
    offers[1].instance[0] = 2;
    offers_count = 1;
    ticks = commands = reads = writes = flushes = allocations = 0;
    version_count = fault = sense_mode = init_stage = ua_left = channel_closes = 0;
    pending = fallback = optical = high_capacity = writeprotect = false;
}
int main(void)
{
    uint8_t data[32768];
    reset_model();
    fallback = optical = true;
    assert(amd64_storvsc_start(NULL) && version_count == 2 && !writes);
    assert(amd64_storvsc_enumerate() && amd64_storvsc_count() == 2);
    const struct amd64_storvsc_device *d = amd64_storvsc_find(0, 3);
    const struct amd64_storvsc_device *cd = amd64_storvsc_find(0, 4);
    assert(d && d->blocks == 4096 && d->sector_bytes == 512 && !d->readonly);
    assert(cd && cd->sector_bytes == 2048 && cd->readonly);
    assert(!amd64_storvsc_find(0, 2) && !amd64_storvsc_find(2, 3));
    assert(amd64_storvsc_read(d, 10, 64, data) && reads == 4);
    for (unsigned i = 0; i < sizeof(data); ++i)
        assert(data[i] == (uint8_t)((10 * 512 + i) * 17 + 3));
    assert(amd64_storvsc_write(d, 10, 64, data) && writes == 4);
    assert(amd64_storvsc_flush(d) && flushes == 1);
    assert(amd64_storvsc_read(cd, 3, 8, data) && reads == 6);
    assert(!amd64_storvsc_write(cd, 3, 1, data));
    assert(!amd64_storvsc_read(d, 4095, 2, data));
    assert(!amd64_storvsc_read(d, UINT64_MAX, 2, data));
    assert(!amd64_storvsc_read(d, 0, UINT32_MAX, data));
    assert(amd64_storvsc_read(d, 4096, 0, NULL));
    assert(!amd64_storvsc_device(2));
    amd64_storvsc_stop();
    assert(channel_closes == 1 && !sv.live);
    reset_model();
    high_capacity = true;
    assert(amd64_storvsc_start(NULL));
    d = amd64_storvsc_find(0, 3);
    assert(d && d->blocks == UINT64_C(0x100000801));
    assert(amd64_storvsc_read(d, UINT64_C(0x100000000), 1, data));
    assert(amd64_storvsc_write(d, UINT64_C(0x100000000), 1, data));
    assert(amd64_storvsc_read(d, UINT64_C(0xfffffffe), 3, data));
    reset_model();
    writeprotect = true;
    assert(amd64_storvsc_start(NULL));
    d = amd64_storvsc_find(0, 3);
    assert(d && d->readonly);
    assert(!amd64_storvsc_write(d, 0, 1, data));
    reset_model();
    offers_count = 2;
    assert(!amd64_storvsc_start(NULL) && !commands);
    reset_model();
    offers_count = 2;
    assert(amd64_storvsc_start(offers[1].instance));
    reset_model();
    offers_count = 2;
    offers[1].subchannel = 1;
    assert(amd64_storvsc_start(NULL));
    reset_model();
    assert(amd64_storvsc_start(NULL));
    ua_left = 2;
    assert(amd64_storvsc_find(0, 3) && !ua_left);
    d = amd64_storvsc_find(0, 3);
    ua_left = 1;
    assert(!amd64_storvsc_read(d, 0, 1, data));
    assert(!amd64_storvsc_write(d, 0, 1, data));
    reset_model();
    assert(amd64_storvsc_start(NULL));
    fault = BAD_LUN_LIST;
    assert(!amd64_storvsc_enumerate());
    reset_model();
    assert(amd64_storvsc_start(NULL));
    d = amd64_storvsc_find(0, 3);
    memset(data, 0xa5, 512);
    fault = SHORT_TRANSFER;
    assert(!amd64_storvsc_read(d, 0, 1, data));
    for (unsigned i = 0; i < 512; ++i)
        assert(data[i] == 0xa5);
    for (unsigned f = 1; f <= BAD_OPERATION; ++f) {
        reset_model();
        assert(amd64_storvsc_start(NULL));
        d = amd64_storvsc_find(0, 3);
        assert(d);
        fault = f;
        assert(!amd64_storvsc_read(d, 0, 1, data));
        if (f == BAD_XID || f == SHORT_PACKET || f == BAD_TYPE || f == DROP || f == BAD_OPERATION) {
            assert(channel_closes == 1 && !sv.channel);
            unsigned old = commands;
            fault = 0;
            assert(!amd64_storvsc_read(d, 0, 1, data));
            assert(!amd64_storvsc_start(NULL) && commands == old && allocations == 1);
        }
    }
    for (sense_mode = 0; sense_mode < 3; ++sense_mode) {
        unsigned mode = sense_mode;
        reset_model();
        sense_mode = mode;
        assert(amd64_storvsc_start(NULL));
        d = amd64_storvsc_find(0, 3);
        fault = CHECK_SENSE;
        assert(!amd64_storvsc_read(d, 0, 1, data));
    }
    puts("Hyper-V StorVSC: passed");
}
