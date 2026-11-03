/* SPDX-License-Identifier: GPL-2.0-or-later */
/* EHCI 1.0 polled persistence backend against a model host controller and
 * a high-speed USB mass-storage device: §2.1.7/§2.1.8 USBLEGSUP handoff,
 * §2.3.1 HCRESET from the halted state, §4.1 initialization, §2.3.9 and
 * §4.2.2 root ports (power, debounce, reset, high-speed detection, release
 * to a companion), §3.5/§3.6 qTD and QH, §4.8 asynchronous schedule enable,
 * §4.10 queue advance and overlay, Appendix B 64-bit structures; USB 2.0
 * §8.6 data toggles, §9.2.6 timings and §9.4 requests; BOT 1.0 §5/§6; the
 * medium behind a high-speed hub on a root port (USB 2.0 ch. 11: hub
 * descriptor, port power and power-good time, per-port connection, reset
 * and speed through hub class requests) on a controller without companions
 * (§2.2.3 N_CC 0); then broken controllers, hubs and devices. A transaction
 * reaches the one high-speed device that holds its address behind enabled
 * ports, never a full- or low-speed device behind the hub (that would need
 * split transactions). Registers are plain memory: the model
 * takes every value that differs from the one it last published as a host
 * write. DMA uses addresses above 4 GiB on pages whose physical order is the
 * reverse of their virtual order. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/amd64/plog_ehci.c"

#define POOL 16u
#define DISK 4096u
#define PORTS 4u
#define BAR 0x400u
#define OP 0x20u
#define EECP 0x68u
#define MS 10u /* ticks per millisecond: about one microframe each */
static _Alignas(4096) uint8_t pool[POOL * 4096];
static uint8_t mmio[BAR], config[256], visible[DISK * 512], durable[DISK * 512];
static uint32_t shadow[BAR / 4];
static unsigned pool_used;
static uint64_t ticks, panic_at, phys;
static unsigned errors; /* error lines logged */
static char last_error[256];
static bool panicking, alloc_fail;
static struct pci_device device = {.class_code = 0x0c0320,
                                   .owner = PCI_OWNER_VMM,
                                   .reservation = "vmm-media",
                                   .bars = {{.size = BAR}},
                                   .command = 6};

/* Device speeds: the backend's NONE, LOW, FULL and HIGH. */
enum { CBW, DATA_IN, DATA_OUT, CSW };
enum fault_kind { FAULT_NONE, FAULT_NAK, FAULT_XACT, FAULT_STALL, FAULT_HSE, FAULT_SHORT };
struct model_port {
    bool pp, ccs, csc, pe, released, resetting;
    uint64_t connect_at, reset_at, enabled_at;
};
static struct {
    /* Controller properties. */
    uint32_t hcs, hcc;
    unsigned reset_delay, delay[PORTS + 1]; /* each port's connection after power */
    bool reset_stuck, halt_stuck, ass_stuck, bios_owned, bios_never, firmware_running, other_cap;
    /* Controller state. */
    bool active, running, resetting, ase, ass, cf, bios;
    uint64_t reset_done_at, run_at, halt_at, ass_at, bios_at;
    uint32_t cmd, sts, segment, async;
    struct model_port port[PORTS + 1];
    uint64_t ring[8];
    uint32_t ring_words[8][3];
    unsigned qhs;
    /* Device properties: the storage device's root port (0 behind the hub)
     * or hub port (0 on a root port); each root port's device speed. */
    unsigned at, down, kind[PORTS + 1];
    /* A root port's device disconnects at this tick; with swap a
     * high-speed device connects there 1 ms later. */
    uint64_t leave_at[PORTS + 1];
    bool swap[PORTS + 1];
    uint8_t descriptor[18], configuration[64];
    unsigned descriptor_length, configuration_length;
    uint64_t capacity;
    bool wce, dpofua, wp;
    struct {
        unsigned opcode, phase, skip;
        enum fault_kind kind;
    } fault;
    /* Device state. */
    unsigned address, value, toggle[2], stage, sent, response_length, bot, sense, length, status;
    bool configured, halt[2], control_stall, in, fua, lie;
    uint64_t address_at;
    uint8_t setup[8], response[16384], cdb[16];
    uint32_t tag, expected, received;
    unsigned requests, commands, writes, reads, syncs, selects, bot_resets, clears, max_bytes,
        releases, port_resets;
} m;
/* A high-speed hub on root port at (0: none), USB 2.0 ch. 11. */
#define HUB_PORTS 15u
struct hub_port {
    bool power, ccs, pe, resetting, c_connect, c_reset;
    uint64_t good_at, connect_at, reset_at, enabled_at;
};
static struct {
    /* Properties: bNbrPorts, the power switching mode (wHubCharacteristics
     * D1:D0), bPwrOn2PwrGood, the reset the hub drives (10-20 ms) and how
     * long after the request it starts; the device speed of each port and
     * its connection after power good. */
    unsigned at, ports, lpsm, good, reset_ms, latency_ms, kind[HUB_PORTS + 1], delay[HUB_PORTS + 1];
    /* Descriptor and response knobs: bytes returned (hub and configuration
     * descriptors, port status), bDescLength (0: the length for bNbrPorts),
     * the type bytes, bMaxPacketSize0 and bConfigurationValue. */
    unsigned descriptor_length, length_field, type, mps0, config_length, config_type, config_value,
        status_length;
    bool descriptor_stall, status_stall, reset_stuck;
    /* Per port: no enable after the reset (over-current); a device that
     * leaves 5 ms into its reset (the hub reporting C_PORT_RESET for it with
     * leave_change) and comes back after back_ms (0: never); a 0.2 ms
     * disconnection bounce_ms after the connection. */
    bool no_enable[HUB_PORTS + 1], leave[HUB_PORTS + 1], leave_change;
    unsigned back_ms[HUB_PORTS + 1], bounce_ms[HUB_PORTS + 1];
    /* State. */
    unsigned address, stage, sent, length;
    bool configured, stall, in;
    uint64_t address_at;
    uint8_t setup[8], response[80];
    struct hub_port port[HUB_PORTS + 1];
    unsigned requests, powers, resets;
} hub;

void vmm_log(enum log_level level, const char *fmt, ...)
{
    if (level != LOG_ERROR)
        return;
    ++errors;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error, sizeof(last_error), fmt, ap);
    va_end(ap);
}
bool log_in_panic(void)
{
    return panicking;
}
void hal_panic(const char *message)
{
    fprintf(stderr, "%s\n", message);
    abort();
}
uint8_t *platform_pci_map_physical(uint64_t base, uint64_t size)
{
    (void)base;
    assert(size <= BAR);
    return mmio;
}
uint32_t platform_pci_host_read(uint16_t bdf, unsigned off, unsigned size)
{
    (void)bdf;
    assert(off + size <= sizeof(config));
    uint32_t value = 0;
    memcpy(&value, config + off, size);
    return value;
}
void platform_pci_host_write(uint16_t bdf, unsigned off, uint32_t value, unsigned size)
{
    (void)bdf;
    assert(off + size <= sizeof(config));
    /* USBLEGSUP: only the HC OS Owned Semaphore byte is the system's. */
    if (off <= EECP + 2 && off + size > EECP)
        assert(off == EECP + 3 && size == 1);
    memcpy(config + off, &value, size);
}
void *hal_alloc_pages(size_t n, uint32_t flags)
{
    if (alloc_fail)
        return NULL;
    assert(pool_used + n <= POOL);
    uint8_t *p = pool + pool_used * 4096;
    pool_used += n;
    if (flags & HAL_PAGE_ZERO)
        memset(p, 0, n * 4096);
    return p;
}
bool amd64_memory_physical(uintptr_t address, uint64_t *pa)
{
    if (address < (uintptr_t)pool || address >= (uintptr_t)pool + sizeof(pool))
        return false;
    uint64_t page = (address - (uintptr_t)pool) / 4096;
    *pa = phys + (POOL - 1 - page) * 4096 + (address & 4095);
    return true;
}
static bool wide(void)
{
    return m.hcc & 1;
}
/* Device view of one physical range; it never crosses a page. */
static uint8_t *model_dma(uint64_t pa, unsigned length)
{
    assert(config[4] & 4); /* Bus Master Enable */
    assert(pa >= phys && pa < phys + POOL * 4096 && (pa & 4095) + length <= 4096);
    uint64_t page = (pa - phys) / 4096;
    return pool + (POOL - 1 - page) * 4096 + (pa & 4095);
}
static uint32_t r32(unsigned off)
{
    uint32_t value;
    memcpy(&value, mmio + off, 4);
    return value;
}
static void w32(unsigned off, uint32_t value)
{
    memcpy(mmio + off, &value, 4);
    shadow[off / 4] = value;
}
static bool written(unsigned off, uint32_t *value)
{
    *value = r32(off);
    return *value != shadow[off / 4];
}
static unsigned port_off(unsigned p)
{
    return OP + 0x44 + (p - 1) * 4;
}
static uint64_t structure(uint32_t link)
{
    return (uint64_t)(wide() ? m.segment : 0) << 32 | (link & ~31u);
}

/* §3.5.4 buffer pointers: page 0 with the current offset, pages 1-4 page
 * aligned; Appendix B: the high dwords in the extended pointers. */
