/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/User/Usb/Ehci.cpp
 */
/* Polled EHCI subset for the VMM medium: one high-speed BOT device on a
 * root port or behind one high-speed hub there (USB 2.0 ch. 11; a
 * high-speed device behind it needs no split transactions); the
 * asynchronous schedule holds a queue head for the default control pipe and
 * one per bulk endpoint and runs only while a transfer is queued (EHCI 1.0
 * §2.1.7, §2.3, §3.5, §3.6, §4.1, §4.2, §4.8, §4.10 and Appendix B). DMA
 * stays in VMM-owned RAM. Apache-2.0 text: LICENSES/Apache-2.0.txt. */
#include "host/amd64/amd64.h"
#include "host/amd64/plog_ehci.h"
#include "persist/protocol.h"
#include "core/log_internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#if !defined(IA64VMM_AMD64_HOST_ONLY)
#define PAGES 4u           /* data buffer: one 16 KiB qTD */
#define CONTROL_SECONDS 5u /* USB 2.0 §9.2.6.4 */
#define BULK_SECONDS 30u
enum {
    USBCMD = 0,
    USBSTS = 4,
    USBINTR = 8,
    CTRLDSSEGMENT = 0x10,
    ASYNCLISTADDR = 0x18,
    CONFIGFLAG = 0x40
};
enum { RS = 1, HCRESET = 2, PSE = 1u << 4, ASE = 1u << 5 };
enum { HSE = 1u << 4, HALTED = 1u << 12, ASS = 1u << 15 };
enum { CCS = 1, CSC = 2, PE = 4, PR = 0x100, PP = 0x1000, PO = 0x2000 };
/* Port writes carry only the read/write fields to keep (PE, PP, port
 * indicator, wake enables): no change bit is acknowledged, the port not
 * released (PO) and nothing suspended or reset by accident. */
#define KEEP(status) ((status) & (uint32_t)(PE | PP | 3u << 14 | 7u << 20))
enum {
    ACTIVE = 0x80,
    HALT = 0x40,
    ERRORS = 0x38
}; /* qTD status: data buffer, babble, transaction error */
/* schedule: queue heads at 0 (control), 80h (bulk IN), 100h (bulk OUT);
 * qTDs at 200h, 240h, 280h; the setup packet at 300h. */
enum { EMPTY, ATTACHED, RESETTING, KEPT }; /* port watch states */
/* Port scan results; DISABLED: connected, but not enabled by its reset. */
enum { NONE, LOW, FULL, HIGH, DISABLED, BROKEN };
/* Hub requests (USB 2.0 §11.24.2, Table 11-17 feature selectors) and the
 * port status, wPortChange in bits 16-31 (Tables 11-21 and 11-22). */
enum { PORT_RESET = 4, PORT_POWER = 8, C_PORT_CONNECTION = 16, C_PORT_RESET = 20 };
enum {
    CONNECTION = 1,
    ENABLE = 2,
    RESET = 0x10,
    LOW_SPEED = 0x200,
    HIGH_SPEED = 0x400,
    CONNECTION_CHANGE = 1u << 16,
    RESET_CHANGE = 0x10u << 16
};
#define MAX_HUB_PORTS 15u
static struct {
    uint8_t *mmio, *schedule, *data;
    uint64_t schedule_pa, data_pa[PAGES];
    unsigned op, ports, port, hub_port, interface, endpoints[2];
    unsigned
        full; /* root ports whose full-speed device goes to a companion once the medium works */
    bool live, running, wide, companions, watching;
    bool hub_refused; /* the hub scan found no single medium and logged why */
    struct {
        unsigned state;
        uint64_t at;
    } watch[16];
} eh;
static const char *who = "PLOG";
static bool quiet;
static struct plog_block block;
static uint32_t reg(unsigned off)
{
    return *(volatile uint32_t *)(eh.mmio + off);
}
static void wr(unsigned off, uint32_t value)
{
    *(volatile uint32_t *)(eh.mmio + off) = value;
}
static uint64_t physical(const void *p)
{
    uint64_t pa;
    if (!amd64_memory_physical((uintptr_t)p, &pa))
        hal_panic("PLOG DMA pointer unmapped");
    return pa;
}
static void pause_ms(unsigned ms)
{
    uint64_t end = hal_counter() + hal_counter_hz() / 1000 * ms;
    while (hal_counter() < end)
        __asm__ volatile("pause");
}
/* Bounded wait for (register & mask) == value; all-ones is a controller that is gone. */
static bool wait_reg(unsigned off, uint32_t mask, uint32_t value, unsigned ms)
{
    uint64_t end = hal_counter() + hal_counter_hz() / 1000 * ms;
    do {
        uint32_t n = reg(off);
        if (n == UINT32_MAX)
            return false;
        if ((n & mask) == value)
            return true;
        __asm__ volatile("pause");
    } while (hal_counter() < end);
    return false;
}
static uint32_t *qh(unsigned i)
{
    return (uint32_t *)(eh.schedule + i * 0x80);
}
static uint32_t *td(unsigned i)
{
    return (uint32_t *)(eh.schedule + 0x200 + i * 0x40);
}
static uint32_t link(unsigned offset)
{
    return (uint32_t)(eh.schedule_pa + offset);
}
/* §4.8: ASE changes only when it equals ASS. */
static bool schedule(bool on)
{
    uint32_t cmd = reg(eh.op + USBCMD);
    if (cmd == UINT32_MAX)
        return false;
    wr(eh.op + USBCMD, on ? cmd | ASE : cmd & ~ASE);
    return wait_reg(eh.op + USBSTS, ASS, on ? ASS : 0, 1000);
}
/* Run/Stop cleared until HCHalted (§2.3.1: within 16 microframes); the
 * schedule enables are cleared once halted. */
