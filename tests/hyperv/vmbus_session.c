/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scripted control replies exercise the actual transport, without executing
 * hypercalls. Wire expectations use OpenVMM VMBus protocol.rs. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host/amd64/vmbus.c"

static _Alignas(4096) uint8_t pages[128 * 4096];
static unsigned allocated, now, cpu, signals, stopped, posts, last_signal;
static unsigned closed[9];
static bool rescind_during_close;
static uint8_t queue[16][240];
static unsigned sizes[16], head, tail;
static enum {
    NORMAL,
    NO_REPLY,
    TRUNCATED_VERSION,
    WRONG_REPLY,
    TRUNCATED_GPADL,
    WRONG_GPADL,
    FAILED_GPADL,
    FAILED_OPEN,
    WRONG_OPEN,
    TRUNCATED_OFFER,
    DUPLICATE_OFFER,
    POST_BUSY,
    VERSION_FALLBACK,
    POST_CONNECTION_TRANSIENT,
    MULTI
} scenario;
static void reply(unsigned type, unsigned bytes)
{
    if (head == tail)
        head = tail = 0;
    assert(tail < 16);
    memset(queue[tail], 0, 240);
    put32(queue[tail], type);
    sizes[tail++] = bytes;
}
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
    return cpu;
}
uint64_t hal_counter(void)
{
    return now++;
}
uint64_t hal_counter_hz(void)
{
    return 1000;
}
void *hal_alloc_pages(size_t count, unsigned flags)
{
    assert(flags == HAL_PAGE_ZERO && allocated + count <= 128);
    uint8_t *p = pages + allocated * 4096;
    allocated += count;
    memset(p, 0, count * 4096);
    return p;
}
bool amd64_memory_physical(uint64_t va, uint64_t *pa)
{
    assert(va >= (uintptr_t)pages && va < (uintptr_t)pages + sizeof(pages));
    *pa = va - (uintptr_t)pages + 0x100000;
    return true;
}
bool amd64_hyperv_start(void)
{
    return true;
}
uint32_t amd64_hyperv_vp(void)
{
    return 7;
}
void amd64_hyperv_stop(void)
{
    stopped++;
}
void amd64_hyperv_events(void) {}
uint16_t amd64_hyperv_signal(uint32_t connection)
{
    assert(connection >= 0x123 && connection < 0x12c);
    last_signal = connection;
    signals++;
    return 0;
}
uint16_t amd64_hyperv_post(uint32_t connection, const void *message, unsigned bytes)
{
    assert(connection == 1);
    posts++;
    const uint8_t *p = message;
    unsigned type = get32(p);
    if (scenario == POST_BUSY)
        return 19;
    if (scenario == NO_REPLY)
        return 0;
    if (scenario == POST_CONNECTION_TRANSIENT && type == 3 && posts == 2)
        return 18;
    if (type == 14) {
        assert(bytes == 40 && get32(p + 12) == 7);
        assert(get32(p + 16) == 0x100000 && get32(p + 24) == 0x101000 && get32(p + 32) == 0x102000);
        assert(get32(p + 8) == (posts == 2 ? 0x30000u : 0x40000u));
        reply(scenario == WRONG_REPLY ? 6 : 15, scenario == TRUNCATED_VERSION ? 8 : 16);
        queue[tail - 1][8] = !(scenario == VERSION_FALLBACK && posts == 1);
    } else if (type == 3) {
        assert(bytes == 8);
        for (unsigned offer = 0; offer < (scenario == MULTI ? 9u : 1u); ++offer) {
            reply(1, scenario == TRUNCATED_OFFER ? 192 : 196);
            for (unsigned i = 0; i < 16; i++) {
                queue[tail - 1][8 + i] = i;
                queue[tail - 1][24 + i] = 31 - i;
            }
            put32(queue[tail - 1] + 184, 42 + offer);
            put32(queue[tail - 1] + 192, 0x123 + offer);
        }
        if (scenario == DUPLICATE_OFFER) {
            memcpy(queue[tail], queue[tail - 1], 240);
            sizes[tail++] = 196;
        }
        reply(4, 8);
    } else if (type == 8) {
        assert(bytes == 76 && get32(p + 8) >= 42 && get32(p + 8) <= 50 && get16(p + 16) == 56 &&
               get16(p + 18) == 1);
        assert(get32(p + 20) == 24576 && get32(p + 24) == 0);
        for (unsigned i = 0; i < 6; i++) {
            uint64_t pfn;
            memcpy(&pfn, p + 28 + 8 * i, 8);
            assert(pfn == 0x100 + allocated - 6 + i);
        }
        reply(10, scenario == TRUNCATED_GPADL ? 16 : 20);
        put32(queue[tail - 1] + 8, scenario == WRONG_GPADL ? 41 : get32(p + 8));
        put32(queue[tail - 1] + 12, get32(p + 12));
        put32(queue[tail - 1] + 16, scenario == FAILED_GPADL);
    } else if (type == 5) {
        assert(bytes == 148 && get32(p + 8) >= 42 && get32(p + 8) <= 50 &&
               get32(p + 12) == get32(p + 16));
        assert(get32(p + 20) == 7 && get32(p + 24) == 3);
        for (unsigned i = 28; i < 148; i++)
            assert(p[i] == 0);
        reply(6, 20);
        put32(queue[tail - 1] + 8, get32(p + 8));
        put32(queue[tail - 1] + 12, get32(p + 12) + (scenario == WRONG_OPEN));
        put32(queue[tail - 1] + 16, scenario == FAILED_OPEN);
    } else if (type == 7) {
        assert(bytes == 12 && get32(p + 8) >= 42 && get32(p + 8) <= 50);
        closed[get32(p + 8) - 42]++;
    } else if (type == 11) {
        assert(bytes == 16 && get32(p + 8) >= 42 && get32(p + 8) <= 50);
        if (rescind_during_close && get32(p + 8) == 42) {
            reply(2, 12);
            put32(queue[tail - 1] + 8, 43);
        }
        reply(12, 12);
        put32(queue[tail - 1] + 8, get32(p + 12));
    } else if (type == 16) {
        assert(bytes == 8);
        reply(17, 8);
    } else
        assert(false);
    return 0;
}
int amd64_hyperv_message(void *out, unsigned capacity, unsigned *bytes)
{
    if (head == tail) {
        head = tail = 0;
        return 0;
    }
    assert(sizes[head] <= capacity);
    *bytes = sizes[head];
    memcpy(out, queue[head], sizes[head]);
    head++;
    return 1;
}
static void reset(unsigned mode)
{
    memset(&bus, 0, sizeof(bus));
    allocated = now = cpu = signals = stopped = posts = head = tail = last_signal = 0;
    memset(closed, 0, sizeof(closed));
    rescind_during_close = false;
    scenario = mode;
}
int main(void)
{
    reset(NORMAL);
    cpu = 1;
    assert(!amd64_vmbus_start() && !allocated);
    reset(NORMAL);
    assert(amd64_vmbus_start());
    assert(amd64_vmbus_offer_count() == 1 && amd64_vmbus_offer(1) == NULL);
    const struct vmbus_offer *offer = amd64_vmbus_offer(0);
    assert(offer && offer->relid == 42 && offer->connection_id == 0x123);
    for (unsigned i = 0; i < 16; i++) {
        assert(offer->type[i] == i);
        assert(offer->instance[i] == 31 - i);
    }
    struct vmbus_offer arbitrary = *offer;
    assert(!amd64_vmbus_open(&arbitrary));
    struct vmbus_channel *channel = amd64_vmbus_open(offer);
    assert(channel);
    uint8_t payload[24];
    memset(payload, 0xa5, sizeof(payload));
    uint64_t pfns[] = {0x500, 0x900};
    assert(!amd64_vmbus_close(NULL));
    assert(!amd64_vmbus_send(NULL, payload, 24, 12, NULL, 0, 0, 0, false));
    assert(!amd64_vmbus_send(channel, payload, 24, 12, pfns, 2, 0, 4096, true));
    assert(!amd64_vmbus_send(channel, payload, 24, 12, pfns, 2, 4096, 4096, true));
    assert(amd64_vmbus_send(channel, payload, 24, 12, pfns, 2, 1, 5000, true));
    const uint8_t expected[] = {9, 0, 6, 0, 9, 0, 1, 0, 12,   0,    0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 1, 0, 0, 0, 0x88, 0x13, 0, 0, 1, 0, 0, 0};
    assert(memcmp(channel->rings + 4096, expected, sizeof(expected)) == 0);
    assert(memcmp(channel->rings + 4096 + 32, pfns, 16) == 0);
    assert(memcmp(channel->rings + 4096 + 48, payload, 24) == 0 && signals == 1);
    assert(*(uint32_t *)(bus.interrupt + 2048 + 4) == (1u << 10));
    uint8_t packet[40] = {11, 0, 2, 0, 5, 0, 0, 0, 12};
    bool notify;
    memcpy(packet + 16, payload, 24);
    assert(vmbus_ring_write(channel->in, (uint8_t *)channel->in + 4096, RING_SIZE, packet, 40,
                            &notify) == 1);
    uint8_t out[64];
    unsigned bytes;
    uint64_t xid;
    uint16_t type;
    assert(amd64_vmbus_receive(channel, out, sizeof(out), &bytes, &xid, &type) == 1);
    assert(bytes == 24 && xid == 12 && type == 11 && memcmp(out, payload, 24) == 0);
    cpu = 1;
    assert(!amd64_vmbus_send(channel, payload, 24, 13, NULL, 0, 0, 0, true));
    cpu = 0;
    uint8_t *old = channel->rings;
    unsigned used = allocated;
    assert(amd64_vmbus_close(channel) && allocated == used);
    channel = amd64_vmbus_open(offer);
    assert(channel && channel->rings != old && channel->handle == 2);
    amd64_vmbus_stop();
    assert(stopped == 1 && !bus.live);
    assert(!amd64_vmbus_start()); /* A stopped/failed session is never reused. */

    const unsigned start_failures[] = {NO_REPLY,        TRUNCATED_VERSION, WRONG_REPLY,
                                       TRUNCATED_OFFER, DUPLICATE_OFFER,   POST_BUSY};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(start_failures); i++) {
        reset(start_failures[i]);
        assert(!amd64_vmbus_start() && !bus.live);
        unsigned retained = allocated;
        assert(!amd64_vmbus_start() && allocated == retained);
        if (scenario == NO_REPLY || scenario == POST_BUSY)
            assert(now >= 5000 && now < 5010);
    }
    const unsigned open_failures[] = {TRUNCATED_GPADL, WRONG_GPADL, WRONG_OPEN};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(open_failures); i++) {
        reset(open_failures[i]);
        assert(amd64_vmbus_start());
        assert(!amd64_vmbus_open(amd64_vmbus_offer(0)) && !bus.live && allocated == 9);
    }
    reset(MULTI);
    assert(amd64_vmbus_start());
    channel = amd64_vmbus_open(amd64_vmbus_offer(0));
    assert(channel);
    scenario = FAILED_OPEN;
    assert(!amd64_vmbus_open(amd64_vmbus_offer(1)) && bus.live && channel->live);
    scenario = FAILED_GPADL;
    assert(!amd64_vmbus_open(amd64_vmbus_offer(1)) && bus.live && channel->live);
    scenario = NORMAL;
    assert(amd64_vmbus_send(channel, payload, 24, 17, NULL, 0, 0, 0, true));
    amd64_vmbus_stop();
    assert(stopped == 1);
    reset(VERSION_FALLBACK);
    assert(amd64_vmbus_start() && posts == 3);
    reset(POST_CONNECTION_TRANSIENT);
    assert(amd64_vmbus_start() && posts == 3);
    reset(NORMAL);
    assert(amd64_vmbus_start() && (channel = amd64_vmbus_open(amd64_vmbus_offer(0))));
    reply(2, 12);
    put32(queue[tail - 1] + 8, 42);
    assert(amd64_vmbus_receive(channel, out, sizeof(out), &bytes, &xid, &type) == -1);
    assert(bus.live && !channel->live &&
           !amd64_vmbus_send(channel, payload, 24, 13, NULL, 0, 0, 0, true));
    reset(NORMAL);
    assert(amd64_vmbus_start() && (channel = amd64_vmbus_open(amd64_vmbus_offer(0))));
    reply(6, 20);
    assert(amd64_vmbus_receive(channel, out, sizeof(out), &bytes, &xid, &type) == -1);
    reset(NORMAL);
    assert(amd64_vmbus_start() && (channel = amd64_vmbus_open(amd64_vmbus_offer(0))));
    channel->out->read = 40;
    channel->out->write = 0;
    /* An outbound full ring has a bounded wait and permanently disables I/O. */
    assert(!amd64_vmbus_send(channel, payload, 24, 13, NULL, 0, 0, 0, true) && bus.live &&
           !channel->live);
    reset(MULTI);
    assert(amd64_vmbus_start() && amd64_vmbus_offer_count() == 9);
    channel = amd64_vmbus_open(amd64_vmbus_offer(0));
    struct vmbus_channel *keyboard = amd64_vmbus_open(amd64_vmbus_offer(1));
    assert(channel && keyboard && channel != keyboard && channel->rings != keyboard->rings);
    assert(!amd64_vmbus_open(amd64_vmbus_offer(0)));
    assert(amd64_vmbus_send(channel, payload, 24, 71, NULL, 0, 0, 0, true));
    assert(last_signal == 0x123 && channel->out->write == 48 && !keyboard->out->write);
    assert(amd64_vmbus_send(keyboard, payload, 24, 72, NULL, 0, 0, 0, false));
    assert(last_signal == 0x124 && keyboard->out->write == 48);
    assert(get16(channel->rings + 4096 + 6) == 1 && !get16(keyboard->rings + 4096 + 6));
    put64(packet + 8, 73);
    assert(vmbus_ring_write(keyboard->in, (uint8_t *)keyboard->in + 4096, RING_SIZE, packet, 40,
                            &notify) == 1);
    assert(amd64_vmbus_receive(channel, out, sizeof(out), &bytes, &xid, &type) == 0);
    assert(amd64_vmbus_receive(keyboard, out, sizeof(out), &bytes, &xid, &type) == 1 && xid == 73);
    reply(2, 12);
    put32(queue[tail - 1] + 8, 43);
    assert(amd64_vmbus_receive(channel, out, sizeof(out), &bytes, &xid, &type) == 0 && bus.live);
    assert(amd64_vmbus_receive(keyboard, out, sizeof(out), &bytes, &xid, &type) == -1);
    assert(amd64_vmbus_close(keyboard) && !closed[1] && channel->live);
    assert(!amd64_vmbus_send(keyboard, payload, 24, 74, NULL, 0, 0, 0, false));
    assert(amd64_vmbus_close(channel) && closed[0] == 1 && bus.live);
    amd64_vmbus_stop();
    assert(stopped == 1);

    reset(MULTI);
    assert(amd64_vmbus_start());
    channel = amd64_vmbus_open(amd64_vmbus_offer(0));
    keyboard = amd64_vmbus_open(amd64_vmbus_offer(1));
    rescind_during_close = true;
    assert(amd64_vmbus_close(channel) && bus.live && !keyboard->live);
    assert(amd64_vmbus_close(keyboard));
    amd64_vmbus_stop();
    assert(stopped == 1);

    reset(MULTI);
    assert(amd64_vmbus_start());
    struct vmbus_channel *channels[8];
    for (unsigned i = 0; i < 8; ++i) {
        channels[i] = amd64_vmbus_open(amd64_vmbus_offer(i));
        assert(channels[i]);
    }
    used = allocated;
    assert(!amd64_vmbus_open(amd64_vmbus_offer(8)) && allocated == used && bus.live);
    assert(amd64_vmbus_close(channels[3]));
    assert(amd64_vmbus_open(amd64_vmbus_offer(8)));
    amd64_vmbus_stop();
    assert(stopped == 1);
    puts("Hyper-V VMBus session tests passed");
    return 0;
}
