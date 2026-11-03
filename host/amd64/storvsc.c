/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Polled Hyper-V synthetic SCSI: primary channel, one command outstanding.
 * Wire layout follows Microsoft OpenVMM storvsp_protocol (MIT), versions
 * 6.2 and 6.0. SCSI CDBs follow SPC/SBC. All DMA uses private VMM pages. */
#include "host/amd64/amd64.h"
#include "host/amd64/storvsc.h"
#include "host/amd64/vmbus.h"
#include "core/log_internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define STOR_PAGES 4u
#define STOR_BYTES (STOR_PAGES * 4096u)
#define STOR_DEVICES 512u
#define STOR_SECONDS 30u
struct stor_srb {
    uint16_t length;
    uint8_t srb_status, scsi_status, port, path, target, lun;
    uint8_t cdb_length, sense_length, direction, reserved;
    uint32_t transfer;
    uint8_t payload[20];
    uint16_t reserved2;
    uint8_t queue_tag, queue_action;
    uint32_t flags, timeout, sort;
} IA64VMM_PACKED;
struct stor_packet {
    uint32_t operation, flags, status;
    union {
        struct stor_srb srb;
        uint8_t payload[52];
        struct {
            uint32_t reserved;
            uint16_t channels, reserved2;
            uint32_t flags, transfer;
            uint64_t reserved3;
        } IA64VMM_PACKED properties;
        struct {
            uint16_t version, revision;
        } IA64VMM_PACKED protocol;
    } u;
} IA64VMM_PACKED;
_Static_assert(sizeof(struct stor_srb) == 52, "StorVSC 6.x SRB size");
_Static_assert(sizeof(struct stor_packet) == 64, "StorVSC 6.x packet size");
_Static_assert(offsetof(struct stor_packet, u.srb.payload) == 28, "StorVSC CDB offset");
_Static_assert(offsetof(struct stor_packet, u.properties.transfer) == 24,
               "StorVSC properties offset");

static struct {
    bool attempted, live;
    uint8_t *data;
    struct vmbus_channel *channel;
    uint64_t pfns[STOR_PAGES], transaction;
    uint32_t max_transfer;
    uint16_t version;
    unsigned count;
    struct amd64_storvsc_device devices[STOR_DEVICES];
} sv;
static const uint8_t scsi_type[16] = {0xd9, 0x63, 0x61, 0xba, 0xa1, 0x04, 0x29, 0x4d,
                                      0xb6, 0x05, 0x72, 0xe2, 0xff, 0xb1, 0xdc, 0x7f};
static uint64_t be(const uint8_t *p, unsigned bytes)
{
    uint64_t value = 0;
    while (bytes--)
        value = value << 8 | *p++;
    return value;
}
static void put_be(uint8_t *p, uint64_t value, unsigned bytes)
{
    while (bytes) {
        p[--bytes] = value;
        value >>= 8;
    }
}
static bool available(void)
{
    return sv.live && hal_cpu_index() == 0 && !log_in_panic();
}
static bool failed(const char *why)
{
    LOGE("StorVSC channel stopped: %s", why);
    sv.live = false;
    /* The host may still hold DMA references. Neither rings nor data pages
     * are freed or reused, and start cannot create a second session. */
    if (sv.channel)
        (void)amd64_vmbus_close(sv.channel);
    sv.channel = NULL;
    return false;
}
/* Only transport framing is checked here; initialization needs the returned
 * NTSTATUS to distinguish version rejection from a broken channel. */
static bool exchange(struct stor_packet *packet, unsigned bytes)
{
    if (!available() || bytes > sv.max_transfer || sv.transaction == UINT64_MAX)
        return false;
    uint64_t transaction = ++sv.transaction;
    packet->flags = 1;
    unsigned pages = (bytes + 4095) / 4096;
    if (!amd64_vmbus_send(sv.channel, packet, sizeof(*packet), transaction, bytes ? sv.pfns : NULL,
                          pages, 0, bytes, true))
        return failed("send failed");
    uint64_t start = hal_counter(), limit = hal_counter_hz() * STOR_SECONDS;
    do {
        if (log_in_panic())
            return failed("panic during command");
        uint8_t response[128];
        unsigned length;
        uint16_t type;
        uint64_t id;
        int got = amd64_vmbus_receive(sv.channel, response, sizeof(response), &length, &id, &type);
        if (got < 0)
            return failed("receive failed");
        if (got) {
            if (length != sizeof(*packet) || type != VMBUS_PACKET_COMPLETION || id != transaction)
                return failed("invalid completion framing or transaction");
            memcpy(packet, response, sizeof(*packet));
            if (packet->operation != 1)
                return failed("invalid completion operation");
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            return true;
        }
        __asm__ volatile("pause");
    } while (hal_counter() - start < limit);
    return failed("command timeout; DMA pages retained");
}
/* SPC fixed (70h/71h) and descriptor (72h/73h) sense. Never inspect bytes
 * beyond the returned length or the sense data's additional-length field. */