static bool halt(void)
{
    uint32_t cmd = reg(eh.op + USBCMD);
    if (cmd == UINT32_MAX)
        return false;
    wr(eh.op + USBCMD, cmd & ~RS);
    if (!wait_reg(eh.op + USBSTS, HALTED, HALTED, 1000))
        return false;
    wr(eh.op + USBCMD, reg(eh.op + USBCMD) & ~(ASE | PSE));
    return true;
}
/* The backend stops; DMA storage of an unfinished transfer is never reused.
 * The ports stay with this controller (CONFIGFLAG 1): the medium never
 * reaches a guest-owned companion controller. */
static void stop(void)
{
    eh.live = false;
    if (!halt())
        LOGE("%s EHCI did not halt USBSTS=%x", who, reg(eh.op + USBSTS));
    eh.running = false;
}
/* §3.5: a qTD with CERR 3 over the data pages (or the setup packet), the
 * 64-bit extended pointers of Appendix B; Active last. */
static void qtd(unsigned i, uint32_t next, uint32_t alternate, uint32_t token, unsigned bytes,
                bool setup)
{
    uint32_t *t = td(i);
    memset(t, 0, 0x40);
    t[0] = next;
    t[1] = alternate;
    if (setup) {
        uint64_t pa = eh.schedule_pa + 0x300;
        t[3] = (uint32_t)pa;
        t[8] = (uint32_t)(pa >> 32);
    } else
        for (unsigned p = 0; p < (bytes + 4095) / 4096; ++p) {
            t[3 + p] = (uint32_t)eh.data_pa[p];
            t[8 + p] = (uint32_t)(eh.data_pa[p] >> 32);
        }
    __atomic_store_n(&t[2], token | bytes << 16 | 3u << 10 | ACTIVE, __ATOMIC_RELEASE);
}
/* Queue the qTDs in mask, from qTD 0, on queue head q (§4.10.2: an
 * inactive overlay follows its Next qTD pointer; DTC 0 keeps the overlay's
 * data toggle) and run the schedule until every qTD retired or one halted.
 * Returns the bytes qTD data moved (0 without one), -2 for a STALL (Halted
 * alone), -1 otherwise. */
static int run(unsigned q, unsigned mask, unsigned data, unsigned length, unsigned seconds)
{
    if (!eh.live || log_in_panic())
        return -1;
    uint32_t *h = qh(q);
    h[3] = 0;
    h[4] = link(0x200);
    h[5] = 1;
    h[6] &= 1u << 31;
    for (unsigned i = 7; i < 17; ++i)
        h[i] = 0;
    __asm__ volatile("mfence" ::: "memory");
    if (!schedule(true)) {
        LOGE("%s EHCI asynchronous schedule did not start", who);
        stop();
        return -1;
    }
    uint64_t end = hal_counter() + hal_counter_hz() * seconds;
    do {
        if (log_in_panic()) {
            eh.live = false;
            return -1;
        }
        uint32_t status = reg(eh.op + USBSTS);
        if (status == UINT32_MAX) {
            eh.live = false;
            return -1;
        }
        if (status & (HSE | HALTED)) {
            LOGE("%s EHCI halted USBSTS=%x", who, status);
            stop();
            return -1;
        }
        int result = 0;
        bool done = true;
        for (unsigned i = 0; i < 3; ++i) {
            if (!(mask >> i & 1))
                continue;
            uint32_t token = __atomic_load_n(&td(i)[2], __ATOMIC_ACQUIRE);
            if (token & HALT) {
                result = token & ERRORS ? -1 : -2;
                break;
            }
            if (token & ACTIVE) {
                done = false;
                break;
            }
            if (i == data)
                result = (int)(length - (token >> 16 & 0x7fff));
        }
        if (result < 0 || done) {
            if (!schedule(false)) {
                LOGE("%s EHCI asynchronous schedule did not stop", who);
                stop();
                return -1;
            }
            if (result < 0)
                LOGE("%s EHCI queue head %u halted token=%x", who, q, h[6]);
            return result;
        }
        __asm__ volatile("pause");
    } while (hal_counter() < end);
    LOGE("%s EHCI transfer timeout queue head %u token=%x", who, q, h[6]);
    stop();
    return -1;
}
/* USB 2.0 §8.5.3: SETUP DATA0, data stage from DATA1, status DATA1 in the
 * other direction; after a short IN data stage the Next qTD pointer leads
 * to the status stage (§4.10.2, Alternate Next terminated). */