static void td_copy(const uint32_t *t, uint8_t *data, unsigned n, bool to_host)
{
    unsigned offset = t[3] & 4095;
    assert(offset + n <= 5 * 4096);
    for (unsigned done = 0; done < n;) {
        unsigned page = (offset + done) / 4096, at = (offset + done) % 4096, chunk = 4096 - at;
        if (chunk > n - done)
            chunk = n - done;
        if (page)
            assert(!(t[3 + page] & 4095));
        uint64_t pa = (t[3 + page] & ~4095u) | (uint64_t)(wide() ? t[8 + page] : 0) << 32;
        uint8_t *p = model_dma(pa + at, chunk);
        if (to_host)
            memcpy(p, data + done, chunk);
        else
            memcpy(data + done, p, chunk);
        done += chunk;
    }
}

/* USB 2.0 §9.4 standard requests, BOT §3.1 reset; the effect of a request
 * takes place with its status stage. */
static bool request_data(void)
{
    uint8_t *s = m.setup;
    unsigned value = s[2] | s[3] << 8, index = s[4] | s[5] << 8, length = s[6] | s[7] << 8;
    m.response_length = 0;
    if (s[0] == 0x80 && s[1] == 6 && value == 0x100) {
        memcpy(m.response, m.descriptor, 18);
        m.response_length = length < m.descriptor_length ? length : m.descriptor_length;
    } else if (s[0] == 0x80 && s[1] == 6 && value == 0x200) {
        memcpy(m.response, m.configuration, m.configuration_length);
        m.response_length = length < m.configuration_length ? length : m.configuration_length;
    } else if (s[0] == 0 && s[1] == 5 && value && value < 128 && !index && !length) {
    } else if (s[0] == 0 && s[1] == 9 && (value == 1 || !value) && !length && m.address) {
    } else if (s[0] == 2 && s[1] == 1 && !value && (index == 0x81 || index == 0x02) && !length &&
               m.configured) {
    } else if (s[0] == 0x21 && s[1] == 0xff && !value && !index && !length && m.configured) {
    } else
        return false;
    return true;
}
static void request_effect(void)
{
    uint8_t *s = m.setup;
    unsigned value = s[2] | s[3] << 8, index = s[4] | s[5] << 8;
    if (s[1] == 5) {
        m.address = value;
        m.address_at = ticks;
    }
    if (s[1] == 9) {
        m.configured = value == 1;
        m.value = value;
        m.toggle[0] = m.toggle[1] = 0;
        m.halt[0] = m.halt[1] = false;
        m.bot = CBW;
    }
    if (s[1] == 1) {
        unsigned e = index == 0x81 ? 0 : 1;
        m.halt[e] = false;
        m.toggle[e] = 0;
        ++m.clears;
    }
    if (s[1] == 0xff) {
        m.bot = CBW;
        ++m.bot_resets;
    }
}
/* A reset of the storage device's port: the Default state (USB 2.0 §9.1.1.3). */
static void storage_reset(void)
{
    m.address = 0;
    m.address_at = 0;
    m.configured = false;
    m.toggle[0] = m.toggle[1] = 0;
    m.halt[0] = m.halt[1] = false;
    m.bot = CBW;
    m.stage = 0;
}

/* §11.11: a powered port's device connects after power good. */
static void power_on(unsigned q)
{
    struct hub_port *hp = &hub.port[q];
    if (hp->power)
        return;
    hp->power = true;
    hp->good_at = ticks + hub.good * 2 * MS;
    hp->connect_at = hub.kind[q] ? hp->good_at + hub.delay[q] : UINT64_MAX;
}
/* USB 2.0 §9.4 standard requests and §11.24.2 hub class requests. */
static bool hub_request(void)
{
    const uint8_t *s = hub.setup;
    unsigned value = s[2] | s[3] << 8, index = s[4] | s[5] << 8, length = s[6] | s[7] << 8, n = 0;
    /* §11.23.1: a high-speed hub with one TT; one interface with its
     * status change endpoint. */
    const uint8_t device[18] = {18, 1, 0, 2, 9, 0, 1, (uint8_t)hub.mps0, 0x86, 0x80, 0x24,
                                0,  0, 1, 0, 0, 0, 1};
    const uint8_t configuration[25] = {9,    (uint8_t)hub.config_type,
                                       25,   0,
                                       1,    (uint8_t)hub.config_value,
                                       0,    0xe0,
                                       0,    9,
                                       4,    0,
                                       0,    1,
                                       9,    0,
                                       0,    0,
                                       7,    5,
                                       0x81, 3,
                                       2,    0,
                                       12};
    /* §11.23.2.1: bDescLength, bNbrPorts, wHubCharacteristics,
     * bPwrOn2PwrGood, bHubContrCurrent, then DeviceRemovable and
     * PortPwrCtrlMask of one bit per port and bit 0 each. */
    unsigned bitmap = hub.ports / 8 + 1, total = 7 + 2 * bitmap;
    uint8_t descriptor[71] = {(uint8_t)(hub.length_field ? hub.length_field : total),
                              (uint8_t)hub.type,
                              (uint8_t)hub.ports,
                              (uint8_t)hub.lpsm,
                              0,
                              (uint8_t)hub.good,
                              0};
    memset(descriptor + 7 + bitmap, 0xff, bitmap);
    if (s[0] == 0x80 && s[1] == 6 && value == 0x100) {
        memcpy(hub.response, device, n = 18);
    } else if (s[0] == 0x80 && s[1] == 6 && value == 0x200) {
        memcpy(hub.response, configuration, n = hub.config_length);
    } else if (s[0] == 0 && s[1] == 5 && value && value < 128 && !index && !length) {
    } else if (s[0] == 0 && s[1] == 9 && value == hub.config_value && !index && !length &&
               hub.address) {
    } else if (s[0] == 0xa0 && s[1] == 6 && value == 0x2900 && !index && hub.configured &&
               !hub.descriptor_stall) {
        memcpy(hub.response, descriptor, n = hub.descriptor_length ? hub.descriptor_length : total);
    } else if (s[0] == 0xa3 && !s[1] && !value && index && index <= hub.ports && length == 4 &&
               hub.configured && !hub.status_stall) {
        assert(index <= HUB_PORTS);
        struct hub_port *hp = &hub.port[index];
        /* §11.23.2.1: a powered-on port is accessed after power good. */
        assert(hp->power && ticks >= hp->good_at);
        /* Tables 11-21 and 11-22: wPortStatus, wPortChange. */
        unsigned kind = hub.kind[index];
        uint32_t status = (hp->ccs ? 1u : 0) | (hp->pe ? 2u : 0) |
                          (hp->resetting && ticks >= hp->reset_at ? 0x10u : 0) | 0x100u |
                          (hp->ccs && kind == LOW ? 0x200u : 0) |
                          (hp->pe && kind == HIGH ? 0x400u : 0) | (hp->c_connect ? 1u << 16 : 0) |
                          (hp->c_reset ? 0x10u << 16 : 0);
        for (unsigned i = 0; i < 4; ++i)
            hub.response[i] = (uint8_t)(status >> (8 * i));
        n = hub.status_length;
    } else if (s[0] == 0x23 && index && index <= hub.ports && !length && hub.configured) {
        /* Table 11-17: SET PORT_POWER and PORT_RESET; CLEAR C_PORT_CONNECTION and C_PORT_RESET. */
        assert(index <= HUB_PORTS);
        if (!(s[1] == 3 && (value == 8 || value == 4)) &&
            !(s[1] == 1 && (value == 16 || value == 20)))
            return false;
    } else
        return false;
    hub.length = n < length ? n : length;
    return true;
}
static void hub_effect(void)
{
    const uint8_t *s = hub.setup;
    unsigned value = s[2] | s[3] << 8, index = s[4] | s[5] << 8;
    if (s[0] == 0 && s[1] == 5) {
        hub.address = value;
        hub.address_at = ticks;
    }
    /* §11.11: without power switching the ports are powered with the configuration. */
    if (s[0] == 0 && s[1] == 9) {
        hub.configured = true;
        for (unsigned q = 1; q <= hub.ports && q <= HUB_PORTS && (hub.lpsm & 2); ++q)
            power_on(q);
    }
    if (s[0] != 0x23)
        return;
    struct hub_port *hp = &hub.port[index];
    if (s[1] == 3 && value == 8) {
        /* Ganged switching: one request powers every port. */
        ++hub.powers;
        for (unsigned q = 1; q <= hub.ports && q <= HUB_PORTS && !(hub.lpsm & 2); ++q)
            if (q == index || !hub.lpsm)
                power_on(q);
    }
    if (s[1] == 3 && value == 4) {
        /* USB 2.0 §7.1.7.3: 100 ms after the connection; one port at a time;
         * a low-speed device shows its speed before any reset. */
        assert(hp->power && ticks >= hp->good_at && hp->ccs && ticks - hp->connect_at >= 100 * MS);
        assert(hub.kind[index] != LOW);
        for (unsigned q = 1; q <= HUB_PORTS; ++q)
            assert(!hub.port[q].resetting);
        hp->resetting = true;
        hp->pe = false;
        hp->reset_at = ticks + hub.latency_ms * MS;
        ++hub.resets;
    }
    if (s[1] == 1 && value == 16)
        hp->c_connect = false;
    if (s[1] == 1 && value == 20)
        hp->c_reset = false;
}
/* The hub's default pipe: returns bytes moved or -2 for a STALL; the
 * effect of a request takes place with its status stage. */
