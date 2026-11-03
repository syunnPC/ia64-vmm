/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "limine.h"
#include "host/amd64/boot_diag.h"
#include "boot/acpi.h"
#include "boot/ranges.h"
#include "boot/media.h"
#include "ia64vmm/pci.h"
#if !defined(IA64VMM_AMD64_HOST_ONLY)
#include "platform/platform.h"
#include "host/amd64/plog_ahci.h"
#include "host/amd64/plog_ehci.h"
#include "host/amd64/plog_nvme.h"
#include "host/amd64/plog_xhci.h"
#include "host/amd64/plog_hyperv.h"
#include "host/amd64/hyperv.h"
#include "host/amd64/hyperv_keyboard.h"
#include "platform/i8042.h"
#endif
#include "host/amd64/amd64.h"
#include "host/amd64/simd.h"
#include "host/fb/fb_console.h"
#include "host/amd64/topology.h"
#include "core/log_internal.h"
#include "core/options.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define REQUEST __attribute__((used, section(".limine_requests")))
__attribute__((used, section(".limine_requests_start"))) static volatile uint64_t requests_start[] =
    LIMINE_REQUESTS_START_MARKER;
REQUEST static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(6);
REQUEST static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .flags = LIMINE_MP_REQUEST_X86_64_X2APIC,
};
REQUEST static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID};
REQUEST static volatile struct limine_hhdm_request hhdm_request = {.id = LIMINE_HHDM_REQUEST_ID};
REQUEST static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID};
REQUEST static volatile struct limine_module_request modules_request = {
    .id = LIMINE_MODULE_REQUEST_ID};
REQUEST static volatile struct limine_rsdp_request rsdp_request = {.id = LIMINE_RSDP_REQUEST_ID};
REQUEST static volatile struct limine_smbios_request smbios_request = {
    .id = LIMINE_SMBIOS_REQUEST_ID};
REQUEST static volatile struct limine_efi_system_table_request efi_request = {
    .id = LIMINE_EFI_SYSTEM_TABLE_REQUEST_ID};
REQUEST static volatile struct limine_executable_address_request address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID};
REQUEST static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID};
REQUEST static volatile struct limine_paging_mode_request paging_request = {
    .id = LIMINE_PAGING_MODE_REQUEST_ID,
    .mode = LIMINE_PAGING_MODE_X86_64_4LVL,
    .max_mode = LIMINE_PAGING_MODE_X86_64_4LVL,
    .min_mode = LIMINE_PAGING_MODE_X86_64_4LVL,
};
__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t requests_end[] =
    LIMINE_REQUESTS_END_MARKER;
static struct boot_info boot;

static const void *media_map(uint64_t pa, size_t size, void *context)
{
    const struct limine_memmap_response *map = context;
    if (!size || pa > UINT64_MAX - size || pa + size > UINT64_MAX - amd64_hhdm)
        return NULL;
    for (uint64_t i = 0; i < map->entry_count; ++i) {
        const struct limine_memmap_entry *m = map->entries[i];
        if (m->type == LIMINE_MEMMAP_RESERVED || m->type == LIMINE_MEMMAP_BAD_MEMORY ||
            m->type > LIMINE_MEMMAP_RESERVED_MAPPED)
            continue;
        if (pa >= m->base && pa - m->base < m->length && size <= m->length - (pa - m->base))
            return (const void *)(uintptr_t)(pa + amd64_hhdm);
    }
    return NULL;
}

static void copy_string(char *dest, size_t capacity, const char *source)
{
    if (!source)
        source = "";
    size_t len = strlen(source);
    if (len >= capacity)
        hal_panic("Limine string exceeds BootInfo capacity");
    memcpy(dest, source, len + 1);
}
#if !defined(IA64VMM_AMD64_HOST_ONLY)
static const uint8_t *smbios_table(uint64_t address, uint64_t length)
{
    (void)length;
    return (const uint8_t *)(uintptr_t)(address + amd64_hhdm);
}
#endif

/* The options module overrides the entry's cmdline (core/options.h).
 * "options-media" selects only media selectors for safe mode.
 * No module leaves the entry's line unchanged. */