static int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, unsigned length)
{
    if (length > 4096)
        return -1;
    uint8_t *s = eh.schedule + 0x300;
    bool in = type & 0x80;
    s[0] = type;
    s[1] = request;
    s[2] = (uint8_t)value;
    s[3] = (uint8_t)(value >> 8);
    s[4] = (uint8_t)index;
    s[5] = (uint8_t)(index >> 8);
    s[6] = (uint8_t)length;
    s[7] = (uint8_t)(length >> 8);
    uint32_t status = link(0x280);
    qtd(2, 1, 1, (length && in ? 0 : 1u << 8) | 1u << 31, 0, false);
    if (length)
        qtd(1, status, 1, (in ? 1u << 8 : 0) | 1u << 31, length, false);
    qtd(0, length ? link(0x240) : status, 1, 2u << 8, 8, true);
    return run(0, length ? 7 : 5, length ? 1 : 3, length, CONTROL_SECONDS);
}
/* which: 0 bulk IN, 1 bulk OUT; one qTD of at most 16 KiB. */
static int bulk(unsigned which, unsigned length)
{
    qtd(0, 1, 1, which ? 0 : 1u << 8, length, false);
    return run(1 + which, 1, 0, length, BULK_SECONDS);
}
/* USB 2.0 §9.4.5: CLEAR_FEATURE(ENDPOINT_HALT) returns the pipe to DATA0. */
static bool clear_halt(unsigned which)
{
    if (control(2, 1, 0, (uint16_t)eh.endpoints[which], 0) != 0)
        return false;
    qh(1 + which)[6] = 0;
    return true;
}
/* BOT §3.1 Bulk-Only Mass Storage Reset, then both pipes (§5.3.4). */
static bool reset_bot(void)
{
    bool ok = eh.live && control(0x21, 0xff, 0, (uint16_t)eh.interface, 0) == 0 && clear_halt(0) &&
              clear_halt(1);
    if (!ok)
        eh.live = false;
    return ok;
}
static int bot_transfer(bool in, void *data, unsigned length)
{
    if (length > PAGES * 4096)
        return -1;
    if (!in)
        memcpy(eh.data, data, length);
    int n = bulk(in ? 0 : 1, length);
    if (in && n > 0)
        memcpy(data, eh.data, (unsigned)n);
    return n;
}
static const struct bot_io bot = {bot_transfer, clear_halt, reset_bot};
static struct scsi_disk disk = {.io = &bot, .wait = pause_ms};
static bool read_blocks(uint64_t lba, unsigned count, void *data)
{
    return scsi_disk_read(&disk, lba, count, data);
}
static bool write_blocks(uint64_t lba, unsigned count, const void *data)
{
    return scsi_disk_write(&disk, lba, count, data);
}
static bool flush(void)
{
    return eh.live && !log_in_panic() && scsi_disk_flush(&disk);
}
/* Queue heads (§3.6): high speed, the default pipe with DTC (toggles from
 * the qTDs), the bulk pipes with the toggle in the overlay; Mult 1. */
static void endpoint(unsigned i, unsigned address, unsigned number, unsigned packet, bool head)
{
    uint32_t *h = qh(i);
    h[1] = address | number << 8 | 2u << 12 | (i == 0 ? 1u << 14 : 0) | (head ? 1u << 15 : 0) |
           packet << 16;
    h[2] = 1u << 30;
}
/* USB 2.0 §9.1.2: the device in the Default state gets this address; its
 * device descriptor is then in the data page. */