static int hub_control(const uint32_t *t, unsigned pid, unsigned length, unsigned dt)
{
    static uint8_t data[80];
    if (pid == 2) {
        assert(length == 8 && !dt);
        td_copy(t, hub.setup, 8, false);
        if (hub.address_at)
            assert(ticks - hub.address_at >= 2 * MS);         /* USB 2.0 §9.2.6.3 */
        assert(ticks - m.port[hub.at].enabled_at >= 10 * MS); /* USB 2.0 §7.1.7.5 */
        ++hub.requests;
        hub.address_at = 0;
        hub.stage = 1;
        hub.sent = 0;
        hub.stall = !hub_request();
        hub.in = hub.setup[0] & 0x80;
        return 8;
    }
    unsigned want = hub.setup[6] | hub.setup[7] << 8;
    assert(hub.stage && dt == 1);
    if (hub.stall)
        return -2;
    if (hub.stage == 1 && want) {
        assert(hub.in && pid == 1 && length <= want);
        unsigned k = hub.length - hub.sent < length ? hub.length - hub.sent : length;
        memcpy(data, hub.response + hub.sent, k);
        hub.sent += k;
        hub.stage = 2;
        if (k)
            td_copy(t, data, k, true);
        return (int)k;
    }
    assert(length == 0 && (pid == 1) == !(hub.in && want));
    hub_effect();
    hub.stage = 0;
    return 0;
}
/* A reset of the hub's root port: the hub and every device behind it
 * lose their state. */
static void hub_reset(void)
{
    if (m.down)
        storage_reset();
    hub.address = 0;
    hub.address_at = 0;
    hub.configured = false;
    hub.stage = 0;
    for (unsigned q = 1; q <= HUB_PORTS; ++q) {
        memset(&hub.port[q], 0, sizeof(hub.port[q]));
        hub.port[q].connect_at = UINT64_MAX;
    }
}
/* The device a high-speed transaction to this address reaches through
 * enabled ports: the hub, the storage device, another high-speed device or
 * none. Two that answer one address are a collision. */
enum { TO_NONE, TO_HUB, TO_STORAGE, TO_OTHER };
static unsigned target(unsigned address)
{
    unsigned found = TO_NONE, n = 0;
    for (unsigned p = 1; p <= PORTS; ++p) {
        if (!m.port[p].pe || m.port[p].released || m.kind[p] != HIGH)
            continue;
        if (p != hub.at) {
            if ((p == m.at ? m.address : 0) == address) {
                found = p == m.at ? TO_STORAGE : TO_OTHER;
                ++n;
            }
            continue;
        }
        if (hub.address == address) {
            found = TO_HUB;
            ++n;
        }
        for (unsigned q = 1; q <= HUB_PORTS; ++q) {
            if (!hub.port[q].pe || hub.kind[q] != HIGH)
                continue;
            if ((q == m.down ? m.address : 0) == address) {
                found = q == m.down ? TO_STORAGE : TO_OTHER;
                ++n;
            }
        }
    }
    assert(n <= 1);
    return found;
}

static void sense(unsigned key)
{
    m.sense = key;
}
/* SPC/SBC subset of the device. */
static void scsi_command(bool in)
{
    uint8_t *c = m.cdb;
    unsigned op = c[0], blocks = c[7] << 8 | c[8], out = 0;
    uint32_t lba = (uint32_t)c[2] << 24 | c[3] << 16 | c[4] << 8 | c[5];
    m.response_length = 0;
    m.sent = 0;
    m.received = 0;
    m.length = 0;
    uint8_t status = 0;
    switch (op) {
    case 0x00:
        break;
    case 0x03:
        memset(m.response, 0, 18);
        m.response[0] = 0x70;
        m.response[2] = (uint8_t)m.sense;
        m.response[7] = 10;
        m.response_length = 18;
        m.sense = 0;
        break;
    case 0x12:
        memset(m.response, 0, 36);
        m.response[4] = 31;
        memcpy(m.response + 8, "MODEL   EHCI STICK      ", 24);
        m.response_length = 36;
        break;
    case 0x1a:
        assert((c[2] & 0x3f) == 8);
        memset(m.response, 0, 24);
        m.response[0] = 23;
        m.response[2] = (m.wp ? 0x80 : 0) | (m.dpofua ? 0x10 : 0);
        m.response[4] = 8;
        m.response[5] = 18;
        m.response[6] = m.wce ? 4 : 0;
        m.response_length = 24;
        break;
    case 0x15:
        out = c[4];
        break;
    case 0x25: {
        uint32_t last = (uint32_t)(m.capacity - 1);
        for (unsigned i = 0; i < 4; ++i)
            m.response[i] = (uint8_t)(last >> (24 - 8 * i));
        m.response[4] = 0;
        m.response[5] = 0;
        m.response[6] = 2;
        m.response[7] = 0;
        m.response_length = 8;
        break;
    }
    case 0x28:
        assert(blocks && lba + blocks <= DISK && blocks * 512 <= sizeof(m.response));
        memcpy(m.response, visible + lba * 512, blocks * 512);
        m.response_length = blocks * 512;
        ++m.reads;
        break;
    case 0x2a:
        assert(blocks && lba + blocks <= DISK);
        out = blocks * 512;
        m.fua = c[1] & 8;
        m.length = lba;
        break;
    case 0x35:
        if (m.wce)
            memcpy(durable, visible, sizeof(durable));
        ++m.syncs;
        break;
    default:
        status = 1;
        sense(5);
        break;
    }
    /* BOT §6.7: the host expects as much as the device moves. */
    if (m.response_length)
        assert(in && m.expected >= m.response_length);
    if (out)
        assert(!in && m.expected == out);
    if (!m.response_length && !out)
        assert(!m.expected);
    m.in = in;
    m.status = status;
    m.bot = m.expected ? (in ? DATA_IN : DATA_OUT) : CSW;
}
static void scsi_data_out(const uint8_t *data, unsigned n)
{
    static uint8_t buffer[16384];
    assert(m.received + n <= m.expected && m.expected <= sizeof(buffer));
    memcpy(buffer + m.received, data, n);
    m.received += n;
    if (m.received < m.expected)
        return;
    if (m.cdb[0] == 0x2a) {
        memcpy(visible + m.length * 512, buffer, m.expected);
        ++m.writes;
        if (m.expected > m.max_bytes)
            m.max_bytes = m.expected;
        if (!m.wce || m.fua)
            memcpy(durable + m.length * 512, buffer, m.expected);
    }
    if (m.cdb[0] == 0x15) {
        assert(m.expected == 24 && buffer[4] == 8 && buffer[5] == 18 && !buffer[0]);
        m.wce = buffer[6] & 4;
        ++m.selects;
    }
    m.bot = CSW;
}
/* One bulk transfer at the device. Returns bytes moved, or -1 for NAK, -2
 * for STALL, -3 for a transaction error. */
static int bulk_device(unsigned e, uint8_t *data, unsigned n)
{
    if (m.halt[e])
        return -2;
    unsigned phase = m.bot, opcode = phase == CBW && n == 31 ? data[15] : m.cdb[0];
    if (m.fault.kind && m.fault.phase == phase && m.fault.opcode == opcode) {
        if (m.fault.skip)
            --m.fault.skip;
        else {
            enum fault_kind kind = m.fault.kind;
            if (kind != FAULT_NAK)
                m.fault.kind = FAULT_NONE;
            if (kind == FAULT_NAK)
                return -1;
            if (kind == FAULT_XACT)
                return -3;
            if (kind == FAULT_STALL) {
                m.halt[e] = true;
                if (phase == DATA_IN) {
                    m.sent = 0;
                    m.bot = CSW;
                }
                return -2;
            }
            if (kind == FAULT_HSE)
                return -4;
            if (kind == FAULT_SHORT) {
                m.response_length /= 2;
                m.lie = true;
            } /* the CSW hides the residue */
        }
    }
    if (e == 1) {
        if (m.bot == CBW) {
            assert(n == 31 && !memcmp(data, "USBC", 4) && !data[13] && data[14] >= 1 &&
                   data[14] <= 16);
            memcpy(&m.tag, data + 4, 4);
            memcpy(&m.expected, data + 8, 4);
            memcpy(m.cdb, data + 15, 16);
            ++m.commands;
            scsi_command(data[12] & 0x80);
        } else {
            assert(m.bot == DATA_OUT);
            scsi_data_out(data, n);
        }
        return (int)n;
    }
    if (m.bot == DATA_IN) {
        unsigned k = m.response_length - m.sent < n ? m.response_length - m.sent : n;
        memcpy(data, m.response + m.sent, k);
        m.sent += k;
        m.bot = CSW;
        if (m.lie) {
            m.sent = m.expected;
            m.lie = false;
        }
        return (int)k;
    }
    assert(m.bot == CSW && n == 13);
    uint32_t residue = m.expected - (m.in ? m.sent : m.received);
    memset(data, 0, 13);
    memcpy(data, "USBS", 4);
    memcpy(data + 4, &m.tag, 4);
    memcpy(data + 8, &residue, 4);
    data[12] = (uint8_t)m.status;
    m.bot = CBW;
    return 13;
}