static bool sense(const struct stor_srb *s, unsigned *key, unsigned *asc, unsigned *ascq)
{
    unsigned n = s->sense_length;
    if (!(s->srb_status & 0x80) || n > sizeof(s->payload) || n < 8)
        return false;
    const uint8_t *p = s->payload;
    unsigned format = p[0] & 0x7f;
    if (format == 0x70 || format == 0x71) {
        if (n < 14 || p[7] < 6)
            return false;
        *key = p[2] & 15;
        *asc = p[12];
        *ascq = p[13];
    } else if (format == 0x72 || format == 0x73) {
        *key = p[1] & 15;
        *asc = p[2];
        *ascq = p[3];
    } else
        return false;
    return true;
}
static bool scsi(unsigned target, unsigned lun, const uint8_t *cdb, unsigned cdb_bytes,
                 unsigned bytes, unsigned direction, unsigned *actual)
{
    if (!available() || cdb_bytes > 16 || bytes > sv.max_transfer)
        return false;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        struct stor_packet p = {.operation = 3};
        struct stor_srb *s = &p.u.srb;
        s->length = sizeof(*s);
        s->target = target;
        s->lun = lun;
        s->cdb_length = cdb_bytes;
        s->sense_length = sizeof(s->payload);
        s->direction = direction;
        s->transfer = bytes;
        s->flags = direction == 1 ? 0x40 : direction == 0 ? 0x80 : 0;
        s->timeout = STOR_SECONDS;
        memcpy(s->payload, cdb, cdb_bytes);
        if (!exchange(&p, bytes))
            return false;
        if (s->length != sizeof(*s) || s->transfer > bytes ||
            s->sense_length > sizeof(s->payload)) {
            LOGE("StorVSC malformed SRB completion");
            return false;
        }
        if (!p.status && s->srb_status == 1 && !s->scsi_status) {
            if (actual)
                *actual = s->transfer;
            return true;
        }
        unsigned key = 0xff, asc = 0, ascq = 0;
        bool have_sense = s->scsi_status == 2 && sense(s, &key, &asc, &ascq);
        LOGE("StorVSC target=%u lun=%u opcode=%x status=%x SRB=%x SCSI=%x sense=%x/%x/%x", target,
             lun, cdb[0], p.status, s->srb_status, s->scsi_status, key, asc, ascq);
        /* UNIT ATTENTION and becoming-ready can precede initial capacity
         * queries. A changed medium after discovery invalidates its identity. */
        bool discovery =
            cdb[0] == 0x12 || cdb[0] == 0x1a || cdb[0] == 0x25 || cdb[0] == 0x9e || cdb[0] == 0xa0;
        if (have_sense && key == 6 && !discovery)
            return failed("medium changed after discovery");
        if (!discovery)
            return false;
        if (!have_sense || (key != 6 && !(key == 2 && asc == 4 && ascq == 1)))
            return false;
        uint64_t start = hal_counter(), delay = hal_counter_hz() / 10;
        while (hal_counter() - start < delay)
            __asm__ volatile("pause");
    }
    return false;
}
static bool exact(unsigned target, unsigned lun, const uint8_t *cdb, unsigned cdb_bytes,
                  unsigned bytes, unsigned direction)
{
    unsigned actual;
    return scsi(target, lun, cdb, cdb_bytes, bytes, direction, &actual) && actual == bytes;
}
static void offer_id(const struct vmbus_offer *o)
{
    const uint8_t *g = o->instance;
    LOGE("StorVSC controller %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
         g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13],
         g[14], g[15]);
}
bool amd64_storvsc_start(const uint8_t instance[16])
{
    if (sv.attempted || hal_cpu_index() != 0 || log_in_panic())
        return false;
    sv.attempted = true;
    if (!amd64_vmbus_start())
        return false;
    const struct vmbus_offer *selected = NULL;
    unsigned matches = 0;
    for (unsigned i = 0; i < amd64_vmbus_offer_count(); ++i) {
        const struct vmbus_offer *o = amd64_vmbus_offer(i);
        if (!o || o->rescinded || o->subchannel || memcmp(o->type, scsi_type, 16) != 0)
            continue;
        if (instance && memcmp(o->instance, instance, 16) != 0)
            continue;
        selected = o;
        ++matches;
    }
    if (matches != 1) {
        LOGE("StorVSC needs exactly one matching primary controller; found %u", matches);
        for (unsigned i = 0; i < amd64_vmbus_offer_count(); ++i) {
            const struct vmbus_offer *o = amd64_vmbus_offer(i);
            if (o && !o->subchannel && !memcmp(o->type, scsi_type, 16))
                offer_id(o);
        }
        return false;
    }
    sv.data = hal_alloc_pages(STOR_PAGES, HAL_PAGE_ZERO);
    if (!sv.data)
        return false;
    for (unsigned i = 0; i < STOR_PAGES; ++i) {
        uint64_t pa;
        if (!amd64_memory_physical((uintptr_t)sv.data + (uintptr_t)i * 4096, &pa) || (pa & 4095))
            return false;
        sv.pfns[i] = pa >> 12;
    }
    sv.channel = amd64_vmbus_open(selected);
    if (!sv.channel)
        return false;
    sv.live = true;
    sv.max_transfer = STOR_BYTES;
    struct stor_packet p = {.operation = 7};
    if (!exchange(&p, 0) || p.status)
        goto fail;
    const uint16_t versions[] = {0x602, 0x600};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(versions); ++i) {
        memset(&p, 0, sizeof(p));
        p.operation = 9;
        p.u.protocol.version = versions[i];
        if (!exchange(&p, 0))
            goto fail;
        if (!p.status) {
            sv.version = versions[i];
            break;
        }
    }
    if (!sv.version)
        goto fail;
    memset(&p, 0, sizeof(p));
    p.operation = 10;
    if (!exchange(&p, 0) || p.status || p.u.properties.transfer < 512)
        goto fail;
    if (p.u.properties.transfer < sv.max_transfer)
        sv.max_transfer = p.u.properties.transfer;
    memset(&p, 0, sizeof(p));
    p.operation = 8;
    if (!exchange(&p, 0) || p.status)
        goto fail;
    LOGI("StorVSC protocol=%x max_transfer=%u", sv.version, sv.max_transfer);
    return true;
