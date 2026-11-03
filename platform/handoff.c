/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform/platform.h"
#include "platform/i8042.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/pci.h"
#include "ia64vmm/log.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"

#define FW_DATA_SIZE (32ULL * 1024 * 1024)
#define LOW_LIMIT 0x100000000ULL

static bool overlaps(uint64_t base, uint64_t size, uint64_t other, uint64_t length)
{
    return size && length && base < other + length && other < base + size;
}

bool platform_check_window(const struct boot_info *bi)
{
    for (unsigned i = 0; i < bi->mem_range_count; ++i) {
        const struct boot_mem_range *r = &bi->mem[i];
        if (r->size > UINT64_MAX - r->base ||
            overlaps(r->base, r->size, IA64VMM_WINDOW_BASE, IA64VMM_WINDOW_SIZE) ||
            (r->type != BOOT_MEM_RESERVED &&
             overlaps(r->base, r->size, IA64VMM_PIB_BASE, IA64VMM_PIB_SIZE))) {
            LOGE("host memory overlaps the emulation window or overflows");
            return false;
        }
    }
    for (unsigned i = 0; i < bi->module_count; ++i) {
        const struct boot_module *m = &bi->modules[i];
        if (m->size > UINT64_MAX - m->hpa ||
            overlaps(m->hpa, m->size, IA64VMM_WINDOW_BASE, IA64VMM_WINDOW_SIZE) ||
            overlaps(m->hpa, m->size, IA64VMM_PIB_BASE, IA64VMM_PIB_SIZE))
            return false;
    }
    if (bi->fb.size > UINT64_MAX - bi->fb.base ||
        overlaps(bi->fb.base, bi->fb.size, IA64VMM_WINDOW_BASE, IA64VMM_WINDOW_SIZE) ||
        overlaps(bi->fb.base, bi->fb.size, IA64VMM_PIB_BASE, IA64VMM_PIB_SIZE))
        return false;
    for (unsigned i = 0; i < gm_region_count(); ++i) {
        const struct gm_region *r = gm_region_at(i);
        if (overlaps(r->base, r->size, IA64VMM_WINDOW_BASE, IA64VMM_WINDOW_SIZE) ||
            overlaps(r->base, r->size, IA64VMM_PIB_BASE, IA64VMM_PIB_SIZE)) {
            LOGE("guest region %s overlaps the emulation window", r->name);
            return false;
        }
    }
    return true;
}

bool platform_ram_contains(uint64_t base, uint64_t size)
{
    if (!size || size > UINT64_MAX - base)
        return false;
    while (size) {
        const struct gm_region *r = gm_lookup(base);
        if (!r || r->kind != GM_RAM)
            return false;
        uint64_t n = r->size - (base - r->base);
        if (n > size)
            n = size;
        size -= n;
        base += n;
    }
    return true;
}

bool platform_ram_write(uint64_t base, const void *src, uint64_t size)
{
    if (!platform_ram_contains(base, size) || size > SIZE_MAX)
        return false;
    const uint8_t *p = src;
    while (size) {
        const struct gm_region *r = gm_lookup(base);
        uint64_t n = r->size - (base - r->base);
        if (n > size)
            n = size;
        if (!gm_write(base, p, n))
            return false;
        base += n;
        p += n;
        size -= n;
    }
    return true;
}