static bool address(unsigned device)
{
    endpoint(0, 0, 0, 64, true);
    /* §9.6.1: a high-speed default pipe has 64-byte packets. */
    if (control(0x80, 6, 0x100, 0, 8) != 8 || eh.data[7] != 64) {
        LOGE("%s EHCI device descriptor unusable", who);
        return false;
    }
    if (control(0, 5, (uint16_t)device, 0, 0) != 0)
        return false;
    pause_ms(2); /* §9.2.6.3 SET_ADDRESS recovery */
    endpoint(0, device, 0, 64, true);
    return control(0x80, 6, 0x100, 0, 18) == 18 && eh.data[0] == 18 && eh.data[1] == 1;
}
static bool hub_init(void);
/* The device on the medium's root port at address 1; a hub (class 9)
 * there, and the high-speed device behind it at address 2. */
static bool enumerate(void)
{
    unsigned device = 1;
    if (!address(device))
        return false;
    if (eh.data[4] == 9) {
        if (!hub_init() || !address(++device))
            return false;
        if (eh.data[4] == 9) {
            LOGE("%s USB hub behind a hub unsupported", who);
            return false;
        }
    }
    if (control(0x80, 6, 0x200, 0, 9) != 9)
        return false;
    unsigned total = eh.data[2] | eh.data[3] << 8;
    struct usb_bot u;
    if (total < 9 || total > 4096 || control(0x80, 6, 0x200, 0, total) != (int)total ||
        !usb_bot_interface(eh.data, total, &u)) {
        LOGE("%s USB device has no BOT interface", who);
        return false;
    }
    /* §5.8.3: high-speed bulk endpoints have 512-byte packets. */
    if (u.mps[0] != 512 || u.mps[1] != 512) {
        LOGE("%s USB bulk packets %u/%u, not 512", who, u.mps[0], u.mps[1]);
        return false;
    }
    eh.interface = u.interface;
    for (unsigned i = 0; i < 2; ++i) {
        eh.endpoints[i] = u.address[i];
        endpoint(1 + i, device, u.address[i] & 15, 512, false);
        qh(1 + i)[6] = 0;
    }
    return control(0, 9, (uint16_t)u.configuration, 0, 0) == 0; /* §9.1.1.5: DATA0 */
}
static unsigned portsc(unsigned p)
{
    return eh.op + 0x44 + (p - 1) * 4;
}
static void release(unsigned p, uint32_t status)
{
    wr(portsc(p), (KEEP(status) & ~PE) | PO);
    LOGI("%s EHCI port %u: %s-speed device released to a companion controller", who, p,
         (status >> 10 & 3) == 1 ? "low" : "full");
}
/* §4.2.2, once the medium works: the full-speed devices noted by the root
 * port scan go to the companion controllers, each port again checked to
 * be connected, unchanged, disabled and still this controller's. */
static void release_full(void)
{
    for (unsigned p = 1; p <= eh.ports; ++p) {
        uint32_t status = reg(portsc(p));
        if ((eh.full >> p & 1) && (status & (CCS | CSC | PE | PO)) == CCS)
            release(p, status);
    }
}
/* §4.2.2: a low-speed device shows the K state before any reset; otherwise
 * a reset (§2.3.9: PE written 0 with PR; USB 2.0 §7.1.7.5: 50 ms) leaves a
 * high-speed device enabled and a full-speed one disabled. */
static unsigned classify(unsigned p)
{
    unsigned off = portsc(p);
    uint32_t status = reg(off);
    if (status == UINT32_MAX)
        return BROKEN;
    if (!(status & CCS))
        return NONE;
    if ((status >> 10 & 3) == 1)
        return LOW;
    wr(off, (KEEP(status) & ~PE) | PR);
    pause_ms(50);
    wr(off, KEEP(reg(off)) & ~PR);
    if (!wait_reg(off, PR, 0, 3)) {
        LOGE("%s EHCI port %u reset did not end", who, p);
        return BROKEN;
    }
    return reg(off) & PE ? HIGH : FULL;
}
/* Ports scanned for 1 s; each connection is classified 100 ms after its
 * own attach (USB 2.0 §7.1.7.3), connections after the window are not.
 * connected: 1, 0, 2 for a connection that changed since the last look
 * (the debounce starts again), or -1 for a controller or hub that is gone. */
