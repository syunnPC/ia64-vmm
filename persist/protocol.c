/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/User/Usb/{Bot,Scsi,Xhci}.cpp
 */
/* Codec subset; Apache-2.0 text is in LICENSES/Apache-2.0.txt. */
#include "persist/protocol.h"
#include "ia64vmm/string.h"
static uint32_t little(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(n >> (i * 8));
}
bool bot_cbw(uint8_t p[31], uint32_t tag, const uint8_t *cdb, unsigned n, uint32_t length, bool in)
{
    if (!cdb || !n || n > 16)
        return false;
    memset(p, 0, 31);
    put(p, 0x43425355);
    put(p + 4, tag);
    put(p + 8, length);
    p[12] = in ? 0x80 : 0;
    p[14] = n;
    memcpy(p + 15, cdb, n);
    return true;
}
enum bot_result bot_csw(const uint8_t *p, unsigned n, uint32_t tag, uint32_t length,
                        uint32_t actual)
{
    /* BOT §6.7: phase error ignores residue and requires reset recovery. */
    if (n != 13 || little(p) != 0x53425355 || little(p + 4) != tag || p[12] > 1)
        return BOT_RESET;
    uint32_t residue = little(p + 8);
    if (residue > length || length - residue > actual)
        return BOT_RESET;
    return p[12] ? BOT_FAILED : BOT_PASSED;
}
bool scsi_range(uint8_t cdb[10], bool write, uint64_t lba, unsigned blocks)
{
    if (!blocks || blocks > 65535 || lba > UINT32_MAX || blocks - 1 > UINT32_MAX - lba)
        return false;
    memset(cdb, 0, 10);
    cdb[0] = write ? 0x2a : 0x28;
    for (unsigned i = 0; i < 4; ++i)
        cdb[2 + i] = (uint8_t)(lba >> ((3 - i) * 8));
    cdb[7] = blocks >> 8;
    cdb[8] = blocks;
    return true;
}
/* USB 2.0 §9.6.3-§9.6.6, USB 3.2 §9.6.7 (bMaxBurst, no streams). */
bool usb_bot_interface(const uint8_t *b, unsigned total, struct usb_bot *u)
{
    memset(u, 0, sizeof(*u));
    if (total < 9 || b[0] < 9 || b[1] != 2 || (unsigned)(b[2] | b[3] << 8) != total)
        return false;
    u->configuration = b[5];
    bool match = false, selected = false;
    unsigned previous = 2;
    for (unsigned at = 0; at < total;) {
        const uint8_t *d = b + at;
        if (total - at < 2 || d[0] < 2 || d[0] > total - at)
            return false;
        if (d[1] == 4 && d[0] >= 9) {
            match = !selected && d[3] == 0 && d[5] == 8 && d[6] == 6 && d[7] == 0x50;
            if (match) {
                selected = true;
                u->interface = d[2];
            }
        } else if (d[1] == 5) {
            /* A companion describes the endpoint right before it. */
            previous = 2;
            if (match && d[0] >= 7 && (d[3] & 3) == 2) {
                unsigned which = d[2] & 0x80 ? 0 : 1;
                if (u->address[which] || !(d[2] & 15) || (d[2] & 0x70))
                    return false;
                u->address[which] = d[2];
                u->mps[which] = d[4] | d[5] << 8;
                previous = which;
                if (!u->mps[which] || u->mps[which] > 1024)
                    return false;
            }
        } else if (match && d[1] == 48 && d[0] >= 6 && previous < 2) {
            if (d[3] & 31 || d[2] > 15)
                return false;
            u->burst[previous] = d[2];
            previous = 2;
        }
        at += d[0];
    }
    return u->address[0] && u->address[1];
}
/* xHCI §4.9.2: withhold the first Cycle bit until the complete TD is
 * visible. A controller must never consume a partially constructed TD. */
void xhci_begin(struct xhci_ring *r)
{
    r->first = r->next;
    r->first_cycle = r->cycle;
    r->building = true;
}
void xhci_commit(struct xhci_ring *r)
{
    struct xhci_trb *t = &r->trbs[r->first];
    __atomic_store_n(&t->control, (t->control & ~1u) | r->first_cycle, __ATOMIC_RELEASE);
    r->building = false;
}
uint64_t xhci_push(struct xhci_ring *r, uint64_t parameter, uint32_t status, uint32_t control)
{
    struct xhci_trb *t = &r->trbs[r->next];
    uint64_t pa = r->physical + r->next * 16;
    t->parameter = parameter;
    t->status = status;
    __atomic_store_n(&t->control,
                     (control & ~1u) | (r->cycle ^ (r->building && r->next == r->first)),
                     __ATOMIC_RELEASE);
    if (++r->next == 255) {
        t = &r->trbs[255];
        t->parameter = r->physical;
        t->status = 0;
        /* xHCI §4.9.2: Link TRB Toggle Cycle at the segment boundary. */
        __atomic_store_n(&t->control, 6u << 10 | 2 | r->cycle, __ATOMIC_RELEASE);
        r->next = 0;
        r->cycle ^= 1;
    }
    return pa;
}
int xhci_completion(const struct xhci_trb *e, unsigned type, uint64_t pointer, unsigned slot,
                    unsigned endpoint, unsigned length)
{
    if ((e->control >> 10 & 63) != type || e->parameter != pointer || (e->control >> 24) != slot ||
        (type == 32 && (e->control >> 16 & 31) != endpoint))
        return -1;
    unsigned code = e->status >> 24, residue = e->status & 0xffffff;
    if (code == 6)
        return -2;
    if (code != 1 && !(type == 32 && code == 13))
        return -1;
    if (type == 33)
        return 0;
    return residue <= length ? (int)(length - residue) : -1;
}
static uint32_t crc_span(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i)
            crc = crc >> 1 ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return crc;
}
uint32_t plog_crc(const void *data, size_t size)
{
    return ~crc_span(UINT32_MAX, data, size);
}
uint32_t plog_crc_pair(const void *a, size_t an, const void *b, size_t bn)
{
    return ~crc_span(crc_span(UINT32_MAX, a, an), b, bn);
}

/* SPC MODE SENSE(6), SBC caching page 08h: require the complete declared
 * page before trusting WCE. DPOFUA advertises WRITE(10)'s FUA support. */
bool scsi_caching(const uint8_t *b, unsigned n, bool *wce, bool *fua)
{
    if (n < 4 || (unsigned)b[0] + 1 > n || b[0] < 3)
        return false;
    unsigned end = b[0] + 1, at = 4u + b[3];
    if (at > end)
        return false;
    *fua = (b[2] & 0x10) != 0;
    while (at < end) {
        if (end - at < 2 || (b[at] & 0x40) || b[at + 1] > end - at - 2)
            return false;
        if ((b[at] & 0x3f) == 8) {
            if (b[at + 1] < 18)
                return false;
            *wce = (b[at + 2] & 4) != 0;
            return true;
        }
        at += 2 + b[at + 1];
    }
    return false;
}
