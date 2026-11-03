/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Polled VMBus subset. Wire definitions: Microsoft OpenVMM
 * vm/devices/vmbus/vmbus_core/src/protocol.rs (MIT); ring semantics:
 * docs.kernel.org/virt/hyperv/vmbus.html. No host memory is guest RAM. */
#include "host/amd64/vmbus.h"
#include "ia64vmm/string.h"

_Static_assert(sizeof(struct vmbus_packet) == 16, "VMBus packet descriptor");
_Static_assert(sizeof(struct vmbus_ring) == 4096, "VMBus ring header");
_Static_assert(offsetof(struct vmbus_ring, features) == 64, "VMBus ring features");
static inline uint16_t get16(const void *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static inline uint32_t get32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static void put16(void *p, uint16_t v)
{
    memcpy(p, &v, 2);
}
static void put32(void *p, uint32_t v)
{
    memcpy(p, &v, 4);
}
static inline void put64(void *p, uint64_t v)
{
    memcpy(p, &v, 8);
}
unsigned vmbus_gpadl_header(void *out, unsigned capacity, uint32_t relid, uint32_t handle,
                            const uint64_t *pfns, unsigned count)
{
    /* One range and no continuation: 20-byte header, 8-byte range header,
     * then PFNs, bounded by the 240-byte SynIC message payload. */
    if (!out || !pfns || !relid || !handle || !count || count > 26 || capacity < 28 + count * 8)
        return 0;
    for (unsigned i = 0; i < count; i++)
        if (pfns[i] >= (UINT64_C(1) << 52))
            return 0;
    uint8_t *p = out;
    unsigned bytes = 28 + count * 8;
    memset(p, 0, bytes);
    put32(p, 8);
    put32(p + 8, relid);
    put32(p + 12, handle);
    put16(p + 16, 8 + count * 8);
    put16(p + 18, 1);
    put32(p + 20, count * 4096);
    memcpy(p + 28, pfns, (size_t)count * 8);
    return bytes;
}
static unsigned occupied(unsigned read, unsigned write, unsigned size)
{
    return write >= read ? write - read : size - read + write;
}
static void ring_copy(void *data, unsigned size, unsigned index, void *buffer, unsigned bytes,
                      bool write)
{
    unsigned first = bytes < size - index ? bytes : size - index;
    if (write) {
        memcpy((uint8_t *)data + index, buffer, first);
        memcpy(data, (uint8_t *)buffer + first, bytes - first);
    } else {
        memcpy(buffer, (uint8_t *)data + index, first);
        memcpy((uint8_t *)buffer + first, data, bytes - first);
    }
}
int vmbus_ring_write(struct vmbus_ring *ring, void *data, unsigned size, const void *packet,
                     unsigned bytes, bool *notify)
{
    *notify = false;
    if (size < 32 || (size & 7) || !packet || bytes < 16 || (bytes & 7) || bytes > size - 8)
        return -1;
    unsigned write = __atomic_load_n(&ring->write, __ATOMIC_RELAXED);
    unsigned read = __atomic_load_n(&ring->read, __ATOMIC_ACQUIRE);
    if (write >= size || read >= size || ((write | read) & 7))
        return -1;
    if (size - occupied(read, write, size) <= bytes + 8)
        return 0;
    ring_copy(data, size, write, (void *)packet, bytes, true);
    uint64_t trailer = (uint64_t)write << 32;
    ring_copy(data, size, (write + bytes) % size, &trailer, 8, true);
    __atomic_store_n(&ring->write, (write + bytes + 8) % size, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&ring->interrupt_mask, __ATOMIC_ACQUIRE))
        *notify = write == __atomic_load_n(&ring->read, __ATOMIC_ACQUIRE);
    return 1;
}
int vmbus_ring_read(struct vmbus_ring *ring, const void *data, unsigned size, void *out,
                    unsigned capacity, unsigned *bytes, uint64_t *transaction, uint16_t *type,
                    bool *notify)
{
    *notify = false;
    *bytes = 0;
    if (size < 32 || (size & 7))
        return -1;
    unsigned read = __atomic_load_n(&ring->read, __ATOMIC_RELAXED);
    unsigned write = __atomic_load_n(&ring->write, __ATOMIC_ACQUIRE);
    if (write >= size || read >= size || ((write | read) & 7))
        return -1;
    unsigned available = occupied(read, write, size);
    if (!available)
        return 0;
    if (available < 24)
        return -1;
    struct vmbus_packet h;
    ring_copy((void *)data, size, read, &h, sizeof(h), false);
    unsigned length = h.length8 * 8u, offset = h.offset8 * 8u;
    if (offset < sizeof(h) || length < offset || length > available - 8 ||
        length - offset > capacity || (!out && length != offset))
        return -1;
    if (length != offset)
        ring_copy((void *)data, size, (read + offset) % size, out, length - offset, false);
    *bytes = length - offset;
    *transaction = h.transaction;
    *type = h.type;
    unsigned consumed = length + 8;
    __atomic_store_n(&ring->read, (read + consumed) % size, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    unsigned pending = __atomic_load_n(&ring->pending_send, __ATOMIC_ACQUIRE);
    write = __atomic_load_n(&ring->write, __ATOMIC_ACQUIRE);
    if (write >= size || (write & 7))
        return -1;
    unsigned free_now = size - occupied((read + consumed) % size, write, size);
    if (pending && pending < size && free_now > pending && free_now - consumed <= pending)
        *notify = true;
    return 1;
}

#if !defined(IA64VMM_VMBUS_WIRE_ONLY)
#include "host/amd64/amd64.h"
#include "host/amd64/hyperv.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "core/log_internal.h"

#define MAX_OFFERS 64u
#define RING_PAGES 3u
#define RING_SIZE ((RING_PAGES - 1) * 4096u)
#define CONTROL_SECONDS 5u
#define MAX_PACKET 1024u
#define MAX_CHANNELS 8u
enum {
    MSG_OFFER = 1,
    MSG_RESCIND = 2,
    MSG_REQUEST = 3,
    MSG_ALL_OFFERS = 4,
    MSG_OPEN = 5,
    MSG_OPEN_RESULT = 6,
    MSG_CLOSE = 7,
    MSG_GPADL_CREATED = 10,
    MSG_GPADL_TEARDOWN = 11,
    MSG_GPADL_TORNDOWN = 12,
    MSG_CONTACT = 14,
    MSG_VERSION = 15,
    MSG_UNLOAD = 16,
    MSG_UNLOADED = 17
};
struct vmbus_channel {
    uint32_t relid, connection, handle;
    uint8_t *rings;
    struct vmbus_ring *out, *in;
    bool allocated, gpadl, opened, live, dedicated, rescinded;
};
static struct {
    struct vmbus_offer offers[MAX_OFFERS];
    struct vmbus_channel channels[MAX_CHANNELS];
    unsigned count;
    uint32_t next_handle;
    uint8_t *interrupt, *monitor;
    bool live, connected, enumerated, used;
} bus;
static bool owner(void)
{
    return bus.live && hal_cpu_index() == 0 && !log_in_panic();
}
static bool fail(const char *reason)
{
    LOGE("VMBus %s", reason);
    bus.live = false; /* Host-visible pages remain reserved after any failure. */
    return false;
}
static bool channel_known(const struct vmbus_channel *channel)
{
    for (unsigned i = 0; i < MAX_CHANNELS; ++i)
        if (channel == bus.channels + i)
            return channel->allocated;
    return false;
}
static bool channel_ready(const struct vmbus_channel *channel)
{
    return owner() && channel_known(channel) && channel->opened && channel->live &&
           !channel->rescinded;
}
static bool channel_fail(struct vmbus_channel *channel, const char *reason)
{
    LOGE("VMBus channel=%u %s", channel->relid, reason);
    channel->live = false; /* Keep this channel's host-visible pages pinned. */
    return false;
}
static uint64_t deadline(void)
{
    return hal_counter() + hal_counter_hz() * CONTROL_SECONDS;
}
static bool expired(uint64_t end)
{
    return (int64_t)(hal_counter() - end) >= 0;
}
static bool physical(const void *p, uint64_t *pa)
{
    return amd64_memory_physical((uintptr_t)p, pa) && !(*pa & 4095);
}
static bool post(const void *message, unsigned bytes)
{
    uint64_t end = deadline();
    do {
        uint16_t status = amd64_hyperv_post(1, message, bytes);
        if (!status)
            return true;
        /* Resource shortages may be transient. VMBus also documents a
         * transient invalid connection (18) when control posts are rapid;
         * a contact rejection remains terminal. */
        if (status != 11 && status != 19 && !(status == 18 && get32(message) != MSG_CONTACT))
            return fail("post rejected");
        if (!owner())
            return false;
        uint64_t retry = hal_counter() + hal_counter_hz() / 1000 + 1;
        do {
            if (expired(end))
                return fail("post timeout");
            __asm__ volatile("pause");
        } while (!expired(retry));
    } while (!expired(end));
    return fail("post timeout");
}
static bool signal(struct vmbus_channel *channel)
{
    if (!channel->dedicated) {
        uint32_t *word = (uint32_t *)(bus.interrupt + 2048) + channel->relid / 32;
        __atomic_fetch_or(word, 1u << (channel->relid & 31), __ATOMIC_RELEASE);
    }
    return !amd64_hyperv_signal(channel->connection) || channel_fail(channel, "signal rejected");
}
/* Returns true for asynchronously handled messages, false for replies. */
static bool asynchronous(const uint8_t *p, unsigned bytes, bool *handled)
{
    uint32_t type = get32(p);
    *handled = true;
    if (type == MSG_OFFER) {
        if (bytes < 196 || bus.count == MAX_OFFERS)
            return fail("invalid channel offer");
        uint32_t relid = get32(p + 184), connection = get32(p + 192);
        if (!relid || relid >= 2048 || !connection)
            return fail("invalid channel identifier");
        for (unsigned i = 0; i < bus.count; i++)
            if (bus.offers[i].relid == relid)
                return fail("duplicate channel offer");
        struct vmbus_offer *o = &bus.offers[bus.count++];
        memcpy(o->type, p + 8, 16);
        memcpy(o->instance, p + 24, 16);
        o->flags = get16(p + 56);
        o->subchannel = get16(p + 180);
        o->relid = relid;
        o->connection_id = connection;
        o->dedicated = get16(p + 190) & 1;
        return true;
    }
    if (type == MSG_RESCIND) {
        if (bytes < 12)
            return fail("truncated rescind");
        uint32_t relid = get32(p + 8);
        for (unsigned i = 0; i < bus.count; i++)
            if (bus.offers[i].relid == relid) {
                bus.offers[i].rescinded = true;
                for (unsigned j = 0; j < MAX_CHANNELS; ++j) {
                    struct vmbus_channel *channel = bus.channels + j;
                    if (channel->allocated && channel->relid == relid) {
                        channel->rescinded = true;
                        channel->live = false;
                    }
                }
                return true;
            }
        return fail("unknown channel rescinded");
    }
    *handled = false;
    return true;
}
static int next(uint8_t p[240], unsigned *bytes)
{
    amd64_hyperv_events();
    int status = amd64_hyperv_message(p, 240, bytes);
    if (status < 0 || (status && *bytes < 8)) {
        fail("invalid control message");
        return -1;
    }
    return status;
}
static bool wait_message(uint32_t type, uint8_t p[240], unsigned *bytes)
{
    uint64_t end = deadline();
    do {
        if (!owner())
            return false;
        int status = next(p, bytes);
        if (status < 0)
            return false;
        if (status) {
            bool handled;
            if (!asynchronous(p, *bytes, &handled))
                return false;
            if (handled)
                continue;
            if (get32(p) != type)
                return fail("unexpected control reply");
            return true;
        }
        __asm__ volatile("pause");
    } while (!expired(end));
    return fail("control timeout");
}
static bool poll_control(void)
{
    for (unsigned i = 0; i < MAX_OFFERS; ++i) {
        uint8_t p[240];
        unsigned bytes;
        int status = next(p, &bytes);
        if (status < 0)
            return false;
        if (!status)
            return true;
        bool handled;
        if (!asynchronous(p, bytes, &handled))
            return false;
        if (!handled)
            return fail("unexpected control reply");
    }
    return true;
}
bool amd64_vmbus_start(void)
{
    if (bus.live)
        return owner();
    if (hal_cpu_index() != 0 || bus.used || !hal_counter_hz() || !amd64_hyperv_start())
        return false;
    bus.used = true;
    bus.live = true;
    bus.interrupt = hal_alloc_pages(1, HAL_PAGE_ZERO);
    bus.monitor = hal_alloc_pages(2, HAL_PAGE_ZERO);
    uint64_t interrupt_pa, monitor_pa, monitor2_pa;
    if (!bus.interrupt || !bus.monitor || !physical(bus.interrupt, &interrupt_pa) ||
        !physical(bus.monitor, &monitor_pa) || !physical(bus.monitor + 4096, &monitor2_pa))
        return fail("control page allocation failed");
    uint8_t p[240];
    unsigned bytes;
    /* Version 4.0 keeps the original control connection ID and event page.
     * 3.0 also supports Gen2 guests and targets their management VP. */
    const uint32_t versions[] = {0x40000, 0x30000};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(versions); i++) {
        memset(p, 0, 40);
        put32(p, MSG_CONTACT);
        put32(p + 8, versions[i]);
        put32(p + 12, amd64_hyperv_vp());
        put64(p + 16, interrupt_pa);
        put64(p + 24, monitor_pa);
        put64(p + 32, monitor2_pa);
        if (!post(p, 40) || !wait_message(MSG_VERSION, p, &bytes))
            return false;
        if (bytes < 16)
            return fail("truncated version response");
        if (p[8]) {
            if (p[9])
                return fail("host connection failed");
            bus.connected = true;
            break;
        }
    }
    if (!bus.connected)
        return fail("unsupported bus version");
    memset(p, 0, 8);
    put32(p, MSG_REQUEST);
    if (!post(p, 8) || !wait_message(MSG_ALL_OFFERS, p, &bytes))
        return false;
    bus.enumerated = true;
    LOGI("VMBus connected offers=%u VP=%u", bus.count, amd64_hyperv_vp());
    return true;
}
unsigned amd64_vmbus_offer_count(void)
{
    return owner() && bus.enumerated ? bus.count : 0;
}
const struct vmbus_offer *amd64_vmbus_offer(unsigned index)
{
    return owner() && bus.enumerated && index < bus.count ? &bus.offers[index] : NULL;
}
struct vmbus_channel *amd64_vmbus_open(const struct vmbus_offer *offer)
{
    if (!owner() || !bus.connected || !bus.enumerated || !offer || !poll_control())
        return NULL;
    bool found = false;
    for (unsigned i = 0; i < bus.count; i++)
        if (offer == &bus.offers[i])
            found = true;
    /* Confidential ring/external-memory offers require sharing transitions
     * that this conventional Gen2 transport does not implement. */
    if (!found || offer->subchannel || offer->rescinded || (offer->flags & 6))
        return NULL;
    struct vmbus_channel *channel = NULL;
    for (unsigned i = 0; i < MAX_CHANNELS; ++i) {
        if (bus.channels[i].allocated && bus.channels[i].relid == offer->relid)
            return NULL;
        if (!bus.channels[i].allocated && !channel)
            channel = bus.channels + i;
    }
    if (!channel)
        return NULL;
    memset(channel, 0, sizeof(*channel));
    channel->allocated = true;
    channel->rings = hal_alloc_pages((size_t)RING_PAGES * 2, HAL_PAGE_ZERO);
    if (!channel->rings) {
        fail("ring allocation failed");
        return NULL;
    }
    uint64_t pfns[RING_PAGES * 2];
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(pfns); i++) {
        uint64_t pa;
        if (!physical(channel->rings + (size_t)i * 4096, &pa)) {
            fail("ring page unmapped");
            return NULL;
        }
        pfns[i] = pa >> 12;
    }
    channel->relid = offer->relid;
    channel->connection = offer->connection_id;
    channel->dedicated = offer->dedicated;
    channel->handle = ++bus.next_handle;
    if (!channel->handle) {
        fail("GPADL handles exhausted");
        return NULL;
    }
    channel->out = (struct vmbus_ring *)channel->rings;
    channel->in = (struct vmbus_ring *)(channel->rings + (size_t)RING_PAGES * 4096);
    /* Polling flow control: do not advertise pending_send support. Inbound
     * pending_send is still honored when releasing host ring space. */
    uint8_t p[240];
    unsigned bytes;
    bytes = vmbus_gpadl_header(p, sizeof(p), channel->relid, channel->handle, pfns,
                               IA64VMM_ARRAY_SIZE(pfns));
    if (!bytes || !post(p, bytes) || !wait_message(MSG_GPADL_CREATED, p, &bytes))
        return NULL;
    if (bytes < 20 || get32(p + 8) != channel->relid || get32(p + 12) != channel->handle) {
        fail("unexpected GPADL creation reply");
        return NULL;
    }
    if (get32(p + 16)) {
        channel_fail(channel, "GPADL creation rejected");
        channel->allocated = false;
        return NULL;
    }
    channel->gpadl = true;
    if (channel->rescinded) {
        (void)amd64_vmbus_close(channel);
        return NULL;
    }
    memset(p, 0, 148);
    put32(p, MSG_OPEN);
    put32(p + 8, channel->relid);
    put32(p + 12, channel->handle);
    put32(p + 16, channel->handle);
    put32(p + 20, amd64_hyperv_vp());
    put32(p + 24, RING_PAGES);
    if (!post(p, 148) || !wait_message(MSG_OPEN_RESULT, p, &bytes))
        return NULL;
    if (bytes < 20 || get32(p + 8) != channel->relid || get32(p + 12) != channel->handle) {
        fail("unexpected channel open reply");
        return NULL;
    }
    if (get32(p + 16)) {
        channel_fail(channel, "open rejected");
        (void)amd64_vmbus_close(channel);
        return NULL;
    }
    channel->opened = channel->live = true;
    if (channel->rescinded) {
        channel->live = false;
        (void)amd64_vmbus_close(channel);
        return NULL;
    }
    return channel;
}
bool amd64_vmbus_close(struct vmbus_channel *channel)
{
    if (!owner() || !channel_known(channel) || !channel->gpadl)
        return false;
    uint8_t p[240] = {0};
    unsigned bytes;
    put32(p, MSG_CLOSE);
    put32(p + 8, channel->relid);
    if (channel->opened && !channel->rescinded && !post(p, 12))
        return false;
    memset(p, 0, 16);
    put32(p, MSG_GPADL_TEARDOWN);
    put32(p + 8, channel->relid);
    put32(p + 12, channel->handle);
    if (!post(p, 16) || !wait_message(MSG_GPADL_TORNDOWN, p, &bytes))
        return false;
    if (bytes < 12 || get32(p + 8) != channel->handle)
        return fail("unexpected GPADL teardown");
    channel->allocated = channel->gpadl = channel->opened = channel->live = false;
    /* Keep pages reserved even after confirmed teardown. */
    channel->rings = NULL;
    channel->out = channel->in = NULL;
    return true;
}
bool amd64_vmbus_send(struct vmbus_channel *channel, const void *payload, unsigned bytes,
                      uint64_t transaction, const uint64_t *pfns, unsigned pfn_count,
                      unsigned data_offset, unsigned data_bytes, bool completion)
{
    if (!channel_ready(channel) || !payload || !bytes || pfn_count > VMBUS_MAX_PFNS ||
        data_offset >= 4096 || (!pfn_count && (data_offset || data_bytes)) ||
        (pfn_count && (!pfns || !data_bytes || data_bytes > pfn_count * 4096 - data_offset ||
                       (data_offset + data_bytes + 4095) / 4096 != pfn_count)))
        return false;
    if (!poll_control() || !channel_ready(channel))
        return false;
    unsigned header = pfn_count ? 32 + pfn_count * 8 : 16;
    if (bytes > MAX_PACKET - header)
        return false;
    for (unsigned i = 0; i < pfn_count; i++)
        if (pfns[i] >= (UINT64_C(1) << 52))
            return false;
    unsigned total = (header + bytes + 7) & ~7u;
    uint8_t p[MAX_PACKET] = {0};
    put16(p, pfn_count ? 9 : VMBUS_PACKET_INBAND);
    put16(p + 2, header / 8);
    put16(p + 4, total / 8);
    put16(p + 6, completion ? 1 : 0);
    put64(p + 8, transaction);
    if (pfn_count) {
        put32(p + 20, 1);
        put32(p + 24, data_bytes);
        put32(p + 28, data_offset);
        memcpy(p + 32, pfns, (size_t)pfn_count * 8);
    }
    memcpy(p + header, payload, bytes);
    uint64_t end = deadline();
    do {
        bool notify;
        int result =
            vmbus_ring_write(channel->out, channel->rings + 4096, RING_SIZE, p, total, &notify);
        if (result < 0)
            return channel_fail(channel, "invalid outbound ring");
        if (result)
            return !notify || signal(channel);
        if (!poll_control() || !channel_ready(channel))
            return false;
        __asm__ volatile("pause");
    } while (!expired(end));
    return channel_fail(channel, "outbound ring timeout");
}
int amd64_vmbus_receive(struct vmbus_channel *channel, void *payload, unsigned capacity,
                        unsigned *bytes, uint64_t *transaction, uint16_t *type)
{
    if (!channel_ready(channel) || !payload || !bytes || !transaction || !type)
        return -1;
    if (!poll_control() || !channel_ready(channel))
        return -1;
    bool notify;
    int result = vmbus_ring_read(channel->in, (uint8_t *)channel->in + 4096, RING_SIZE, payload,
                                 capacity, bytes, transaction, type, &notify);
    if (result < 0) {
        channel_fail(channel, "invalid inbound packet");
        return -1;
    }
    if (notify && !signal(channel))
        return -1;
    return result;
}
void amd64_vmbus_stop(void)
{
    if (!owner() || !bus.connected)
        return;
    for (unsigned i = 0; i < MAX_CHANNELS; ++i)
        if (bus.channels[i].gpadl && !amd64_vmbus_close(bus.channels + i))
            return;
    uint8_t p[240] = {0};
    unsigned bytes;
    put32(p, MSG_UNLOAD);
    if (!post(p, 8) || !wait_message(MSG_UNLOADED, p, &bytes))
        return;
    bus.connected = false;
    bus.live = false;
    amd64_hyperv_stop();
}
#endif