static bool scan(const char *where, unsigned ports, int (*connected)(unsigned),
                 unsigned (*speed_of)(unsigned), unsigned *speed)
{
    static const char *const names[] = {"", "low-speed device", "full-speed device",
                                        "high-speed device", "not enabled by its reset"};
    uint64_t hz = hal_counter_hz(), window = hal_counter() + hz, attached[16];
    for (unsigned p = 0; p < 16; ++p) {
        attached[p] = UINT64_MAX;
        speed[p] = NONE;
    }
    bool pending;
    do {
        pending = false;
        for (unsigned p = 1; p <= ports; ++p) {
            if (speed[p])
                continue;
            uint64_t now = hal_counter();
            int on = connected(p);
            if (on < 0)
                return false;
            if (on != 1)
                attached[p] = UINT64_MAX;
            if (!on)
                continue;
            if (attached[p] == UINT64_MAX) {
                if (now >= window)
                    continue;
                attached[p] = now;
            }
            if (now - attached[p] < hz / 10) {
                pending = true;
                continue;
            }
            speed[p] = speed_of(p);
            if (speed[p] == BROKEN)
                return false;
            if (speed[p] == NONE)
                attached[p] = UINT64_MAX;
            else
                LOGI("%s EHCI %s port %u: %s", who, where, p, names[speed[p]]);
        }
        __asm__ volatile("pause");
    } while (pending || hal_counter() < window);
    return true;
}
static int root_connected(unsigned p)
{
    uint32_t status = reg(portsc(p));
    return status == UINT32_MAX ? -1 : (int)(status & CCS);
}
/* Root ports, powered and scanned; the connections after the window are
 * left to the port watch. Exactly one high-speed device holds the medium,
 * directly or behind a hub. Low-speed devices go to the companion
 * controllers at once (they have no bulk endpoints, USB 2.0 §5.8). A
 * full-speed device on another port may be a stick that did not link at
 * high speed: it goes only once the medium works, its connection change
 * acknowledged now so that the release finds the same device. Without
 * companion controllers (§2.2.3: N_CC 0) no port is handed over and there
 * is no watch. */
static bool root_ports(bool power)
{
    for (unsigned p = 1; p <= eh.ports && power; ++p) {
        uint32_t status = reg(portsc(p));
        if (!(status & PP))
            wr(portsc(p), (KEEP(status) & ~PE) | PP);
    }
    unsigned speed[16];
    if (!scan("root", eh.ports, root_connected, classify, speed))
        return false;
    unsigned high = 0, medium = 0;
    for (unsigned p = 1; p <= eh.ports; ++p)
        if (speed[p] == HIGH) {
            ++high;
            medium = p;
        }
    eh.full = 0;
    for (unsigned p = 1; p <= eh.ports && eh.companions; ++p) {
        uint32_t status = reg(portsc(p));
        if (speed[p] == LOW)
            release(p, status);
        if (speed[p] == FULL) {
            wr(portsc(p), KEEP(status) | CSC);
            eh.full |= 1u << p;
        }
    }
    if (high > 1) {
        LOGE("%s requires one directly attached high-speed device; %u found", who, high);
        return false;
    }
    if (!high) {
        for (unsigned p = 1; p <= eh.ports; ++p)
            if (speed[p] == FULL) {
                LOGE("%s EHCI port %u: a full-speed device; the medium must link at high speed",
                     who, p);
                return false;
            }
        if (quiet)
            LOGI("%s: no stick on the reserved controller", who);
        else
            LOGE("%s no root-port stick attached", who);
        return false;
    }
    eh.port = medium;
    eh.watching = eh.companions;
    for (unsigned p = 0; p < 16; ++p)
        eh.watch[p].state = EMPTY;
    pause_ms(10); /* USB 2.0 §7.1.7.5 reset recovery */
    return true;
}
/* Port requests to the hub at address 1 (USB 2.0 §11.24.2.13: SET_FEATURE
 * 03h, §11.24.2.2: CLEAR_FEATURE 01h; §11.24.2.7: GET_STATUS, 4 bytes). */
static bool port_feature(uint8_t request, unsigned feature, unsigned p)
{
    return control(0x23, request, (uint16_t)feature, (uint16_t)p, 0) == 0;
}
static bool port_status(unsigned p, uint32_t *status)
{
    if (control(0xa3, 0, 0, (uint16_t)p, 4) != 4)
        return false;
    *status =
        eh.data[0] | eh.data[1] << 8 | (uint32_t)eh.data[2] << 16 | (uint32_t)eh.data[3] << 24;
    return true;
}
/* A connection change (C_PORT_CONNECTION) is acknowledged; a device that
 * is connected again starts its debounce again (USB 2.0 §7.1.7.3). */
static int hub_connected(unsigned p)
{
    uint32_t status;
    if (!port_status(p, &status))
        return -1;
    if (!(status & CONNECTION_CHANGE))
        return (int)(status & CONNECTION);
    if (!port_feature(1, C_PORT_CONNECTION, p))
        return -1;
    return status & CONNECTION ? 2 : 0;
}
/* A low-speed device shows its speed before any reset; otherwise the hub
 * drives the reset (USB 2.0 §7.1.7.5: 10-20 ms) and then reports
 * C_PORT_RESET with the port enabled and the device's speed. A device that
 * left during the reset is no device. */