/* §4.10.2/§4.10.3: one transaction set of the overlay's qTD per visit. */
static void model_qh(uint64_t qpa)
{
    uint32_t *q = (uint32_t *)model_dma(qpa, wide() ? 68 : 48);
    if (q[6] & 0x40)
        return; /* halted */
    if (!(q[6] & 0x80)) {
        uint32_t next = (q[6] >> 16 & 0x7fff) && !(q[5] & 1) ? q[5] : q[4];
        if (next & 1)
            return;
        uint32_t *t = (uint32_t *)model_dma(structure(next), wide() ? 52 : 32);
        if (!(t[2] & 0x80))
            return;
        /* Overlay; DTC 0 keeps the queue head's data toggle (and ping state). */
        uint32_t keep = q[6] & (1u << 31 | 1);
        q[3] = (uint32_t)structure(next);
        q[4] = t[0];
        q[5] = t[1];
        q[6] = t[2];
        if (!(q[1] >> 14 & 1))
            q[6] = (q[6] & ~(1u << 31 | 1)) | keep;
        for (unsigned i = 0; i < 5; ++i) {
            q[7 + i] = t[3 + i];
            if (wide())
                q[12 + i] = t[8 + i];
        }
    }
    uint32_t *t = (uint32_t *)model_dma(structure(q[3]), wide() ? 52 : 32);
    uint32_t token = q[6];
    unsigned address = q[1] & 127, endpoint = q[1] >> 8 & 15, mps = q[1] >> 16 & 0x7ff,
             pid = token >> 8 & 3;
    unsigned length = token >> 16 & 0x7fff, dt = token >> 31;
    assert((token >> 10 & 3) && !(token >> 12 & 7));
    assert((q[1] >> 12 & 3) == 2 && !(q[1] >> 27 & 1)); /* high speed, no control endpoint flag */
    static uint8_t data[20480];
    int moved;
    /* The host talks to the hub and the storage device only; a transaction
     * nobody answers is a transaction error. */
    unsigned to = target(address);
    assert(to != TO_OTHER);
    if (to == TO_NONE)
        moved = -3;
    else if (to == TO_HUB) {
        assert(endpoint == 0 && mps == 64 && (q[1] >> 14 & 1));
        moved = hub_control(t, pid, length, dt);
    } else if (endpoint == 0) {
        assert(mps == 64 && (q[1] >> 14 & 1)); /* DTC: toggles from the qTDs */
        if (pid == 2) {
            /* §8.5.3: SETUP is DATA0 and starts a new control transfer. */
            assert(length == 8 && !dt);
            td_copy(t, m.setup, 8, false);
            if (m.address_at)
                assert(ticks - m.address_at >= 2 * MS); /* USB 2.0 §9.2.6.3 */
            uint64_t enabled = m.down ? hub.port[m.down].enabled_at : m.port[m.at].enabled_at;
            assert(ticks - enabled >= 10 * MS); /* USB 2.0 §7.1.7.5 */
            ++m.requests;
            m.address_at = 0;
            m.stage = 1;
            m.sent = 0;
            m.control_stall = !request_data();
            m.in = m.setup[0] & 0x80;
            moved = 8;
        } else {
            unsigned want = m.setup[6] | m.setup[7] << 8;
            bool status = m.stage == 2 || !want;
            assert(m.stage && dt == 1);
            if (m.control_stall)
                moved = -2;
            else if (!status) {
                assert((pid == 1) == m.in && length <= want);
                if (m.in) {
                    unsigned k =
                        m.response_length - m.sent < length ? m.response_length - m.sent : length;
                    memcpy(data, m.response + m.sent, k);
                    m.sent += k;
                    moved = (int)k;
                } else
                    moved = (int)length;
                m.stage = 2;
            } else {
                assert(length == 0 && (pid == 1) == !(m.in && want));
                request_effect();
                m.stage = 0;
                moved = 0;
            }
            if (moved > 0 && pid == 1)
                td_copy(t, data, (unsigned)moved, true);
        }
    } else {
        assert(m.configured && mps == 512 &&
               !(q[1] >> 14 & 1)); /* DTC 0: the queue head's toggle */
        unsigned e = endpoint == 1 && pid == 1 ? 0 : endpoint == 2 && pid == 0 ? 1 : 2;
        assert(e < 2);
        if (e == 1)
            td_copy(t, data, length, false);
        moved = bulk_device(e, data, length);
        if (moved >= 0) {
            /* USB 2.0 §8.6: both ends agree on the toggle of every packet. */
            unsigned packets = moved ? ((unsigned)moved + 511) / 512 : 1;
            if (e == 0 && moved && (unsigned)moved < length && !(moved % 512))
                ++packets;
            assert(dt == m.toggle[e]);
            m.toggle[e] ^= packets & 1;
            dt ^= packets & 1;
            if (e == 0 && moved)
                td_copy(t, data, (unsigned)moved, true);
        }
        if (moved == -4) {
            m.sts |= 1u << 4 | 1u << 12;
            m.running = m.ass = false;
            return; /* Host System Error */
        }
    }
    if (moved == -1)
        return; /* NAK: retried at the next visit */
    if (moved == -2)
        token = (token & ~0x80u) | 0x40;
    else if (moved == -3)
        token = (token & ~(0x80u | 3u << 10)) | 0x48;
    else {
        unsigned remaining = length - (unsigned)moved;
        token = (token & ~(0x80u | 0x7fffu << 16 | 1u << 31)) | remaining << 16 | dt << 31;
    }
    q[6] = token;
    t[2] = (t[2] & ~0xffff00ffu) | (token & 0xffff00ff);
}
static void model_ring(void)
{
    /* §4.8: a circular list of queue heads, one with H set. */
    uint64_t start = structure(m.async), at = start;
    unsigned heads = 0;
    m.qhs = 0;
    do {
        assert(m.qhs < 8);
        uint32_t *q = (uint32_t *)model_dma(at, wide() ? 68 : 48);
        assert((q[0] >> 1 & 3) == 1 && !(q[0] & 1));
        assert(q[2] >> 30 && !(q[2] & 0xffff)); /* Mult nonzero, no S-mask/C-mask */
        heads += q[1] >> 15 & 1;
        m.ring[m.qhs] = at;
        memcpy(m.ring_words[m.qhs], q, 12);
        ++m.qhs;
        at = structure(q[0]);
    } while (at != start);
    assert(heads == 1);
}
static void model_hc_reset(void)
{
    m.resetting = false;
    m.running = m.ase = m.ass = m.cf = false;
    m.firmware_running = false;
    m.cmd = 0x00080000;
    m.sts = 1u << 12;
    m.segment = 0;
    m.async = 0;
    w32(OP + 0x10, 0);
    w32(OP + 0x14, 0);
    w32(OP + 0x18, 0);
    w32(OP + 8, 0);
    for (unsigned p = 1; p <= PORTS; ++p) {
        struct model_port *pt = &m.port[p];
        memset(pt, 0, sizeof(*pt));
        pt->pp = !(m.hcs >> 4 & 1);
        pt->connect_at = UINT64_MAX;
    }
}
static bool attached(unsigned p)
{
    return m.kind[p] != NONE;
}
static uint32_t port_value(unsigned p)
{
    struct model_port *pt = &m.port[p];
    unsigned line = pt->ccs && !pt->pe && !pt->resetting ? (m.kind[p] == LOW ? 1u : 2u) : 0;
    return (pt->ccs ? 1u : 0) | (pt->csc ? 2u : 0) | (pt->pe ? 4u : 0) |
           (pt->resetting ? 0x100u : 0) | line << 10 | (pt->pp ? 0x1000u : 0) |
           (!m.cf || pt->released ? 0x2000u : 0);
}
static void model_port(unsigned p, uint32_t v)
{
    struct model_port *pt = &m.port[p];
    assert(!m.bios && !(v & 0xc0) && !(v >> 16 & 15)); /* no suspend, resume or test mode */
    if (v & 2)
        pt->csc = false;
    if (pt->pe && !(v & 4))
        assert(false); /* an enabled port is never disabled */
    if ((m.hcs >> 4 & 1) && (v & 0x1000) && !pt->pp) {
        pt->pp = true;
        if (attached(p))
            pt->connect_at = ticks + m.delay[p];
    }
    /* A port given to a companion is never taken back. */
    if (pt->released)
        assert(v & 0x2000);
    if ((v & 0x2000) && !pt->released) {
        /* §4.2.2: only a device that is not high speed goes to a companion;
         * §2.2.3: none without companion controllers (N_CC 0). */
        assert(m.cf && pt->ccs && m.kind[p] != HIGH && (m.hcs >> 12 & 15));
        pt->released = true;
        pt->ccs = pt->pe = false;
        pt->csc = true;
        ++m.releases; /* gone from this side */
    }
    if ((v & 0x100) && !pt->resetting) {
        /* §2.3.9: PR only while running, with PE written 0; USB 2.0 §7.1.7.3:
         * 100 ms after the connection. Low-speed devices are released instead. */
        assert(m.running && m.cf && pt->pp && pt->ccs && !(v & 4) &&
               ticks - pt->connect_at >= 100 * MS);
        assert(m.kind[p] != LOW);
        pt->resetting = true;
        pt->reset_at = ticks;
        pt->pe = false;
        ++m.port_resets;
    } else if (!(v & 0x100) && pt->resetting) {
        /* USB 2.0 §7.1.7.5: at least 50 ms of root port reset. */
        assert(ticks - pt->reset_at >= 50 * MS);
        pt->resetting = false;
        if (m.kind[p] == HIGH) {
            pt->pe = true;
            pt->enabled_at = ticks;
        }
        if (p == m.at)
            storage_reset();
        if (p == hub.at)
            hub_reset();
    }
}
static void model_tick(void)
{
    if (!m.active)
        return;
    uint32_t v;
    /* Ports and schedules the firmware left may DMA until the reset; while
     * the BIOS owns the controller its command register stays. */
    assert(!m.firmware_running || m.bios || !(config[4] & 4));
    if (m.bios)
        assert((config[4] | config[5] << 8) == 6);
    /* USBLEGSUP: the BIOS releases after the OS Owned Semaphore. */
    if (m.bios && (config[EECP + 3] & 1) && !m.bios_at)
        m.bios_at = m.bios_never ? UINT64_MAX : ticks + 5 * MS;
    if (m.bios && ticks >= m.bios_at) {
        m.bios = false;
        config[EECP + 2] &= ~1u;
    }
    for (unsigned off = 0; off < OP; off += 4)
        assert(!written(off, &v)); /* capability registers */
    if (written(OP, &v)) {
        assert(!m.bios && !(v & (1u << 4 | 1u << 6 |
                                 1u << 7))); /* no periodic schedule, doorbell or light reset */
        if (v & 2) {
            /* §2.3.1: HCRESET only while HCHalted. */
            assert(m.sts >> 12 & 1);
            m.resetting = true;
            m.reset_done_at = m.reset_stuck ? UINT64_MAX : ticks + m.reset_delay;
        } else {
            bool rs = v & 1, ase = v >> 5 & 1;
            if (rs && !(m.cmd & 1)) {
                assert(!m.resetting && !r32(OP + 8)); /* no interrupts */
                assert(!(config[EECP + 4] & 0x3f) && !(config[EECP + 5] & 0xe0)); /* SMIs off */
                m.run_at = ticks + 1;
            }
            if (!rs && (m.cmd & 1))
                m.halt_at = m.halt_stuck ? UINT64_MAX : ticks + 1;
            if (ase != m.ase) {
                /* §4.8: a running controller's ASE changes only when it
                 * equals ASS; the schedule starts at ASYNCLISTADDR. */
                assert(!m.running || m.ase == m.ass);
                if (ase) {
                    assert(rs);
                    model_ring();
                }
                m.ase = ase;
                m.ass_at = ase && m.ass_stuck ? UINT64_MAX : ticks + 1;
            }
        }
        m.cmd = v & ~2u;
    }
    if (written(OP + 4, &v))
        m.sts &= ~(v & 0x3f);
    if (written(OP + 8, &v))
        assert(!v);
    assert(!written(OP + 0xc, &v));
    if (written(OP + 0x10, &v)) {
        assert(wide() && !m.ase && !m.ass);
        m.segment = v;
        shadow[(OP + 0x10) / 4] = v;
    }
    if (written(OP + 0x18, &v)) {
        /* §2.3.7: 32-byte aligned; written while the schedule is off. */
        assert(!(v & 31) && !m.ase && !m.ass);
        m.async = v;
        shadow[(OP + 0x18) / 4] = v;
    }
    if (written(OP + 0x40, &v)) {
        /* All ports go back to the companions only once halted. */
        assert(!m.bios && (v == 1 || (!v && !m.running && (m.sts >> 12 & 1))));
        if (!v)
            m.cf = false;
        else if (!m.cf) {
            m.cf = true;
            for (unsigned p = 1; p <= PORTS; ++p)
                if (m.port[p].pp && attached(p))
                    m.port[p].connect_at = ticks + m.delay[p];
        }
    }
    for (unsigned p = 1; p <= PORTS; ++p)
        if (written(port_off(p), &v))
            model_port(p, v);
    if (m.resetting && ticks >= m.reset_done_at)
        model_hc_reset();
    if (!m.running && (m.cmd & 1) && ticks >= m.run_at) {
        m.running = true;
        m.sts &= ~(1u << 12);
    }
    if (m.running && !(m.cmd & 1) && ticks >= m.halt_at) {
        m.running = m.ass = false;
        m.sts |= 1u << 12;
    }
    if (m.running && m.ase != m.ass && ticks >= m.ass_at)
        m.ass = m.ase;
    for (unsigned p = 1; p <= PORTS; ++p) {
        struct model_port *pt = &m.port[p];
        if (m.cf && pt->pp && !pt->ccs && !pt->released && ticks >= pt->connect_at) {
            pt->ccs = pt->csc = true;
            pt->connect_at = ticks;
        }
    }
    for (unsigned p = 1; p <= PORTS; ++p)
        if (m.leave_at[p] && ticks >= m.leave_at[p]) {
            struct model_port *pt = &m.port[p];
            if (pt->ccs)
                pt->csc = true;
            m.kind[p] = NONE;
            pt->ccs = pt->pe = false;
            pt->connect_at = UINT64_MAX;
            m.leave_at[p] = 0;
            if (m.swap[p]) {
                m.kind[p] = HIGH;
                pt->connect_at = ticks + MS;
            }
        }
    for (unsigned q = 1; q <= HUB_PORTS && hub.at; ++q) {
        struct hub_port *hp = &hub.port[q];
        if (hp->power && hub.kind[q] && !hp->ccs && ticks >= hp->connect_at) {
            hp->ccs = hp->c_connect = true;
            hp->connect_at = ticks;
        }
        if (hp->ccs && hub.bounce_ms[q] && ticks >= hp->connect_at + hub.bounce_ms[q] * MS) {
            hp->ccs = hp->pe = false;
            hp->c_connect = true;
            hp->connect_at = ticks + 2;
            hub.bounce_ms[q] = 0;
        }
        if (hp->resetting && hub.leave[q] && ticks >= hp->reset_at + 5 * MS) {
            hp->resetting = hp->ccs = hp->pe = false;
            hp->c_connect = true;
            hp->c_reset = hub.leave_change;
            hub.leave[q] = false;
            hp->connect_at = hub.back_ms[q] ? ticks + hub.back_ms[q] * MS : UINT64_MAX;
        }
        /* §11.24.2.13: the hub ends its reset with the port enabled and
         * C_PORT_RESET set. */
        if (hp->resetting && !hub.reset_stuck && ticks >= hp->reset_at + hub.reset_ms * MS) {
            hp->resetting = false;
            hp->pe = hp->ccs && !hub.no_enable[q];
            hp->c_reset = true;
            hp->enabled_at = ticks;
            if (q == m.down)
                storage_reset();
        }
    }
    if (m.running && m.ass) {
        for (unsigned i = 0; i < m.qhs; ++i) {
            /* The host leaves the queue heads alone while the schedule runs. */
            uint32_t *q = (uint32_t *)model_dma(m.ring[i], 12);
            assert(!memcmp(q, m.ring_words[i], 12));
        }
        for (unsigned i = 0; i < m.qhs && m.running; ++i)
            model_qh(m.ring[i]);
    }
    w32(OP, m.cmd | (m.resetting ? 2u : 0));
    w32(OP + 4, m.sts | (m.ass ? 1u << 15 : 0));
    w32(OP + 0x10, m.segment);
    w32(OP + 0x18, m.async);
    w32(OP + 0x40, m.cf ? 1u : 0);
    for (unsigned p = 1; p <= PORTS; ++p)
        w32(port_off(p), port_value(p));
}
uint64_t hal_counter_hz(void)
{
    return 1000 * MS;
}
uint64_t hal_counter(void)
{
    if (++ticks == panic_at)
        panicking = true;
    model_tick();
    return ticks;
}

