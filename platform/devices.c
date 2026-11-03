/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform/platform.h"
#include "platform/vbios_trace.h"
#include "ia64/state.h"
#include "core/log_internal.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/engine.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/pci.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"
#include "persist/persist.h"
#include "platform/storage.h"
#include "platform/ahci.h"
#include "platform/i8042.h"

static uint64_t width_mask(unsigned size)
{
    return size == 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
}
static uint64_t port_read(void *, struct vcpu *, uint64_t, unsigned);
static void port_write(void *, struct vcpu *, uint64_t, uint64_t, unsigned);
static unsigned service_port_width(const struct vcpu *v)
{
    unsigned width = v ? v->service_port >> 16 : 0;
    return width == 1 || width == 2 || width == 4 ? width : 0;
}
static uint64_t service_port_offset(const struct vcpu *v)
{
    unsigned p = v->service_port & 0xffff;
    return ((uint64_t)(p >> 2) << 12) | (p & 0xfff);
}

/* service_lock held. The first request decides; a
 * later one, from any vCPU or source, only ends its vCPU's slice. */
static void request_reset(struct platform_state *s, struct vcpu *v, uint64_t type, unsigned source,
                          uint64_t status)
{
    if (!s->reset_requested) {
        s->reset_requested = true;
        s->reset_type = type;
        s->reset_source = source;
        s->reset_status = status;
    } else
        LOGI("guest reset request type=%llu source=%s after an earlier one: ignored",
             (unsigned long long)type, platform_reset_source_name(source));
    if (v)
        vcpu_request_exit(v, VCPU_EXIT_SHUTDOWN);
}
/* Port reset requests (ACPI reset register, CF9h, i8042):
 * a cold restart. None of them reaches the host. */
static void request_restart(struct platform_state *s, struct vcpu *v, unsigned source,
                            uint16_t port, uint64_t value)
{
    LOGI("guest reset request source=%s port=%04x value=%02llx", platform_reset_source_name(source),
         (unsigned)port, (unsigned long long)value);
    platform_lock(&s->service_lock);
    request_reset(s, v, 0, source, 0);
    platform_unlock(&s->service_lock);
}
const char *platform_reset_source_name(unsigned source)
{
    static const char *const names[] = {"efi", "acpi", "cf9", "i8042"};
    return source < IA64VMM_ARRAY_SIZE(names) ? names[source] : "unknown";
}
bool platform_reset_restarts(const struct platform_state *s)
{
    uint64_t type = s->reset_type;
    return s->reset_requested && !s->panic && (type == 0 || type == 1 || type == 3);
}

static uint64_t service_read_unlocked(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    struct platform_state *s = opaque;
    if ((off & 7) + size > 8)
        return 0;
    uint64_t value = 0;
    switch (off & ~UINT64_C(7)) {
    case IA64VMM_SVC_MAGIC:
        value = IA64VMM_SVC_MAGIC_VALUE;
        break;
    case IA64VMM_SVC_VERSION:
        value = 1;
        break;
    case IA64VMM_SVC_RTC:
        value = platform_rtc_seconds();
        break;
    case IA64VMM_SVC_ITC_HZ:
        value = IA64VMM_ITC_HZ;
        break;
    case IA64VMM_SVC_TR_STATUS:
        value = ia64_pal_tr_status(v);
        break;
    case IA64VMM_SVC_TR_VALID:
        value = ia64_pal_tr_valid(v);
        break;
    case IA64VMM_SVC_DEBUG_FLAGS:
        value = platform_debug_flags();
        break;
    case IA64VMM_SVC_PAL_XR0:
    case IA64VMM_SVC_PAL_EXPECTED:
    case IA64VMM_SVC_PAL_PALE_PMI:
    case IA64VMM_SVC_PAL_SALE_PMI:
    case IA64VMM_SVC_PAL_EVENT:
    case IA64VMM_SVC_PAL_PMI_VECTOR:
        value = ia64_pal_service_read(v, (unsigned)(off & ~UINT64_C(7)));
        break;
    case IA64VMM_SVC_PORT_DATA:
        value = service_port_width(v)
                    ? port_read(s, v, service_port_offset(v), service_port_width(v))
                    : UINT32_MAX;
        break;
    default:
        break;
    }
    return (value >> ((off & 7) * 8)) & width_mask(size);
}
static uint64_t service_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    if ((off & ~UINT64_C(7)) == IA64VMM_SVC_PORT_DATA)
        return service_read_unlocked(opaque, v, off, size);
    platform_lock(&((struct platform_state *)opaque)->service_lock);
    uint64_t result = service_read_unlocked(opaque, v, off, size);
    platform_unlock(&((struct platform_state *)opaque)->service_lock);
    return result;
}