static void merge_options(const char *entry)
{
    const struct limine_module_response *mods = modules_request.response;
    const char *file = "";
    size_t file_len = 0;
    bool have = false, media_only = false;
    for (uint64_t i = 0; mods && i < mods->module_count; ++i) {
        const struct limine_file *m = mods->modules[i];
        if (!m->string || (strcmp(m->string, OPTIONS_MODULE_NAME) &&
                           strcmp(m->string, OPTIONS_MEDIA_MODULE_NAME)))
            continue;
        if (have)
            hal_panic("more than one Limine options module");
        file = m->address;
        file_len = m->size;
        have = true;
        media_only = !strcmp(m->string, OPTIONS_MEDIA_MODULE_NAME);
    }
    unsigned tokens = 0;
    enum options_status status =
        media_only
            ? options_merge_media(file, file_len, entry, boot.cmdline, sizeof(boot.cmdline),
                                  &tokens)
            : options_merge(file, file_len, entry, boot.cmdline, sizeof(boot.cmdline), &tokens);
    if (status == OPTIONS_OVERFLOW)
        hal_panic(
            "options module plus entry command line exceed the 1023-byte BootInfo command line");
    if (status == OPTIONS_BAD_BYTE)
        hal_panic("options module holds a non-ASCII or control byte");
    if (!have)
        return;
    LOGI("%s module: %u token(s) from vmm-options.conf (%llu byte(s)); effective command line:",
         media_only ? OPTIONS_MEDIA_MODULE_NAME : OPTIONS_MODULE_NAME, tokens,
         (unsigned long long)file_len);
    for (const char *s = boot.cmdline;;) {
        char piece[128]; /* the formatter has no precision */
        size_t n = options_chunk(s, 120);
        memcpy(piece, s, n);
        piece[n] = 0;
        LOGI("  cmdline: %s", piece);
        s += n;
        while (*s == ' ')
            ++s;
        if (!*s)
            break;
    }
}
static unsigned requested_vcpus(const char *line);
static uint64_t reservation_size(void)
{
    uint64_t mib = 128;
    const char *s = boot.cmdline;
    while (*s) {
        while (*s == ' ' || *s == '\t')
            ++s;
        const char *end = s;
        while (*end && *end != ' ' && *end != '\t')
            ++end;
        if (end - s >= 12 && !strncmp(s, "reserve_mib=", 12)) {
            const char *p = s + 12;
            mib = 0;
            if (p == end)
                hal_panic("reserve_mib needs a positive decimal value");
            for (; p < end; ++p) {
                if (*p < '0' || *p > '9' || mib > (UINT32_MAX - (unsigned)(*p - '0')) / 10)
                    hal_panic("invalid reserve_mib value");
                mib = mib * 10 + (unsigned)(*p - '0');
            }
            if (!mib || mib > UINT32_MAX / 256u)
                hal_panic("reserve_mib outside allocator limits");
        }
        s = end;
    }
    return mib * UINT64_C(1048576);
}
static void add_range(uint64_t base, uint64_t size, enum boot_mem_type type)
{
    if (!size)
        return;
    if (size > UINT64_MAX - base || base + size > UINT64_MAX - amd64_hhdm)
        hal_panic("Limine memory range overflow");
    if (!boot_range_append(boot.mem, &boot.mem_range_count,
                           (struct boot_mem_range){
                               base,
                               size,
                               type,
                               (uint8_t *)(uintptr_t)(amd64_hhdm + base),
                           }))
        hal_panic("BootInfo memory range capacity exceeded during reservation split (256)");
}
static void copy_framebuffer(void)
{
    struct limine_framebuffer_response *response = fb_request.response;
    if (!response || !response->framebuffer_count) {
        LOGW("Limine framebuffer response absent; serial/RAM log only");
        return;
    }
    struct limine_framebuffer *fb = response->framebuffers[0];
    if (fb->memory_model != LIMINE_FRAMEBUFFER_RGB || fb->width > UINT32_MAX ||
        fb->height > UINT32_MAX || fb->pitch > UINT32_MAX || !fb->height ||
        fb->pitch > UINT64_MAX / fb->height || (fb->bpp != 16 && fb->bpp != 24 && fb->bpp != 32))
        hal_panic("unsupported Limine framebuffer geometry / RGB format");
    boot.fb = (struct boot_framebuffer){
        .format = BOOT_FB_RGB_MASKS,
        .base = (uintptr_t)fb->address - amd64_hhdm,
        .hva = fb->address,
        .size = fb->pitch * fb->height,
        .width = fb->width,
        .height = fb->height,
        .pitch = fb->pitch,
        .bpp = fb->bpp,
        .red_size = fb->red_mask_size,
        .red_shift = fb->red_mask_shift,
        .green_size = fb->green_mask_size,
        .green_shift = fb->green_mask_shift,
        .blue_size = fb->blue_mask_size,
        .blue_shift = fb->blue_mask_shift,
    };
}
static void copy_mp(void)
{
    struct limine_mp_response *mp = mp_request.response;
    if (!mp || !mp->cpu_count || mp->cpu_count > BOOT_MAX_CPUS)
        hal_panic("Limine MP response missing or CPU capacity exceeded");
    boot.host_cpu_count = mp->cpu_count;
    boot.management_cpu = 0;
    unsigned next = 1;
    bool bsp = false;
    for (unsigned i = 0; i < boot.host_cpu_count; ++i) {
        const struct limine_mp_info *cpu = mp->cpus[i];
        unsigned at = cpu->lapic_id == mp->bsp_lapic_id ? 0 : next++;
        if (!at) {
            if (bsp)
                hal_panic("duplicate BSP in MP response");
            bsp = true;
        }
        if (at >= boot.host_cpu_count)
            hal_panic("MP response lacks a BSP");
        for (unsigned j = 0; j < i; ++j)
            if (mp->cpus[j]->lapic_id == cpu->lapic_id)
                hal_panic("duplicate APIC ID in MP response");
        boot.cpus[at].apic_id = cpu->lapic_id;
        boot.cpus[at].processor_id = cpu->processor_id;
        boot.cpus[at].vcpu_index = UINT32_MAX;
    }
    if (!bsp)
        hal_panic("MP response lacks a BSP");
}
static void boot_ap(struct limine_mp_info *info)
{
    /* Read the only needed field before leaving the Limine stack/CR3.
     * No pointer to the MP response survives the handoff. */
    unsigned index = info->extra_argument;
    amd64_enter_cpu(amd64_memory_root(), amd64_smp_stack(index), amd64_cpu_loop,
                    (void *)(uintptr_t)index);
}
static unsigned requested_vcpus(const char *line)
{
    unsigned result = 1;
    bool seen = false;
    while (*line) {
        while (*line == ' ' || *line == '\t')
            ++line;
        const char *end = line;
        while (*end && *end != ' ' && *end != '\t')
            ++end;
        if (end - line >= 6 && !strncmp(line, "vcpus=", 6)) {
            if (seen)
                hal_panic("duplicate vcpus option");
            seen = true;
            const char *p = line + 6;
            result = 0;
            if (p == end)
                hal_panic("vcpus needs a positive decimal value");
            for (; p != end; ++p) {
                if (*p < '0' || *p > '9' || result > (BOOT_MAX_CPUS - (unsigned)(*p - '0')) / 10)
                    hal_panic("invalid vcpus value");
                result = result * 10 + (unsigned)(*p - '0');
            }
            if (!result)
                hal_panic("vcpus must be positive");
        }
        line = end;
    }
    return result;
}
static const char *const placement_names[] = {"physical-core", "logical-thread", "core"};
static enum amd64_placement_mode placement_mode(const char *line)
{
    enum amd64_placement_mode mode = AMD64_PLACE_PHYSICAL_CORE;
    bool seen = false;
    while (*line) {
        while (*line == ' ' || *line == '\t')
            ++line;
        const char *end = line;
        while (*end && *end != ' ' && *end != '\t')
            ++end;
        if (end - line >= 10 && !strncmp(line, "placement=", 10)) {
            if (seen)
                hal_panic("duplicate placement option");
            seen = true;
            bool known = false;
            for (unsigned i = 0; i < 3; ++i) {
                size_t n = strlen(placement_names[i]);
                if ((size_t)(end - line) == 10 + n && !strncmp(line + 10, placement_names[i], n)) {
                    mode = (enum amd64_placement_mode)i;
                    known = true;
                }
            }
            if (!known)
                hal_panic("invalid placement mode");
        }
        line = end;
    }
    return mode;
}
static void ap_ud(void *unused)
{
    (void)unused;
    __asm__ volatile("ud2");
}
static void ap_exception_test(void)
{
    if (!hal_cpu_start(hal_vcpu_cpu(0), ap_ud, NULL))
        hal_panic("cannot dispatch AP #UD selftest");
    uint64_t until = hal_counter() + hal_counter_hz() * 3;
    for (;;) {
        hal_cpu_poll();
        log_drain();
        enum hal_shutdown_kind kind;
        int code;
        if (hal_shutdown_requested(&kind, &code))
            hal_shutdown(kind, code);
        if (hal_counter() >= until)
            hal_panic("AP #UD selftest did not reach management");
        hal_idle_until(until);
    }
}
static void timer_test_on_cpu(void *unused)
{
    (void)unused;
    amd64_timer_selftest();
}
static void timer_tests(unsigned count)
{
    for (unsigned cpu = 1; cpu < count; ++cpu) {
        if (!hal_cpu_start(cpu, timer_test_on_cpu, NULL))
            hal_panic("cannot dispatch per-CPU timer selftest");
    }
    amd64_timer_selftest();
    uint64_t until = hal_counter() + hal_counter_hz() * 3;
    for (unsigned cpu = 1; cpu < count; ++cpu) {
        while (hal_cpu_busy(cpu)) {
            if (hal_counter() >= until)
                hal_panic("per-CPU timer selftest timed out");
            hal_cpu_poll();
            log_drain();
            if (hal_cpu_busy(cpu))
                hal_idle_until(until);
        }
    }
    log_drain();
}