static void setup(void)
{
    memset(&eh, 0, sizeof(eh));
    memset(&block, 0, sizeof(block));
    memset(&m, 0, sizeof(m));
    memset(&hub, 0, sizeof(hub));
    memset(&disk, 0, sizeof(disk));
    disk.io = &bot;
    disk.wait = pause_ms;
    memset(mmio, 0, sizeof(mmio));
    memset(shadow, 0, sizeof(shadow));
    memset(config, 0, sizeof(config));
    memset(visible, 0, sizeof(visible));
    memset(durable, 0, sizeof(durable));
    pool_used = 0;
    ticks = panic_at = 0;
    errors = 0;
    panicking = alloc_fail = false;
    phys = UINT64_C(0x2345600000);
    device.bars[0].io = false;
    device.bars[0].size = BAR;
    /* 4 ports with power control and companions; 64-bit structures, EECP 68h. */
    m.hcs = PORTS | 1u << 4 | 1u << 8 | 2u << 12;
    m.hcc = 1 | EECP << 8;
    m.reset_delay = 5 * MS;
    m.active = true;
    m.bios_owned = true;
    for (unsigned p = 1; p <= PORTS; ++p)
        m.delay[p] = 20 * MS;
    m.at = 2;
    m.kind[2] = HIGH;
    m.capacity = DISK;
    m.wce = true;
    const uint8_t descriptor[18] = {18,   1,    0,    2, 0, 0, 0, 64, 0x34,
                                    0x12, 0x78, 0x56, 0, 1, 0, 0, 0,  1};
    const uint8_t configuration[32] = {9, 2, 32, 0, 1, 1,    0, 0x80, 50, 9,    4,
                                       0, 0, 2,  8, 6, 0x50, 0, 7,    5,  0x81, 2,
                                       0, 2, 0,  7, 5, 0x02, 2, 0,    2,  0};
    memcpy(m.descriptor, descriptor, 18);
    memcpy(m.configuration, configuration, 32);
    m.descriptor_length = 18;
    m.configuration_length = 32;
    config[4] = 6;
}
static void program(void)
{
    w32(0, 0x01000000u | OP);
    w32(4, m.hcs);
    w32(8, m.hcc);
    unsigned at = EECP;
    if (m.other_cap) {
        /* A vendor capability first, then USBLEGSUP. */
        config[EECP] = 0x0a;
        config[EECP + 1] = 0x70;
        at = 0x70;
    }
    m.hcc = (m.hcc & ~0xff00u) | EECP << 8;
    w32(8, m.hcc);
    config[at] = 1;
    config[at + 1] = 0;
    config[at + 2] = m.bios_owned && at == EECP ? 1 : 0;
    config[at + 4] = 0x3f;
    config[at + 5] = 0xe0; /* SMIs enabled */
    m.bios = m.bios_owned;
    if (at != EECP) {
        config[EECP + 2] = 0;
        m.bios = false;
    }
    m.cmd = 0x00080000;
    m.sts = 1u << 12;
    for (unsigned p = 1; p <= PORTS; ++p) {
        m.port[p].connect_at = UINT64_MAX;
        m.port[p].pp = !(m.hcs >> 4 & 1);
    }
    w32(OP, m.cmd);
    w32(OP + 4, m.sts);
    for (unsigned p = 1; p <= PORTS; ++p)
        w32(port_off(p), port_value(p));
}
/* Started with media_log=1; the model then takes the last writes. */
static const struct plog_block *begin(void)
{
    program();
    const struct plog_block *b = amd64_plog_ehci(&device, true);
    (void)hal_counter();
    return b;
}
static void pattern(uint8_t *p, unsigned bytes, unsigned seed)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = (uint8_t)(i * 7 + seed * 13 + 1);
}
/* Halted; the model takes the last writes first. */
static bool halted(void)
{
    (void)hal_counter();
    return !m.running && (m.sts >> 12 & 1) && !(m.cmd & 1);
}
/* Halted with every port still routed to this controller (CONFIGFLAG 1),
 * the medium's not released: it never reaches a companion. */