static void service_write_unlocked(void *opaque, struct vcpu *v, uint64_t off, uint64_t value,
                                   unsigned size)
{
    struct platform_state *s = opaque;
    value &= width_mask(size);
    switch (off) {
    case IA64VMM_SVC_PORT_SELECT:
        if (v && size == 8)
            v->service_port = (value & ~UINT64_C(0x7ffff)) ? 0 : (uint32_t)value;
        break;
    case IA64VMM_SVC_PORT_DATA:
        if (size == 8 && service_port_width(v))
            port_write(s, v, service_port_offset(v), value, service_port_width(v));
        break;
    case IA64VMM_SVC_CONSOLE: {
        char c = (char)value;
        vmm_log_guest_text(&c, 1);
        break;
    }
    case IA64VMM_SVC_WATCHDOG:
        s->watchdog = value;
        LOGD("firmware watchdog %llu sec (recorded)", (unsigned long long)value);
        break;
    case IA64VMM_SVC_GOP_OWNED:
        s->guest_gop_owned = value != 0;
        break;
    case IA64VMM_SVC_RESET_STATUS:
        s->pending_reset_status = value;
        break;
    case IA64VMM_SVC_RESET: {
        unsigned slot = 0;
        uint64_t ip = v ? vcpu_instruction_pointer(v, &slot) : 0;
        LOGI("EFI ResetSystem type=%llu status=%llx ip=%llx slot=%u", (unsigned long long)value,
             (unsigned long long)s->pending_reset_status, (unsigned long long)ip, slot);
        request_reset(s, v, value, PLATFORM_RESET_EFI, s->pending_reset_status);
        break;
    }
    case IA64VMM_SVC_HALT:
        if (v)
            vcpu_request_exit(v, VCPU_EXIT_HALT);
        break;
    case IA64VMM_SVC_TR_SELECT:
        if (size == 8)
            ia64_pal_tr_select(v, value);
        break;
    case IA64VMM_SVC_TR_READ:
        if (size == 8)
            ia64_pal_tr_read(v, value);
        break;
    case IA64VMM_SVC_PAL_XR0:
    case IA64VMM_SVC_PAL_EXPECTED:
    case IA64VMM_SVC_PAL_PALE_PMI:
    case IA64VMM_SVC_PAL_SALE_PMI:
    case IA64VMM_SVC_PAL_EVENT:
    case IA64VMM_SVC_PAL_PMI_VECTOR:
    case IA64VMM_SVC_PAL_STATE_SAVE:
    case IA64VMM_SVC_PAL_STATE_RESUME:
        if (size == 8)
            ia64_pal_service_write(v, (unsigned)off, value);
        break;
    case IA64VMM_SVC_PANIC:
        s->panic = true;
        s->panic_code = value;
        vmm_log_fb_enable(true);
        LOGE("firmware panic %016llx", (unsigned long long)value);
        if (v) {
            vcpu_dump(v);
            vcpu_request_exit(v, VCPU_EXIT_SHUTDOWN);
        }
        break;
    case IA64VMM_SVC_MAP_SIZE:
        if (size == 8)
            s->map_size = value;
        break;
    case IA64VMM_SVC_LOAD_BASE:
        if (size == 8)
            s->load_base = value;
        break;
    case IA64VMM_SVC_LOAD_SIZE:
        if (size == 8)
            s->load_size = value;
        break;
    case IA64VMM_SVC_LOAD_TYPE:
        if (size == 8 && (value == 1 || value == 2) && s->load_size &&
            platform_ram_contains(s->load_base, s->load_size)) {
            if (s->loader_range_count < IA64VMM_ARRAY_SIZE(s->loader_ranges))
                s->loader_ranges[s->loader_range_count++] =
                    (struct ia64vmm_ram_range){s->load_base, s->load_size};
            else
                s->preboot_overflow = true;
            LOGI("EFI loader allocation base=%llx size=%llx type=%llu",
                 (unsigned long long)s->load_base, (unsigned long long)s->load_size,
                 (unsigned long long)value);
        }
        break;
    case IA64VMM_SVC_IMAGE_BASE:
        if (size == 8)
            s->image_base = value;
        break;
    case IA64VMM_SVC_IMAGE_SIZE:
        if (size == 8 && value && platform_ram_contains(s->image_base, value)) {
            if (s->image_range_count < PLATFORM_IMAGE_RANGES)
                s->image_ranges[s->image_range_count++] =
                    (struct ia64vmm_ram_range){s->image_base, value};
            else
                s->preboot_overflow = true;
        }
        LOGI("EFI loaded image base=%llx size=%llx", (unsigned long long)s->image_base,
             (unsigned long long)value);
        break;
    case IA64VMM_SVC_EXIT_BOOT_SERVICES:
        if (size != 8 || s->boot_services_exited)
            break;
        s->map_key = value;
        s->boot_services_exited = s->handoff_pending = s->ebs_exit_requested = true;
        LOGI("EFI ExitBootServices succeeded map_key=%llu map_size=%llu", (unsigned long long)value,
             (unsigned long long)s->map_size);
        if (v)
            vcpu_request_exit(v, VCPU_EXIT_DEBUG);
        break;
    case IA64VMM_SVC_DEBUG:
        LOGI("firmware debug %016llx", (unsigned long long)value);
        break;
    case IA64VMM_SVC_INPUT_KEY:
        if (size == 8)
            LOGI("EFI input key scan=%04x unicode=%04x source=%s",
                 (unsigned)((value >> 16) & 0xffff), (unsigned)(value & 0xffff),
                 value & IA64VMM_INPUT_KEY_EVENT ? "event" : "read");
        break;
    case IA64VMM_SVC_CONSOLE_FLUSH:
        if (size == 8)
            log_flush_guest_text();
        break;
    default:
        LOGW("ignored service write offset=%llx size=%u", (unsigned long long)off, size);
        break;
    }
}
static void nvram_committed(struct platform_state *);
static void service_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    if (off == IA64VMM_SVC_PORT_SELECT || off == IA64VMM_SVC_PORT_DATA) {
        service_write_unlocked(opaque, v, off, value, size);
        return;
    }
    /* A completed FVB update; nvram_lock orders before service_lock. */
    if (off == IA64VMM_SVC_NVRAM_COMMIT) {
        struct platform_state *s = opaque;
        platform_lock(&s->nvram_lock);
        nvram_committed(s);
        platform_unlock(&s->nvram_lock);
        return;
    }
    platform_lock(&((struct platform_state *)opaque)->service_lock);
    service_write_unlocked(opaque, v, off, value, size);
    platform_unlock(&((struct platform_state *)opaque)->service_lock);
}