/* Generic display route: what the host
 * firmware left at physical C0000 and in the real-mode IVT. Pure reads
 * through the VMM's own HHDM (the BootInfo ranges, amd64_memory_init) or,
 * for a range the map leaves out entirely, a UC host MMIO mapping. Runs
 * before APs start and before the guest or any display programming. */
#if !defined(IA64VMM_AMD64_HOST_ONLY)
static char legacy_rom_line[256];
static uint64_t mapped_bytes(const struct boot_info *bi, uint64_t base, uint64_t limit)
{
    uint64_t at = base;
    for (bool grown = true; grown && at < limit;) {
        grown = false;
        for (unsigned i = 0; i < bi->mem_range_count; ++i) {
            const struct boot_mem_range *r = &bi->mem[i];
            if (r->base <= at && at - r->base < r->size) {
                at = r->base + r->size;
                grown = true;
            }
        }
    }
    return (at < limit ? at : limit) - base;
}
/* A UEFI memory map often leaves C0000-FFFFF (and page 0) out entirely.
 * Such a range is read through the existing host MMIO mapping
 * (amd64_map_mmio, UC, as for BARs): ROM, shadow RAM or a master abort
 * (all-ones) all read harmlessly. A range the BootInfo map covers even in
 * part keeps its own mapping and only its covered bytes are read. */