static bool kept(void)
{
    return halted() && m.cf && !m.port[m.at].released && !m.port[hub.at].released;
}
/* Hot plug on a powered, routed port. */
static void attach(unsigned p, unsigned kind, unsigned ms)
{
    m.kind[p] = kind;
    m.port[p].connect_at = ticks + ms * MS;
}
static void detach(unsigned p)
{
    struct model_port *pt = &m.port[p];
    /* §4.2.2: a disconnection on a companion's port returns it here. */
    if (pt->ccs || pt->released)
        pt->csc = !pt->released;
    m.kind[p] = NONE;
    pt->ccs = pt->pe = pt->released = false;
    pt->connect_at = UINT64_MAX;
}
/* The storage device behind a high-speed hub on root port at: eight
 * ports with per-port power switching, power good after 100 ms, a 15 ms
 * reset; every device connects 30 ms after power good. */
static void hub_on(unsigned at, unsigned down)
{
    m.kind[m.at] = NONE;
    m.at = 0;
    m.down = down;
    m.kind[at] = HIGH;
    hub.at = at;
    hub.ports = 8;
    hub.lpsm = 1;
    hub.good = 50;
    hub.reset_ms = 15;
    hub.type = 0x29;
    hub.mps0 = 64;
    hub.config_length = 25;
    hub.config_type = 2;
    hub.config_value = 1;
    hub.status_length = 4;
    for (unsigned q = 1; q <= HUB_PORTS; ++q) {
        hub.delay[q] = 30 * MS;
        hub.port[q].connect_at = UINT64_MAX;
    }
    hub.kind[down] = HIGH;
}
/* A PCH EHCI: two root ports, no port power control, no companion
 * controllers; the hub on root port 1, the stick on hub port 3. */
static void pch(void)
{
    m.hcs = 2;
    hub_on(1, 3);
}
static void hub_attach(unsigned q, unsigned kind, unsigned ms)
{
    hub.kind[q] = kind;
    if (hub.port[q].power)
        hub.port[q].connect_at = ticks + ms * MS;
}
static void hub_detach(unsigned q)
{
    struct hub_port *hp = &hub.port[q];
    if (hp->ccs)
        hp->c_connect = true;
    hub.kind[q] = NONE;
    hp->ccs = hp->pe = false;
    hp->connect_at = UINT64_MAX;
}
/* The management loop: the watch at least at the times it asks for, and
 * also every millisecond (other duties wake the management CPU). */
static void manage(unsigned ms)
{
    uint64_t end = ticks + ms * MS;
    while (ticks < end) {
        uint64_t next = amd64_plog_ehci_watch(), soon = ticks + MS;
        assert(next == UINT64_MAX || (next > ticks && next <= ticks + 100 * MS));
        while (ticks < next && ticks < soon && ticks < end)
            (void)hal_counter();
    }
}