/* nvram_lock held: the window holds a completed update, the one written
 * to the VMM medium at the end of the run. */
static void nvram_committed(struct platform_state *s)
{
    nvram_stage(s->nvram);
    platform_lock(&s->service_lock);
    ++s->commits;
    LOGD("NVRAM commit %llu", (unsigned long long)s->commits);
    platform_unlock(&s->service_lock);
}

static uint64_t nvram_read_unlocked(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    (void)v;
    struct platform_state *s = opaque;
    if (off >= IA64VMM_NVRAM_SIZE || size > IA64VMM_NVRAM_SIZE - off)
        return width_mask(size);
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= (uint64_t)s->nvram[off + i] << (i * 8);
    return value;
}
static uint64_t nvram_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    platform_lock(&((struct platform_state *)opaque)->nvram_lock);
    uint64_t result = nvram_read_unlocked(opaque, v, off, size);
    platform_unlock(&((struct platform_state *)opaque)->nvram_lock);
    return result;
}

static void nvram_write_unlocked(void *opaque, struct vcpu *v, uint64_t off, uint64_t value,
                                 unsigned size)
{
    (void)v;
    struct platform_state *s = opaque;
    if (off >= IA64VMM_NVRAM_SIZE || size > IA64VMM_NVRAM_SIZE - off)
        return;
    for (unsigned i = 0; i < size; ++i)
        s->nvram[off + i] = value >> (i * 8);
}
static void nvram_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    platform_lock(&((struct platform_state *)opaque)->nvram_lock);
    nvram_write_unlocked(opaque, v, off, value, size);
    platform_unlock(&((struct platform_state *)opaque)->nvram_lock);
}