static const uint8_t *legacy_bytes(const struct boot_info *bi, uint64_t base, uint64_t limit,
                                   uint64_t *len, bool *uc)
{
    *len = mapped_bytes(bi, base, limit);
    if (*len)
        return (const uint8_t *)(uintptr_t)(amd64_hhdm + base);
    for (unsigned i = 0; i < bi->mem_range_count; ++i)
        if (bi->mem[i].base < (limit + 4095) / 4096 * 4096 &&
            base / 4096 * 4096 < bi->mem[i].base + bi->mem[i].size)
            return NULL; /* shares a page with a mapped range: keep its attributes */
    const uint8_t *p =
        amd64_map_mmio(base / 4096 * 4096, (limit + 4095) / 4096 * 4096 - base / 4096 * 4096);
    if (!p)
        return NULL;
    *len = limit - base;
    *uc = true;
    return p + base % 4096;
}
/* Copy [base,
 * limit) with the rules above, stretch by stretch: what the map covers
 * through the HHDM, whole pages it leaves out through the UC host MMIO
 * map, and the bytes of a page shared with a mapped range not read (they
 * stay zero in the copy and are counted). */
static char legacy_snapshot_lines[3][256];
static void legacy_copy(const struct boot_info *bi, uint64_t base, uint64_t limit, uint8_t *dst,
                        bool *uc, uint64_t *unread)
{
    for (uint64_t at = base; at < limit;) {
        uint64_t len = mapped_bytes(bi, at, limit);
        if (len) {
            memcpy(dst + (at - base), (const void *)(uintptr_t)(amd64_hhdm + at), len);
            at += len;
            continue;
        }
        uint64_t next = limit;
        for (unsigned i = 0; i < bi->mem_range_count; ++i)
            if (bi->mem[i].base > at && bi->mem[i].base < next)
                next = bi->mem[i].base;
        uint64_t lo = IA64VMM_ALIGN_UP(at, 4096), hi = IA64VMM_ALIGN_DOWN(next, 4096);
        if (hi > lo) {
            const uint8_t *p = amd64_map_mmio(lo, hi - lo);
            if (p) {
                memcpy(dst + (lo - base), p, hi - lo);
                *uc = true;
            } else
                *unread += hi - lo;
            *unread += (lo - at) + (next - hi);
        } else
            *unread += next - at;
        at = next;
    }
}
static void legacy_rom_probe(struct boot_info *bi)
{
    uint64_t rom_len, ivt_len;
    bool rom_uc = false, ivt_uc = false;
    const uint8_t *rom = legacy_bytes(bi, 0xc0000, 0xd0000, &rom_len, &rom_uc);
    const uint8_t *ivt = legacy_bytes(bi, 0x40, 0x44, &ivt_len, &ivt_uc);
    size_t n =
        platform_legacy_rom_describe(legacy_rom_line, sizeof(legacy_rom_line), rom, (size_t)rom_len,
                                     ivt_len == 4 ? ivt : NULL, (size_t)ivt_len);
    if (rom_uc || ivt_uc)
        vmm_snprintf(legacy_rom_line + n, sizeof(legacy_rom_line) - n,
                     " [read UC outside the map:%s%s]", rom_uc ? " C0000" : "",
                     ivt_uc ? " IVT" : "");
    LOGI("%s", legacy_rom_line);
    /* The copies display=vbios uses for a host image: taken here,
     * after ExitBootServices and before memory reclaim or any guest write,
     * so later code never maps host low memory. Pure reads. */
    struct boot_legacy *l = &bi->legacy;
    uint8_t *low = hal_alloc_pages(0xa0000 / 4096, HAL_PAGE_ZERO),
            *option = hal_alloc_pages(0x20000 / 4096, HAL_PAGE_ZERO);
    if (!low || !option) {
        if (low)
            hal_free_pages(low, 0xa0000 / 4096);
        if (option)
            hal_free_pages(option, 0x20000 / 4096);
        LOGW("legacy snapshot: VMM reservation exhausted; no copy of host 0-9ffff/c0000-dffff");
        return;
    }
    legacy_copy(bi, 0, 0xa0000, low, &l->low_uc, &l->low_unread);
    legacy_copy(bi, 0xc0000, 0xe0000, option, &l->option_uc, &l->option_unread);
    *l = (struct boot_legacy){low,       0xa0000,      option,        0x20000,
                              l->low_uc, l->option_uc, l->low_unread, l->option_unread};
    for (unsigned i = 0; i < 3; ++i)
        platform_legacy_snapshot_describe(legacy_snapshot_lines[i],
                                          sizeof(legacy_snapshot_lines[i]), i, low, 0xa0000, option,
                                          0x20000);
    LOGI(
        "legacy snapshot: host 0-9ffff%s, c0000-dffff%s copied; %llu/%llu byte(s) in pages shared with the map not read",
        l->low_uc ? " (partly UC)" : "", l->option_uc ? " (partly UC)" : "",
        (unsigned long long)l->low_unread, (unsigned long long)l->option_unread);
    for (unsigned i = 0; i < 3; ++i)
        LOGI("%s", legacy_snapshot_lines[i]);
}
static void legacy_lines(void)
{
    LOGI("%s", legacy_rom_line); /* again, on the photographed screen */
    for (unsigned i = 0; i < 3; ++i)
        if (legacy_snapshot_lines[i][0])
            LOGI("%s", legacy_snapshot_lines[i]);
}
static void keyboard_input(void *context, uint16_t code, bool released, bool e0, bool e1)
{
    (void)context;
    platform_i8042_input(code, released, e0, e1);
}