fail:
    if (sv.live)
        failed("initialization failed");
    return false;
}
unsigned amd64_storvsc_count(void)
{
    return sv.live ? sv.count : 0;
}
const struct amd64_storvsc_device *amd64_storvsc_device(unsigned index)
{
    return available() && index < sv.count ? &sv.devices[index] : NULL;
}
const struct amd64_storvsc_device *amd64_storvsc_find(unsigned target, unsigned lun)
{
    if (!available() || target >= 2 || lun >= 256)
        return NULL;
    for (unsigned i = 0; i < sv.count; ++i)
        if (sv.devices[i].target == target && sv.devices[i].lun == lun)
            return &sv.devices[i];
    if (sv.count == STOR_DEVICES)
        return NULL;
    uint8_t inquiry[36], cdb[16] = {0x12, 0, 0, 0, sizeof(inquiry)};
    if (!exact(target, lun, cdb, 6, sizeof(inquiry), 1))
        return NULL;
    memcpy(inquiry, sv.data, sizeof(inquiry));
    unsigned type = inquiry[0] & 31;
    if ((inquiry[0] & 0xe0) || (type != 0 && type != 5))
        return NULL;
    bool readonly = type == 5;
    if (!readonly) {
        /* SPC mode parameter header: WP describes write-protected disks.
         * Some hosts omit MODE SENSE; write completion still enforces WP. */
        uint8_t mode[6] = {0x1a, 8, 0x3f, 0, 4};
        unsigned actual;
        if (scsi(target, lun, mode, sizeof(mode), 4, 1, &actual) && actual >= 4 && sv.data[0] >= 3)
            readonly = (sv.data[2] & 0x80) != 0;
    }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    if (!exact(target, lun, cdb, 10, 8, 1))
        return NULL;
    uint64_t last = be(sv.data, 4);
    uint32_t sector = be(sv.data + 4, 4);
    if (last == UINT32_MAX) {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x9e;
        cdb[1] = 0x10;
        cdb[13] = 32;
        if (!exact(target, lun, cdb, 16, 32, 1))
            return NULL;
        last = be(sv.data, 8);
        sector = be(sv.data + 8, 4);
    }
    if (last == UINT64_MAX || (sector != 512 && sector != 2048 && sector != 4096) ||
        sector > sv.max_transfer || (type == 5 && sector != 2048))
        return NULL;
    struct amd64_storvsc_device *d = &sv.devices[sv.count++];
    *d = (struct amd64_storvsc_device){.blocks = last + 1,
                                       .sector_bytes = sector,
                                       .target = target,
                                       .lun = lun,
                                       .peripheral_type = type,
                                       .readonly = readonly};
    LOGI("StorVSC target=%u lun=%u blocks=%llu sector=%u readonly=%u", target, lun,
         (unsigned long long)d->blocks, sector, d->readonly);
    return d;
}
bool amd64_storvsc_enumerate(void)
{
    if (!available())
        return false;
    unsigned bytes = sv.max_transfer < 4096 ? sv.max_transfer : 4096;
    uint8_t cdb[12] = {0xa0}, luns[256];
    unsigned actual, count = 0;
    put_be(cdb + 6, bytes, 4);
    if (!scsi(0, 0, cdb, sizeof(cdb), bytes, 1, &actual))
        return false;
    if (actual < 8)
        return false;
    uint32_t list_bytes = be(sv.data, 4);
    if (list_bytes % 8 || list_bytes > actual - 8 || list_bytes / 8 > sizeof(luns))
        return false;
    for (unsigned i = 0; i < list_bytes; i += 8) {
        const uint8_t *entry = sv.data + 8 + i;
        /* Peripheral addressing, target's single 8-bit LUN namespace. */
        if (entry[0])
            continue;
        bool valid = true;
        for (unsigned j = 2; j < 8; ++j)
            if (entry[j])
                valid = false;
        if (valid)
            luns[count++] = entry[1];
    }
    for (unsigned i = 0; i < count; ++i) {
        (void)amd64_storvsc_find(0, luns[i]);
        if (!available())
            return false;
    }
    return true;
}
static bool known(const struct amd64_storvsc_device *d)
{
    if (!available() || !d)
        return false;
    for (unsigned i = 0; i < sv.count; ++i)
        if (d == &sv.devices[i])
            return true;
    return false;
}
static bool transfer(const struct amd64_storvsc_device *d, uint64_t lba, unsigned blocks,
                     uint8_t *data, bool write)
{
    if (!known(d) || (write && d->readonly) || lba > d->blocks || blocks > d->blocks - lba ||
        (blocks && !data) || (uint64_t)blocks * d->sector_bytes > SIZE_MAX)
        return false;
    while (blocks) {
        unsigned n = sv.max_transfer / d->sector_bytes;
        if (n > blocks)
            n = blocks;
        unsigned bytes = n * d->sector_bytes;
        uint8_t cdb[16] = {0};
        unsigned length;
        if (lba <= UINT32_MAX && n - 1 <= UINT32_MAX - lba) {
            cdb[0] = write ? 0x2a : 0x28;
            length = 10;
            put_be(cdb + 2, lba, 4);
            put_be(cdb + 7, n, 2);
        } else {
            cdb[0] = write ? 0x8a : 0x88;
            length = 16;
            put_be(cdb + 2, lba, 8);
            put_be(cdb + 10, n, 4);
        }
        if (write)
            memcpy(sv.data, data, bytes);
        if (!exact(d->target, d->lun, cdb, length, bytes, write ? 0 : 1))
            return false;
        if (!write)
            memcpy(data, sv.data, bytes);
        blocks -= n;
        lba += n;
        data += bytes;
    }
    return true;
}
bool amd64_storvsc_read(const struct amd64_storvsc_device *d, uint64_t lba, unsigned blocks,
                        void *data)
{
    return transfer(d, lba, blocks, data, false);
}
bool amd64_storvsc_write(const struct amd64_storvsc_device *d, uint64_t lba, unsigned blocks,
                         const void *data)
{
    return transfer(d, lba, blocks, (uint8_t *)data, true);
}
bool amd64_storvsc_flush(const struct amd64_storvsc_device *d)
{
    if (!known(d))
        return false;
    if (d->readonly)
        return true;
    uint8_t cdb[10] = {0x35};
    return exact(d->target, d->lun, cdb, sizeof(cdb), 0, 2);
}
void amd64_storvsc_stop(void)
{
    if (!sv.live || hal_cpu_index() != 0)
        return;
    sv.live = false;
    if (sv.channel)
        (void)amd64_vmbus_close(sv.channel);
    sv.channel = NULL;
}