bool platform_decode_port(uint64_t off, uint16_t *port)
{
    if (off >= IA64VMM_LEGACY_IO_SIZE)
        return false;
    uint16_t p = (uint16_t)((off >> 12) << 2) | (off & 3);
    if ((((uint64_t)p >> 2) << 12 | (p & 0xfff)) != off)
        return false;
    *port = p;
    return true;
}

bool platform_allow_ports(struct platform_state *s, uint16_t first, unsigned count)
{
    if (!count || count > 65536u - first)
        return false;
    for (unsigned p = first; p < (unsigned)first + count; ++p)
        s->allowed_ports[p / 8] |= 1u << (p & 7);
    return true;
}

static bool allowed(struct platform_state *s, uint16_t port, unsigned size)
{
    if (size > 4 || size > 65536u - port)
        return false;
    for (unsigned i = 0; i < size; ++i)
        if (!(s->allowed_ports[(port + i) / 8] & (1u << ((port + i) & 7))))
            return false;
    return true;
}

static void denied(struct platform_state *s, uint16_t port)
{
    unsigned bit = 1u << (port & 7);
    if (__atomic_fetch_or(&s->denied_ports[port / 8], bit, __ATOMIC_RELAXED) & bit)
        return;
    LOGD("unassigned guest port %04x", (unsigned)port);
}

static uint32_t pm_timer(void)
{
    uint64_t hz = hal_counter_hz(), now = hal_counter();
    if (!hz || hz > UINT64_MAX / 3579545)
        return 0;
    return ((now / hz) * 3579545 + (now % hz) * 3579545 / hz) & 0xffffff;
}

static uint8_t pm_byte(struct platform_state *s, unsigned off)
{
    if (off < 2)
        return s->pm_status >> (off * 8);
    if (off < 4)
        return s->pm_enable >> ((off - 2) * 8);
    if (off < 6)
        return s->pm_control >> ((off - 4) * 8);
    if (off >= 8 && off < 12)
        return pm_timer() >> ((off - 8) * 8);
    return 0;
}

static bool crosses_pm(uint16_t port, unsigned size)
{
    unsigned start = IA64VMM_ACPI_PM_PORT_BASE;
    unsigned end = start + IA64VMM_ACPI_PM_PORT_SIZE;
    return port < end && (unsigned)port + size > start;
}

bool platform_config_ports(uint16_t port, unsigned size)
{
    return port < 0xd00 && (unsigned)port + size > 0xcf8;
}
/* PCI Local Bus 3.0 §3.2.2.3.2 configuration mechanism #1 over the mediated
 * configuration space. Only a doubleword access at CF8h is CONFIG_ADDRESS;
 * other accesses to CF8h-CFBh are not configuration accesses. The guest's port bridge and the VBIOS container
 * share it; the container passes `visible` to restrict the functions its
 * ROM may reach. */