/* The persistence backend of the reserved vmm_media controller by its
 * class: NVMe (01/08/02), AHCI (01/06/01), EHCI (0C/03/20), else xHCI. */
static const struct plog_block *media_backend(bool log)
{
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *p = platform_pci_device(i);
        if (p->owner != PCI_OWNER_VMM || !p->reservation || strcmp(p->reservation, "vmm-media"))
            continue;
        if (p->class_code == 0x010802)
            return amd64_plog_nvme(p, log);
        if (p->class_code == 0x010601)
            return amd64_plog_ahci(p, log);
        if (p->class_code == 0x0c0320) {
            media_watch_set(amd64_plog_ehci_watch);
            return amd64_plog_ehci(p, log);
        }
    }
    return amd64_plog_xhci(log);
}
#endif
static void finish_boot(void *arg)
{
    struct boot_info *bi = arg;
    amd64_stage("boot: CPU start");
    memset(boot_stack, 0, (uintptr_t)boot_stack_top - (uintptr_t)boot_stack);
    amd64_smp_online(0);
    log_set_async();
    struct limine_mp_response *mp = mp_request.response;
    for (unsigned i = 0; i < bi->host_cpu_count; ++i) {
        struct limine_mp_info *cpu = mp->cpus[i];
        unsigned index = 0;
        while (index < bi->host_cpu_count && bi->cpus[index].apic_id != cpu->lapic_id)
            ++index;
        if (index == bi->host_cpu_count)
            hal_panic("MP copy lost a CPU");
        if (!index)
            continue;
        cpu->extra_argument = index;
        __atomic_store_n(&cpu->goto_address, boot_ap, __ATOMIC_RELEASE);
    }
    amd64_smp_wait();
    log_drain();
    enum amd64_placement_mode mode = placement_mode(bi->cmdline);
    if (!amd64_placement(bi, mode, requested_vcpus(bi->cmdline)))
        hal_panic("insufficient verified cores/threads for management + requested vCPUs");
    LOGI("placement=%s management=CPU0 vcpus=%u", placement_names[mode], bi->vcpu_count);
    for (unsigned i = 0; i < bi->host_cpu_count; ++i) {
        const struct boot_cpu_info *cpu = &bi->cpus[i];
        LOGI(
            "CPU%u processor=%u APIC=%u package=%u core=%u SMT=%u type=%02x topology=%x verified=%u role=%s vcpu=%u",
            i, cpu->processor_id, cpu->apic_id, cpu->package_id, cpu->core_id, cpu->smt_id,
            cpu->core_type, cpu->topology_leaf, cpu->topology_valid,
            i == 0                          ? "management"
            : cpu->vcpu_index == UINT32_MAX ? "idle-helper"
                                            : "vcpu",
            cpu->vcpu_index);
    }
    amd64_helpers_init();
    LOGI("helper CPUs=%u", hal_helper_count());
    for (unsigned i = 0; i < hal_helper_count(); ++i) {
        const struct boot_cpu_info *cpu = &bi->cpus[hal_helper_cpu(i)];
        LOGI("helper%u=CPU%u rank=%u APIC=%u package=%u core=%u SMT=%u verified=%u", i,
             hal_helper_cpu(i), hal_helper_rank(i), cpu->apic_id, cpu->package_id, cpu->core_id,
             cpu->smt_id, cpu->topology_valid);
    }
    /* Clear all temporary protocol pointers before giving these physical
     * pages to the guest. AP online acknowledgements prove no Limine code,
     * stack, page table or MP pointer remains in use on any CPU. */
    mp_request.response = NULL;
    memmap_request.response = NULL;
    hhdm_request.response = NULL;
    fb_request.response = NULL;
    modules_request.response = NULL;
    rsdp_request.response = NULL;
    smbios_request.response = NULL;
    efi_request.response = NULL;
    address_request.response = NULL;
    cmdline_request.response = NULL;
    paging_request.response = NULL;
    amd64_checkpoint_init(bi);
#if !defined(IA64VMM_AMD64_HOST_ONLY)
    const char *media;
    size_t media_len;
    bool has_media = platform_cmdline_value(bi->cmdline, "vmm_media", &media, &media_len);
    bool automatic = !has_media || (media_len == 4 && !strncmp(media, "auto", 4));
    bool hyperv = amd64_hyperv_detect();
    bool list = has_media && media_len == 4 && !strncmp(media, "list", 4);
    bool hyperv_list = list && hyperv;
    bool hyperv_media =
        (automatic && hyperv) || (has_media && options_hyperv_lun(media, media_len) >= 0);
    if (hyperv_media && !hyperv)
        hal_panic("vmm_media=hyperv requires Hyper-V");
    const char *guest_media;
    size_t guest_media_len;
    if (!hyperv_media &&
        platform_cmdline_value(bi->cmdline, "guest_media", &guest_media, &guest_media_len))
        hal_panic("guest_media requires Hyper-V storage");
    if (!hyperv_list && list) {
        amd64_stage("controller discovery");
        if (!platform_pci_discover())
            hal_panic("PCI discovery failed");
        platform_pci_list_controllers();
        legacy_lines();
        amd64_checkpoint("inventory");
        LOGI("Controller discovery complete; halted before guest setup");
        hal_shutdown(HAL_SHUTDOWN_HALT, 0);
    }
#endif
    amd64_stage("IOMMU handoff");
    amd64_iommu_handoff(bi);
    amd64_stage("memory reclaim");
    amd64_reclaim(bi);
    amd64_shutdown_init(bi);
#if !defined(IA64VMM_AMD64_HOST_ONLY)
    if (hyperv_list) {
        if (!amd64_hyperv_storage_discover()) {
            LOGW("Hyper-V storage unavailable; listing PCI controllers");
            if (!platform_pci_discover())
                hal_panic("PCI discovery failed");
            platform_pci_list_controllers();
        }
        LOGI("Controller discovery complete; no disk writes issued");
        hal_shutdown(HAL_SHUTDOWN_HALT, 0);
    }
    if (hyperv_media) {
        if (!amd64_hyperv_storage_start()) {
            if (!automatic)
                hal_panic("Hyper-V storage initialization failed");
            LOGW("Hyper-V storage unavailable; listing PCI controllers");
            if (!platform_pci_discover())
                hal_panic("PCI discovery failed");
            platform_pci_list_controllers();
            hal_shutdown(HAL_SHUTDOWN_HALT, 0);
        }
        platform_pci_external_media(true);
        platform_pci_root_buses(0, 0);
    }
#endif
    const struct log_ring *ring = log_get_ring();
    LOGI("relocation BootInfo=%016llx log=%016llx CR3=%016llx; no Limine storage retained",
         (unsigned long long)((uintptr_t)bi - amd64_hhdm),
         (unsigned long long)((uintptr_t)ring - amd64_hhdm),
         (unsigned long long)amd64_memory_root());
#if !defined(IA64VMM_AMD64_HOST_ONLY)
    amd64_stage("PCI inventory");
    if (automatic && !hyperv) {
        if (bi->boot_media.status != BOOT_MEDIA_PCI ||
            !platform_pci_boot_media(bi->boot_media.segment, bi->boot_media.bdf,
                                     bi->boot_media.class_code))
            hal_panic("automatic VMM media selection requires a supported UEFI boot controller");
    }
    if (!platform_pci_inventory(bi->cmdline))
        hal_panic("PCI inventory/assignment failed");
    amd64_stage("IOMMU DMA protection");
    amd64_iommu_protect(bi);
    platform_pci_list_controllers();
    legacy_lines();
    const struct plog_block *hyperv_block = NULL;
    if (hyperv_media && !(hyperv_block = amd64_plog_hyperv(bi)))
        hal_panic("Hyper-V storage initialization failed");
    if (hyperv_media) {
        if (amd64_hyperv_keyboard_start(keyboard_input, NULL))
            platform_i8042_configure(true);
        else
            LOGW("Hyper-V keyboard unavailable");
    }
    /* The log and the EFI variable slots share the reserved controller. */
    if (plog_requested(bi->cmdline) || nvram_requested(bi)) {
        const struct plog_block *media =
            hyperv_media ? hyperv_block : media_backend(plog_requested(bi->cmdline));
        if (plog_requested(bi->cmdline))
            plog_init(bi, media);
        nvram_init(bi, media);
    }
    amd64_checkpoint("inventory");
#endif
    amd64_stage("host self-tests");
    amd64_ipi_selftest(); /* Also verify TSC offsets inside cross-CPU brackets. */
    amd64_idle_selftest(!amd64_option(bi->cmdline, "host_mwait=off"));
    if (amd64_option(bi->cmdline, "selftest=timer"))
        timer_tests(bi->host_cpu_count);
    if (amd64_option(bi->cmdline, "selftest=host"))
        amd64_memory_selftest();
#if !defined(IA64VMM_AMD64_HOST_ONLY)
    if (amd64_option(bi->cmdline, "selftest=simd"))
        amd64_simd_selftest(bi->host_cpu_count);
    if (amd64_option(bi->cmdline, "selftest=hostwin") ||
        amd64_option(bi->cmdline, "selftest=hostwin-fatal"))
        amd64_hostwin_selftest(amd64_option(bi->cmdline, "selftest=hostwin-fatal"),
                               amd64_option(bi->cmdline, "hwb_fatal_outside=1"));
#endif
    if (amd64_option(bi->cmdline, "selftest=ap-ud"))
        ap_exception_test();
    if (amd64_option(bi->cmdline, "selftest=pf")) {
        LOGI("selftest=pf: deliberate write to unmapped address 0000000000001000");
        *(volatile uint64_t *)(uintptr_t)0x1000 = 1;
        hal_panic("selftest=pf failed to fault");
    }
    if (amd64_option(bi->cmdline, "selftest=ud"))
        __asm__ volatile("ud2");
    if (amd64_option(bi->cmdline, "selftest=gp"))
        __asm__ volatile("movw $0xdead, %%ax; movw %%ax, %%ds" ::: "rax");
    amd64_checkpoint("self-tests");
    amd64_stage("guest execution");
    vmm_main(bi);
}