bool platform_find_ram(uint64_t size, uint64_t alignment, uint64_t limit,
                       const struct ia64vmm_ram_range *avoid, unsigned count, bool highest,
                       uint64_t *base)
{
    if (!size || !alignment || (alignment & (alignment - 1)))
        return false;
    bool found = false;
    uint64_t selected = 0;
    for (unsigned i = 0; i < gm_region_count(); ++i) {
        const struct gm_region *r = gm_region_at(i);
        if (r->kind != GM_RAM)
            continue;
        uint64_t lo = r->base, hi = r->base + r->size;
        while (i + 1 < gm_region_count()) {
            r = gm_region_at(i + 1);
            if (r->kind != GM_RAM || r->base != hi)
                break;
            hi += r->size;
            ++i;
        }
        if (hi > limit)
            hi = limit;
        if (hi <= lo || size > hi - lo || lo > UINT64_MAX - (alignment - 1))
            continue;
        uint64_t at =
            highest ? IA64VMM_ALIGN_DOWN(hi - size, alignment) : IA64VMM_ALIGN_UP(lo, alignment);
        for (;;) {
            if (at < lo || at > hi || size > hi - at)
                break;
            bool conflict = false;
            for (unsigned j = 0; j < count; ++j) {
                if (avoid[j].size > UINT64_MAX - avoid[j].base)
                    return false;
                if (!overlaps(at, size, avoid[j].base, avoid[j].size))
                    continue;
                if (highest) {
                    if (avoid[j].base < size)
                        goto next_range;
                    at = IA64VMM_ALIGN_DOWN(avoid[j].base - size, alignment);
                } else {
                    uint64_t end = avoid[j].base + avoid[j].size;
                    if (end > UINT64_MAX - (alignment - 1))
                        goto next_range;
                    at = IA64VMM_ALIGN_UP(end, alignment);
                }
                conflict = true;
                break;
            }
            if (conflict)
                continue;
            if (!found || (highest ? at > selected : at < selected))
                selected = at;
            found = true;
            break;
        }
    next_range:;
    }
    if (found)
        *base = selected;
    return found;
}

bool platform_cmdline_value(const char *line, const char *key, const char **value, size_t *length)
{
    size_t n = strlen(key);
    while (*line) {
        while (*line == ' ' || *line == '\t')
            ++line;
        const char *end = line;
        while (*end && *end != ' ' && *end != '\t')
            ++end;
        if ((size_t)(end - line) > n && !strncmp(line, key, n) && line[n] == '=') {
            *value = line + n + 1;
            *length = (size_t)(end - *value);
            return true;
        }
        line = end;
    }
    return false;
}

static uint32_t fb_mask(unsigned size, unsigned shift)
{
    if (!size || size > 32 || shift >= 32 || size > 32 - shift)
        return 0;
    return (uint32_t)((UINT64_C(1) << size) - 1) << shift;
}

bool platform_handoff_ram_append(struct ia64vmm_handoff *h, uint64_t base, uint64_t size)
{
    if (!size || (base | size) & 4095 || size > UINT64_MAX - base ||
        h->ram_range_count > IA64VMM_HANDOFF_MAX_RAM)
        return false;
    unsigned n = h->ram_range_count;
    if (n && h->ram[n - 1].base + h->ram[n - 1].size == base)
        h->ram[n - 1].size += size;
    else {
        if (n == IA64VMM_HANDOFF_MAX_RAM) {
            LOGE("handoff RAM range capacity exceeded: need >%u ranges", IA64VMM_HANDOFF_MAX_RAM);
            return false;
        }
        h->ram[n] = (struct ia64vmm_ram_range){base, size};
        ++h->ram_range_count;
    }
    return true;
}
bool platform_handoff_ram(struct ia64vmm_handoff *h)
{
    h->ram_range_count = 0;
    for (unsigned i = 0; i < gm_region_count(); ++i) {
        const struct gm_region *r = gm_region_at(i);
        if (r->kind == GM_RAM && !platform_handoff_ram_append(h, r->base, r->size))
            return false;
    }
    LOGI("handoff RAM ranges=%u/%u (guest regions=%u)", h->ram_range_count, IA64VMM_HANDOFF_MAX_RAM,
         gm_region_count());
    return true;
}

bool platform_handoff(const struct boot_info *bi, const struct fw_image *fw,
                      struct ia64vmm_handoff *h, uint64_t *gpa, struct ia64_entry_regs *regs)
{
    return platform_handoff_model(bi, fw, IA64_CPU_MONTECITO, h, gpa, regs);
}