static unsigned hub_speed(unsigned p)
{
    uint32_t status;
    if (!port_status(p, &status))
        return BROKEN;
    if (status & LOW_SPEED)
        return LOW;
    if (!port_feature(3, PORT_RESET, p))
        return BROKEN;
    uint64_t end = hal_counter() + hal_counter_hz() / 2;
    do {
        if (!port_status(p, &status))
            return BROKEN;
        if (!(status & RESET) && (status & (RESET_CHANGE | CONNECTION)) != CONNECTION)
            break;
        if (hal_counter() >= end) {
            LOGE("%s hub port %u reset did not end", who, p);
            return BROKEN;
        }
        __asm__ volatile("pause");
    } while (true);
    if ((status & RESET_CHANGE) && !port_feature(1, C_PORT_RESET, p))
        return BROKEN;
    if (!(status & CONNECTION))
        return NONE;
    if (!(status & ENABLE))
        return DISABLED;
    return status & HIGH_SPEED ? HIGH : FULL;
}
/* The high-speed hub on the medium's root port, at address 1: configured
 * (hub requests need the Configured state), every port powered (one request
 * powers a ganged hub's ports; a hub without power switching has them on),
 * then accessed after bPwrOn2PwrGood (USB 2.0 §11.11, §11.23.2.1) and
 * scanned like the root ports. Exactly one high-speed device behind it is
 * the medium. Full- and low-speed devices behind it would need split
 * transactions and a transaction translator: they stay unused. */
static bool hub_init(void)
{
    /* §9.6.3: the configuration descriptor's header gives its value. */
    if (control(0x80, 6, 0x200, 0, 9) != 9 || eh.data[1] != 2 ||
        control(0, 9, eh.data[5], 0, 0) != 0)
        return false;
    /* §11.23.2.1: one to 15 ports here; bDescLength covers at least
     * DeviceRemovable (bit 0 and one bit per port). */
    if (control(0xa0, 6, 0x2900, 0, 7) != 7 || eh.data[1] != 0x29 || !eh.data[2] ||
        eh.data[2] > MAX_HUB_PORTS || eh.data[0] < 8 + eh.data[2] / 8) {
        LOGE("%s USB hub descriptor unusable", who);
        return false;
    }
    unsigned ports = eh.data[2], good = eh.data[5] * 2u;
    for (unsigned p = 1; p <= ports; ++p)
        if (!port_feature(3, PORT_POWER, p))
            return false;
    pause_ms(good);
    unsigned speed[16], high = 0, medium = 0;
    if (!scan("hub", ports, hub_connected, hub_speed, speed))
        return false;
    for (unsigned p = 1; p <= ports; ++p)
        if (speed[p] == HIGH) {
            ++high;
            medium = p;
        }
    eh.hub_refused = high != 1;
    if (high > 1) {
        LOGE("%s requires one high-speed device behind the hub; %u found", who, high);
        return false;
    }
    if (!high) {
        for (unsigned p = 1; p <= ports; ++p)
            if (speed[p] == FULL || speed[p] == DISABLED) {
                LOGE("%s hub port %u: %s", who, p,
                     speed[p] == FULL ? "a full-speed device; the medium must link at high speed"
                                      : "a device not enabled by its reset");
                return false;
            }
        if (quiet)
            LOGI("%s: no stick on the reserved controller", who);
        else
            LOGE("%s no stick behind the hub on root port %u", who, eh.port);
        return false;
    }
    eh.hub_port = medium;
    pause_ms(10); /* USB 2.0 §7.1.7.5 reset recovery */
    return true;
}
/* §4.2.2 on the root ports other than the medium's while the guest runs,
 * with companion controllers only: a new connection is debounced for
 * 100 ms; then a low-speed device, or, while the controller runs, one that
 * a reset leaves disabled (full speed), goes to its companion controller.
 * A high-speed device stays (the medium may have been moved), and so does
 * any other once the controller halted (§2.3.9: no reset while HCHalted).
 * The ports of a hub are not watched: nothing behind it can go to a
 * companion. Management CPU; returns the time of the next visit. */