/* Before the first SIMD instruction: the screen shows why the VMM stops,
 * and the entry's exit= option applies (no options module is merged). */
static __attribute__((noinline, cold)) IA64VMM_NORETURN void simd_baseline_stop(unsigned missing)
{
    char names[96], message[192];
    amd64_hhdm = hhdm_request.response->offset;
    amd64_shutdown_exit_options(cmdline_request.response->cmdline);
    copy_framebuffer();
    fb_console_early(&boot.fb);
    vmm_snprintf(
        message, sizeof(message),
        "host CPU lacks the SIMD baseline (SSE4.2, POPCNT, AVX, XSAVE of x87/SSE/AVX state): missing %s",
        amd64_simd_describe(missing, names, sizeof(names)));
    hal_panic(message);
}

IA64VMM_NORETURN void boot_main(void)
{
    amd64_serial_init();
    LOGI("AMD64 Limine entry: IA-64 VMM");
    if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision))
        hal_panic("Limine base revision 6 unsupported");
    if (!memmap_request.response)
        hal_panic("Limine memory map response missing");
    if (!hhdm_request.response)
        hal_panic("Limine HHDM response missing");
    if (!address_request.response)
        hal_panic("Limine executable address response missing");
    if (!paging_request.response || paging_request.response->mode != LIMINE_PAGING_MODE_X86_64_4LVL)
        hal_panic("Limine did not provide required 4-level paging");
    if (!cmdline_request.response)
        hal_panic("Limine executable command line response missing");
    amd64_boot_diag_cpu(false, hhdm_request.response->offset);
    /* Everything above runs general-registers-only code (BM_GPR_ONLY_SOURCES). */
    unsigned missing = amd64_simd_enable();
    if (missing)
        simd_baseline_stop(missing);
    amd64_cpu_init();
    amd64_boot_diag_cpu(true, hhdm_request.response->offset);
    amd64_hhdm = hhdm_request.response->offset;
    amd64_image_pa = address_request.response->physical_base;
    amd64_image_va = address_request.response->virtual_base;
    if (amd64_image_va != (uintptr_t)__image_start || (amd64_hhdm & ((UINT64_C(1) << 30) - 1)) ||
        amd64_hhdm < UINT64_C(0xffff800000000000) || amd64_hhdm >= AMD64_CODE_BASE)
        hal_panic("unsupported Limine executable/HHDM layout");
    boot.version = BOOT_INFO_VERSION;
    boot.host_name = "amd64-limine";
    copy_mp();
    merge_options(cmdline_request.response->cmdline);
    /* Configure debug-exit before later setup failures can panic. ACPI is
     * initialized only after the new HHDM is installed. */
    amd64_shutdown_init(&boot);
    copy_framebuffer();
    fb_console_early(&boot.fb);
    struct limine_memmap_response *map = memmap_request.response;
    if (!map->entry_count || map->entry_count > BOOT_MAX_MEM_RANGES)
        hal_panic("Limine memory map is empty or too large");
    for (uint64_t i = 0; i < map->entry_count; ++i) {
        const struct limine_memmap_entry *m = map->entries[i];
        enum boot_mem_type type =
            m->type <= LIMINE_MEMMAP_FRAMEBUFFER ? (enum boot_mem_type)m->type : BOOT_MEM_RESERVED;
        add_range(m->base, m->length, type);
    }
    uint64_t efi_table = efi_request.response ? (uintptr_t)efi_request.response->address : 0;
    (void)boot_media_read(&boot.boot_media, efi_table >= amd64_hhdm ? efi_table - amd64_hhdm : 0,
                          media_map, map);
    if (rsdp_request.response)
        boot.acpi_rsdp = (uintptr_t)rsdp_request.response->address - amd64_hhdm;
    else
        LOGW("Limine RSDP response absent; ACPI power-off unavailable");