static unsigned cf8_lock;
static bool selected(uint32_t latch, platform_config_filter visible, void *ctx)
{
    return (latch & 0x80000000u) && (!visible || visible(ctx, (uint16_t)((latch >> 8) & 0xffff)));
}
static uint64_t platform_config_port_read_unlocked(uint32_t *latch, uint16_t port, unsigned size,
                                                   platform_config_filter visible, void *ctx)
{
    if (size > 4 || (port & (size - 1)))
        return width_mask(size);
    if (port >= 0xcf8 && port < 0xcfc)
        return port == 0xcf8 && size == 4 ? *latch : width_mask(size);
    if (port < 0xcfc || port + size > 0xd00 || !selected(*latch, visible, ctx))
        return width_mask(size);
    uint64_t off = ((uint64_t)((*latch >> 8) & 0xffff) << 12) | (*latch & 0xfc) | (port - 0xcfc);
    return platform_pci_read(off, size);
}
uint64_t platform_config_port_read(uint32_t *latch, uint16_t port, unsigned size,
                                   platform_config_filter visible, void *ctx)
{
    platform_lock(&cf8_lock);
    uint64_t result = platform_config_port_read_unlocked(latch, port, size, visible, ctx);
    platform_unlock(&cf8_lock);
    return result;
}
static void platform_config_port_write_unlocked(uint32_t *latch, uint16_t port, uint64_t value,
                                                unsigned size, platform_config_filter visible,
                                                void *ctx)
{
    if (size > 4 || (port & (size - 1)))
        return;
    if (port >= 0xcf8 && port < 0xcfc) {
        if (port == 0xcf8 && size == 4)
            *latch = (uint32_t)value;
    } else if (port >= 0xcfc && port + size <= 0xd00 && selected(*latch, visible, ctx)) {
        uint64_t off =
            ((uint64_t)((*latch >> 8) & 0xffff) << 12) | (*latch & 0xfc) | (port - 0xcfc);
        platform_pci_write(off, value, size);
    }
}
void platform_config_port_write(uint32_t *latch, uint16_t port, uint64_t value, unsigned size,
                                platform_config_filter visible, void *ctx)
{
    platform_lock(&cf8_lock);
    platform_config_port_write_unlocked(latch, port, value, size, visible, ctx);
    platform_unlock(&cf8_lock);
}

/* Opt-in port_trace=N[:FIRST-LAST] (bring-up): log the first N legacy
 * port accesses in the port range (default all) from an instruction in a
 * nonzero virtual region (OS code; firmware and loaders run identity-
 * mapped), whether forwarded to a guest-owned host port or denied. */
static void trace_port(struct platform_state *s, struct vcpu *v, const char *what, uint16_t port,
                       unsigned size, uint64_t value)
{
    (void)vbios_guest_trace_port(v, port, size, what[0] == 'o', (uint32_t)value);
    unsigned slot = 0;
    if (!s->port_trace || port < s->port_trace_first || port > s->port_trace_last)
        return;
    uint64_t ip = v ? vcpu_instruction_pointer(v, &slot) : 0;
    if (!(ip >> 61))
        return;
    --s->port_trace;
    LOGW("port_trace: %s port=%04x size=%u value=%llx ip=%llx:%u", what, (unsigned)port, size,
         (unsigned long long)value, (unsigned long long)ip, slot);
}

/* The keyboard controller's output port bit 0 drives the host's reset line
 * (IBM PC AT keyboard controller: D1h writes the next 60h byte to the
 * output port; F0h-FFh pulse output port bits 3:0 whose command bit is 0).
 * Neither reaches the host; one that would assert reset restarts the guest. */
static bool i8042_consumed(struct platform_state *s, struct vcpu *v, uint16_t port, uint8_t value)
{
    if (port == 0x64) {
        s->i8042_output_pending = value == 0xd1;
        if (value != 0xd1 && value < 0xf0)
            return false;
        if (value != 0xd1 && !(value & 1))
            request_restart(s, v, PLATFORM_RESET_I8042, port, value);
        else
            LOGD("i8042 command %02x not forwarded", (unsigned)value);
        return true;
    }
    if (port != 0x60 || !s->i8042_output_pending)
        return false;
    s->i8042_output_pending = false;
    if (!(value & 1))
        request_restart(s, v, PLATFORM_RESET_I8042, port, value);
    else
        LOGD("i8042 output port %02x not forwarded", (unsigned)value);
    return true;
}