uint64_t amd64_plog_ehci_watch(void)
{
    if (!eh.watching || log_in_panic())
        return UINT64_MAX;
    uint64_t hz = hal_counter_hz(), now = hal_counter(), next = now + hz / 10;
    for (unsigned p = 1; p <= eh.ports; ++p) {
        if (p == eh.port)
            continue;
        unsigned off = portsc(p);
        uint32_t status = reg(off);
        if (status == UINT32_MAX) {
            eh.watching = false;
            return UINT64_MAX;
        }
        unsigned *state = &eh.watch[p].state;
        uint64_t *at = &eh.watch[p].at;
        /* A companion's port is left alone until its device disconnects. */
        if (status & PO) {
            *state = EMPTY;
            continue;
        }
        if ((status & CSC) || !(status & CCS)) {
            /* A disconnection or a new connection. */
            if (*state == RESETTING) {
                wr(off, KEEP(status) & ~PR);
                status = reg(off);
            }
            if (status & CSC)
                wr(off, KEEP(status) | CSC);
            *state = EMPTY;
            if (!(status & CCS))
                continue;
        }
        if (*state == EMPTY) {
            *state = ATTACHED;
            *at = now;
        }
        if (*state == ATTACHED && now - *at >= hz / 10) {
            if ((status >> 10 & 3) == 1) {
                release(p, status);
                *state = EMPTY;
                continue;
            }
            if (!eh.running) {
                *state = KEPT;
                continue;
            }
            wr(off, (KEEP(status) & ~PE) | PR);
            *state = RESETTING;
            *at = now;
        } else if (*state == RESETTING && now - *at >= hz / 20) {
            wr(off, KEEP(reg(off)) & ~PR);
            *state = KEPT;
            if (wait_reg(off, PR, 0, 3) && !(reg(off) & PE)) {
                release(p, reg(off));
                *state = EMPTY;
            }
            continue;
        }
        if (*state == ATTACHED && *at + hz / 10 < next)
            next = *at + hz / 10;
        if (*state == RESETTING && *at + hz / 20 < next)
            next = *at + hz / 20;
    }
    return next;
}
/* §2.1.7/§2.1.8: HC OS Owned Semaphore in each USBLEGSUP of the PCI
 * extended capability list, HC BIOS Owned cleared within 1 s; the SMI
 * enables off and their status acknowledged. */