#if !defined(IA64VMM_AMD64_HOST_ONLY)
    /* Base revision 5+: SMBIOS addresses are HHDM pointers, the tables
     * mapped (Limine PROTOCOL.md "Base Revision 5"). */
    if (smbios_request.response)
        smbios_identity(smbios_request.response->entry_32, smbios_request.response->entry_64,
                        smbios_table, &boot.identity);
    char identity[160];
    nvram_identity_text(&boot.identity, identity, sizeof(identity));
    LOGI("host identity: %s", identity);
#endif
    uint64_t requested = reservation_size(), minimum = 0;
    if (amd64_option(boot.cmdline, "engine=tier1") ||
        amd64_option(boot.cmdline, "selftest=hostwin") ||
        amd64_option(boot.cmdline, "selftest=hostwin-fatal"))
        minimum =
            options_tier1_reserve_floor_mib(requested_vcpus(boot.cmdline)) * UINT64_C(1048576);
    struct boot_reservation reserve;
    enum boot_reserve_status status = boot_reserve_plan(&boot, requested, minimum, &reserve);
    LOGI("VMM reserve requested_bytes=%llu minimum_bytes=%llu known_ram_bytes=%llu "
         "half_ram_bytes=%llu largest_usable_bytes=%llu granted_bytes=%llu",
         (unsigned long long)requested, (unsigned long long)minimum,
         (unsigned long long)reserve.ram_bytes, (unsigned long long)reserve.half_bytes,
         (unsigned long long)reserve.largest_bytes, (unsigned long long)reserve.size);
    if (status == BOOT_RESERVE_INVALID)
        hal_panic("invalid RAM map or VMM reservation request");
    if (status != BOOT_RESERVE_OK)
        hal_panic("VMM minimum exceeds half of known RAM or largest usable range");
    uint64_t reserve_size = reserve.size, reserve_base = reserve.base;
    amd64_boot_diag_reserve(&boot, reserve_base, reserve_size);
    boot.mem_range_count = 0;
    for (uint64_t i = 0; i < map->entry_count; ++i) {
        const struct limine_memmap_entry *m = map->entries[i];
        enum boot_mem_type type =
            m->type <= LIMINE_MEMMAP_FRAMEBUFFER ? (enum boot_mem_type)m->type : BOOT_MEM_RESERVED;
        if (m->type == LIMINE_MEMMAP_USABLE && reserve_base >= m->base &&
            reserve_base + reserve_size <= m->base + m->length) {
            add_range(m->base, reserve_base - m->base, type);
            add_range(reserve_base, reserve_size, BOOT_MEM_VMM);
            add_range(reserve_base + reserve_size,
                      m->base + m->length - reserve_base - reserve_size, type);
        } else
            add_range(m->base, m->length, type);
    }
    LOGI("BootInfo ranges after reservation=%u/%u (Limine=%llu)", boot.mem_range_count,
         BOOT_MAX_MEM_RANGES, (unsigned long long)map->entry_count);
    struct limine_module_response *mods = modules_request.response;
    if (mods) {
        if (mods->module_count > BOOT_MAX_MODULES)
            hal_panic("too many Limine modules");
        for (uint64_t i = 0; i < mods->module_count; ++i) {
            const struct limine_file *source = mods->modules[i];
            if (source->string && (!strcmp(source->string, OPTIONS_MODULE_NAME) ||
                                   !strcmp(source->string, OPTIONS_MEDIA_MODULE_NAME)))
                continue; /* merged above */
            struct boot_module *dest = &boot.modules[boot.module_count++];
            copy_string(dest->name, sizeof(dest->name), source->string);
            dest->hpa = (uintptr_t)source->address - amd64_hhdm;
            dest->hva = source->address;
            dest->size = source->size;
        }
    } else
        LOGW("Limine module response absent (no modules loaded)");
    /* The exit summary names the step a fatal exception interrupts. */
    amd64_stage("boot: VMM reservation and page tables");
    /* Keep a deterministic 2 MiB tail outside allocator use. The BootInfo
     * reservation still covers it, so guest RAM excludes this ring. */
    if (reserve_size <= 2 * 1048576u)
        hal_panic("VMM reservation too small for crash ring");
    amd64_memory_init(&boot, reserve_base, reserve_size - 2 * 1048576u);
    amd64_stage("boot: log and module relocation");
    log_relocate_fixed(
        (void *)(uintptr_t)(amd64_hhdm + reserve_base + reserve_size - 2 * 1048576u));
    LOGI("VMM reservation base=%016llx size=%llu MiB from highest suitable usable RAM",
         (unsigned long long)reserve_base, (unsigned long long)(reserve_size / 1048576));
    for (unsigned i = 0; i < boot.module_count; ++i) {
        struct boot_module *m = &boot.modules[i];
        if (!m->hva || !m->size || m->size > SIZE_MAX - 4095)
            hal_panic("invalid Limine module for relocation");
        uint8_t *copy = hal_alloc_pages(IA64VMM_ALIGN_UP(m->size, 4096) / 4096, HAL_PAGE_ZERO);
        if (!copy)
            hal_panic("VMM reservation cannot hold boot modules");
        memcpy(copy, m->hva, m->size);
        m->hva = copy;
        m->hpa = (uintptr_t)copy - amd64_hhdm;
        LOGI("relocation module %s=%016llx size=%llu", m->name, (unsigned long long)m->hpa,
             (unsigned long long)m->size);
    }
    amd64_stage("boot: ACPI copy");
    amd64_copy_acpi(&boot);
    amd64_stage("boot: BootInfo relocation");
    struct boot_info *stable =
        hal_alloc_pages(IA64VMM_ALIGN_UP(sizeof(boot), 4096) / 4096, HAL_PAGE_ZERO);
    if (!stable)
        hal_panic("cannot relocate BootInfo");
    *stable = boot;
    /* The temporary image-resident copy no longer holds boot-time pointers. */
    memset(&boot, 0, sizeof(boot));
    amd64_stage("boot: framebuffer console");
    fb_console_init(&stable->fb);
#if !defined(IA64VMM_AMD64_HOST_ONLY)
    amd64_stage("boot: legacy memory snapshot");
    legacy_rom_probe(stable);
#endif
    amd64_stage("boot: ACPI shutdown setup");
    amd64_shutdown_init(stable);
    amd64_stage("boot: time calibration");
    amd64_time_init(stable);
    amd64_stage("boot: SMP preparation");
    amd64_smp_prepare(stable);
    amd64_enter_cpu(amd64_memory_root(), amd64_smp_stack(0), finish_boot, stable);
}