static uint64_t port_read_unlocked(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    (void)v;
    struct platform_state *s = opaque;
    uint16_t port;
    if (!platform_decode_port(off, &port))
        return width_mask(size);
    uint32_t sequence;
    if (vbios_guest_trace_sequence(v, port, size, &sequence))
        return sequence;
    if (port >= IA64VMM_ACPI_PM_PORT_BASE &&
        port - IA64VMM_ACPI_PM_PORT_BASE + size <= IA64VMM_ACPI_PM_PORT_SIZE) {
        uint64_t value = 0;
        for (unsigned i = 0; i < size; ++i)
            value |= (uint64_t)pm_byte(s, port - IA64VMM_ACPI_PM_PORT_BASE + i) << (i * 8);
        return value;
    }
    if (crosses_pm(port, size)) {
        denied(s, port);
        return width_mask(size);
    }
    if (platform_config_ports(port, size))
        return platform_config_port_read(&s->pci_config_address, port, size, NULL, NULL);
    if (platform_i8042_configured() && (port == 0x60 || port == 0x64))
        return size == 1 ? platform_i8042_read(port) : width_mask(size);
    if (allowed(s, port, size)) {
        uint64_t value = hal_port_in(port, size);
        if (port == 0x60 && size == 1)
            LOGD("i8042 data=%02x", (unsigned)value);
        trace_port(s, v, "in", port, size, value);
        return value;
    }
    trace_port(s, v, "in-denied", port, size, width_mask(size));
    denied(s, port);
    return width_mask(size);
}
static uint64_t port_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    struct platform_state *s = opaque;
    uint16_t port;
    unsigned *lock =
        platform_decode_port(off, &port) && crosses_pm(port, size) ? &s->pm_lock : &s->port_lock;
    platform_lock(lock);
    uint64_t result = port_read_unlocked(opaque, v, off, size);
    platform_unlock(lock);
    return result;
}

static void port_write_unlocked(void *opaque, struct vcpu *v, uint64_t off, uint64_t value,
                                unsigned size)
{
    struct platform_state *s = opaque;
    uint16_t port;
    if (!platform_decode_port(off, &port))
        return;
    if (port == 0x80 && vbios_guest_trace_port(v, port, size, true, (uint32_t)value))
        return;
    if (port >= IA64VMM_ACPI_PM_PORT_BASE &&
        port - IA64VMM_ACPI_PM_PORT_BASE + size <= IA64VMM_ACPI_PM_PORT_SIZE) {
        unsigned at = port - IA64VMM_ACPI_PM_PORT_BASE;
        for (unsigned i = 0; i < size; ++i) {
            unsigned byte = at + i;
            uint16_t data = (value >> (i * 8)) & 0xff;
            unsigned shift = (byte & 1) * 8;
            if (byte < 2)
                s->pm_status &= ~(data << shift);
            else if (byte < 4)
                s->pm_enable = (s->pm_enable & ~(0xffu << shift)) | (data << shift);
            else if (byte < 6)
                s->pm_control = (s->pm_control & ~(0xffu << shift)) | (data << shift);
        }
        if (at <= 5 && at + size > 4 && (s->pm_control & 0x2000)) {
            s->pm_control &= ~0x2000u;
            /* The fork's ACPI core powers off for SLP_TYP=0 (S4 value 0). */
            unsigned type = (s->pm_control >> 10) & 7;
            if (type == 0) {
                platform_lock(&s->service_lock);
                request_reset(s, v, 2, PLATFORM_RESET_ACPI, 0);
                platform_unlock(&s->service_lock);
            }
        }
        if (at == 12 && size == 1 && (value & 0xff) == 1)
            request_restart(s, v, PLATFORM_RESET_ACPI, port, value & 0xff);
        return;
    }
    if (crosses_pm(port, size)) {
        denied(s, port);
        return;
    }
    if (port == 0xcf9 && size == 1) {
        if (value & 4)
            request_restart(s, v, PLATFORM_RESET_CF9, port, value);
        return;
    }
    if (platform_config_ports(port, size)) {
        platform_config_port_write(&s->pci_config_address, port, value, size, NULL, NULL);
        return;
    }
    if (platform_i8042_configured() && (port == 0x60 || port == 0x64)) {
        if (size == 1) {
            (void)i8042_consumed(s, v, port, (uint8_t)value);
            platform_i8042_write(port, (uint8_t)value);
        }
        return;
    }
    if (allowed(s, port, size) && size == 1 && i8042_consumed(s, v, port, (uint8_t)value))
        return;
    if (allowed(s, port, size)) {
        trace_port(s, v, "out", port, size, value);
        hal_port_out(port, value, size);
    } else {
        trace_port(s, v, "out-denied", port, size, value);
        denied(s, port);
    }
}
static void port_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    struct platform_state *s = opaque;
    uint16_t port;
    unsigned *lock =
        platform_decode_port(off, &port) && crosses_pm(port, size) ? &s->pm_lock : &s->port_lock;
    platform_lock(lock);
    port_write_unlocked(opaque, v, off, value, size);
    platform_unlock(lock);
}