bool platform_handoff_model(const struct boot_info *bi, const struct fw_image *fw,
                            enum ia64_cpu_model model, struct ia64vmm_handoff *h, uint64_t *gpa,
                            struct ia64_entry_regs *regs)
{
    if (!ia64_cpu_model_valid(model) || !platform_pci_inventory(bi->cmdline))
        return false;
    memset(h, 0, sizeof(*h));
    h->magic = IA64VMM_HANDOFF_MAGIC;
    h->version = IA64VMM_HANDOFF_VERSION;
    h->size = sizeof(*h);
    ia64vmm_handoff_cpu_model(h, model);
    h->flags = IA64VMM_HF_SHUTDOWN_ON_BOOT_RETURN | IA64VMM_HF_NO_BOOT_MENU_WAIT;
    if (!platform_debug_policy(bi->cmdline, h))
        return false;
    const char *value;
    size_t length;
    if (platform_cmdline_value(bi->cmdline, "bootmenu", &value, &length) && length == 1 &&
        *value == '1')
        h->flags &= ~IA64VMM_HF_NO_BOOT_MENU_WAIT;
    /* Intel hold-and-restore probe for real-hardware photographs. */
    if (platform_cmdline_value(bi->cmdline, "vga_probe", &value, &length)) {
        uint32_t seconds = 0;
        for (size_t i = 0; i < length && seconds <= 3600; ++i) {
            if (value[i] < '0' || value[i] > '9') {
                seconds = UINT32_MAX;
                break;
            }
            seconds = seconds * 10 + (uint32_t)(value[i] - '0');
        }
        if (!length || seconds > 3600) {
            LOGE("vga_probe must be 0..3600 seconds");
            return false;
        }
        h->vga_probe_sec = seconds;
    }
    /* One processor per placed vCPU;
     * core rejects a count outside 1..IA64VMM_MAX_CPUS, the firmware a count
     * above its own limit. */
    h->cpu_count = hal_vcpu_count();
    h->itc_hz = IA64VMM_ITC_HZ;
    if (!platform_handoff_ram(h))
        return false;
    if (!platform_ram_contains(fw->base, fw->size))
        return false;
    h->firmware_base = fw->base;
    h->firmware_size = fw->size;
    struct ia64vmm_ram_range avoid[2] = {{fw->base, fw->size}};
    uint64_t data;
    /* Highest 64 KiB-aligned 32 MiB fit below 4 GiB, excluding the firmware.
     * The rule works with fragmented identity RAM and needs no low-RAM table. */
    if (!platform_find_ram(FW_DATA_SIZE, 65536, LOW_LIMIT, avoid, 1, true, &data)) {
        LOGE("no 32 MiB firmware data span below 4 GiB");
        return false;
    }
    h->fw_data_base = data;
    h->fw_data_size = FW_DATA_SIZE;
    avoid[1] = (struct ia64vmm_ram_range){data, FW_DATA_SIZE};
    for (unsigned i = 0; i < bi->module_count; ++i) {
        const struct boot_module *m = &bi->modules[i];
        if (strncmp(m->name, "bootdisk", sizeof(m->name)))
            continue;
        uint64_t image_base;
        /* EFI describes RAM in whole 8 KiB
         * IA-64 granules and reserves the
         * image inside them: place it on a granule with the granule-rounded
         * span free. At 4 KiB, a RAM range ending on an odd 4 KiB boundary
         * can leave the image's last page undescribed and invalidate the
         * firmware descriptor reservation. */
        if (!m->hva || !m->size || m->size > SIZE_MAX || m->size > UINT64_MAX - 8191 ||
            !platform_find_ram(IA64VMM_ALIGN_UP(m->size, 8192), 8192, LOW_LIMIT, avoid, 2, true,
                               &image_base) ||
            !platform_ram_write(image_base, m->hva, m->size))
            return false;
        h->boot_image_base = image_base;
        h->boot_image_size = m->size;
        h->boot_image_kind = m->size >= 2 && m->hva[0] == 'M' && m->hva[1] == 'Z'
                                 ? IA64VMM_BOOT_PE_IMAGE
                                 : IA64VMM_BOOT_DISK_IMAGE;
        break;
    }
    h->legacy_io_base = IA64VMM_LEGACY_IO_BASE;
    h->legacy_io_size = IA64VMM_LEGACY_IO_SIZE;
    h->service_base = IA64VMM_SERVICE_BASE;
    h->service_size = IA64VMM_SERVICE_SIZE;
    h->nvram_base = IA64VMM_NVRAM_BASE;
    h->nvram_size = IA64VMM_NVRAM_SIZE;
    h->ecam_base = IA64VMM_ECAM_BASE;
    h->ecam_size = IA64VMM_ECAM_SIZE;
    h->ecam_bus_last = 255;
    if (!platform_pci_handoff(h))
        return false;
    if (platform_i8042_configured())
        h->flags |= IA64VMM_HF_I8042_ENABLED | IA64VMM_HF_I8042_KEYBOARD_ONLY;
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *d = platform_pci_device(i);
        if (d->owner == PCI_OWNER_GUEST && d->class_code >> 8 == 0x0601)
            h->flags |= IA64VMM_HF_I8042_ENABLED;
    }
    h->iosapic_base = IA64VMM_IOSAPIC_BASE;
    h->iosapic_gsi_base = IA64VMM_IOSAPIC_GSI_BASE;
    h->iosapic_gsi_count = IA64VMM_IOSAPIC_GSI_COUNT;
    h->pib_base = IA64VMM_PIB_BASE;
    h->uart_base = h->flags & IA64VMM_HF_UART_PRESENT ? IA64VMM_UART_BASE : 0;
    h->acpi_pm_port_base = IA64VMM_ACPI_PM_PORT_BASE;
    h->acpi_pm_port_size = IA64VMM_ACPI_PM_PORT_SIZE;
    const struct boot_framebuffer *fb = &bi->fb;
    if (fb->format != BOOT_FB_NONE && fb->bpp == 32 && fb->width && fb->height &&
        fb->pitch >= (uint64_t)fb->width * 4 && fb->size >= (uint64_t)fb->pitch * fb->height) {
        h->fb_red_mask = fb_mask(fb->red_size, fb->red_shift);
        h->fb_green_mask = fb_mask(fb->green_size, fb->green_shift);
        h->fb_blue_mask = fb_mask(fb->blue_size, fb->blue_shift);
        if (!h->fb_red_mask || !h->fb_green_mask || !h->fb_blue_mask ||
            (h->fb_red_mask & h->fb_green_mask) || (h->fb_red_mask & h->fb_blue_mask) ||
            (h->fb_green_mask & h->fb_blue_mask))
            return false;
        h->fb_reserved_mask = ~(h->fb_red_mask | h->fb_green_mask | h->fb_blue_mask);
        h->fb_base = fb->base;
        h->fb_size = fb->size;
        h->fb_width = fb->width;
        h->fb_height = fb->height;
        h->fb_pitch = fb->pitch;
        h->fb_format =
            h->fb_red_mask == 0xff0000 && h->fb_green_mask == 0xff00 && h->fb_blue_mask == 0xff
                ? IA64VMM_FB_BGRX8888
            : h->fb_red_mask == 0xff && h->fb_green_mask == 0xff00 && h->fb_blue_mask == 0xff0000
                ? IA64VMM_FB_RGBX8888
                : IA64VMM_FB_BITMASK;
    }
    if (!platform_vga_handoff(fb, h))
        return false;
    if (!platform_vbios_handoff(h))
        return false;
    if (h->vga_probe_sec)
        LOGI("vga_probe=%u kind=%u legacy_vga=%u", h->vga_probe_sec, h->vga_kind,
             !!(h->flags & IA64VMM_HF_LEGACY_VGA));
    /* fw_data+1 MiB leaves room for the base-relative low tables. Entry stack
     * and RSE use the fork's assist offsets (6 MiB and 16 MiB). */
    *gpa = data + 0x100000;
    if (!platform_ram_write(*gpa, h, sizeof(*h)))
        return false;
    platform_entry_regs(h, *gpa, fw->entry, fw->global_pointer, 0, regs);
    LOGI("handoff=%llx fw_data=%llx boot=%llx (%u)", (unsigned long long)*gpa,
         (unsigned long long)data, (unsigned long long)h->boot_image_base, h->boot_image_kind);
    return true;
}

/* Each CPU gets a separate entry stack and backing store in firmware data. */
void platform_entry_regs(const struct ia64vmm_handoff *h, uint64_t handoff_gpa, uint64_t entry,
                         uint64_t gp, unsigned cpu, struct ia64_entry_regs *regs)
{
    memset(regs, 0, sizeof(*regs));
    regs->ip = regs->br0 = entry;
    regs->gr[1] = gp;
    regs->gr[8] = handoff_gpa;
    regs->gr[9] = sizeof(*h);
    regs->gr[10] = IA64VMM_PLATFORM_ID;
    regs->gr[12] = h->fw_data_base + IA64VMM_ENTRY_STACK_TOP_OFFSET - 16 -
                   (uint64_t)cpu * IA64VMM_ENTRY_STACK_STRIDE;
    regs->bsp = regs->bspstore =
        h->fw_data_base + IA64VMM_ENTRY_RSE_OFFSET + (uint64_t)cpu * IA64VMM_ENTRY_RSE_STRIDE;
    regs->k0 = IA64VMM_LEGACY_IO_BASE;
}