int main(void)
{
    static uint8_t data[32 * 512], back[32 * 512];
    /* §4.1 from the power-on state, BIOS handoff included. */
    setup();
    const struct plog_block *b = begin();
    assert(b && b->sectors == DISK && m.configured && m.value == 1 && m.address);
    assert(!m.bios && (config[EECP + 3] & 1) && m.port_resets == 1 && !m.releases);
    assert(m.segment == phys >> 32 && (config[4] & 6) == 6 && !r32(OP + 8));
    assert(!m.ase && !m.ass); /* the schedule is off between transfers */
    /* Writes, flushes and reads through one to four qTD pages; the bulk
     * toggles follow every packet. */
    const unsigned counts[] = {1, 2, 7, 8, 9, 16, 17, 31, 32};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        unsigned n = counts[i], lba = 100 + i * 40, syncs = m.syncs;
        pattern(data, n * 512, i);
        assert(b->write(lba, n, data) && !memcmp(visible + lba * 512, data, n * 512));
        assert(memcmp(durable + lba * 512, data, n * 512)); /* in the device's write cache */
        assert(b->flush() && m.syncs == syncs + 1 && !memcmp(durable + lba * 512, data, n * 512));
        memset(back, 0, sizeof(back));
        assert(b->read(lba, n, back) && !memcmp(back, data, n * 512));
    }
    assert(m.max_bytes == 16384);
    for (unsigned i = 0; i < 40; ++i) {
        pattern(data, 512, 50 + i);
        assert(b->write(i, 1, data) && b->read(i, 1, back) && !memcmp(back, data, 512));
    }
    unsigned commands = m.commands;
    assert(!b->write(DISK, 1, data) && !b->read(DISK - 1, 2, back) && !b->write(0, 33, data));
    assert(m.commands == commands);
    /* DPOFUA: FUA writes, no SYNCHRONIZE CACHE. */
    setup();
    m.dpofua = true;
    b = begin();
    assert(b && b->write(4, 1, data) && !memcmp(durable + 4 * 512, data, 512) && b->flush() &&
           !m.syncs);
    /* Without the write cache: no SYNCHRONIZE CACHE either. */
    setup();
    m.wce = false;
    b = begin();
    assert(b && b->write(4, 1, data) && b->flush() && !m.syncs);
    /* Without 64-bit addressing: DMA below 4 GiB, no CTRLDSSEGMENT. */
    setup();
    m.hcc &= ~1u;
    phys = 0x7f000000;
    b = begin();
    assert(b && !m.segment && b->write(9, 16, data) && b->read(9, 16, back) &&
           !memcmp(back, data, 16 * 512));
    /* Without 64-bit addressing and RAM above 4 GiB: refused before the
     * controller gets any schedule pointer. */
    setup();
    m.hcc &= ~1u;
    assert(!begin() && !m.async && !(m.cmd & 1));
    /* No port power control; no BIOS ownership; the device on the last port. */
    setup();
    m.hcs &= ~(1u << 4);
    m.bios_owned = false;
    m.kind[2] = NONE;
    m.at = 4;
    m.kind[4] = HIGH;
    assert(begin());
    /* Connections of their own lengths inside the 1 s window, each reset
     * 100 ms after its own attach (USB 2.0 §7.1.7.3). §4.2.2: low-speed (no
     * reset) and full-speed devices on the other ports go to the companion
     * controllers; the high-speed stick stays. */
    setup();
    m.kind[3] = LOW;
    m.delay[3] = 400 * MS;
    m.kind[1] = FULL;
    m.delay[1] = 700 * MS;
    m.delay[2] = 300 * MS;
    b = begin();
    assert(b && m.releases == 2 && m.port_resets == 2 && m.port[1].released && m.port[3].released);
    assert(!m.port[2].released && m.port[2].pe && b->read(0, 1, back));
    /* The port watch during the guest run: a new full-speed device goes
     * after its debounce and reset, a low-speed one without a reset. */
    unsigned resets = m.port_resets;
    attach(4, FULL, 30);
    manage(400);
    assert(m.port[4].released && m.releases == 3 && m.port_resets == resets + 1);
    detach(4);
    attach(4, LOW, 10);
    manage(300);
    assert(m.port[4].released && m.releases == 4 && m.port_resets == resets + 1);
    /* A high-speed device on another port stays here (the medium moved). */
    detach(4);
    attach(4, HIGH, 10);
    manage(400);
    assert(!m.port[4].released && m.port[4].pe && m.port_resets == resets + 2 && m.releases == 4);
    assert(b->read(0, 1, back));
    /* A replugged low-speed device on a companion's port comes back and goes again. */
    detach(3);
    manage(50);
    attach(3, LOW, 10);
    manage(300);
    assert(m.port[3].released && m.releases == 5);
    /* No port work after a panic. */
    panicking = true;
    detach(3);
    attach(3, LOW, 10);
    manage(300);
    panicking = false;
    assert(!m.port[3].released && m.releases == 5);
    /* A stick attached late in the window: debounced, reset and recovered
     * after the window ends. */
    setup();
    m.delay[2] = 900 * MS;
    assert(begin() && ticks > 1050 * MS);
    /* A connection after the 1 s window is left to the watch. */
    setup();
    m.kind[1] = FULL;
    m.delay[1] = 1500 * MS;
    b = begin();
    assert(b && !m.releases && m.port_resets == 1);
    manage(800);
    assert(m.port[1].released && m.releases == 1 && m.port_resets == 2);
    /* The shutdown hook after the guest run: halted, every port to the
     * companions (CONFIGFLAG 0); once only, never after a panic. */
    setup();
    b = begin();
    assert(b);
    amd64_plog_ehci_shutdown();
    assert(halted() && !m.cf && amd64_plog_ehci_watch() == UINT64_MAX);
    amd64_plog_ehci_shutdown();
    assert(!m.cf && !b->read(0, 1, back));
    setup();
    b = begin();
    assert(b);
    panicking = true;
    amd64_plog_ehci_shutdown();
    panicking = false;
    assert(m.cf && !halted());
    /* USBLEGSUP after another extended capability. */
    setup();
    m.other_cap = true;
    assert(begin() && (config[0x73] & 1));
    /* The firmware left the controller running: bus mastering waits for
     * the halt and the reset. */
    setup();
    program();
    m.firmware_running = true;
    m.cmd = 0x00080021;
    m.sts = 1u << 15;
    m.running = m.ase = m.ass = true;
    m.cf = true;
    m.async = 0x7e000000;
    w32(OP, m.cmd);
    w32(OP + 4, m.sts | 1u << 15);
    w32(OP + 0x18, m.async);
    w32(OP + 0x40, 1);
    m.qhs = 0;
    b = amd64_plog_ehci(&device, true);
    assert(b);

    /* Behind a high-speed hub on a root port (USB 2.0 ch. 11), as on a PCH
     * whose EHCI ports all sit behind an integrated rate-matching hub and
     * that has no companion controllers. The hub and the medium get their
     * own addresses; every hub port is powered and, after bPwrOn2PwrGood,
     * each connection is reset 100 ms after its own attach (the model
     * checks order and timings). Full- and low-speed devices behind the hub
     * would need split transactions: they stay there, never addressed. */
    setup();
    pch();
    hub.kind[1] = FULL;
    hub.kind[5] = LOW;
    hub.delay[5] = 600 * MS;
    m.kind[2] = FULL;
    b = begin();
    assert(b && b->sectors == DISK && hub.configured && hub.address && m.configured && m.address);
    assert(m.address != hub.address && hub.port[3].pe && !m.releases && m.port_resets == 2 &&
           !errors);
    for (unsigned q = 1; q <= hub.ports; ++q)
        assert(hub.port[q].power);
    pattern(data, 32 * 512, 21);
    assert(b->write(300, 32, data) && b->flush() && b->read(300, 32, back) &&
           !memcmp(back, data, 32 * 512));
    /* Without companions nothing is routed, so there is no port watch: a
     * device plugged into the other root port or into a hub port during
     * the guest run is not reset. */
    assert(amd64_plog_ehci_watch() == UINT64_MAX);
    unsigned hub_resets = hub.resets;
    detach(2);
    attach(2, LOW, 10);
    hub_attach(7, HIGH, 10);
    manage(400);
    assert(m.port_resets == 2 && !m.releases && hub.resets == hub_resets && !hub.port[7].pe &&
           b->read(0, 1, back));
    /* The stick moved to another hub port: the next transfer fails and the
     * backend stops; the moved stick is not reset and every port stays. */
    hub_detach(3);
    m.down = 6;
    hub_attach(6, HIGH, 10);
    manage(300);
    uint64_t start = ticks;
    assert(!b->write(0, 1, data) && ticks - start < 1000 * MS && m.cf && !hub.port[6].pe);
    assert(!b->read(0, 1, back) && !b->flush() && hub.resets == hub_resets);
    amd64_plog_ehci_shutdown();
    assert(m.cf && !m.releases);
    /* The hub removed during the run: the same. */
    setup();
    pch();
    b = begin();
    assert(b);
    detach(1);
    start = ticks;
    assert(!b->write(0, 1, data) && ticks - start < 1000 * MS && m.cf && !b->read(0, 1, back));
    /* The shutdown hook after the guest run: halted, CONFIGFLAG 0. */
    setup();
    pch();
    b = begin();
    assert(b);
    amd64_plog_ehci_shutdown();
    assert(halted() && !m.cf);
    /* Ganged power switching (one request powers every port), none
     * (powered with the configuration), short and long power-good times. */
    setup();
    pch();
    hub.lpsm = 0;
    hub.good = 100;
    assert(begin());
    setup();
    pch();
    hub.lpsm = 2;
    hub.good = 0;
    assert(begin());
    setup();
    pch();
    hub.good = 255;
    assert(begin());
    /* Connections at their own times: the stick late in the hub's 1 s
     * window, after the other devices' resets; a device attached after the
     * window is not reset. */
    setup();
    pch();
    hub.delay[3] = 900 * MS;
    hub.kind[4] = FULL;
    hub.delay[4] = 200 * MS;
    hub.kind[2] = FULL;
    hub.delay[2] = 1500 * MS;
    b = begin();
    manage(600);
    assert(b && hub.port[4].pe && b->read(0, 1, back) && hub.port[2].ccs && !hub.port[2].pe);
    /* The hub on a controller with companions: a low-speed device on
     * another root port still goes to a companion. */
    setup();
    hub_on(2, 4);
    m.kind[3] = LOW;
    b = begin();
    assert(b && m.releases == 1 && m.port[3].released && !m.port[2].released &&
           b->read(0, 1, back));
    /* A hub and a stick both on root ports: two high-speed devices there,
     * refused before any request. */
    setup();
    hub_on(1, 3);
    hub.kind[3] = NONE;
    m.down = 0;
    m.at = 2;
    m.kind[2] = HIGH;
    assert(!begin() && kept() && !m.requests && !hub.requests);
    /* Two high-speed devices behind the hub: refused before any request to
     * either (both would answer address 0). */
    setup();
    pch();
    hub.kind[6] = HIGH;
    assert(!begin() && kept() && !m.requests);
    /* No high-speed device behind the hub: a full-speed one is refused and
     * stays; without any device the controller only reports it. */
    setup();
    pch();
    hub.kind[3] = FULL;
    m.down = 0;
    assert(!begin() && kept() && !m.requests);
    setup();
    pch();
    hub.kind[3] = NONE;
    m.down = 0;
    program();
    assert(!amd64_plog_ehci(&device, false) && kept() && !hub.resets && !errors);
    setup();
    pch();
    hub.kind[3] = NONE;
    m.down = 0; /* with media_log=1: an error */
    assert(!begin() && kept() && errors);
    setup();
    pch();
    hub.kind[3] = LOW;
    m.down = 0;
    program();
    assert(!amd64_plog_ehci(&device, false) && kept() && !errors);
    /* A medium behind the hub that fails its enumeration is an error. */
    setup();
    pch();
    m.descriptor_length = 8;
    assert(!begin() && kept() && !m.configured && errors);
    /* One hub level only. */
    setup();
    pch();
    m.descriptor[4] = 9;
    assert(!begin() && kept() && !m.configured && !m.commands);
    /* Broken hubs: refused, bounded, the controller halted with its ports. */
    setup();
    pch();
    hub.descriptor_stall = true;
    assert(!begin() && kept() && !hub.powers);
    setup();
    pch();
    hub.descriptor_length = 6;
    assert(!begin() && kept() && !hub.powers);
    setup();
    pch();
    hub.type = 0x22;
    assert(!begin() && kept() && !hub.powers);
    setup();
    pch();
    hub.ports = 0;
    program();
    assert(!amd64_plog_ehci(&device, false) && kept() && !hub.powers && errors);
    setup();
    pch();
    hub.length_field = 8; /* bDescLength without DeviceRemovable */
    assert(!begin() && kept() && !hub.powers);
    setup();
    pch();
    hub.ports = 16; /* more than the backend tracks */
    assert(!begin() && kept() && !hub.powers);
    setup();
    pch();
    hub.mps0 = 8;
    assert(!begin() && kept() && !hub.address);
    setup();
    pch();
    hub.status_stall = true;
    program(); /* an error even without media_log=1 */
    assert(!amd64_plog_ehci(&device, false) && kept() && !hub.resets && !m.requests && errors);
    setup();
    pch();
    hub.reset_stuck = true;
    assert(!begin() && kept() && ticks < 4000 * MS && !m.requests);
    setup();
    pch();
    hub.config_type = 4;
    assert(!begin() && kept() && !hub.configured);
    setup();
    pch();
    hub.config_length = 8;
    assert(!begin() && kept() && !hub.configured);
    setup();
    pch();
    hub.status_length = 2;
    assert(!begin() && kept() && !hub.resets);
    /* The configuration value of the hub's descriptor; fifteen hub ports. */
    setup();
    pch();
    hub.config_value = 2;
    assert(begin() && hub.configured);
    setup();
    pch();
    hub.ports = 15;
    hub.kind[3] = NONE;
    m.down = 15;
    hub.kind[15] = HIGH;
    assert(begin() && hub.port[15].pe);
    /* USB 2.0 §7.1.7.3: a disconnection during the debounce (reported by
     * C_PORT_CONNECTION, here too short to be seen otherwise) restarts it;
     * the model checks the 100 ms after the last connection. */
    setup();
    pch();
    hub.bounce_ms[3] = 60;
    hub.kind[2] = FULL;
    hub.bounce_ms[2] = 80;
    assert(begin());
    /* A device that leaves during its reset and stays gone is no device. */
    setup();
    pch();
    hub.kind[3] = NONE;
    m.down = 0;
    hub.kind[2] = FULL;
    hub.leave[2] = true;
    program();
    assert(!amd64_plog_ehci(&device, false) && kept() && !errors && hub.resets == 1);
    /* The stick leaves during its reset, which the hub reports as ended
     * (C_PORT_RESET), and comes back within the window; the hub starts the
     * second reset 3 ms after the request, so only a C_PORT_RESET reported
     * after that request ends the wait. */
    setup();
    pch();
    hub.latency_ms = 3;
    hub.leave[3] = true;
    hub.leave_change = true;
    hub.back_ms[3] = 50;
    assert(begin() && hub.resets == 2);
    /* A port that its reset leaves disabled (over-current, say) is not
     * reported as a full-speed device. */
    setup();
    pch();
    hub.no_enable[3] = true;
    assert(!begin() && kept() && errors && !strstr(last_error, "full-speed"));
    setup();
    pch();
    hub.kind[3] = FULL;
    m.down = 0;
    assert(!begin() && kept() && strstr(last_error, "full-speed"));
    /* Full-speed devices on the other root ports go to the companions only
     * once the medium works: with a hub that holds no medium, or a medium
     * without a BOT interface, a full-speed stick on another root port
     * stays; a port whose device left by then is not handed over. */
    setup();
    hub_on(1, 3);
    hub.kind[3] = NONE;
    m.down = 0;
    m.kind[2] = FULL;
    assert(!begin() && kept() && !m.releases);
    setup();
    hub_on(1, 3);
    hub.kind[3] = FULL;
    m.down = 0;
    m.kind[2] = FULL;
    assert(!begin() && kept() && !m.releases);
    setup();
    hub_on(1, 3);
    m.configuration[16] = 0x62;
    m.kind[2] = FULL;
    assert(!begin() && kept() && !m.releases);
    setup();
    m.configuration[16] = 0x62;
    m.kind[1] = FULL;
    assert(!begin() && kept() && !m.releases);
    setup();
    hub_on(1, 3);
    m.kind[2] = FULL;
    m.kind[4] = LOW;
    b = begin();
    assert(b && m.releases == 2 && m.port[2].released && m.port[4].released && b->read(0, 1, back));
    setup();
    hub_on(1, 3);
    m.kind[2] = FULL;
    m.leave_at[2] = 1500 * MS;
    assert(begin() && !m.releases && !m.port[2].released);
    setup();
    hub_on(1, 3);
    m.kind[2] = FULL;
    m.leave_at[2] = 1500 * MS;
    m.swap[2] = true;
    assert(begin() && !m.releases && !m.port[2].released);
    /* A full-speed hub on a root port, the stick behind it: a full-speed
     * device there, refused and kept; the low-speed device still goes. */
    setup();
    m.kind[2] = FULL;
    m.kind[4] = LOW;
    assert(!begin() && kept() && m.releases == 1 && m.port[4].released && !m.port[2].released);

    /* Broken controllers and devices: bounded, the controller halted. */
    setup();
    m.bios_never = true;
    assert(!begin() && ticks >= 1000 * MS && ticks < 1200 * MS && !(m.cmd & 1) && !m.resetting);
    setup();
    m.reset_stuck = true;
    assert(!begin() && ticks < 3000 * MS && !(config[4] & 4));
    setup();
    m.kind[2] = NONE;
    assert(!begin() && ticks >= 1000 * MS && ticks < 1300 * MS && kept() && errors);
    /* Two high-speed devices: refused; only the low-speed device goes, the
     * full-speed one may be the medium and stays. */
    setup();
    m.kind[3] = HIGH;
    m.kind[1] = FULL;
    m.kind[4] = LOW;
    assert(!begin() && kept() && !m.requests && m.releases == 1 && m.port[4].released &&
           !m.port[1].released);
    /* A medium that links at full speed is refused, not released. */
    setup();
    m.kind[2] = FULL;
    assert(!begin() && !m.releases && m.port_resets == 1 && kept());
    setup();
    m.kind[2] = LOW;
    assert(!begin() && m.releases == 1 && !m.port_resets && halted() && m.cf);
    setup();
    m.descriptor[4] = 9; /* the hub class without a hub descriptor */
    assert(!begin() && kept() && !m.commands);
    setup();
    m.descriptor[7] = 8;
    assert(!begin() && kept());
    setup();
    m.configuration[22] = 0x40;
    m.configuration[23] = 0;
    assert(!begin() && kept() && !m.configured);
    setup();
    m.descriptor_length = 8; /* a short device descriptor */
    assert(!begin() && kept() && !m.configured);
    setup();
    m.configuration_length = 25; /* a short configuration */
    assert(!begin() && kept() && !m.configured);
    setup();
    m.configuration[16] = 0x62; /* UAS only */
    assert(!begin() && kept());
    setup();
    m.wp = true; /* a write-protected medium */
    assert(!begin() && kept() && !m.writes);
    setup();
    m.ass_stuck = true;
    assert(!begin() && ticks < 5000 * MS && kept());
    setup();
    alloc_fail = true;
    assert(!begin() && !(m.cmd & 1));
    setup();
    memset(mmio, 0xff, sizeof(mmio));
    m.active = false;
    assert(!amd64_plog_ehci(&device, true) && ticks < 100 * MS);
    setup();
    device.bars[0].io = true;
    assert(!begin() && !m.resetting && !(config[EECP + 3] & 1));
    setup();
    device.bars[0].size = OP + 0x44; /* no room for the ports */
    assert(!begin() && !(config[EECP + 3] & 1));
    setup();
    program();
    config[EECP + 1] = 0x20; /* a next pointer into the header */
    assert(!amd64_plog_ehci(&device, true));

    /* A transaction error on a bulk OUT: BOT reset recovery (§5.3.4), both
     * pipes cleared and DATA0 again; that command fails, the next works. */
    setup();
    b = begin();
    assert(b);
    m.fault.opcode = 0x2a;
    m.fault.phase = DATA_OUT;
    m.fault.kind = FAULT_XACT;
    assert(!b->write(0, 4, data) && m.bot_resets == 1 && m.clears == 2);
    pattern(data, 2048, 77);
    assert(b->write(0, 4, data) && b->read(0, 4, back) && !memcmp(back, data, 2048));
    /* A STALL in the data stage: CLEAR_FEATURE(ENDPOINT_HALT) on that pipe,
     * then the CSW (BOT §6.7.2); the next command works. */
    m.fault.opcode = 0x28;
    m.fault.phase = DATA_IN;
    m.fault.kind = FAULT_STALL;
    unsigned clears = m.clears;
    assert(!b->read(0, 4, back) && m.clears == clears + 1 && m.bot_resets == 1);
    assert(b->read(0, 4, back) && !memcmp(back, data, 2048));
    /* Fewer bytes than the CSW residue accounts for: phase error and reset
     * recovery (BOT §6.7). */
    m.fault.opcode = 0x28;
    m.fault.phase = DATA_IN;
    m.fault.kind = FAULT_SHORT;
    assert(!b->read(0, 4, back) && m.bot_resets == 2);
    assert(b->read(0, 4, back) && !memcmp(back, data, 2048));
    /* A device that NAKs the CSW forever: bounded, the backend stops and the
     * controller halts. */
    setup();
    b = begin();
    assert(b);
    m.fault.opcode = 0x2a;
    m.fault.phase = CSW;
    m.fault.kind = FAULT_NAK;
    uint64_t at = ticks;
    assert(!b->write(0, 1, data) && ticks - at >= 30000 * MS && ticks - at < 33000 * MS && kept());
    commands = m.commands;
    at = ticks;
    assert(!b->write(0, 1, data) && !b->read(0, 1, back) && !b->flush() && m.commands == commands &&
           ticks == at);
    /* The watch on the halted controller: no reset; a full-speed device
     * stays, a low-speed one goes. The shutdown hook does nothing. */
    resets = m.port_resets;
    attach(3, FULL, 10);
    manage(400);
    assert(!m.port[3].released && m.port_resets == resets);
    attach(4, LOW, 10);
    manage(300);
    assert(m.port[4].released && m.port_resets == resets);
    amd64_plog_ehci_shutdown();
    assert(kept());
    /* Host System Error: the controller halted itself. */
    setup();
    b = begin();
    assert(b);
    m.fault.opcode = 0x2a;
    m.fault.phase = CBW;
    m.fault.kind = FAULT_HSE;
    at = ticks;
    assert(!b->write(0, 1, data) && ticks - at < 1000 * MS && kept());
    assert(!b->read(0, 1, back));
    /* A controller that is gone: all-ones registers. */
    setup();
    b = begin();
    assert(b);
    m.active = false;
    memset(mmio, 0xff, sizeof(mmio));
    at = ticks;
    assert(!b->write(0, 1, data) && ticks - at < 100 * MS);
    /* A panic during the wait: return at once, the controller untouched. */
    setup();
    b = begin();
    assert(b);
    m.fault.opcode = 0x2a;
    m.fault.phase = CSW;
    m.fault.kind = FAULT_NAK;
    panic_at = ticks + 50 * MS;
    at = ticks;
    assert(!b->write(0, 1, data) && ticks - at < 1000 * MS && m.running);
    /* After a VMM panic no transfer reaches the device. */
    setup();
    b = begin();
    assert(b);
    panicking = true;
    commands = m.commands;
    unsigned requests = m.requests;
    assert(!b->write(0, 1, data) && !b->read(0, 1, back) && !b->flush());
    assert(m.commands == commands && m.requests == requests && !m.ase);
    puts("PLOG EHCI: passed");
}
