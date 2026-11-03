/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Independent byte-oriented model of the Microsoft OpenVMM (MIT)
 * hyperv_keyboard_protocol 1.0 request, response and keystroke layouts. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host/amd64/hyperv_keyboard.c"

struct vmbus_channel {
    unsigned id;
};
static struct vmbus_channel keyboard_channel = {2}, storage_channel = {1};
static struct vmbus_offer offers[3];
static unsigned offer_count, opens, closes, sends, receives, bus_stops, input_count, cpu_index,
    wakes;
static uint64_t ticks;
static bool bus_ok, open_ok, send_ok, receive_ok, panic;
enum {
    NORMAL,
    COMPLETION_FIRST,
    NO_COMPLETION,
    REJECT,
    TIMEOUT,
    SHORT_RESPONSE,
    RESERVED_RESPONSE,
    WRONG_COMPLETION_ID,
    NONEMPTY_COMPLETION,
    WRONG_RESPONSE_TYPE,
    EARLY_KEY
};
static unsigned mode;
struct packet {
    uint8_t data[256];
    unsigned bytes;
    uint16_t type;
    uint64_t xid;
};
static struct packet packets[256];
static unsigned head, tail;
static struct {
    uint16_t code;
    bool released, e0, e1;
} input[256];
static uint8_t context;
void vmm_log(enum log_level level, const char *format, ...)
{
    (void)level;
    (void)format;
}
bool log_in_panic(void)
{
    return panic;
}
unsigned hal_cpu_index(void)
{
    return cpu_index;
}
uint64_t hal_counter(void)
{
    return ++ticks;
}
uint64_t hal_counter_hz(void)
{
    return 10;
}
void hal_cpu_request(unsigned cpu_id, unsigned requests)
{
    assert(cpu_id == 0 && requests == HAL_CPU_WAKE);
    ++wakes;
}
bool amd64_vmbus_start(void)
{
    return bus_ok;
}
unsigned amd64_vmbus_offer_count(void)
{
    return offer_count;
}
const struct vmbus_offer *amd64_vmbus_offer(unsigned index)
{
    return offers + index;
}
struct vmbus_channel *amd64_vmbus_open(const struct vmbus_offer *offer)
{
    assert(offer == offers + 1);
    ++opens;
    return open_ok ? &keyboard_channel : NULL;
}
bool amd64_vmbus_close(struct vmbus_channel *channel)
{
    assert(channel == &keyboard_channel && channel != &storage_channel);
    ++closes;
    return true;
}
void amd64_vmbus_stop(void)
{
    ++bus_stops;
}
static struct packet *queue(unsigned bytes, uint16_t type, uint64_t xid)
{
    assert(tail < 256);
    struct packet *p = packets + tail++;
    memset(p, 0, sizeof(*p));
    p->bytes = bytes;
    p->type = type;
    p->xid = xid;
    return p;
}
static void key(uint16_t code, uint32_t flags)
{
    struct packet *p = queue(16, 6, 0);
    p->data[0] = 3;
    p->data[4] = code;
    p->data[5] = code >> 8;
    for (unsigned i = 0; i < 4; ++i)
        p->data[8 + i] = flags >> (i * 8);
    /* Alignment padding is not part of the protocol message. */
    memset(p->data + 12, 0xa5, 4);
}
bool amd64_vmbus_send(struct vmbus_channel *channel, const void *payload, unsigned bytes,
                      uint64_t xid, const uint64_t *pfns, unsigned pfn_count, unsigned offset,
                      unsigned data_bytes, bool completion)
{
    const uint8_t expected[] = {1, 0, 0, 0, 0, 0, 1, 0};
    assert(channel == &keyboard_channel && bytes == 8 && !memcmp(payload, expected, 8));
    assert(xid == 1 && !pfns && !pfn_count && !offset && !data_bytes && !completion);
    ++sends;
    if (!send_ok)
        return false;
    if (mode == TIMEOUT)
        return true;
    if (mode == COMPLETION_FIRST)
        queue(0, 11, 1);
    if (mode == EARLY_KEY)
        key(0x1e, 0);
    struct packet *p =
        queue(mode == SHORT_RESPONSE ? 4 : 8, mode == WRONG_RESPONSE_TYPE ? 9 : 6, 0);
    p->data[0] = 2;
    p->data[4] = mode == REJECT ? 0 : mode == RESERVED_RESPONSE ? 3 : 1;
    if (mode != COMPLETION_FIRST && mode != NO_COMPLETION) {
        struct packet *c =
            queue(mode == NONEMPTY_COMPLETION ? 8 : 0, 11, mode == WRONG_COMPLETION_ID ? 2 : 1);
        (void)c;
    }
    return true;
}
int amd64_vmbus_receive(struct vmbus_channel *channel, void *payload, unsigned capacity,
                        unsigned *bytes, uint64_t *xid, uint16_t *type)
{
    assert(channel == &keyboard_channel);
    ++receives;
    if (!receive_ok)
        return -1;
    if (head == tail)
        return 0;
    const struct packet *p = packets + head++;
    assert(p->bytes <= capacity);
    memcpy(payload, p->data, p->bytes);
    *bytes = p->bytes;
    *xid = p->xid;
    *type = p->type;
    return 1;
}
static void inject(void *arg, uint16_t code, bool released, bool e0, bool e1)
{
    assert(arg == &context && input_count < 256);
    input[input_count].code = code;
    input[input_count].released = released;
    input[input_count].e0 = e0;
    input[input_count].e1 = e1;
    ++input_count;
}
static void reset(void)
{
    memset(&kb, 0, sizeof(kb));
    memset(offers, 0, sizeof(offers));
    const uint8_t guid[] = {0x6d, 0xad, 0x12, 0xf9, 0x17, 0x2b, 0xea, 0x48,
                            0xbd, 0x65, 0xf9, 0x27, 0xa6, 0x1c, 0x76, 0x84};
    memcpy(offers[1].type, guid, 16);
    offers[1].relid = 7;
    offer_count = 2;
    opens = closes = sends = receives = bus_stops = input_count = cpu_index = wakes = 0;
    head = tail = 0;
    ticks = 0;
    mode = NORMAL;
    bus_ok = open_ok = send_ok = receive_ok = true;
    panic = false;
}
int main(void)
{
    reset();
    assert(!amd64_hyperv_keyboard_start(NULL, NULL) && !opens);
    reset();
    offer_count = 1;
    assert(!amd64_hyperv_keyboard_start(inject, &context) && !opens && !bus_stops);
    reset();
    offers[1].subchannel = 1;
    assert(!amd64_hyperv_keyboard_start(inject, &context) && !opens);
    reset();
    offers[1].rescinded = true;
    assert(!amd64_hyperv_keyboard_start(inject, &context) && !opens);
    reset();
    offer_count = 3;
    offers[2] = offers[1];
    assert(!amd64_hyperv_keyboard_start(inject, &context) && !opens);
    reset();
    cpu_index = 1;
    assert(!amd64_hyperv_keyboard_start(inject, &context) && !opens);
    reset();
    bus_ok = false;
    assert(!amd64_hyperv_keyboard_start(inject, &context) && !opens);
    reset();
    open_ok = false;
    assert(!amd64_hyperv_keyboard_start(inject, &context) && opens == 1 && !closes);
    reset();
    send_ok = false;
    assert(!amd64_hyperv_keyboard_start(inject, &context) && closes == 1 && !bus_stops);
    const unsigned errors[] = {REJECT, TIMEOUT, SHORT_RESPONSE, RESERVED_RESPONSE,
                               WRONG_RESPONSE_TYPE};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        reset();
        mode = errors[i];
        assert(!amd64_hyperv_keyboard_start(inject, &context));
        assert(closes == 1 && !bus_stops && !input_count);
    }
    const unsigned accepted[] = {NORMAL, COMPLETION_FIRST, NO_COMPLETION, EARLY_KEY};
    for (unsigned i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        reset();
        mode = accepted[i];
        assert(amd64_hyperv_keyboard_start(inject, &context) && !input_count);
        assert(amd64_hyperv_keyboard_start(inject, &context) && opens == 1 && sends == 1);
        key(0x1e, 0);
        key(0x1e, 2);
        key(0x1d, 4);
        key(0x45, 8);
        key(0x1d, 6);
        key(0x1234, 1);
        key(0x1e, 12);
        key(0x80, 0);
        key(0, 0);
        amd64_hyperv_keyboard_poll();
        assert(input_count == 5 && !closes);
        assert(input[0].code == 0x1e && !input[0].released && !input[0].e0 && !input[0].e1);
        assert(input[1].released && input[2].e0 && input[3].e1 && input[4].released && input[4].e0);
        amd64_hyperv_keyboard_stop();
        amd64_hyperv_keyboard_stop();
        assert(closes == 1 && !bus_stops);
        unsigned old = receives;
        amd64_hyperv_keyboard_poll();
        assert(receives == old);
    }
    reset();
    assert(amd64_hyperv_keyboard_start(inject, &context));
    amd64_hyperv_keyboard_poll();
    for (unsigned i = 0; i < 100; ++i)
        key(0x20, 0);
    amd64_hyperv_keyboard_poll();
    assert(input_count && input_count < 100 && wakes == 1);
    amd64_hyperv_keyboard_poll();
    amd64_hyperv_keyboard_poll();
    amd64_hyperv_keyboard_poll();
    assert(input_count == 100);
    for (unsigned broken = 0; broken < 7; ++broken) {
        reset();
        assert(amd64_hyperv_keyboard_start(inject, &context));
        amd64_hyperv_keyboard_poll();
        if (broken == 0) {
            key(0x1e, 0);
            packets[tail - 1].bytes = 8;
        }
        if (broken == 1) {
            key(0x1e, 0);
            packets[tail - 1].data[6] = 1;
        }
        if (broken == 2)
            key(0x1e, 16);
        if (broken == 3) {
            struct packet *p = queue(8, 6, 0);
            p->data[0] = 4;
        }
        if (broken == 4)
            queue(0, 11, 2);
        if (broken == 5)
            queue(8, 11, 1);
        if (broken == 6)
            receive_ok = false;
        amd64_hyperv_keyboard_poll();
        assert(closes == 1 && !input_count && !bus_stops);
    }
    for (unsigned pending = WRONG_COMPLETION_ID; pending <= NONEMPTY_COMPLETION; ++pending) {
        reset();
        mode = pending;
        assert(amd64_hyperv_keyboard_start(inject, &context));
        amd64_hyperv_keyboard_poll();
        assert(closes == 1 && !bus_stops);
    }
    reset();
    assert(amd64_hyperv_keyboard_start(inject, &context));
    panic = true;
    amd64_hyperv_keyboard_poll();
    amd64_hyperv_keyboard_stop();
    assert(!closes);
    puts("Hyper-V keyboard protocol tests passed");
}
