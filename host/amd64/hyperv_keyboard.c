/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Hyper-V keyboard 1.0. Wire layout: Microsoft OpenVMM's MIT-licensed
 * hyperv_keyboard_protocol; messages stay in the channel's private rings. */
#include "host/amd64/hyperv_keyboard.h"
#include "host/amd64/vmbus.h"
#include "core/log_internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define KEYBOARD_PACKETS 64u
#define KEYBOARD_SECONDS 5u
static struct {
    struct vmbus_channel *channel;
    amd64_hyperv_key_callback callback;
    void *context;
    bool attempted, live, completion_seen;
} kb;
static const uint8_t keyboard_type[16] = {0x6d, 0xad, 0x12, 0xf9, 0x17, 0x2b, 0xea, 0x48,
                                          0xbd, 0x65, 0xf9, 0x27, 0xa6, 0x1c, 0x76, 0x84};
static uint16_t u16(const uint8_t *p)
{
    uint16_t n;
    memcpy(&n, p, 2);
    return n;
}
static uint32_t u32(const uint8_t *p)
{
    uint32_t n;
    memcpy(&n, p, 4);
    return n;
}
static bool owner(void)
{
    return kb.channel && hal_cpu_index() == 0 && !log_in_panic();
}
void amd64_hyperv_keyboard_stop(void)
{
    if (!owner())
        return;
    kb.live = false;
    (void)amd64_vmbus_close(kb.channel);
    kb.channel = NULL;
}
static bool failed(const char *reason)
{
    LOGW("Hyper-V keyboard unavailable: %s", reason);
    amd64_hyperv_keyboard_stop();
    return false;
}
/* 0: completion/event, 1: accepted, 2: rejected, -1: invalid. A protocol
 * response is independent of the optional VMBus send completion. */
static int message(const uint8_t *p, unsigned bytes, uint16_t type, uint64_t transaction,
                   bool negotiating)
{
    if (type == VMBUS_PACKET_COMPLETION) {
        if (transaction != 1 || bytes || kb.completion_seen)
            return -1;
        kb.completion_seen = true;
        return 0;
    }
    if (type != VMBUS_PACKET_INBAND || bytes < 4)
        return -1;
    switch (u32(p)) {
    case 2:
        if (!negotiating || bytes != 8 || (u32(p + 4) & ~1u))
            return -1;
        return u32(p + 4) & 1 ? 1 : 2;
    case 3: {
        /* Twelve protocol bytes plus four bytes of VMBus alignment. The
         * latter have no protocol meaning and need not contain zeros. */
        if (bytes != 16 || u16(p + 6))
            return -1;
        uint16_t code = u16(p + 4);
        uint32_t flags = u32(p + 8);
        if (flags & ~15u)
            return -1;
        /* Unicode cannot be represented as a raw PS/2 scan code. E0/E1
         * are mutually exclusive prefixes; break is carried separately. */
        if ((flags & 1) || (flags & 12) == 12 || !code || code > 0x7f)
            return 0;
        if (!negotiating)
            kb.callback(kb.context, code, (flags & 2) != 0, (flags & 4) != 0, (flags & 8) != 0);
        return 0;
    }
    default:
        return -1;
    }
}
bool amd64_hyperv_keyboard_start(amd64_hyperv_key_callback callback, void *context)
{
    if (!callback || hal_cpu_index() != 0 || log_in_panic())
        return false;
    if (kb.live)
        return kb.callback == callback && kb.context == context;
    if (kb.attempted || !hal_counter_hz())
        return false;
    kb.attempted = true;
    if (!amd64_vmbus_start())
        return false;
    const struct vmbus_offer *selected = NULL;
    for (unsigned i = 0; i < amd64_vmbus_offer_count(); ++i) {
        const struct vmbus_offer *o = amd64_vmbus_offer(i);
        if (!o || o->rescinded || o->subchannel || memcmp(o->type, keyboard_type, 16) != 0)
            continue;
        if (selected)
            return failed("multiple primary offers");
        selected = o;
    }
    if (!selected)
        return false;
    kb.channel = amd64_vmbus_open(selected);
    if (!kb.channel)
        return false;
    kb.callback = callback;
    kb.context = context;
    const uint32_t request[2] = {1, 0x10000};
    if (!amd64_vmbus_send(kb.channel, request, sizeof(request), 1, NULL, 0, 0, 0, false))
        return failed("protocol request failed");
    uint64_t start = hal_counter(), limit = hal_counter_hz() * KEYBOARD_SECONDS;
    do {
        if (!owner())
            return false;
        uint8_t packet[256];
        unsigned bytes;
        uint16_t type;
        uint64_t transaction;
        int status =
            amd64_vmbus_receive(kb.channel, packet, sizeof(packet), &bytes, &transaction, &type);
        if (status < 0)
            return failed("channel failed during negotiation");
        if (status) {
            int result = message(packet, bytes, type, transaction, true);
            if (result < 0)
                return failed("invalid negotiation packet");
            if (result == 2)
                return failed("protocol 1.0 rejected");
            if (result == 1) {
                kb.live = true;
                LOGI("Hyper-V keyboard protocol 1.0 ready");
                return true;
            }
        }
        __asm__ volatile("pause");
    } while (hal_counter() - start < limit);
    return failed("protocol response timeout");
}
void amd64_hyperv_keyboard_poll(void)
{
    for (unsigned i = 0; i < KEYBOARD_PACKETS && kb.live && owner(); ++i) {
        uint8_t packet[256];
        unsigned bytes;
        uint16_t type;
        uint64_t transaction;
        int status =
            amd64_vmbus_receive(kb.channel, packet, sizeof(packet), &bytes, &transaction, &type);
        if (!status)
            return;
        if (status < 0 || message(packet, bytes, type, transaction, false) < 0) {
            (void)failed("invalid input packet or channel removed");
            return;
        }
    }
    /* A nonempty ring does not generate another empty-to-nonempty IRQ.
     * Continue on the next management pass after exhausting this budget. */
    if (kb.live && owner())
        hal_cpu_request(0, HAL_CPU_WAKE);
}