static bool handoff(const struct pci_device *d, unsigned eecp)
{
    for (unsigned n = 0; eecp && n < 48; ++n) {
        if (eecp < 0x40 || eecp > 0xf8 || (eecp & 3))
            return false;
        uint32_t cap = platform_pci_host_read(d->bdf, eecp, 4);
        if (cap == UINT32_MAX)
            return false;
        if ((cap & 255) == 1) {
            platform_pci_host_write(d->bdf, eecp + 3, 1, 1);
            uint64_t end = hal_counter() + hal_counter_hz();
            while (platform_pci_host_read(d->bdf, eecp, 4) & 1u << 16) {
                if (hal_counter() >= end) {
                    LOGE("%s EHCI BIOS kept the controller USBLEGSUP=%x", who, cap);
                    return false;
                }
                __asm__ volatile("pause");
            }
            platform_pci_host_write(d->bdf, eecp + 4, 0xe0000000u, 4);
        }
        eecp = cap >> 8 & 255;
    }
    return !eecp;
}
static const struct plog_block *start(const struct pci_device *d)
{
    if (d->bars[0].io || d->bars[0].size < 12) {
        LOGE("%s EHCI BAR0 unusable", who);
        return NULL;
    }
    eh.mmio = platform_pci_map_physical(d->bars[0].base, d->bars[0].size);
    if (!eh.mmio)
        return NULL;
    /* The firmware's command register stays while the BIOS may own the
     * controller (memory decode added if it is off); then bus mastering
     * waits for the reset of any schedule the firmware left running. */
    if (!(d->command & 2))
        platform_pci_host_write(d->bdf, 4, d->command | 2, 2);
    uint32_t cap = reg(0), hcs = reg(4), hcc = reg(8);
    unsigned version = cap >> 16, ports = hcs & 15;
    eh.op = cap & 255;
    eh.wide = hcc & 1;
    if (cap == UINT32_MAX || hcs == UINT32_MAX || hcc == UINT32_MAX ||
        (version >> 8 != 1 && version != 0x95) || eh.op < 0x10 || (eh.op & 3) || !ports ||
        eh.op + 0x44 + ports * 4 > d->bars[0].size) {
        LOGE("%s EHCI controller unsupported HCIVERSION=%x HCSPARAMS=%x", who, version, hcs);
        return NULL;
    }
    if (!handoff(d, hcc >> 8 & 255))
        return NULL;
    platform_pci_host_write(d->bdf, 4, (d->command & ~4u) | 2, 2);
    eh.ports = ports;
    eh.companions = hcs >> 12 & 15;
    /* §2.3.1: halt, then HCRESET (only from the halted state). */
    if (!halt()) {
        LOGE("%s EHCI did not halt", who);
        return NULL;
    }
    wr(eh.op + USBCMD, HCRESET);
    if (!wait_reg(eh.op + USBCMD, HCRESET, 0, 1000)) {
        LOGE("%s EHCI reset timeout", who);
        return NULL;
    }
    platform_pci_host_write(d->bdf, 4, d->command | 6, 2);
    /* Without 64-bit addressing every structure and buffer lies below
     * 4 GiB; with it, one page holds all queue heads and qTDs, inside the
     * one 4 GiB segment of CTRLDSSEGMENT. */
    eh.schedule = hal_alloc_pages(1, HAL_PAGE_ZERO);
    eh.data = hal_alloc_pages(PAGES, HAL_PAGE_ZERO);
    if (!eh.schedule || !eh.data) {
        LOGE("%s EHCI DMA allocation failed", who);
        return NULL;
    }
    eh.schedule_pa = physical(eh.schedule);
    bool low = eh.schedule_pa <= UINT32_MAX - 4095;
    for (unsigned i = 0; i < PAGES; ++i) {
        eh.data_pa[i] = physical(eh.data + i * 4096);
        low = low && eh.data_pa[i] <= UINT32_MAX - 4095;
    }
    if (!eh.wide && !low) {
        LOGE("%s EHCI without 64-bit addressing; low-memory DMA allocation unavailable", who);
        return NULL;
    }
    /* The bulk queue heads get their endpoints at configuration; until then
     * their overlays end at a terminated Next qTD pointer. */
    qh(0)[0] = link(0x80) | 2;
    qh(1)[0] = link(0x100) | 2;
    qh(2)[0] = link(0) | 2;
    for (unsigned i = 0; i < 3; ++i) {
        endpoint(i, 0, 0, i ? 512 : 64, !i);
        qh(i)[4] = 1;
        qh(i)[5] = 1;
    }
    /* §4.1: segment, no interrupts, the list, Run (interrupt threshold 8
     * microframes), then all ports to this controller. */
    if (eh.wide)
        wr(eh.op + CTRLDSSEGMENT, (uint32_t)(eh.schedule_pa >> 32));
    wr(eh.op + USBINTR, 0);
    wr(eh.op + ASYNCLISTADDR, link(0));
    wr(eh.op + USBCMD, 8u << 16 | RS);
    eh.running = true;
    if (!wait_reg(eh.op + USBSTS, HALTED, 0, 1000)) {
        LOGE("%s EHCI did not run", who);
        return NULL;
    }
    wr(eh.op + CONFIGFLAG, 1);
    eh.live = true;
    if (!root_ports(hcs >> 4 & 1))
        return NULL;
    if (!enumerate()) {
        if (!eh.hub_refused)
            LOGE("%s USB BOT enumeration failed", who);
        return NULL;
    }
    disk.who = who;
    if (!scsi_disk_start(&disk))
        return NULL;
    release_full();
    block = (struct plog_block){disk.sectors, read_blocks, write_blocks, flush};
    LOGI("%s EHCI BOT root-port=%u hub-port=%u HCIVERSION=%x sectors=%llu", who, eh.port,
         eh.hub_port, version, (unsigned long long)block.sectors);
    return &block;
}
const struct plog_block *amd64_plog_ehci(const struct pci_device *d, bool log)
{
    who = log ? "PLOG" : "NVRAM stick";
    quiet = !log;
    const struct plog_block *b = start(d);
    /* Failed after Run/Stop: halt the controller; its DMA pages stay. */
    if (!b && eh.running) {
        stop();
        LOGI("%s EHCI halted after the failed start; its ports stay with it", who);
    }
    eh.live = b != NULL;
    return b;
}
/* After the guest run, before the VMM powers off, resets or halts the
 * machine: halt, then CONFIGFLAG 0 gives every port to the companion
 * controllers, if any (§4.2). Nothing after a panic or once the backend
 * stopped. */
void amd64_plog_ehci_shutdown(void)
{
    if (!eh.live || log_in_panic())
        return;
    eh.live = eh.watching = false;
    if (halt()) {
        wr(eh.op + CONFIGFLAG, 0);
        if (eh.companions)
            LOGI("%s EHCI ports returned to the companion controllers", who);
        else
            LOGI("%s EHCI halted, CONFIGFLAG 0", who);
    } else
        LOGE("%s EHCI did not halt USBSTS=%x", who, reg(eh.op + USBSTS));
    eh.running = false;
}

#endif