static uint64_t ecam_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    (void)opaque;
    (void)v;
    return platform_pci_read(off, size);
}
static void ecam_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    (void)opaque;
    (void)v;
    platform_pci_write(off, value, size);
}

static uint64_t iosapic_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    struct platform_state *s = opaque;
    __atomic_store_n(&s->iosapic_logged, true, __ATOMIC_RELAXED);
    return platform_iosapic_ops.read(&s->iosapic, v, off, size);
}
static void iosapic_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    struct platform_state *s = opaque;
    __atomic_store_n(&s->iosapic_logged, true, __ATOMIC_RELAXED);
    platform_iosapic_ops.write(&s->iosapic, v, off, value, size);
}
static uint64_t pib_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    struct platform_state *s = opaque;
    __atomic_store_n(&s->pib_logged, true, __ATOMIC_RELAXED);
    return platform_pib_ops.read(NULL, v, off, size);
}
static void pib_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    struct platform_state *s = opaque;
    __atomic_store_n(&s->pib_logged, true, __ATOMIC_RELAXED);
    platform_pib_ops.write(NULL, v, off, value, size);
}

static void keyboard_irq(void *context, bool asserted)
{
    platform_iosapic_set_pin(&((struct platform_state *)context)->iosapic, 1, asserted);
}

bool platform_devices_init(struct platform_state *s)
{
    static const struct gm_mmio_ops service = {service_read, service_write};
    static const struct gm_mmio_ops nvram = {nvram_read, nvram_write};
    static const struct gm_mmio_ops ports = {port_read, port_write};
    static const struct gm_mmio_ops ecam = {ecam_read, ecam_write};
    static const struct gm_mmio_ops iosapic = {iosapic_read, iosapic_write};
    static const struct gm_mmio_ops pib = {pib_read, pib_write};
    memset(s, 0, sizeof(*s));
    if (!platform_iosapic_init(&s->iosapic, IA64VMM_IOSAPIC_GSI_BASE, IA64VMM_IOSAPIC_GSI_COUNT))
        return false;
    /* ACPI 6.5 §5.2.9 SCI_INT / §4.8.2.1.3: the SCI is a shareable, level,
     * active-low input, so its idle electrical level is high. The PM block
     * raises no SCI events yet; an idle-low pin would be permanently asserted
     * once a guest programs it level/active-low. */
    s->iosapic.input[IA64VMM_SCI_GSI - IA64VMM_IOSAPIC_GSI_BASE] = true;
    s->pm_control = 1; /* VPC ACPI-only reset state: SCI_EN. */
    s->nvram = hal_alloc_pages(IA64VMM_NVRAM_SIZE / 4096, HAL_PAGE_ZERO);
    if (!s->nvram)
        return false;
    const struct gm_region regions[] = {
        {IA64VMM_SERVICE_BASE, IA64VMM_SERVICE_SIZE, GM_EMULATED, "service", NULL, &service, s},
        {IA64VMM_NVRAM_BASE, IA64VMM_NVRAM_SIZE, GM_EMULATED, "nvram", NULL, &nvram, s},
        {IA64VMM_LEGACY_IO_BASE, IA64VMM_LEGACY_IO_SIZE, GM_EMULATED, "legacy-io", NULL, &ports, s},
        {IA64VMM_ECAM_BASE, IA64VMM_ECAM_SIZE, GM_EMULATED, "ecam", NULL, &ecam, s},
        {IA64VMM_IOSAPIC_BASE, IA64VMM_IOSAPIC_SIZE, GM_EMULATED, "iosapic", NULL, &iosapic, s},
        {IA64VMM_PIB_BASE, IA64VMM_PIB_SIZE, GM_EMULATED, "pib", NULL, &pib, s},
    };
    /* Registration has no rollback API: validate capacity before the first
     * insertion. Window collisions have already been checked by core. */
    if (gm_region_count() >
        GM_MAX_REGIONS - IA64VMM_ARRAY_SIZE(regions) - (platform_debug_uart_present() ? 1u : 0u))
        return false;
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(regions); ++i)
        if (!gm_add_region(&regions[i]))
            return false;
    if (platform_debug_uart_present()) {
        const struct gm_region uart = {IA64VMM_UART_BASE,
                                       IA64VMM_UART_RUNTIME_SIZE,
                                       GM_EMULATED,
                                       "debug-uart",
                                       NULL,
                                       &platform_uart_ops,
                                       s};
        if (!gm_add_region(&uart))
            return false;
    }
    if (platform_i8042_configured() && !platform_i8042_init(keyboard_irq, s))
        return false;
    return platform_pci_map(s) && platform_storage_init(s) && platform_vga_map(s) &&
           platform_vbios_map(s) && platform_abort_init(s);
}

void platform_devices_restart(struct platform_state *s)
{
    static struct platform_state next;
    memset(&next, 0, sizeof(next));
    next.nvram = s->nvram;
    next.commits = s->commits;
    next.iosapic_logged = s->iosapic_logged;
    next.pib_logged = s->pib_logged;
    memcpy(next.allowed_ports, s->allowed_ports, sizeof(next.allowed_ports));
    memcpy(next.denied_ports, s->denied_ports, sizeof(next.denied_ports));
    /* The ranges point at themselves and at s: copied back to the same place. */
    next.abort_log = s->abort_log;
    memcpy(next.abort_ranges, s->abort_ranges, sizeof(next.abort_ranges));
    next.abort_range_count = s->abort_range_count;
    next.abort_regions_seen = s->abort_regions_seen;
    *s = next;
    platform_iosapic_init(&s->iosapic, IA64VMM_IOSAPIC_GSI_BASE, IA64VMM_IOSAPIC_GSI_COUNT);
    s->iosapic.input[IA64VMM_SCI_GSI - IA64VMM_IOSAPIC_GSI_BASE] = true;
    s->pm_control = 1;
    platform_pci_restart();
    if (platform_i8042_configured())
        platform_i8042_reset();
    if (platform_ahci_present()) {
        s->iosapic.input[PLATFORM_STORAGE_GSI] = true;
        platform_ahci_reset();
    }
}

void platform_devices_cleanup(struct platform_state *s)
{
    platform_i8042_cleanup();
    platform_ahci_cleanup();
    platform_vbios_cleanup();
    platform_abort_cleanup(s);
    platform_pci_cleanup();
    hal_free_pages(s->nvram, IA64VMM_NVRAM_SIZE / 4096);
    s->nvram = NULL;
}
