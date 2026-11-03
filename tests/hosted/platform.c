/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Tests/Kernel/Device/{PciEcamAddressTests,PciBarProbeTests}.cpp,
 * Tests/Platform/Device/PortPciConfigurationSpaceTests.cpp
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <elf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "platform/platform.h"
#include "platform/storage.h"
#include "platform/ahci.h"
#include "platform/acpi-irq.h"
#include "platform/i8042.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/pci.h"
#include "core/log_internal.h"
#include "core/vm.h"
#include "persist/persist.h"
#include "tests/hal/hal_linux.h"
/* A real struct vcpu: core/smp.c's executor writes its fields (parked,
 * the reset-hold kick and exit words); the CPU itself stays mocked. */
#include "ia64/state.h"

#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #e);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static unsigned notifications, dumps, fb_bytes, steps, scenario, port_reads, port_writes;
static bool mock_reserve, cpu_attached;
static unsigned reserve_budget_calls, tier1_budget_calls;
static uint64_t configured_snapshot_budget = CODE_SNAPSHOT_DEFAULT_BYTES;
bool __wrap_hal_memory_get_stats(struct hal_memory_stats *out)
{
    if (!mock_reserve)
        return false;
    CHECK(cpu_attached);
    *out = (struct hal_memory_stats){128u << 20, 80u << 20, 80u << 20};
    return true;
}
static uint16_t expected_port = 0x3f8;
static bool mock_time;
static uint64_t mock_now, idle_deadline;
static uint64_t mock_ip;
static unsigned instruction_queries;
static struct ia64_entry_regs entry;
static struct vcpu_config config;
static unsigned fp_options, fp_config_calls;
void engine_fp_configure(unsigned flags)
{
    fp_options = flags;
    ++fp_config_calls;
}

static struct vcpu cpu;
/* This suite mocks the CPU; TR formatting/buffers are tested by test-ia64. */
void ia64_pal_tr_select(struct vcpu *v, uint64_t selector)
{
    CHECK(v == &cpu);
    (void)selector;
}
void ia64_pal_tr_read(struct vcpu *v, uint64_t buffer)
{
    CHECK(v == &cpu);
    (void)buffer;
}
uint64_t ia64_pal_tr_status(const struct vcpu *v)
{
    (void)v;
    return 0;
}
uint64_t ia64_pal_tr_valid(const struct vcpu *v)
{
    (void)v;
    return 0;
}
void ia64_pal_complete_services(struct vcpu *v)
{
    CHECK(v == &cpu);
}
/* The native PAL service registers (ia64/pal.c, tested by test-ia64):
 * record what the window hands them. */
static unsigned pal_service_offset, pal_service_writes;
static uint64_t pal_service_value;
uint64_t ia64_pal_service_read(const struct vcpu *v, unsigned offset)
{
    CHECK(v == &cpu);
    pal_service_offset = offset;
    return UINT64_C(0x1000) + offset;
}
void ia64_pal_service_write(struct vcpu *v, unsigned offset, uint64_t value)
{
    CHECK(v == &cpu);
    pal_service_offset = offset;
    pal_service_value = value;
    ++pal_service_writes;
}

void code_notify_vmm_write(gpa_t base, uint64_t length)
{
    (void)base;
    (void)length;
    ++notifications;
}
static void fb_sink(const char *text, size_t n)
{
    (void)text;
    fb_bytes += n;
}
uint64_t __real_hal_counter(void);
uint64_t __wrap_hal_counter(void)
{
    return mock_time ? mock_now : __real_hal_counter();
}
void __real_hal_idle_until(uint64_t deadline);
void __wrap_hal_idle_until(uint64_t deadline)
{
    if (mock_time) {
        idle_deadline = deadline;
        mock_now = deadline;
    } else
        __real_hal_idle_until(deadline);
}
uint32_t __wrap_hal_port_in(uint16_t port, unsigned size)
{
    CHECK(port == expected_port && size == 4);
    ++port_reads;
    return 0xa5a5a5a5;
}
/* Restart tests record forwarded byte writes instead. */
static bool record_ports;
static unsigned recorded;
static uint32_t recorded_writes[16];
void __wrap_hal_port_out(uint16_t port, uint32_t value, unsigned size)
{
    if (record_ports) {
        CHECK(size == 1 && recorded < 16);
        recorded_writes[recorded++] = (uint32_t)port << 8 | (value & 0xff);
        return;
    }
    CHECK(port == expected_port && size == 4 && value == 0x1234);
    ++port_writes;
}
uint64_t vcpu_instruction_pointer(const struct vcpu *v, unsigned *slot)
{
    CHECK(v == &cpu);
    if (slot)
        *slot = 0;
    return mock_ip ? mock_ip : entry.ip;
}
bool vcpu_instruction_physical_address(struct vcpu *v, uint64_t *pa)
{
    CHECK(v == &cpu);
    ++instruction_queries;
    *pa = mock_ip ? mock_ip : entry.ip;
    return true;
}
void vcpu_destroy(struct vcpu *v)
{
    CHECK(v == &cpu);
}
static unsigned progress_samples, virtual_reads;
void vcpu_set_stop_fault(unsigned kind)
{
    CHECK(kind == 1);
}
void vcpu_set_log_faults(unsigned count)
{
    CHECK(count == 64);
}
void vcpu_set_break_ip(uint64_t ip, unsigned count)
{
    (void)ip;
    (void)count;
    CHECK(0);
}
void vcpu_set_break_mem(uint64_t pa)
{
    (void)pa;
    CHECK(0);
}
bool vcpu_read_virtual(struct vcpu *v, uint64_t va, uint64_t *value)
{
    CHECK(v == &cpu && !(va & 7));
    ++virtual_reads;
    *value = va ^ UINT64_C(0x5a5a);
    return !(va & 8);
}
/* Milestone fixture: engine_run retires 1000 instructions
 * per full slice and 1 per single step; scenario 8 also advances the clock
 * and the translation time by known amounts. */
static uint64_t mock_retired, mock_compile;
void vcpu_progress(const struct vcpu *v, uint64_t *ip, unsigned *slot, uint64_t *psr,
                   uint64_t *retired, const char **fault, uint64_t *fault_ip, uint64_t *fault_info,
                   uint64_t *faults)
{
    CHECK(v == &cpu);
    *ip = entry.ip;
    *slot = 0;
    *psr = 0;
    *retired = mock_retired;
    ++progress_samples;
    *fault = NULL;
    *fault_ip = *fault_info = *faults = 0;
}
/* Overrides core/vm.c's weak default; only scenario 8 has a translation clock. */
bool engine_compile_ticks(uint64_t *ticks)
{
    if (scenario != 8)
        return false;
    *ticks = mock_compile;
    return true;
}
/* This platform-only suite substitutes the engine, including its validator. */
bool code_validity_parse_mode(const char *value, size_t length, enum code_validity_mode *mode)
{
    (void)value;
    (void)length;
    *mode = CODE_VALIDITY_COMPARE_ALWAYS;
    return true;
}
bool code_validity_configure(enum code_validity_mode mode, uint64_t budget)
{
    CHECK(mode == CODE_VALIDITY_COMPARE_ALWAYS && budget == configured_snapshot_budget);
    return true;
}
void code_validity_dump_stats(void) {}
uint64_t code_validity_reserve_budget(uint64_t free_bytes, uint64_t reserve_bytes)
{
    CHECK(reserve_bytes == (128u << 20));
    CHECK(mock_reserve && free_bytes == (48u << 20));
    ++reserve_budget_calls;
    return 24u << 20;
}
bool tier0_configure_cmdline(const char *cmdline)
{
    (void)cmdline;
    return true;
}
void tier0_destroy(void) {}
/* engine/engine.c's Tier1 composition (Tier1 product dispatch): no Tier1 here,
 * so engine=tier1 is a setup error, as in a binary without Tier1. */
bool engine_tier1_configure(const char *cmdline, enum code_validity_mode validity)
{
    (void)cmdline;
    (void)validity;
    return false;
}
/* engine=lockstep tier1=on (Tier1 checked execution): counted, and a setup
 * error here for the same reason. */
static unsigned lockstep_configures;
bool engine_tier1_lockstep_configure(const char *cmdline, enum code_validity_mode validity)
{
    (void)cmdline;
    (void)validity;
    ++lockstep_configures;
    return false;
}
bool engine_vcpu_attach(struct vcpu *v)
{
    CHECK(v == &cpu);
    cpu_attached = true;
    return config.engine != ENGINE_TIER1;
}
void engine_vcpu_detach(struct vcpu *v)
{
    CHECK(v == &cpu);
}
void engine_tier1_memory_budget(uint64_t free_bytes, uint64_t snapshot_bytes,
                                uint64_t reserve_bytes)
{
    CHECK(reserve_bytes == (128u << 20));
    CHECK(mock_reserve && free_bytes == (48u << 20));
    CHECK(snapshot_bytes == (configured_snapshot_budget == CODE_SNAPSHOT_DEFAULT_BYTES
                                 ? 24u << 20
                                 : configured_snapshot_budget));
    ++tier1_budget_calls;
}
void engine_tier1_destroy(void) {}
void engine_dump_stats(void) {}
bool ia64_set_trace(struct vcpu *v, unsigned count)
{
    CHECK(v == &cpu && count <= 65536);
    return true;
}
struct vcpu *vcpu_create(const struct vcpu_config *cfg)
{
    config = *cfg;
    return &cpu;
}
static unsigned boots;
/* The slots are loaded once, before the first boot,
 * in the firmware's format, and written once when the run ends. */
static unsigned nvram_loads, nvram_saves, nvram_format = 9;
static bool reset_fails;
void __real_nvram_load(unsigned format, uint8_t *window);
void __wrap_nvram_load(unsigned format, uint8_t *window)
{
    ++nvram_loads;
    nvram_format = format;
    __real_nvram_load(format, window);
}
void __real_nvram_save(void);
void __wrap_nvram_save(void)
{
    ++nvram_saves;
    __real_nvram_save();
}
bool __real_platform_pci_reset_functions(void);
bool __wrap_platform_pci_reset_functions(void)
{
    return !reset_fails && __real_platform_pci_reset_functions();
}
void vcpu_reset(struct vcpu *v, const struct ia64_entry_regs *regs)
{
    CHECK(v == &cpu);
    entry = *regs;
    ++boots;
}
static enum vcpu_exit_reason requested;
void vcpu_request_exit(struct vcpu *v, enum vcpu_exit_reason reason)
{
    CHECK(v == &cpu);
    requested = reason;
}
uint64_t vcpu_next_timer_deadline(const struct vcpu *v)
{
    CHECK(v == &cpu);
    return mock_now + (scenario == 8 ? 7500000 : 100);
}
void vcpu_dump(const struct vcpu *v)
{
    CHECK(v == &cpu);
    ++dumps;
    unsigned before = fb_bytes;
    LOGI("test dump");
    CHECK(fb_bytes > before);
}
static uint64_t read_mmio(uint64_t addr, unsigned width)
{
    const struct gm_region *r = gm_lookup(addr);
    CHECK(r && r->kind == GM_EMULATED);
    return r->ops->read(r->opaque, &cpu, addr - r->base, width);
}
static void write_mmio(uint64_t addr, uint64_t value, unsigned width)
{
    const struct gm_region *r = gm_lookup(addr);
    CHECK(r && r->kind == GM_EMULATED);
    r->ops->write(r->opaque, &cpu, addr - r->base, value, width);
}
struct vcpu_exit engine_run(struct vcpu *v, uint64_t budget)
{
    CHECK(v == &cpu && budget > 0);
    mock_retired += budget == 1 ? 1 : 1000;
    if (scenario == 8) {
        /* 3.25 ms inside engine_run per slice; 1 ms of it translating in
         * full slices. The milestone lines truncate to whole ms. */
        mock_now += 3250000;
        if (budget != 1)
            mock_compile += 1000000;
    }
    /* More than 256 distinct executable pages in one loader allocation. */
    if (scenario == 9 && steps < 300) {
        CHECK(budget == 100000);
        mock_ip = 0x600000 + steps++ * 4096;
        return (struct vcpu_exit){.reason = VCPU_EXIT_BUDGET};
    }
    if (scenario >= 7 && scenario <= 11) {
        uint64_t service = IA64VMM_SERVICE_BASE;
        unsigned phase = ++steps - (scenario == 9 ? 300 : 0);
        if (phase == 1) {
            CHECK(budget == 100000);
            mock_ip = 0x600000;
            return (struct vcpu_exit){.reason = VCPU_EXIT_BUDGET};
        }
        if (phase == 2) {
            mock_ip = entry.ip;
            write_mmio(service + IA64VMM_SVC_IMAGE_BASE, 0x300000, 8);
            write_mmio(service + IA64VMM_SVC_IMAGE_SIZE, 0x1000, 8);
            for (unsigned i = 0; i < 3; ++i) {
                write_mmio(service + IA64VMM_SVC_LOAD_BASE, 0x200000 + i * 0x200000, 8);
                write_mmio(service + IA64VMM_SVC_LOAD_SIZE,
                           scenario == 9 && i == 2 ? 0x200000 : 0x1000, 8);
                write_mmio(service + IA64VMM_SVC_LOAD_TYPE, 1, 8);
            }
            write_mmio(service + IA64VMM_SVC_MAP_SIZE, 96, 8);
            write_mmio(service + IA64VMM_SVC_EXIT_BOOT_SERVICES, 42, 8);
            CHECK(requested == VCPU_EXIT_DEBUG);
            return (struct vcpu_exit){.reason = requested};
        }
        if (phase <= 5) {
            /* The EBS caller relocated beyond its reported PE range. Its
             * return must not be accepted as the kernel allocation entry. */
            CHECK(budget == (scenario >= 10 ? 100000 : 1));
            mock_ip = phase == 3 ? 0x200000 : phase == 4 ? 0x600000 : 0x400000;
            return (struct vcpu_exit){.reason = VCPU_EXIT_BUDGET};
        }
        CHECK(budget == 100000);
        if (phase == 6)
            return (struct vcpu_exit){.reason = VCPU_EXIT_HALT};
        write_mmio(service + IA64VMM_SVC_RESET, 2, 8);
        return (struct vcpu_exit){.reason = requested};
    }
    if (++steps == 1) {
        if (scenario == 6)
            mock_now += 2 * hal_counter_hz(); /* Past timeout and one sample. */
        return (struct vcpu_exit){.reason = VCPU_EXIT_BUDGET};
    }
    if (steps == 2)
        return (struct vcpu_exit){.reason = VCPU_EXIT_HALT};
    if (scenario == 4)
        return (struct vcpu_exit){.reason = VCPU_EXIT_UNIMPLEMENTED};
    if (scenario == 5)
        return (struct vcpu_exit){.reason = VCPU_EXIT_BAD_ACCESS};
    uint64_t service = IA64VMM_SERVICE_BASE;
    /* Scenario 2: ResetSystem(EfiResetPlatformSpecific)
     * with an error status, then a CF9h reset, then a shutdown. NVRAM
     * survives each restart; the firmware image is loaded again. */
    static uint8_t firmware_byte;
    if (scenario == 2 && boots == 1) {
        write_mmio(IA64VMM_NVRAM_BASE + 0x200, 0xa5, 1);
        CHECK(gm_read(entry.ip, &firmware_byte, 1));
        uint8_t clobber = (uint8_t)~firmware_byte;
        CHECK(gm_write(entry.ip, &clobber, 1));
        write_mmio(service + IA64VMM_SVC_RESET_STATUS, UINT64_C(0x8000000000000001), 8);
        write_mmio(service + IA64VMM_SVC_RESET, 3, 8);
        return (struct vcpu_exit){.reason = requested};
    }
    if (scenario == 2 && boots == 2) {
        uint8_t now;
        CHECK(read_mmio(IA64VMM_NVRAM_BASE + 0x200, 1) == 0xa5);
        CHECK(gm_read(entry.ip, &now, 1) && now == firmware_byte);
        write_mmio(IA64VMM_LEGACY_IO_BASE + ((0xcf9 >> 2) << 12 | (0xcf9 & 0xfff)), 0x06, 1);
        return (struct vcpu_exit){.reason = requested};
    }
    if (scenario == 3)
        write_mmio(service + IA64VMM_SVC_PANIC, 0x123, 8);
    else {
        write_mmio(service + IA64VMM_SVC_RESET_STATUS, scenario == 1 ? 1 : 0, 8);
        write_mmio(service + IA64VMM_SVC_RESET, 2, 8);
    }
    return (struct vcpu_exit){.reason = requested};
}

static uint8_t *add_ram(uint64_t base, uint64_t size)
{
    uint8_t *p = hal_alloc_pages((size + 4095) / 4096, HAL_PAGE_ZERO);
    CHECK(p);
    struct gm_region r = {.base = base, .size = size, .kind = GM_RAM, .name = "test-ram", .hva = p};
    CHECK(gm_add_region(&r));
    return p;
}
static uint8_t *load_file(const char *file, size_t *size)
{
    FILE *f = fopen(file, "rb");
    CHECK(f);
    CHECK(!fseek(f, 0, SEEK_END));
    long length = ftell(f);
    CHECK(length > 0);
    *size = (size_t)length;
    rewind(f);
    uint8_t *p = malloc(*size);
    CHECK(p);
    CHECK(fread(p, 1, *size, f) == *size);
    fclose(f);
    return p;
}
static uint64_t word(uint64_t addr)
{
    uint64_t value;
    CHECK(gm_read(addr, &value, 8));
    return value;
}

static void test_loader(const char *file)
{
    size_t length;
    uint8_t *elf = load_file(file, &length);
    add_ram(0x100000, 0x10000);
    add_ram(0x200000, 0x200000);
    struct fw_image linked, relocated;
    CHECK(fw_load(elf, length, 0, &linked));
    CHECK(linked.base == 0x100000 && linked.entry == linked.base);
    CHECK(linked.global_pointer == linked.base + 0x30 && linked.pal_entry == 0);
    CHECK(fw_load(elf, length, 0x200000, &relocated));
    CHECK(relocated.entry == linked.entry + 0x100000);
    CHECK(relocated.global_pointer == linked.global_pointer + 0x100000);
    CHECK(relocated.pal_entry == 0);
    CHECK(word(0x200030) == 0x200000 && word(0x200040) == 0x200000);
    CHECK(word(0x200048) == 0x200030 && word(0x200050) == 0x200040);
    uint64_t lo = word(0x200000), hi = word(0x200008);
    uint64_t slot1 = ((lo >> 46) | (hi << 18)) & ((UINT64_C(1) << 41) - 1);
    uint64_t slot2 = hi >> 23;
    uint64_t immediate = ((slot2 >> 13) & 0x7f) | ((slot2 >> 27) & 0x1ff) << 7 |
                         ((slot2 >> 22) & 0x1f) << 16 | ((slot2 >> 21) & 1) << 21 | slot1 << 22 |
                         ((slot2 >> 36) & 1) << 63;
    CHECK(immediate == 0x200030 && notifications == 2);
    CHECK(linked.pale_init == linked.base && linked.pale_check == linked.base + 0x20);
    CHECK(fw_load(elf, length, 0x200000, &relocated) && relocated.pal_entry == 0);
    CHECK(relocated.pale_init == 0x200000 && relocated.pale_check == 0x200020);
    CHECK(!fw_load(elf, length, 0x200001, &relocated));
    CHECK(!fw_load(elf, length, 0x500000, &relocated));
    for (size_t n = 0; n < 64; ++n)
        CHECK(!fw_load(elf, n, 0, &relocated));
    elf[18] = 0;
    CHECK(!fw_load(elf, length, 0, &relocated));
    free(elf);
}

static void test_handoff(void)
{
    add_ram(0x900000, 0x1700000);
    add_ram(0, 0x9f000);
    add_ram(0x100000, 0x700000);
    add_ram(0x2000000, 0x2000000);
    struct boot_info bi = {0};
    uint8_t image[] = {'M', 'Z', 1, 2};
    bi.module_count = 1;
    strcpy(bi.modules[0].name, "bootdisk");
    bi.modules[0].hva = image;
    bi.modules[0].size = sizeof(image);
    bi.fb = (struct boot_framebuffer){.format = BOOT_FB_RGB_MASKS,
                                      .bpp = 32,
                                      .base = 0x80000000,
                                      .size = 4096 * 768,
                                      .width = 1000,
                                      .height = 768,
                                      .pitch = 4096,
                                      .red_size = 8,
                                      .red_shift = 16,
                                      .green_size = 8,
                                      .green_shift = 8,
                                      .blue_size = 8};
    struct fw_image fw = {.base = 0x100000,
                          .size = 0x180000,
                          .entry = 0x100000,
                          .global_pointer = 0x101000,
                          .pal_entry = 0x100060};
    struct ia64vmm_handoff h, copy;
    struct ia64_entry_regs regs;
    uint64_t at;
    CHECK(platform_handoff(&bi, &fw, &h, &at, &regs));
    CHECK(h.magic == IA64VMM_HANDOFF_MAGIC && h.version == 1 && h.size == sizeof(h));
    CHECK(h.ram_range_count == 3 && h.ram[0].size == 0x9f000);
    CHECK(h.ram[1].base == 0x100000 && h.ram[1].size == 0x700000);
    CHECK(h.ram[2].base == 0x900000 && h.ram[2].size == 0x3700000);
    CHECK(h.fw_data_base == 0x2000000 && h.fw_data_size == 0x2000000);
    CHECK(at == h.fw_data_base + 0x100000);
    CHECK(gm_read(at, &copy, sizeof(copy)) && !memcmp(&h, &copy, sizeof(h)));
    CHECK(h.boot_image_kind == IA64VMM_BOOT_PE_IMAGE);
    CHECK(!memcmp(gm_direct_ptr(h.boot_image_base, sizeof(image)), image, sizeof(image)));
    CHECK(h.boot_image_base + h.boot_image_size <= h.fw_data_base);
    CHECK(h.fb_format == IA64VMM_FB_BGRX8888 && h.fb_pitch == 4096 && h.fb_width == 1000);
    CHECK(h.cpu_count == 1 && h.itc_hz == IA64VMM_ITC_HZ && h.ecam_bus_last == 255);
    platform_i8042_configure(true);
    CHECK(platform_handoff(&bi, &fw, &h, &at, &regs));
    CHECK((h.flags & (IA64VMM_HF_I8042_ENABLED | IA64VMM_HF_I8042_KEYBOARD_ONLY)) ==
          (IA64VMM_HF_I8042_ENABLED | IA64VMM_HF_I8042_KEYBOARD_ONLY));
    CHECK(h.pci_io_base <= 0x60 && h.pci_io_base + h.pci_io_size >= 0x65);
    platform_i8042_configure(false);
    CHECK(h.service_base == IA64VMM_SERVICE_BASE && h.legacy_io_base == IA64VMM_LEGACY_IO_BASE);
    CHECK(regs.ip == fw.entry && regs.gr[1] == fw.global_pointer && !regs.psr);
    CHECK(regs.gr[8] == at && regs.gr[9] == sizeof(h) && regs.gr[10] == IA64VMM_PLATFORM_ID);
    CHECK(regs.bsp == h.fw_data_base + 0x600000 && regs.bsp == regs.bspstore);
    CHECK(!regs.rsc && regs.k0 == IA64VMM_LEGACY_IO_BASE);
    CHECK(platform_handoff_model(&bi, &fw, IA64_CPU_MADISON, &h, &at, &regs));
    CHECK(gm_read(at, &copy, sizeof(copy)) && copy.cpu_report.model == IA64_CPU_MADISON &&
          copy.cpu_report.cpuid3 == 0x1f010504 && ia64vmm_handoff_cpu_valid(&copy));
    strcpy(bi.cmdline, "bootmenu=1");
    image[0] = 0;
    bi.fb.bpp = 24;
    CHECK(platform_handoff(&bi, &fw, &h, &at, &regs));
    CHECK(h.boot_image_kind == IA64VMM_BOOT_DISK_IMAGE && !h.fb_format);
    CHECK(!(h.flags & IA64VMM_HF_NO_BOOT_MENU_WAIT));
    CHECK(!platform_ram_contains(0x9e000, 0x3000) && !platform_ram_contains(0x7ff000, 0x3000));
    /* vga_probe=N: decimal seconds 0..3600 reach the handoff. */
    CHECK(!h.vga_probe_sec);
    strcpy(bi.cmdline, "bootmenu=1 vga_probe=20");
    CHECK(platform_handoff(&bi, &fw, &h, &at, &regs));
    CHECK(h.vga_probe_sec == 20);
    strcpy(bi.cmdline, "vga_probe=3600");
    CHECK(platform_handoff(&bi, &fw, &h, &at, &regs));
    CHECK(h.vga_probe_sec == 3600 && gm_read(at, &copy, sizeof(copy)) &&
          copy.vga_probe_sec == 3600);
    strcpy(bi.cmdline, "vga_probe=0");
    CHECK(platform_handoff(&bi, &fw, &h, &at, &regs) && !h.vga_probe_sec);
    const char *bad[] = {"vga_probe=3601", "vga_probe=", "vga_probe=2x", "vga_probe=-1",
                         "vga_probe=99999999999"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        strcpy(bi.cmdline, bad[i]);
        CHECK(!platform_handoff(&bi, &fw, &h, &at, &regs));
    }
}

static void test_window(void)
{
    struct boot_info bi = {0};
    CHECK(platform_check_window(&bi));
    bi.mem_range_count = 1;
    bi.mem[0] = (struct boot_mem_range){
        .base = IA64VMM_WINDOW_BASE - 4096, .size = 8192, .type = BOOT_MEM_RESERVED};
    CHECK(!platform_check_window(&bi));
    bi.mem[0].size = 4096;
    CHECK(platform_check_window(&bi));
    add_ram(IA64VMM_WINDOW_BASE, 4096);
    CHECK(!platform_check_window(&bi));
}
static uint64_t port_addr(unsigned port)
{
    return IA64VMM_LEGACY_IO_BASE + ((uint64_t)(port >> 2) << 12 | (port & 0xfff));
}
/* CF8h is CONFIG_ADDRESS only for a doubleword
 * (PCI Local Bus 3.0 §3.2.2.3.2); a CF9h byte with bit 2 set and an i8042
 * output-port reset (IBM PC AT keyboard controller: F0h-FFh pulse output
 * port bits 3:0 whose command bit is 0; D1h writes the next 60h byte to the
 * output port; bit 0 is the reset line) restart the guest and never reach
 * the host. platform_devices_restart returns every per-boot field to its
 * power-on value and keeps NVRAM, the regions and the port allow-list. */
static void expect_restart(struct platform_state *s, unsigned source)
{
    CHECK(s->reset_requested && s->reset_type == 0 && s->reset_source == source);
    CHECK(platform_reset_restarts(s) && requested == VCPU_EXIT_SHUTDOWN);
    requested = 0;
    platform_devices_restart(s);
    CHECK(!s->reset_requested && !platform_reset_restarts(s));
}
static void test_restart_sources(struct platform_state *s)
{
    platform_devices_restart(s);
    requested = 0;
    write_mmio(port_addr(0xcf8), 0x80001004, 4);
    CHECK(read_mmio(port_addr(0xcf8), 4) == 0x80001004);
    write_mmio(port_addr(0xcf8), 0x55, 1);
    write_mmio(port_addr(0xcfa), 0x1234, 2);
    write_mmio(port_addr(0xcfb), 0x66, 1);
    write_mmio(port_addr(0xcf8), 0x7788, 2);
    CHECK(s->pci_config_address == 0x80001004 && read_mmio(port_addr(0xcf8), 4) == 0x80001004);
    CHECK(read_mmio(port_addr(0xcf8), 1) == 0xff && read_mmio(port_addr(0xcfa), 2) == 0xffff);
    write_mmio(port_addr(0xcf9), 0x02, 1);
    CHECK(!s->reset_requested && read_mmio(port_addr(0xcf9), 1) == 0xff);
    write_mmio(port_addr(0xcf9), 0x06, 1);
    CHECK(s->pci_config_address == 0x80001004);
    expect_restart(s, PLATFORM_RESET_CF9);
    CHECK(!s->pci_config_address);
    write_mmio(port_addr(0xcf9), 0x0e, 1);
    expect_restart(s, PLATFORM_RESET_CF9);
    write_mmio(port_addr(0x200c), 1, 1);
    expect_restart(s, PLATFORM_RESET_ACPI);
    write_mmio(port_addr(0x200c), 2, 1);
    CHECK(!s->reset_requested);
    /* The i8042 belongs to the guest only when its ports are allowed. */
    write_mmio(port_addr(0x64), 0xfe, 1);
    CHECK(!s->reset_requested);
    CHECK(platform_allow_ports(s, 0x60, 1) && platform_allow_ports(s, 0x64, 1));
    record_ports = true;
    recorded = 0;
    write_mmio(port_addr(0x64), 0xad, 1);
    write_mmio(port_addr(0x60), 0xf4, 1);
    write_mmio(port_addr(0x64), 0xd1, 1);
    write_mmio(port_addr(0x60), 0x03, 1);
    write_mmio(port_addr(0x64), 0xd1, 1);
    write_mmio(port_addr(0x64), 0xae, 1);
    write_mmio(port_addr(0x60), 0x02, 1);
    write_mmio(port_addr(0x64), 0xf1, 1);
    write_mmio(port_addr(0x64), 0xff, 1);
    write_mmio(port_addr(0x60), 0x02, 1);
    CHECK(!s->reset_requested && recorded == 5);
    CHECK(recorded_writes[0] == 0x64ad && recorded_writes[1] == 0x60f4 &&
          recorded_writes[2] == 0x64ae && recorded_writes[3] == 0x6002 &&
          recorded_writes[4] == 0x6002);
    write_mmio(port_addr(0x64), 0xd1, 1);
    write_mmio(port_addr(0x60), 0xfe, 1);
    CHECK(recorded == 5);
    expect_restart(s, PLATFORM_RESET_I8042);
    write_mmio(port_addr(0x64), 0xfe, 1);
    CHECK(recorded == 5);
    expect_restart(s, PLATFORM_RESET_I8042);
    write_mmio(port_addr(0x64), 0xf0, 1);
    CHECK(recorded == 5);
    expect_restart(s, PLATFORM_RESET_I8042);
    /* A pending D1h ends with the restart: the next 60h byte is data again. */
    write_mmio(port_addr(0x64), 0xd1, 1);
    platform_devices_restart(s);
    write_mmio(port_addr(0x60), 0xfe, 1);
    CHECK(!s->reset_requested && recorded == 6);
    record_ports = false;
    /* Per-boot state returns to its power-on value; NVRAM and regions stay. */
    unsigned regions = gm_region_count();
    write_mmio(IA64VMM_NVRAM_BASE + 0x100, 0x5a, 1);
    write_mmio(port_addr(0x2002), 0xaabb, 2);
    write_mmio(port_addr(0x2004), 0x0000, 2);
    write_mmio(IA64VMM_IOSAPIC_BASE, 0x10 + 2 * 3, 4);
    write_mmio(IA64VMM_IOSAPIC_BASE + 0x10, 0x31, 4);
    CHECK(!(s->iosapic.rte[3] & IA64_IOSAPIC_MASK));
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_MAP_SIZE, 48, 8);
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_EXIT_BOOT_SERVICES, 3, 8);
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_GOP_OWNED, 1, 8);
    CHECK(s->boot_services_exited && s->guest_gop_owned);
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_RESET_STATUS, 5, 8);
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_RESET, 3, 8);
    CHECK(s->reset_source == PLATFORM_RESET_EFI && platform_reset_restarts(s));
    platform_devices_restart(s);
    CHECK(read_mmio(IA64VMM_NVRAM_BASE + 0x100, 1) == 0x5a && gm_region_count() == regions);
    CHECK(read_mmio(port_addr(0x2002), 2) == 0 && read_mmio(port_addr(0x2004), 2) == 1);
    CHECK((s->iosapic.rte[3] & IA64_IOSAPIC_MASK) && s->iosapic.input[IA64VMM_SCI_GSI]);
    CHECK(!s->boot_services_exited && !s->guest_gop_owned && !s->map_size && !s->reset_status);
    CHECK(s->allowed_ports[0x64 / 8] & (1u << (0x64 & 7)));
    /* The first request decides: a later one of any source, and a later
     * status, change nothing. */
    write_mmio(port_addr(0x2004), 0x2000, 2);
    write_mmio(port_addr(0xcf9), 0x06, 1);
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_RESET_STATUS, 7, 8);
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_RESET, 0, 8);
    CHECK(s->reset_type == 2 && s->reset_source == PLATFORM_RESET_ACPI && !s->reset_status &&
          !platform_reset_restarts(s));
    platform_devices_restart(s);
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_RESET_STATUS, 9, 8);
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_RESET, 1, 8);
    write_mmio(port_addr(0x2004), 0x2000, 2);
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_RESET_STATUS, 0, 8);
    CHECK(s->reset_type == 1 && s->reset_source == PLATFORM_RESET_EFI && s->reset_status == 9 &&
          platform_reset_restarts(s));
    platform_devices_restart(s);
    /* Shutdown and unknown types never restart. */
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_RESET, 2, 8);
    CHECK(!platform_reset_restarts(s));
    platform_devices_restart(s);
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_RESET, 4, 8);
    CHECK(!platform_reset_restarts(s));
    platform_devices_restart(s);
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_RESET, 1, 8);
    CHECK(platform_reset_restarts(s));
    write_mmio(IA64VMM_SERVICE_BASE + IA64VMM_SVC_PANIC, 7, 8);
    CHECK(!platform_reset_restarts(s));
    platform_devices_restart(s);
    requested = 0;
}
static void test_devices(void)
{
    struct ia64vmm_handoff policy = {0};
    CHECK(platform_debug_policy("", &policy));
    CHECK(!(policy.flags & (IA64VMM_HF_UART_PRESENT | IA64VMM_HF_UART_CONSOLE)));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        struct platform_state off;
        CHECK(platform_devices_init(&off));
        CHECK(!gm_lookup(IA64VMM_UART_BASE));
        platform_devices_cleanup(&off);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        struct platform_state keyboard;
        platform_i8042_configure(true);
        CHECK(platform_devices_init(&keyboard));
        unsigned reads = port_reads, writes = port_writes;
        write_mmio(port_addr(0x64), 0x60, 1);
        write_mmio(port_addr(0x60), 0x65, 1);
        write_mmio(port_addr(0x64), 0xae, 1);
        write_mmio(port_addr(0x60), 0xf4, 1);
        CHECK(read_mmio(port_addr(0x60), 1) == 0xfa);
        platform_i8042_input(0x1c, false, false, false);
        CHECK((read_mmio(port_addr(0x64), 1) & 1) && keyboard.iosapic.input[1]);
        CHECK(read_mmio(port_addr(0x60), 1) == 0x1c && !keyboard.iosapic.input[1]);
        platform_i8042_input(0x48, false, true, false);
        CHECK(read_mmio(port_addr(0x60), 1) == 0xe0);
        CHECK(read_mmio(port_addr(0x60), 1) == 0x48);
        write_mmio(port_addr(0x64), 0x60, 1);
        write_mmio(port_addr(0x64), 0xd1, 1);
        write_mmio(port_addr(0x60), 1, 1);
        write_mmio(port_addr(0x64), 0xd0, 1);
        CHECK(read_mmio(port_addr(0x60), 1) == 1);
        write_mmio(port_addr(0x64), 0x20, 1);
        CHECK(read_mmio(port_addr(0x60), 1) == 0x65);
        write_mmio(port_addr(0x64), 0xd1, 1);
        write_mmio(port_addr(0x60), 2, 1);
        expect_restart(&keyboard, PLATFORM_RESET_I8042);
        CHECK(!(read_mmio(port_addr(0x64), 1) & 1));
        CHECK(port_reads == reads && port_writes == writes);
        platform_devices_cleanup(&keyboard);
        platform_i8042_configure(false);
        _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    struct platform_state s;
    CHECK(platform_debug_policy("debug_serial=console debug_screen=1", &policy));
    CHECK((policy.flags & (IA64VMM_HF_UART_PRESENT | IA64VMM_HF_UART_CONSOLE)) ==
          (IA64VMM_HF_UART_PRESENT | IA64VMM_HF_UART_CONSOLE));
    CHECK(vmm_guest_serial_screen_enabled());
    CHECK(!platform_debug_policy("debug_serial=bogus", &policy));
    CHECK(!platform_debug_policy("debug_screen=2", &policy));
    CHECK(platform_debug_policy("debug_serial=device", &policy));
    CHECK(!(policy.flags & IA64VMM_HF_UART_CONSOLE));
    CHECK(platform_devices_init(&s));
    CHECK(read_mmio(port_addr(0x2004), 2) == 1);
    uint64_t svc = IA64VMM_SERVICE_BASE;
    CHECK(read_mmio(svc, 8) == IA64VMM_SVC_MAGIC_VALUE);
    CHECK(read_mmio(svc + IA64VMM_SVC_VERSION, 8) == 1);
    CHECK(read_mmio(svc + IA64VMM_SVC_ITC_HZ, 8) == IA64VMM_ITC_HZ);
    CHECK(read_mmio(svc + IA64VMM_SVC_CONSOLE, 8) == 0);
    CHECK(read_mmio(svc + IA64VMM_SVC_RTC, 8) > 1700000000);
    write_mmio(svc + IA64VMM_SVC_MAGIC, 0, 8);
    CHECK(read_mmio(svc, 8) == IA64VMM_SVC_MAGIC_VALUE);
    write_mmio(svc + IA64VMM_SVC_CONSOLE, 'x', 1);
    write_mmio(svc + IA64VMM_SVC_CONSOLE, '\n', 8);
    unsigned before_prompt = fb_bytes;
    write_mmio(svc + IA64VMM_SVC_CONSOLE, 'p', 1);
    write_mmio(svc + IA64VMM_SVC_CONSOLE_FLUSH, 0, 1);
    CHECK(fb_bytes == before_prompt);
    write_mmio(svc + IA64VMM_SVC_CONSOLE_FLUSH, 0, 8);
    CHECK(fb_bytes == before_prompt + 10);
    write_mmio(svc + IA64VMM_SVC_WATCHDOG, 300, 8);
    CHECK(s.watchdog == 300);
    write_mmio(svc + IA64VMM_SVC_RESET_STATUS, 0x8000000000000001, 8);
    write_mmio(svc + IA64VMM_SVC_RESET, 1, 8);
    CHECK(s.reset_requested && s.reset_type == 1 && requested == VCPU_EXIT_SHUTDOWN);
    CHECK(s.reset_status == 0x8000000000000001);
    write_mmio(svc + IA64VMM_SVC_HALT, 0, 8);
    CHECK(requested == VCPU_EXIT_HALT);
    /* The native PAL registers belong to the accessing processor; writes
     * are 8 bytes (platform.h). */
    const unsigned pal_registers[] = {IA64VMM_SVC_PAL_XR0,      IA64VMM_SVC_PAL_EXPECTED,
                                      IA64VMM_SVC_PAL_PALE_PMI, IA64VMM_SVC_PAL_SALE_PMI,
                                      IA64VMM_SVC_PAL_EVENT,    IA64VMM_SVC_PAL_PMI_VECTOR};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(pal_registers); ++i) {
        unsigned off = pal_registers[i];
        CHECK(read_mmio(svc + off, 8) == UINT64_C(0x1000) + off && pal_service_offset == off);
        unsigned writes = pal_service_writes;
        write_mmio(svc + off, 0x8000000000123000 + i, 4);
        CHECK(pal_service_writes == writes);
        write_mmio(svc + off, 0x8000000000123000 + i, 8);
        CHECK(pal_service_writes == writes + 1 && pal_service_offset == off &&
              pal_service_value == 0x8000000000123000 + i);
    }
    const unsigned pal_operations[] = {IA64VMM_SVC_PAL_STATE_SAVE, IA64VMM_SVC_PAL_STATE_RESUME};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(pal_operations); ++i) {
        unsigned off = pal_operations[i], writes = pal_service_writes;
        write_mmio(svc + off, 0x8000000000123000 + i, 4);
        CHECK(pal_service_writes == writes);
        write_mmio(svc + off, 0x8000000000123000 + i, 8);
        CHECK(pal_service_writes == writes + 1 && pal_service_offset == off &&
              pal_service_value == 0x8000000000123000 + i);
    }
    add_ram(0x100000, 4096);
    write_mmio(svc + IA64VMM_SVC_LOAD_BASE, 0x100000, 8);
    write_mmio(svc + IA64VMM_SVC_LOAD_SIZE, 4096, 8);
    write_mmio(svc + IA64VMM_SVC_LOAD_TYPE, 1, 8);
    CHECK(s.loader_range_count == 1 && s.loader_ranges[0].base == 0x100000);
    write_mmio(svc + IA64VMM_SVC_LOAD_SIZE, UINT64_MAX, 8);
    write_mmio(svc + IA64VMM_SVC_LOAD_TYPE, 1, 8);
    CHECK(s.loader_range_count == 1);
    write_mmio(svc + IA64VMM_SVC_IMAGE_BASE, 0x100000, 8);
    write_mmio(svc + IA64VMM_SVC_IMAGE_SIZE, 1024, 8);
    CHECK(s.image_range_count == 1);
    write_mmio(svc + IA64VMM_SVC_MAP_SIZE, 48, 8);
    write_mmio(svc + IA64VMM_SVC_EXIT_BOOT_SERVICES, 3, 1);
    CHECK(!s.boot_services_exited);
    write_mmio(svc + IA64VMM_SVC_EXIT_BOOT_SERVICES, 3, 8);
    CHECK(s.boot_services_exited && s.handoff_pending && s.map_size == 48 && s.map_key == 3);
    CHECK(requested == VCPU_EXIT_DEBUG);
    write_mmio(svc + IA64VMM_SVC_DEBUG, 0xdeadbeef, 8);
    write_mmio(svc + IA64VMM_SVC_PANIC, 9, 8);
    CHECK(s.panic && s.panic_code == 9 && dumps == 1);
    uint64_t nv = IA64VMM_NVRAM_BASE;
    nvram_load(1, s.nvram);
    write_mmio(nv + 3, 0x123456789abcdef0, 8);
    CHECK(read_mmio(nv + 3, 8) == 0x123456789abcdef0 && read_mmio(nv + 5, 2) == 0x9abc);
    CHECK(!nvram_pending());
    write_mmio(svc + IA64VMM_SVC_NVRAM_COMMIT, 0, 8);
    CHECK(s.commits == 1 && nvram_pending());
    for (unsigned p = 0; p < 65536; ++p) {
        uint16_t decoded;
        CHECK(platform_decode_port(port_addr(p) - IA64VMM_LEGACY_IO_BASE, &decoded));
        CHECK(decoded == p);
    }
    uint16_t port;
    CHECK(!platform_decode_port(0x1000, &port));
    CHECK(read_mmio(port_addr(0x3f8), 1) == 0xff);
    CHECK(s.denied_ports[0x3f8 / 8] & (1u << (0x3f8 & 7)));
    CHECK(platform_allow_ports(&s, 0x3f8, 8));
    CHECK(!platform_allow_ports(&s, 65535, 2));
    /* port_trace counts accesses (forwarded or denied) from region>0 (OS)
     * IPs inside its port range. */
    s.port_trace = 2;
    s.port_trace_first = 0;
    s.port_trace_last = 0xffff;
    mock_ip = 0x100000;
    CHECK(read_mmio(port_addr(0x3f8), 4) == 0xa5a5a5a5 && port_reads == 1 && s.port_trace == 2);
    mock_ip = UINT64_C(0xe000000084000000);
    write_mmio(port_addr(0x3f8), 0x1234, 4);
    CHECK(port_writes == 1 && s.port_trace == 1);
    CHECK(read_mmio(port_addr(0x2f8), 1) == 0xff && s.port_trace == 0); /* denied, traced */
    s.port_trace = 2;
    s.port_trace_first = 0x60;
    s.port_trace_last = 0x64;
    CHECK(read_mmio(port_addr(0x3f8), 4) == 0xa5a5a5a5 &&
          s.port_trace == 2); /* outside the range */
    CHECK(read_mmio(port_addr(0x64), 1) == 0xff && s.port_trace == 1);
    s.port_trace = 0;
    mock_ip = 0;
    port_reads = 1;
    CHECK(platform_allow_ports(&s, 0x1fff, 32));
    CHECK(read_mmio(port_addr(0x1fff), 2) == 0xffff && port_reads == 1);
    write_mmio(port_addr(0x1fff), 0x1234, 2);
    CHECK(port_writes == 1);
    write_mmio(port_addr(0x2002), 0xaabb, 2);
    CHECK(read_mmio(port_addr(0x2002), 2) == 0xaabb);
    s.reset_requested = false;
    write_mmio(port_addr(0x2004), 0x2000, 2);
    CHECK(s.reset_type == 2 && !s.reset_status);
    s.reset_requested = false;
    write_mmio(port_addr(0x200c), 1, 1);
    CHECK(s.reset_type == 0 && s.reset_source == PLATFORM_RESET_ACPI);
    test_restart_sources(&s);
    CHECK(read_mmio(IA64VMM_ECAM_BASE + 0x100, 4) == UINT32_MAX);
    write_mmio(IA64VMM_IOSAPIC_BASE, 0x123, 4);
    CHECK(s.iosapic_logged);
    /* An idle SCI stays deasserted when programmed level/active-low. */
    write_mmio(IA64VMM_IOSAPIC_BASE, 0x10 + 2 * IA64VMM_SCI_GSI, 4);
    write_mmio(IA64VMM_IOSAPIC_BASE + 0x10, 0x31 | (1u << 15) | (1u << 13), 4);
    CHECK(!s.iosapic.rejected && !(s.iosapic.rte[IA64VMM_SCI_GSI] & IA64_IOSAPIC_REMOTE_IRR));
    CHECK(read_mmio(IA64VMM_PIB_BASE, 8) == 0 && s.pib_logged);
    expected_port = 0x3f9;
    write_mmio(svc + IA64VMM_SVC_PORT_SELECT, (4u << 16) | expected_port, 8);
    CHECK(read_mmio(svc + IA64VMM_SVC_PORT_DATA, 8) == 0xa5a5a5a5 && port_reads == 2);
    write_mmio(svc + IA64VMM_SVC_PORT_DATA, 0x1234, 8);
    CHECK(port_writes == 2);
    write_mmio(svc + IA64VMM_SVC_PORT_SELECT, (4u << 16) | 0x70, 8);
    CHECK(read_mmio(svc + IA64VMM_SVC_PORT_DATA, 8) == UINT32_MAX && port_reads == 2);
    write_mmio(svc + IA64VMM_SVC_PORT_SELECT, UINT64_MAX, 8);
    CHECK(read_mmio(svc + IA64VMM_SVC_PORT_DATA, 8) == UINT32_MAX && port_reads == 2);
    CHECK(read_mmio(IA64VMM_UART_BASE + 5, 1) == 0x60);
    write_mmio(IA64VMM_UART_BASE + 3, 0x80, 1);
    write_mmio(IA64VMM_UART_BASE, 'z', 1);
    write_mmio(IA64VMM_UART_BASE + 3, 0, 1);
    write_mmio(IA64VMM_UART_BASE, '\n', 1);
    write_mmio(IA64VMM_UART_BASE + 4, 0x10, 1);
    write_mmio(IA64VMM_UART_BASE, 'L', 1);
    CHECK(read_mmio(IA64VMM_UART_BASE, 1) == 'L');
    uint64_t cursor = 0, lost;
    char raw[8];
    CHECK(vmm_guest_serial_read(&cursor, raw, sizeof(raw), &lost) == 1 && !lost && raw[0] == '\n');
    platform_devices_cleanup(&s);
}

/* Lines starting with prefix that the run logged after ring position from,
 * read back from the RAM ring (the FB mirror is off while the guest runs). */
#define LOG_LINE 320
static unsigned logged_lines(uint64_t from, const char *prefix, char (*out)[LOG_LINE], unsigned max)
{
    const struct log_ring *ring = log_get_ring();
    CHECK(ring->write_pos - from <= LOG_RING_SIZE);
    char line[LOG_LINE];
    size_t n = 0;
    unsigned count = 0;
    for (uint64_t at = from; at < ring->write_pos; ++at) {
        char c = ring->data[at % LOG_RING_SIZE];
        if (c != '\n') {
            if (n < sizeof(line) - 1)
                line[n++] = c;
            continue;
        }
        line[n] = 0;
        n = 0;
        if (strncmp(line, prefix, strlen(prefix)))
            continue;
        CHECK(count < max);
        strcpy(out[count++], line);
    }
    return count;
}
/* Exact milestone lines per scenario. Scenarios 0-5 stop
 * after a full slice, a 100-tick halt and a final slice; 6 spends 2 s in
 * its first slice and hits timeout_sec=1; 7 walks EBS, the loader's resume
 * and the kernel entry with a frozen clock; 8 repeats 7 with 3.25 ms per
 * slice, 1 ms translation per full slice and a 7.5 ms halt; 10-11 keep
 * full budgets after EBS with observation absent or explicitly disabled. */
static const char *const vm_milestones[][5] = {
    [0 ... 1] = {"[info] milestone exit retired=3000 exec_ms=0 idle_ms=0 wall_ms=0"},
    [3 ... 5] = {"[info] milestone exit retired=3000 exec_ms=0 idle_ms=0 wall_ms=0"},
    [2] = {"[info] milestone exit retired=3000 exec_ms=0 idle_ms=0 wall_ms=0",
           "[info] milestone exit retired=4000 exec_ms=0 idle_ms=0 wall_ms=0",
           "[info] milestone exit retired=5000 exec_ms=0 idle_ms=0 wall_ms=0"},
    [6] = {"[info] milestone exit retired=1000 exec_ms=2000 idle_ms=0 wall_ms=2000"},
    [7] = {"[info] milestone ebs retired=2000 exec_ms=0 idle_ms=0 wall_ms=0",
           "[info] milestone loader_resumed retired=2001 exec_ms=0 idle_ms=0 wall_ms=0",
           "[info] milestone kernel_entry retired=2003 exec_ms=0 idle_ms=0 wall_ms=0",
           "[info] milestone exit retired=4003 exec_ms=0 idle_ms=0 wall_ms=0"},
    [8] =
        {"[info] milestone ebs retired=2000 exec_ms=6 idle_ms=0 compile_ms=2 wall_ms=6",
         "[info] milestone loader_resumed retired=2001 exec_ms=9 idle_ms=0 compile_ms=2 wall_ms=9",
         "[info] milestone kernel_entry retired=2003 exec_ms=16 idle_ms=0 compile_ms=2 wall_ms=16",
         "[info] milestone exit retired=4003 exec_ms=22 idle_ms=7 compile_ms=4 wall_ms=30"},
    [10 ... 11] = {"[info] milestone ebs retired=2000 exec_ms=0 idle_ms=0 wall_ms=0",
                   "[info] milestone exit retired=7000 exec_ms=0 idle_ms=0 wall_ms=0"},
};
static void test_vm(const char *file)
{
    size_t n;
    uint8_t *elf = load_file(file, &n);
    add_ram(0x100000, 63 * 1024 * 1024);
    struct boot_info bi = {0};
    bi.module_count = 1;
    strcpy(bi.modules[0].name, "firmware");
    bi.modules[0].hva = elf;
    bi.modules[0].size = n;
    static const struct {
        const char *tokens;
        unsigned flags;
        bool valid;
    } fp_cases[] = {
        {"", 0, true},
        {"fp_experimental=1", ENGINE_FP_EXPERIMENTAL, true},
        {"fp_experimental=1 fp_approx=1 fp_host=0 fp_check=1 fp_stats=1", 31, true},
        {"fp_experimental=0 fp_approx=1", ENGINE_FP_APPROX, true},
        {"fp_experimental=1 fp_experimental=0", ENGINE_FP_EXPERIMENTAL, true},
        {"fp_experimental=2", 0, false},
        {"fp_experimental=", 0, false},
        {"fp_approx=yes", 0, false},
        {"fp_host=2", 0, false},
    };
    for (unsigned i = 0; i < sizeof(fp_cases) / sizeof(fp_cases[0]); ++i) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (!pid) {
            scenario = 0;
            mock_time = true;
            snprintf(bi.cmdline, sizeof(bi.cmdline), "engine=lockstep %s", fp_cases[i].tokens);
            int rc = vm_run_guest(&bi);
            CHECK(rc == (fp_cases[i].valid ? 0 : VMM_EXIT_SETUP));
            CHECK(fp_config_calls == (fp_cases[i].valid ? 1u : 0u));
            if (fp_cases[i].valid)
                CHECK(fp_options == fp_cases[i].flags);
            _exit(0);
        }
        int status;
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    static const char *const invalid_samples[] = {"0", "3601", "offx"};
    for (unsigned i = 0; i < sizeof(invalid_samples) / sizeof(invalid_samples[0]); ++i) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (!pid) {
            scenario = 0;
            mock_time = true;
            snprintf(bi.cmdline, sizeof(bi.cmdline), "engine=lockstep sample_sec=%s",
                     invalid_samples[i]);
            CHECK(vm_run_guest(&bi) == VMM_EXIT_SETUP);
            _exit(0);
        }
        int status;
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    static const int codes[] = {0, 1, 0, 1, 2, 3, 5, 0, 0, 0, 0, 0};
    CHECK(hal_counter_hz() == UINT64_C(1000000000)); /* vm_milestones assumes ns ticks */
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (!pid) {
            scenario = i;
            mock_time = true;
            mock_reserve = i == 6;
            strcpy(
                bi.cmdline,
                i == 6
                    ? "timeout_sec=1 sample_sec=1 dump_va=g40:8,bad!:8,1000:24,2004:12 stop_fault=1 log_faults=64 port_trace=12:60-64"
                : i == 8           ? "engine=lockstep halt_retry_us=0 handoff_steps=10"
                : i >= 7 && i <= 9 ? "engine=lockstep handoff_steps=10"
                : i == 11          ? "engine=lockstep handoff_steps=0"
                : i == 0           ? "engine=lockstep sample_sec=off"
                                   : "engine=lockstep");
            uint64_t from = log_get_ring()->write_pos;
            CHECK(vm_run_guest(&bi) == codes[i]);
            static char lines[8][LOG_LINE], samples[8][LOG_LINE];
            unsigned count = logged_lines(from, "[info] milestone ", lines, 8);
            unsigned sampled = logged_lines(from, "[info] sample t=", samples, 8);
            for (unsigned k = 0; i != 9 && k < count; ++k) {
                if (!vm_milestones[i][k] || strcmp(lines[k], vm_milestones[i][k]))
                    fprintf(stderr, "scenario %u milestone %u: logged \"%s\", expected \"%s\"\n", i,
                            k, lines[k], vm_milestones[i][k] ? vm_milestones[i][k] : "(none)");
                CHECK(vm_milestones[i][k] && !strcmp(lines[k], vm_milestones[i][k]));
            }
            if (i == 9) {
                CHECK(count == 4);
                CHECK(strstr(lines[2], "milestone kernel_entry "));
                char entries[2][LOG_LINE];
                CHECK(logged_lines(from, "[info] EFI loader done ", entries, 2) == 1);
                CHECK(strstr(entries[0], "allocation=400000+1000"));
                CHECK(!logged_lines(from, "[warn] EFI entry observation capacity exhausted",
                                    entries, 2));
            } else
                CHECK(count < 5 && !vm_milestones[i][count]);
            if (i >= 10) {
                char diagnostics[1][LOG_LINE];
                CHECK(!instruction_queries);
                CHECK(!logged_lines(from, "[info] EFI pre-exit code allocation ", diagnostics, 1));
                CHECK(!logged_lines(from, "[info] EFI handoff observation ", diagnostics, 1));
                CHECK(
                    !logged_lines(from, "[warn] EFI kernel entry not recognised ", diagnostics, 1));
            }
            /* sample_sec stays opt-in; every vcpu_progress call is a sample or a milestone. */
            CHECK(i == 6 ? sampled > 0 : !sampled);
            /* Each sample line carries the VMM reservation's use and peak. */
            for (unsigned k = 0; k < sampled; ++k)
                CHECK(
                    strstr(samples[k], " reserve_used_bytes=83886080 reserve_peak_bytes=83886080"));
            CHECK(progress_samples == sampled + count);
            CHECK(virtual_reads == (i == 6 ? 5u : 0u));
            char restarts[3][LOG_LINE];
            CHECK(logged_lines(from, "[info] guest restart ", restarts, 3) == (i == 2 ? 2u : 0u));
            CHECK(boots == (i == 2 ? 3u : 1u));
            if (i == 2) {
                CHECK(!strcmp(
                    restarts[0],
                    "[info] guest restart 1 source=efi type=platform status=8000000000000001"));
                CHECK(!strcmp(restarts[1], "[info] guest restart 2 source=cf9 type=cold status=0"));
            }
            /* Save native EFI variables once at run end, across guest restarts. */
            CHECK(nvram_loads == 1 && nvram_saves == 1 && nvram_format == 1);
            CHECK(entry.gr[10] == IA64VMM_PLATFORM_ID && entry.bsp == entry.bspstore);
            CHECK(config.pal_entry == 0 && config.itc_hz == IA64VMM_ITC_HZ);
            if (i != 6)
                CHECK(config.engine == ENGINE_LOCKSTEP &&
                      idle_deadline == (i == 8 ? 27000000 : 100));
            CHECK(!codes[i] ? !dumps : dumps > 0);
            if (i >= 7)
                CHECK(steps == (i == 9 ? 307u : 7u));
            _exit(0);
        }
        int status;
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    /* A failed function reset ends the run (with the one write); exit=halt
     * is applied after vm_run_guest's write. */
    for (unsigned k = 0; k < 2; ++k) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (!pid) {
            scenario = k ? 0 : 2;
            mock_time = true;
            reset_fails = !k;
            strcpy(bi.cmdline, k ? "engine=lockstep exit=halt" : "engine=lockstep");
            CHECK(vm_run_guest(&bi) == (k ? VMM_EXIT_SUCCESS : VMM_EXIT_FAILURE));
            CHECK(boots == 1 && nvram_loads == 1 && nvram_saves == 1);
            _exit(0);
        }
        int status;
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    /* tier1=: only on or off, only with engine=lockstep;
     * on configures checked Tier1 (a setup error in this Tier1-less suite),
     * off leaves plain lockstep. */
    static const struct {
        const char *cmdline;
        int code;
        unsigned configures;
    } tier1[] = {
        {"engine=lockstep tier1=on", VMM_EXIT_SETUP, 1},
        {"engine=tier0 tier1=on", VMM_EXIT_SETUP, 0},
        {"engine=interp tier1=off", VMM_EXIT_SETUP, 0},
        {"engine=lockstep tier1=yes", VMM_EXIT_SETUP, 0},
        {"engine=lockstep tier1=", VMM_EXIT_SETUP, 0},
        {"engine=lockstep tier1=off", 0, 0},
    };
    for (unsigned i = 0; i < sizeof(tier1) / sizeof(tier1[0]); ++i) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (!pid) {
            scenario = 0;
            mock_time = true;
            strcpy(bi.cmdline, tier1[i].cmdline);
            int code = vm_run_guest(&bi);
            if (code != tier1[i].code || lockstep_configures != tier1[i].configures)
                fprintf(stderr, "vm %s: exit %d, %u checked-Tier1 configures\n", tier1[i].cmdline,
                        code, lockstep_configures);
            CHECK(code == tier1[i].code && lockstep_configures == tier1[i].configures);
            _exit(0);
        }
        int status;
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    /* SMP replay reaches the engine capability check. */
    pid_t replay_pid = fork();
    CHECK(replay_pid >= 0);
    if (!replay_pid) {
        CHECK(!hal_linux_configure(true, 2, 0, NULL, 0));
        strcpy(bi.cmdline, "engine=lockstep tier1=on smp_serialize=1");
        CHECK(vm_run_guest(&bi) == VMM_EXIT_SETUP);
        CHECK(lockstep_configures == 1);
        _exit(0);
    }
    int replay_status;
    CHECK(waitpid(replay_pid, &replay_status, 0) == replay_pid && WIFEXITED(replay_status) &&
          !WEXITSTATUS(replay_status));
#ifndef IA64VMM_TIER1_CHECKED
    /* A normal product rejects even zero-valued replay options, for every
     * engine, before an engine-specific setup can silently ignore them. */
    static const char *const engines[] = {"interp", "tier0", "tier1", "lockstep"};
    static const char *const replay[] = {"tier1_check_every=1", "tier1_check_every=0",
                                         "tier1_check_after=0"};
    for (unsigned e = 0; e < 4; ++e)
        for (unsigned o = 0; o < 3; ++o) {
            pid_t pid = fork();
            CHECK(pid >= 0);
            if (!pid) {
                snprintf(bi.cmdline, sizeof(bi.cmdline), "engine=%s %s", engines[e], replay[o]);
                uint64_t from = log_get_ring()->write_pos;
                CHECK(vm_run_guest(&bi) == VMM_EXIT_SETUP);
                char lines[2][LOG_LINE];
                CHECK(logged_lines(from,
                                   "[error] Tier1 replay options require a TIER1_CHECKED=1 build",
                                   lines, 2) == 1);
                _exit(0);
            }
            int status;
            CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
        }
#endif
    /* The effective default is sized after attachment; an explicit budget
     * remains authoritative even when the reserve is smaller. */
    for (unsigned user = 0; user < 2; ++user) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (!pid) {
            scenario = 0;
            mock_time = mock_reserve = true;
            configured_snapshot_budget = user ? 100u << 20 : CODE_SNAPSHOT_DEFAULT_BYTES;
            strcpy(bi.cmdline, user ? "engine=tier0 code_snapshot_mib=100" : "engine=tier0");
            uint64_t from = log_get_ring()->write_pos;
            CHECK(vm_run_guest(&bi) == 0);
            CHECK(reserve_budget_calls == !user && tier1_budget_calls == 1);
            char lines[1][LOG_LINE];
            CHECK(logged_lines(from, "[info] code snapshot budget_bytes=", lines, 1) == 1);
            CHECK(strstr(lines[0], user ? "104857600 policy=user" : "25165824 policy=default"));
            _exit(0);
        }
        int status;
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    free(elf);
}
/* Config-space fixture includes a reserved root and boot controller, a bridge,
 * AHCI with I/O/ROM BARs, and a downstream function with a 64-bit BAR. */
struct fake_pci_function {
    uint16_t bdf;
    uint8_t config[256];
    uint32_t masks[64];
    bool probing[64];
    unsigned writes;
    unsigned last_read; /* offset + 1 of the last access when it was a read */
};
static struct fake_pci_function fake_pci[8];
static uint8_t fake_bar_memory[32768];
static uint8_t fake_vga_memory[IA64VMM_VGA_MEMORY_SIZE];
static struct fake_pci_function *fake_function(uint16_t bdf)
{
    for (unsigned i = 0; i < 8; ++i)
        if (fake_pci[i].bdf == bdf)
            return &fake_pci[i];
    return NULL;
}
static void fake_put(struct fake_pci_function *f, unsigned off, uint32_t value, unsigned size)
{
    for (unsigned i = 0; i < size; ++i)
        f->config[off + i] = value >> (i * 8);
}
static uint32_t fake_pci_read(void *opaque, uint16_t bdf, unsigned off, unsigned size)
{
    (void)opaque;
    struct fake_pci_function *f = fake_function(bdf);
    if (!f || off + size > 256)
        return UINT32_MAX;
    f->last_read = off + 1;
    if (f->probing[off / 4])
        return f->masks[off / 4];
    uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= (uint32_t)f->config[off + i] << (i * 8);
    return value;
}
static void fake_pci_write(void *opaque, uint16_t bdf, unsigned off, uint32_t value, unsigned size)
{
    (void)opaque;
    struct fake_pci_function *f = fake_function(bdf);
    CHECK(f && off + size <= 256);
    ++f->writes;
    f->last_read = 0;
    if (size == 4 && (value == UINT32_MAX || value == 0xfffffffeu)) {
        CHECK(!(fake_pci_read(NULL, bdf, 4, 2) & 7));
        f->probing[off / 4] = true;
    } else {
        f->probing[off / 4] = false;
        fake_put(f, off, value, size);
    }
}
static uint8_t *fake_pci_map(void *opaque, uint64_t base, uint64_t size)
{
    (void)opaque;
    if (base == IA64VMM_VGA_MEMORY_BASE && size == sizeof(fake_vga_memory))
        return fake_vga_memory;
    CHECK(size <= sizeof(fake_bar_memory));
    return fake_bar_memory;
}
static uint64_t pci_address(uint16_t bdf, unsigned reg)
{
    return ((uint64_t)bdf << 12) | reg;
}
static bool storage_read(void *context, uint64_t lba, unsigned count, void *buffer)
{
    (void)context;
    (void)lba;
    (void)count;
    (void)buffer;
    CHECK(false);
    return false;
}
static bool storage_flush(void *context)
{
    (void)context;
    CHECK(false);
    return false;
}
static bool storage_resolve(void *context, uint16_t bdf, unsigned pin, uint32_t *gsi, bool *low,
                            bool *level)
{
    (void)context;
    CHECK(pin == 1);
    *gsi = 24 + bdf / 8;
    *low = *level = true;
    return true;
}
static bool storage_route(void *context, uint32_t gsi, unsigned cpu_index, unsigned vector,
                          bool low)
{
    (void)context;
    CHECK(gsi >= 25 && cpu_index == 0 && vector && low);
    return true;
}
static void storage_mask(void *context, uint32_t gsi, bool masked)
{
    (void)context;
    (void)gsi;
    (void)masked;
}
static void test_storage(void)
{
    /* Separate maps: registrations are permanent within one VMM run. */
    for (unsigned scenario_index = 0; scenario_index < 3; ++scenario_index) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (!pid) {
            static const struct pci_host_ops ops = {fake_pci_read, fake_pci_write, fake_pci_map};
            static const struct platform_intx_ops irq = {
                .resolve = storage_resolve, .route = storage_route, .mask = storage_mask};
            for (unsigned i = 0; i < 8; ++i) {
                memset(fake_pci + i, 0, sizeof(fake_pci[i]));
                fake_pci[i].bdf = 0xffff;
            }
            unsigned functions = scenario_index == 2 ? 5 : scenario_index;
            for (unsigned i = 0; i < functions; ++i) {
                struct fake_pci_function *p = fake_pci + i;
                p->bdf = (i + 1) * 8;
                fake_put(p, 0, 0x12348086, 4);
                fake_put(p, 8, 0x02000000, 4);
                fake_put(p, 0x3d, 1, 1);
                /* Firmware IRQ bytes differ from the assigned guest GSIs. */
                fake_put(p, 0x3c, scenario_index == 1 ? 20 : 1, 1);
            }
            platform_pci_set_host(&ops, NULL);
            platform_pci_external_media(true);
            platform_pci_root_buses(0, 0);
            platform_intx_set_host(&irq, NULL);
            CHECK(platform_pci_inventory("vmm_media=hyperv:0"));
            const struct platform_block block = {.sectors = 1024,
                                                 .sector_bytes = 512,
                                                 .read_only = true,
                                                 .read = storage_read,
                                                 .flush = storage_flush};
            CHECK(platform_storage_attach(&block));
            const struct platform_block dvd = {.sectors = 128,
                                               .sector_bytes = 2048,
                                               .read_only = true,
                                               .read = storage_read,
                                               .flush = storage_flush};
            CHECK(platform_storage_attach(&dvd));
            if (!scenario_index) {
                for (unsigned i = 2; i < PLATFORM_AHCI_MAX_PORTS; ++i)
                    CHECK(platform_storage_attach(&block));
                CHECK(!platform_storage_attach(&block));
                CHECK(platform_storage_attach(NULL) && !platform_storage_configured());
                CHECK(platform_storage_attach(&block) && platform_storage_attach(&dvd));
            }
            struct platform_state state;
            if (scenario_index == 2) {
                CHECK(!platform_devices_init(&state));
                CHECK(platform_pci_device(4)->irq_line == PLATFORM_STORAGE_GSI);
                CHECK(!platform_ahci_present());
            } else {
                CHECK(platform_devices_init(&state));
                CHECK(platform_ahci_present());
                CHECK(platform_pci_read(pci_address(PLATFORM_STORAGE_BDF, 0), 4) == 0x29228086);
                CHECK(platform_pci_read(pci_address(PLATFORM_STORAGE_BDF, 8), 4) >> 8 == 0x010601);
                CHECK(platform_pci_read(pci_address(PLATFORM_STORAGE_BDF, 0x24), 4) ==
                      PLATFORM_STORAGE_BASE);
                platform_pci_write(pci_address(PLATFORM_STORAGE_BDF, 0x24), UINT32_MAX, 4);
                CHECK(platform_pci_read(pci_address(PLATFORM_STORAGE_BDF, 0x24), 4) == 0xffffe000);
                platform_pci_write(pci_address(PLATFORM_STORAGE_BDF, 0x24), PLATFORM_STORAGE_BASE,
                                   4);
                const struct gm_region *bar = gm_lookup(PLATFORM_STORAGE_BASE);
                CHECK(bar && bar->kind == GM_EMULATED && bar->size == PLATFORM_AHCI_SIZE);
                platform_pci_write(pci_address(PLATFORM_STORAGE_BDF, 4), 2, 2);
                CHECK(read_mmio(PLATFORM_STORAGE_BASE + 0x0c, 4) == 3);
                CHECK((read_mmio(PLATFORM_STORAGE_BASE, 4) & 31) == 1);
                CHECK(read_mmio(PLATFORM_STORAGE_BASE + 0x1a4, 4) == 0xeb140101);
                struct ia64vmm_handoff handoff = {0};
                CHECK(platform_pci_handoff(&handoff));
                CHECK(handoff.irq_route_count == functions + 1);
                const struct ia64vmm_irq_route *route = handoff.irq_routes + functions;
                CHECK(route->bdf == PLATFORM_STORAGE_BDF && route->parent == 0xffff &&
                      route->pin == 0 && route->gsi == PLATFORM_STORAGE_GSI);
                CHECK(handoff.pci_mmio_range_count == 1 &&
                      handoff.pci_mmio[0].base == PLATFORM_STORAGE_BASE &&
                      handoff.pci_mmio[0].size == PLATFORM_AHCI_SIZE);
                CHECK(state.iosapic.input[PLATFORM_STORAGE_GSI]);
            }
            platform_devices_cleanup(&state);
            CHECK(platform_storage_attach(NULL));
            platform_pci_set_host(NULL, NULL);
            platform_intx_set_host(NULL, NULL);
            _exit(0);
        }
        int status;
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
}
static void test_pci_addresses(void)
{
    struct pci_ecam_region r = {0x42000000, 7, 32, 63};
    struct pci_address a = {7, 32, 3, 2};
    uint64_t at = 0;
    CHECK(pci_ecam_address(&r, a, 0xfc, &at) && at == 0x4201a0fc);
    a = (struct pci_address){7, 63, 31, 7};
    CHECK(pci_ecam_address(&r, a, 0xffc, &at) && at == 0x43fffffc);
    a = (struct pci_address){8, 32, 0, 0};
    CHECK(!pci_ecam_address(&r, a, 0, &at));
    a = (struct pci_address){7, 31, 0, 0};
    CHECK(!pci_ecam_address(&r, a, 0, &at));
    a = (struct pci_address){7, 32, 32, 0};
    CHECK(!pci_ecam_address(&r, a, 0, &at));
    a = (struct pci_address){7, 32, 0, 8};
    CHECK(!pci_ecam_address(&r, a, 0, &at));
    a = (struct pci_address){7, 32, 0, 0};
    CHECK(!pci_ecam_address(&r, a, 2, &at));
    CHECK(!pci_ecam_address(&r, a, 0x1000, &at));
    r.base += 4096;
    CHECK(!pci_ecam_address(&r, a, 0, &at));
    r = (struct pci_ecam_region){0xfffffffffff00000ULL, 7, 0, 1};
    a.bus = 0;
    CHECK(!pci_ecam_address(&r, a, 0, &at));
    r.first = 2;
    CHECK(!pci_ecam_address(&r, a, 0, &at));
    a = (struct pci_address){0, 0, 2, 0};
    CHECK(pci_cf8_address(a, 0) == 0x80001000);
    CHECK(pci_cf8_address(a, 4) == 0x80001004);
    CHECK(!pci_cf8_address(a, 0x100) && !pci_cf8_address(a, 6));
    a = (struct pci_address){0, 0xab, 31, 7};
    CHECK(pci_cf8_address(a, 0xfc) == 0x80abfffc);
    a.segment = 1;
    CHECK(!pci_cf8_address(a, 4));
}
static void boot_media_fixture(uint32_t class_code)
{
    memset(fake_pci, 0, sizeof(fake_pci));
    for (unsigned i = 0; i < 8; ++i)
        fake_pci[i].bdf = 0xffff;
    for (unsigned i = 0; i < 3; ++i) {
        struct fake_pci_function *f = fake_pci + i;
        f->bdf = (i + 1) * 8;
        fake_put(f, 0, 0x12348086, 4);
        fake_put(f, 8, (i == 2 ? class_code : 0x010601) << 8, 4);
        fake_put(f, 4, 7, 2);
        fake_put(f, 0x10, 0xe2000000 + i * 0x10000, 4);
        f->masks[0x10 / 4] = 0xfffff000;
    }
}
static void test_pci_boot_media(void)
{
    static const struct pci_host_ops ops = {fake_pci_read, fake_pci_write, fake_pci_map};
    const uint32_t supported[] = {0x0c0320, 0x0c0330, 0x010802, 0x010601};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(supported); ++i)
        for (unsigned automatic = 0; automatic < 2; ++automatic) {
            pid_t pid = fork();
            CHECK(pid >= 0);
            if (!pid) {
                boot_media_fixture(supported[i]);
                platform_pci_set_host(&ops, NULL);
                platform_pci_root_buses(0, 0);
                CHECK(platform_pci_boot_media(0, 24, supported[i]));
                CHECK(platform_pci_inventory(automatic ? "vmm_media=auto" : ""));
                CHECK(platform_pci_count() == 3);
                CHECK(platform_pci_device(0)->owner == PCI_OWNER_GUEST);
                CHECK(platform_pci_device(1)->owner == PCI_OWNER_GUEST);
                CHECK(platform_pci_device(2)->owner == PCI_OWNER_VMM);
                CHECK(!strcmp(platform_pci_device(2)->reservation, "vmm-media"));
                CHECK(platform_pci_read(pci_address(24, 0), 4) == UINT32_MAX);
                CHECK(platform_pci_read(pci_address(24, 0x10), 4) == UINT32_MAX);
                unsigned writes = fake_pci[2].writes;
                platform_pci_write(pci_address(24, 4), 0, 2);
                platform_pci_write(pci_address(24, 0x10), UINT32_MAX, 4);
                CHECK(fake_pci[2].writes == writes);
                CHECK(platform_pci_read(pci_address(8, 0), 4) == 0x12348086);
                struct platform_state state;
                CHECK(platform_devices_init(&state));
                CHECK(gm_lookup(0xe2000000)->kind == GM_DIRECT_MMIO);
                CHECK(gm_lookup(0xe2010000)->kind == GM_DIRECT_MMIO);
                const struct gm_region *reserved = gm_lookup(0xe2020000);
                CHECK(!reserved || reserved->kind != GM_DIRECT_MMIO);
                platform_devices_cleanup(&state);
                _exit(0);
            }
            int status;
            CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
        }
    boot_media_fixture(0x010802);
    platform_pci_set_host(&ops, NULL);
    CHECK(!platform_pci_inventory(""));
    CHECK(!platform_pci_inventory("vmm_media=auto"));
    for (unsigned i = 0; i < 8; ++i)
        CHECK(!fake_pci[i].writes);
    const uint32_t unsupported[] = {0x01018f, 0x010400, 0x0c0300, 0x0c0310, 0x020000, 0x10010601};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(unsupported); ++i) {
        platform_pci_set_host(&ops, NULL);
        CHECK(!platform_pci_boot_media(0, 24, unsupported[i]));
        CHECK(!platform_pci_inventory("vmm_media=auto"));
    }
    platform_pci_set_host(&ops, NULL);
    CHECK(!platform_pci_boot_media(1, 24, 0x010802));
    CHECK(!platform_pci_inventory(""));
    platform_pci_set_host(&ops, NULL);
    CHECK(platform_pci_boot_media(0, 32, 0x010802));
    CHECK(!platform_pci_inventory(""));
    platform_pci_set_host(&ops, NULL);
    CHECK(platform_pci_boot_media(0, 24, 0x010601));
    CHECK(!platform_pci_inventory("vmm_media=auto"));
    for (unsigned i = 0; i < 8; ++i)
        CHECK(!fake_pci[i].writes);
    platform_pci_set_host(&ops, NULL);
    CHECK(platform_pci_boot_media(0, 24, 0x010802));
    CHECK(!platform_pci_inventory("vmm_media=invalid"));
    platform_pci_set_host(&ops, NULL);
    CHECK(!platform_pci_inventory(""));
    CHECK(platform_pci_boot_media(0, 24, 0x010802));
    CHECK(platform_pci_inventory("vmm_media=00:01.0"));
    CHECK(platform_pci_device(0)->owner == PCI_OWNER_VMM);
    CHECK(platform_pci_device(2)->owner == PCI_OWNER_GUEST);
    platform_pci_set_host(&ops, NULL);
    platform_pci_external_media(true);
    CHECK(platform_pci_boot_media(0, 24, 0x010802));
    CHECK(platform_pci_inventory("vmm_media=hyperv:0"));
    for (unsigned i = 0; i < 3; ++i)
        CHECK(platform_pci_device(i)->owner == PCI_OWNER_GUEST);
    platform_pci_set_host(NULL, NULL);
    memset(fake_pci, 0, sizeof(fake_pci));
}
static void test_pci_ari(void)
{
    static const struct pci_host_ops ops = {fake_pci_read, fake_pci_write, fake_pci_map};
    memset(fake_pci, 0, sizeof(fake_pci));
    for (unsigned i = 0; i < 8; ++i)
        fake_pci[i].bdf = 0xffff;
    const uint16_t ids[] = {0x08, 0x100, 0x101, 0x109, 0x1ff};
    for (unsigned i = 0; i < 5; ++i) {
        fake_pci[i].bdf = ids[i];
        fake_put(fake_pci + i, 0, 0x12348086 + (i << 16), 4);
    }
    struct fake_pci_function *port = fake_pci;
    fake_put(port, 0x0e, 1, 1);
    fake_put(port, 0x18, 0x00010100, 4);
    fake_put(port, 6, 0x10, 2);
    fake_put(port, 0x34, 0x40, 1);
    fake_put(port, 0x40, 0x00420010, 4); /* Root Port, capability version 2 */
    fake_put(port, 0x64, 0x20, 4);
    fake_put(port, 0x68, 0x20, 2);
    platform_pci_set_host(&ops, NULL);
    platform_pci_root_buses(0, 1);
    platform_pci_external_media(true);
    CHECK(platform_pci_inventory(""));
    CHECK(platform_pci_count() == 5); /* F0.MF=0; high function 255 included. */
    for (unsigned i = 1; i < 5; ++i) {
        CHECK(platform_pci_device(i)->bdf == ids[i]);
        CHECK(platform_pci_device(i)->parent == 0x08);
        CHECK(platform_pci_device(i)->ari);
    }
    CHECK(platform_pci_read(pci_address(0x100, 0x0e), 1) == 0x80);
    /* Function 9 and 255 appear as ordinary conventional multifunction
     * devices with read-only presence functions at 8 and 248. */
    CHECK(platform_pci_read(pci_address(0x108, 0), 2) == 0x8086);
    CHECK(platform_pci_read(pci_address(0x1f8, 0x0e), 1) == 0x80);
    CHECK(platform_pci_read(pci_address(0x108, 0x10), 4) == 0);
    unsigned writes = fake_pci[3].writes;
    platform_pci_write(pci_address(0x108, 4), 7, 2);
    CHECK(fake_pci[3].writes == writes);
    CHECK(platform_pci_read(pci_address(0x109, 0), 4) == 0x12378086);
    /* The guest cannot turn forwarding off on the active host port. */
    platform_pci_write(pci_address(0x08, 0x68), 0, 2);
    CHECK(fake_pci_read(NULL, 0x08, 0x68, 2) == 0x20);
    CHECK(platform_pci_read(pci_address(0x109, 0x100), 4) == 0);
    struct acpi_irq *routes = calloc(1, sizeof(*routes));
    CHECK(routes);
    routes->prepared = true;
    routes->nroutes = 1;
    routes->routes[0] = (struct acpi_irq_route){.bus = 1, .address = 0xffff, .gsi = 40};
    uint32_t gsi;
    bool low, level;
    CHECK(acpi_irq_resolve(routes, 0x109, 1, &gsi, &low, &level) && gsi == 40);
    routes->routes[0] = (struct acpi_irq_route){.bus = 0, .address = 0x1ffff, .gsi = 41};
    CHECK(acpi_irq_resolve(routes, 0x1ff, 1, &gsi, &low, &level) && gsi == 41);
    free(routes);
    /* A reserved conventional function zero stays present for ARI siblings. */
    platform_pci_set_host(&ops, NULL);
    platform_pci_root_buses(0, 1);
    fake_put(fake_pci + 1, 8, 0x01060100, 4);
    CHECK(platform_pci_inventory("vmm_media=01:00.0"));
    CHECK(platform_pci_read(pci_address(0x100, 0x0e), 1) == 0x80);
    CHECK(platform_pci_read(pci_address(0x100, 4), 4) == 0);
    platform_pci_set_host(NULL, NULL);
    memset(fake_pci, 0, sizeof(fake_pci));
}
static void test_pci(void)
{
    test_pci_addresses();
    static const struct pci_host_ops ops = {fake_pci_read, fake_pci_write, fake_pci_map};
    /* Function 6: a native-mode PIIX3 IDE (no compatibility ports, no BARs),
     * whose timing block 0x40-0x4b is the guest's; function 7: a PCH SATA
     * controller in IDE mode as D31:F5 (native, prog-if 0x85). */
    const uint16_t bdfs[] = {0, 8, 16, 248, 256, 263, 264, 272};
    const uint32_t classes[] = {0x060000, 0x060400, 0x010802, 0x010601,
                                0x020000, 0x060100, 0x01018f, 0x010185};
    const uint32_t ids[] = {0x12348086, 0x12348086, 0x12348086, 0x29228086,
                            0x12348086, 0x12348086, 0x70108086, 0x1c088086};
    for (unsigned i = 0; i < 8; ++i) {
        fake_pci[i].bdf = bdfs[i];
        fake_put(&fake_pci[i], 0, ids[i], 4);
        fake_put(&fake_pci[i], 8, classes[i] << 8, 4);
        fake_put(&fake_pci[i], 4, 7, 2);
    }
    fake_put(&fake_pci[1], 0xe, 1, 1);
    fake_put(&fake_pci[1], 0x18, 0x00010100, 4);
    fake_put(&fake_pci[1], 0x20, 0xe000e000, 4);
    struct fake_pci_function *a = &fake_pci[3], *wide = &fake_pci[4];
    fake_put(a, 0x24, 0xe0000000, 4);
    a->masks[0x24 / 4] = 0xfffff000;
    fake_put(a, 0x10, 0x3f9, 4);
    a->masks[0x10 / 4] = 0xfffffff9;
    fake_put(a, 0x30, 0xe1000001, 4);
    a->masks[0x30 / 4] = 0xfffff800;
    fake_put(wide, 0x10, 0x2004, 4);
    fake_put(wide, 0x14, 1, 4);
    fake_put(wide, 0xe, 0x80, 1);
    wide->masks[0x10 / 4] = 0xffffe004;
    wide->masks[0x14 / 4] = UINT32_MAX;
    fake_put(a, 6, 0x10, 2);
    fake_put(a, 0x34, 0x40, 1);
    fake_put(a, 0x40, 0x00806005, 4);
    fake_put(a, 0x60, 0x11, 4);
    platform_pci_set_host(&ops, NULL);
    CHECK(!platform_pci_inventory(""));
    CHECK(!platform_pci_inventory("vmm_media=00:20.0"));
    CHECK(!platform_pci_inventory("vmm_media=ff:00.0"));
    platform_pci_set_host(&ops, NULL);
    CHECK(platform_pci_inventory("vmm_media=00:02.0"));
    CHECK(platform_pci_count() == 8);
    CHECK(platform_pci_device(0)->owner == PCI_OWNER_VMM);
    CHECK(platform_pci_device(2)->owner == PCI_OWNER_VMM);
    CHECK(platform_pci_device(4)->parent == 8);
    CHECK(platform_pci_device(5)->bdf == 263 && platform_pci_device(5)->parent == 8);
    CHECK(platform_pci_device(3)->bars[5].size == 4096);
    CHECK(platform_pci_device(4)->bars[0].base == 0x100002000ULL);
    CHECK(platform_pci_device(4)->bars[0].size == 8192);
    unsigned reserved_writes = fake_pci[2].writes, ahci_writes = a->writes;
    CHECK(platform_pci_read(pci_address(16, 0), 4) == UINT32_MAX);
    CHECK(platform_pci_read(0, 2) == 0xffff);
    CHECK(platform_pci_read(pci_address(248, 0), 4) == 0x29228086);
    CHECK(platform_pci_read(pci_address(248, 0x100), 4) == 0);
    CHECK(platform_pci_read(pci_address(248, 3), 2) == 0xffff);
    platform_pci_write(pci_address(16, 4), 7, 2);
    CHECK(fake_pci[2].writes == reserved_writes);
    platform_pci_write(pci_address(248, 0x24), UINT32_MAX, 4);
    CHECK(platform_pci_read(pci_address(248, 0x24), 4) == 0xfffff000);
    CHECK(fake_pci_read(NULL, 248, 0x24, 4) == 0xe0000000 && a->writes == ahci_writes);
    platform_pci_write(pci_address(248, 0x24), 0xd0000000, 4);
    CHECK(platform_pci_read(pci_address(248, 0x24), 4) == 0xe0000000 && a->writes == ahci_writes);
    platform_pci_write(pci_address(248, 0x25), 0x40, 1);
    CHECK(fake_pci_read(NULL, 248, 0x24, 4) == 0xe0000000);
    platform_pci_write(pci_address(256, 0x10), UINT32_MAX, 4);
    platform_pci_write(pci_address(256, 0x14), UINT32_MAX, 4);
    CHECK(platform_pci_read(pci_address(256, 0x10), 4) == 0xffffe004);
    CHECK(platform_pci_read(pci_address(256, 0x14), 4) == UINT32_MAX);
    CHECK(fake_pci_read(NULL, 256, 0x14, 4) == 1);
    platform_pci_write(pci_address(256, 0x10), 0x2004, 4);
    platform_pci_write(pci_address(256, 0x14), 1, 4);
    CHECK(platform_pci_read(pci_address(256, 0x10), 4) == 0x2004);
    platform_pci_write(pci_address(248, 4), 0xffff, 2);
    CHECK((fake_pci_read(NULL, 248, 4, 2) & 0x407) == 0x407);
    CHECK(!(fake_pci_read(NULL, 248, 4, 2) & 0xfbf8));
    platform_pci_write(pci_address(248, 4), 0, 1);
    CHECK((fake_pci_read(NULL, 248, 4, 2) & 7) == 0);
    platform_pci_write(pci_address(248, 4), UINT32_MAX, 4);
    CHECK(fake_pci_read(NULL, 248, 6, 2) == 0x10);
    CHECK(platform_pci_read(pci_address(248, 4), 4) == 0x00100407);
    unsigned bridge_writes = fake_pci[1].writes;
    platform_pci_write(pci_address(8, 0x20), 0, 4);
    platform_pci_write(pci_address(8, 0x3e), 0x40, 2);
    platform_pci_write(pci_address(8, 4), 0, 2);
    CHECK(fake_pci[1].writes == bridge_writes);
    ahci_writes = a->writes;
    platform_pci_write(pci_address(248, 0), 0, 4);
    platform_pci_write(pci_address(248, 0x44), 0xfee00000, 4);
    platform_pci_write(pci_address(248, 0x42), 0x81, 2);
    platform_pci_write(pci_address(248, 0x62), 0x8000, 2);
    CHECK(a->writes == ahci_writes && platform_pci_device(3)->irq_writes == 3);
    CHECK(platform_pci_read(pci_address(248, 0x44), 4) == 0xfee00000);
    CHECK(platform_pci_read(pci_address(248, 0x42), 2) == 0x81);
    CHECK(!(fake_pci_read(NULL, 248, 0x42, 2) & 1));
    CHECK(!(fake_pci_read(NULL, 248, 0x62, 2) & 0x8000));
    /* PIIX IDE timing block 0x40-0x4b: guest writes reach the device (the
     * firmware's IDE Decode Enable); outside the block, and on a function
     * that is not a PIIX IDE, they stay rejected. */
    struct fake_pci_function *ide = &fake_pci[6];
    unsigned ide_writes = ide->writes, ide_rejected = platform_pci_device(6)->rejected_writes;
    platform_pci_write(pci_address(264, 0x40), 0x8000, 2);
    platform_pci_write(pci_address(264, 0x42), 0xa307, 2);
    platform_pci_write(pci_address(264, 0x48), 0x00010001, 4);
    CHECK(ide->writes == ide_writes + 3 && fake_pci_read(NULL, 264, 0x40, 4) == 0xa3078000);
    CHECK(fake_pci_read(NULL, 264, 0x48, 4) == 0x00010001);
    CHECK(platform_pci_read(pci_address(264, 0x40), 4) == 0xa3078000);
    platform_pci_write(pci_address(264, 0x4c), 1, 1);
    CHECK(ide->writes == ide_writes + 3 &&
          platform_pci_device(6)->rejected_writes == ide_rejected + 1);
    unsigned pch_writes = fake_pci[7].writes;
    platform_pci_write(pci_address(272, 0x40), 0xa307, 2);
    platform_pci_write(pci_address(272, 0x4a), 0x0102, 2);
    CHECK(fake_pci[7].writes == pch_writes + 2 && fake_pci_read(NULL, 272, 0x40, 2) == 0xa307 &&
          fake_pci_read(NULL, 272, 0x4a, 2) == 0x0102);
    unsigned nic_writes = fake_pci[4].writes;
    platform_pci_write(pci_address(256, 0x40), 0x8000, 2);
    CHECK(fake_pci[4].writes == nic_writes && !fake_pci_read(NULL, 256, 0x40, 2));
    struct ia64vmm_handoff h = {0};
    CHECK(platform_pci_handoff(&h));
    CHECK(h.pci_mmio_range_count == 3 && h.pci_mmio[2].base == 0x100002000ULL);
    CHECK(h.pci_mmio_base == 0xe0000000 && h.pci_mmio64_base == 0x100002000ULL);
    /* The guest-owned ISA bridge's i8042 ports join the root I/O window. */
    CHECK(h.pci_io_base == 0x60 && h.pci_io_size == 0x3f8 + 8 - 0x60);
    /* The default snapshot requires a real PCI image header and PCIR. */
    fake_bar_memory[0] = 0x55;
    fake_bar_memory[1] = 0xaa;
    fake_bar_memory[0x18] = 0x40;
    memcpy(fake_bar_memory + 0x40, "PCIR", 4);
    fake_bar_memory[0x4a] = 0x18;
    fake_bar_memory[0x50] = 4;
    fake_bar_memory[0x55] = 0x80;
    struct platform_state state;
    CHECK(platform_devices_init(&state));
    /* Every unclaimed address below 2^IA64VMM_PHYS_ADDR_BITS master-aborts
     * (PCI 3.0 §3.3.3), not only the legacy window and PCI apertures: above
     * guest RAM, reads are all-ones and writes are dropped. */
    const struct gm_region *gap = platform_unclaimed_region(UINT64_C(0x100000034), 4);
    CHECK(gap && gap->kind == GM_EMULATED && !gm_lookup(UINT64_C(0x100000034)));
    gap->ops->write(gap->opaque, NULL, UINT64_C(0x100000034) - gap->base, 0, 4);
    CHECK(gap->ops->read(gap->opaque, NULL, UINT64_C(0x100000034) - gap->base, 4) == UINT32_MAX);
    CHECK(!platform_unclaimed_region(UINT64_C(1) << IA64VMM_PHYS_ADDR_BITS, 4));
    CHECK(state.allowed_ports[0x60 / 8] & (1u << (0x60 & 7)));
    CHECK(state.allowed_ports[0x64 / 8] & (1u << (0x64 & 7)));
    CHECK(!(state.allowed_ports[0x61 / 8] & (1u << (0x61 & 7))));
    CHECK(gm_lookup(0xe0000000)->kind == GM_DIRECT_MMIO);
    CHECK(!gm_lookup(0xe0001000));
    CHECK(read_mmio(port_addr(0x3f8), 4) == 0xa5a5a5a5 && port_reads == 1);
    CHECK(read_mmio(0xe1000000, 1) == 0x55);
    write_mmio(0xe1000000, 0xff, 1);
    CHECK(read_mmio(0xe1000000, 1) == 0x55);
    CHECK(fake_pci_read(NULL, 248, 0x30, 4) == 0xe1000001);
    platform_pci_write(pci_address(248, 0x30), 0xe1000000, 4);
    CHECK(read_mmio(0xe1000000, 1) == 0xff);
    platform_pci_write(pci_address(248, 0x30), 0xe1000001, 4);
    CHECK(read_mmio(0xe1000000, 1) == 0x55);
    reserved_writes = fake_pci[2].writes;
    CHECK(read_mmio(IA64VMM_ECAM_BASE + pci_address(16, 0), 4) == UINT32_MAX);
    CHECK(read_mmio(IA64VMM_ECAM_BASE + pci_address(248, 0), 4) == 0x29228086);
    write_mmio(port_addr(0xcf8), 0x80001004, 4);
    write_mmio(port_addr(0xcfc), 7, 2);
    CHECK(fake_pci[2].writes == reserved_writes);
    CHECK(read_mmio(port_addr(0xcfc), 4) == UINT32_MAX);
    write_mmio(port_addr(0xcf8), 0x8000f800, 4);
    CHECK(read_mmio(port_addr(0xcfc), 4) == 0x29228086);
    write_mmio(port_addr(0xcf8), 0x8000f824, 4);
    write_mmio(port_addr(0xcfc), UINT32_MAX, 4);
    CHECK(read_mmio(port_addr(0xcfc), 4) == 0xfffff000);
    CHECK(read_mmio(IA64VMM_ECAM_BASE + pci_address(248, 0x24), 4) == 0xfffff000);
    /* Restart: quiesce clears Bus Master
     * Enable, sets INTx Disable and disables MSI/MSI-X in host configuration
     * space, then reads the command register back; reserved functions and
     * bridges are never touched. The next boot's mediation state is the
     * first boot's, with Bus Master Enable clear. */
    write_mmio(port_addr(0xcf8), 0, 4);
    platform_pci_write(pci_address(248, 4), 7, 2);
    CHECK(platform_pci_device(3)->boot_irq_config[0x42] == 0x81);
    platform_pci_write(pci_address(248, 0x42), 0x80, 2);
    CHECK(platform_pci_device(3)->guest_bus_master &&
          platform_pci_read(pci_address(248, 0x42), 2) == 0x80);
    fake_put(a, 0x42, 0x81, 2);
    fake_put(a, 0x62, 0x8000, 2);
    unsigned untouched[3] = {fake_pci[0].writes, fake_pci[1].writes, fake_pci[2].writes};
    unsigned isa_writes = fake_pci[5].writes;
    platform_pci_quiesce();
    CHECK(fake_pci[5].writes == isa_writes);
    CHECK(a->last_read == 4 + 1);
    CHECK((fake_pci_read(NULL, 248, 4, 2) & 0x407) == 0x403);
    CHECK(!(fake_pci_read(NULL, 248, 0x42, 2) & 1) &&
          (fake_pci_read(NULL, 248, 0x62, 2) & 0xc000) == 0x4000);
    CHECK(fake_pci[0].writes == untouched[0] && fake_pci[1].writes == untouched[1] &&
          fake_pci[2].writes == untouched[2]);
    platform_pci_write(pci_address(248, 0x24), UINT32_MAX, 4);
    platform_pci_write(pci_address(248, 0x30), 0xe1000000, 4);
    fake_put(a, 4, 0x407, 2);
    platform_devices_restart(&state);
    CHECK(!platform_pci_device(3)->guest_bus_master);
    /* The guest had written 0x407 before the first boot started. */
    CHECK(platform_pci_device(3)->boot_command == 0x407);
    CHECK(platform_pci_read(pci_address(248, 4), 2) == 0x403 &&
          (fake_pci_read(NULL, 248, 4, 2) & 0x407) == 0x403);
    CHECK(platform_pci_read(pci_address(248, 0x42), 2) == 0x81);
    CHECK(platform_pci_read(pci_address(248, 0x24), 4) == 0xe0000000);
    CHECK(read_mmio(0xe1000000, 1) == 0x55);
    CHECK(fake_pci[0].writes == untouched[0] && fake_pci[1].writes == untouched[1] &&
          fake_pci[2].writes == untouched[2]);
    for (unsigned i = 3; i < 8; ++i)
        fake_put(&fake_pci[i], 4, 7, 2);
    platform_devices_cleanup(&state);
    platform_pci_set_host(NULL, NULL);
    /* 1 MiB 32/64-bit BARs
     * restore both halves and command/status, with decode off during sizing.
     * Unlike its runtime probe, our pre-guest probe also quiesces BME. */
    fake_put(a, 4, 0x00100007, 4);
    fake_put(a, 0x24, 0xc1200000, 4);
    a->masks[0x24 / 4] = 0xfff00000;
    fake_put(wide, 0x10, 0xc1200004, 4);
    wide->masks[0x10 / 4] = 0xfff00004;
    platform_pci_set_host(&ops, NULL);
    CHECK(platform_pci_inventory("vmm_media=00:02.0"));
    CHECK(platform_pci_device(3)->bars[5].size == 0x100000);
    CHECK(platform_pci_device(4)->bars[0].base == 0x1c1200000ULL);
    CHECK(platform_pci_device(4)->bars[0].size == 0x100000);
    CHECK(fake_pci_read(NULL, 248, 4, 4) == 0x00100007);
    CHECK(fake_pci_read(NULL, 248, 0x24, 4) == 0xc1200000);
    CHECK(fake_pci_read(NULL, 256, 0x10, 4) == 0xc1200004);
    CHECK(fake_pci_read(NULL, 256, 0x14, 4) == 1 && fake_pci_read(NULL, 256, 4, 2) == 7);
    fake_put(a, 0x24, 0xc1201000, 4);
    fake_put(wide, 0x10, 0xc1201004, 4);
    fake_put(a, 0x14, 0x2002, 4);
    a->masks[0x14 / 4] = 0xfffff000;
    fake_put(a, 0x20, 0xc1300006, 4);
    a->masks[0x20 / 4] = 0xfff00000;
    platform_pci_set_host(&ops, NULL);
    CHECK(platform_pci_inventory("vmm_media=00:02.0"));
    CHECK(!platform_pci_device(3)->bars[5].size && !platform_pci_device(4)->bars[0].size);
    CHECK(!platform_pci_device(3)->bars[1].size && !platform_pci_device(3)->bars[4].size);
    CHECK(fake_pci_read(NULL, 248, 0x14, 4) == 0x2002);
    /* Match only the inherited GOP's real VGA function, with every ancestor
     * routing VGA. Direct memory must supersede the low aperture abort. */
    fake_put(wide, 8, 0x03000000, 4);
    fake_put(wide, 0x10, 0x2004, 4);
    fake_put(wide, 0x14, 1, 4);
    wide->masks[0x10 / 4] = 0xffffe004;
    struct boot_framebuffer fb = {.base = 0x100002000ULL, .size = 4096};
    struct ia64vmm_handoff display = {0};
    platform_pci_set_host(&ops, NULL);
    CHECK(platform_pci_inventory("vmm_media=00:02.0"));
    CHECK(platform_vga_handoff(&fb, &display) && !(display.flags & IA64VMM_HF_LEGACY_VGA));
    fake_put(&fake_pci[1], 0x3e, 8, 2);
    fake_put(wide, 0, 0xbeef80ee, 4);
    platform_pci_set_host(&ops, NULL);
    CHECK(platform_pci_inventory("vmm_media=00:02.0"));
    CHECK(platform_vga_handoff(&fb, &display) && (display.flags & IA64VMM_HF_LEGACY_VGA));
    CHECK(display.vga_bdf == wide->bdf && display.vga_base == IA64VMM_VGA_MEMORY_BASE);
    CHECK(display.vga_kind == IA64VMM_VGA_GENERIC);
    /* A display with Bus Master Enable clear that the guest never gave it
     * keeps its state at a restart; one that kept the host's Bus Master
     * Enable, or that the guest gave it, is quiesced. */
    fake_put(wide, 4, 0x0003, 2);
    unsigned display_writes = wide->writes;
    platform_pci_quiesce();
    CHECK(wide->writes == display_writes);
    fake_put(wide, 4, 0x0007, 2);
    platform_pci_quiesce();
    CHECK(wide->writes > display_writes && !(fake_pci_read(NULL, 256, 4, 2) & 4));
    platform_pci_write(pci_address(256, 4), 7, 2);
    CHECK(fake_pci_read(NULL, 256, 4, 2) & 4);
    platform_pci_quiesce();
    CHECK(wide->last_read == 4 + 1 && !(fake_pci_read(NULL, 256, 4, 2) & 4));
    CHECK(platform_vga_map(&state));
    CHECK(gm_lookup(0xb8000)->kind == GM_DIRECT_MMIO);
    CHECK(gm_direct_ptr(0xb8000, 2) == fake_vga_memory + 0x18000);
    CHECK(state.allowed_ports[0x3c0 / 8] & (1u << (0x3c0 & 7)));
    for (unsigned port = 0x1ce; port <= 0x1d0; ++port)
        CHECK(state.allowed_ports[port / 8] & (1u << (port & 7)));
    fb.base = 0x200000;
    CHECK(platform_vga_handoff(&fb, &display) && !(display.flags & IA64VMM_HF_LEGACY_VGA));
    platform_pci_set_host(NULL, NULL);
}
/* A Sandy Bridge IGD at 00:02.0 as an gen6 GOP leaves it: memory
 * decode on, I/O decode off, GGC.IVD clear. 64-bit BAR0 (GTTMMADR, 4 MiB),
 * BAR2 (GMADR, 256 MiB, holds the framebuffer), BAR4 I/O. */
static void intel_fixture(uint16_t device, uint16_t command, uint16_t ggc, uint32_t bar0_mask)
{
    memset(fake_pci, 0, sizeof(fake_pci));
    for (unsigned i = 0; i < 8; ++i)
        fake_pci[i].bdf = 0xffff;
    struct fake_pci_function *host = &fake_pci[0], *igd = &fake_pci[1], *ahci = &fake_pci[2];
    host->bdf = 0;
    fake_put(host, 0, 0x01048086, 4);
    fake_put(host, 8, 0x060000 << 8, 4);
    igd->bdf = 16;
    fake_put(igd, 0, 0x8086u | (uint32_t)device << 16, 4);
    fake_put(igd, 8, 0x030000 << 8, 4);
    fake_put(igd, 4, command, 2);
    fake_put(igd, 0x50, ggc, 2);
    fake_put(igd, 0x10, 0xe0000004, 4);
    igd->masks[0x10 / 4] = bar0_mask;
    fake_put(igd, 0x14, 0, 4);
    igd->masks[0x14 / 4] = UINT32_MAX;
    fake_put(igd, 0x18, 0xc000000c, 4);
    igd->masks[0x18 / 4] = 0xf000000c;
    fake_put(igd, 0x1c, 0, 4);
    igd->masks[0x1c / 4] = UINT32_MAX;
    /* BAR4 decodes 16 I/O address bits, as the gen6 IGD's IGD: 31:16 read 0. */
    fake_put(igd, 0x20, 0x3001, 4);
    igd->masks[0x20 / 4] = 0x0000ffc1;
    ahci->bdf = 248;
    fake_put(ahci, 0, 0x1c038086, 4);
    fake_put(ahci, 8, 0x010601 << 8, 4);
    fake_put(ahci, 4, 7, 2);
    fake_put(ahci, 0x24, 0xe2000000, 4);
    ahci->masks[0x24 / 4] = 0xfffff800;
}
static void test_vga_intel(void)
{
    static const struct pci_host_ops ops = {fake_pci_read, fake_pci_write, fake_pci_map};
    struct boot_framebuffer fb = {.base = 0xc0000000ULL, .size = 1366 * 768 * 4};
    struct ia64vmm_handoff h = {0};
    intel_fixture(0x0126, 0x0006, 0x0209, 0xffc00004);
    platform_pci_set_host(&ops, NULL);
    CHECK(platform_pci_inventory("vmm_media=00:1f.0"));
    CHECK(platform_pci_device(1)->owner == PCI_OWNER_GUEST &&
          platform_pci_device(1)->bars[0].size == 0x400000);
    CHECK(platform_pci_device(1)->bars[4].io && platform_pci_device(1)->bars[4].base == 0x3000 &&
          platform_pci_device(1)->bars[4].size == 64);
    CHECK(platform_debug_policy("debug_serial=device debug_screen=1", &h));
    CHECK(vmm_guest_serial_screen_enabled());
    CHECK(platform_vga_handoff(&fb, &h) && (h.flags & IA64VMM_HF_LEGACY_VGA));
    CHECK(h.vga_kind == IA64VMM_VGA_INTEL_GEN6 && h.vga_bdf == 16 && h.vga_device == 0x0126);
    CHECK(h.vga_control_base == 0xe0000000ULL && h.vga_control_size == 0x400000);
    /* VGA is the console: the transition runs, so the mirror stays off. */
    CHECK(!vmm_guest_serial_screen_enabled());
    /* A console UART keeps the GOP: the serial mirror stays available. */
    CHECK(platform_debug_policy("debug_serial=console debug_screen=1", &h));
    CHECK(platform_vga_handoff(&fb, &h) && h.vga_kind == IA64VMM_VGA_INTEL_GEN6);
    CHECK(vmm_guest_serial_screen_enabled());
    /* Broken variants: GGC.IVD set, memory decode off, Ivy Bridge (gen7)
     * without decode, undersized BAR0 (generic kind only). */
    const struct {
        uint16_t device, command, ggc;
        uint32_t mask;
        bool legacy;
        unsigned kind;
    } cases[] = {
        {0x0126, 0x0006, 0x020b, 0xffc00004, false, 0},
        {0x0126, 0x0004, 0x0209, 0xffc00004, false, 0},
        {0x0166, 0x0006, 0x0209, 0xffc00004, false, 0},
        {0x0166, 0x0007, 0x0209, 0xffc00004, true, IA64VMM_VGA_GENERIC},
        {0x0126, 0x0006, 0x0209, 0xfff80004, true, IA64VMM_VGA_GENERIC},
        {0x0102, 0x0007, 0x0209, 0xffc00004, true, IA64VMM_VGA_INTEL_GEN6},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        intel_fixture(cases[i].device, cases[i].command, cases[i].ggc, cases[i].mask);
        platform_pci_set_host(&ops, NULL);
        CHECK(platform_pci_inventory("vmm_media=00:1f.0"));
        memset(&h, 0, sizeof(h));
        CHECK(platform_vga_handoff(&fb, &h));
        CHECK(!!(h.flags & IA64VMM_HF_LEGACY_VGA) == cases[i].legacy);
        if (cases[i].legacy)
            CHECK(h.vga_kind == cases[i].kind);
        /* Reads only: the VMM never writes the IGD while deciding. */
        CHECK(fake_pci_read(NULL, 16, 4, 2) == cases[i].command &&
              fake_pci_read(NULL, 16, 0x50, 2) == cases[i].ggc);
    }
    platform_pci_set_host(NULL, NULL);
}

/* Generic route: option ROM header and PCIR fields at C0000, the
 * raw int 10h vector, and each unreadable/malformed case. */
static void test_legacy_rom(void)
{
    static uint8_t rom[0x10000];
    uint8_t ivt[4] = {0x30, 0x02, 0x00, 0xc0};
    char line[256];
    memset(rom, 0xff, sizeof(rom));
    rom[0] = 0x55;
    rom[1] = 0xaa;
    rom[2] = 0x80;
    rom[0x18] = 0x40;
    rom[0x19] = 0;
    memcpy(rom + 0x40, "PCIR\x86\x80\x26\x01", 8);
    rom[0x4d] = 0x00;
    rom[0x4e] = 0x00;
    rom[0x4f] = 0x03; /* class 030000 */
    rom[0x50] = 0x80;
    rom[0x51] = 0x00;
    rom[0x54] = 0x00;
    rom[0x55] = 0x80;
    platform_legacy_rom_describe(line, sizeof(line), rom, sizeof(rom), ivt, 4);
    CHECK(!strcmp(line, "host C0000: 55 aa size=80 (64 KiB) PCIR@0040 8086:0126 class=030000 "
                        "type=00(x86) len=0080 last=1; IVT int10=c000:0230 (raw)"));
    rom[0x54] = 3;
    rom[0x55] = 0;
    platform_legacy_rom_describe(line, sizeof(line), rom, sizeof(rom), ivt, 4);
    CHECK(strstr(line, "type=03(EFI) len=0080 last=0"));
    rom[0x40] = 'X';
    platform_legacy_rom_describe(line, sizeof(line), rom, sizeof(rom), NULL, 0);
    CHECK(strstr(line, "PCIR@0040 without the PCIR signature (58 43 49 52)") &&
          strstr(line, "; IVT 0040 not in the host memory map, not read"));
    rom[0x18] = 0xf0;
    rom[0x19] = 0xff;
    platform_legacy_rom_describe(line, sizeof(line), rom, sizeof(rom), ivt, 4);
    CHECK(strstr(line, "PCIR@fff0 beyond the readable 10000 bytes"));
    platform_legacy_rom_describe(line, sizeof(line), rom, 0x10, ivt, 4);
    CHECK(strstr(line, "PCIR offset beyond the readable 10 bytes"));
    rom[0] = 0xff;
    rom[1] = 0xff;
    platform_legacy_rom_describe(line, sizeof(line), rom, sizeof(rom), ivt, 4);
    CHECK(!strcmp(
        line, "host C0000: ff ff size=80 (no option ROM signature); IVT int10=c000:0230 (raw)"));
    CHECK(platform_legacy_rom_describe(line, sizeof(line), NULL, 0, NULL, 0) == strlen(line));
    CHECK(!strcmp(
        line,
        "host C0000: not in the host memory map, not read; IVT 0040 not in the host memory map, not read"));
    /* A short buffer is cut, terminated, and never overrun. */
    char small[16];
    memset(small, 'z', sizeof(small));
    CHECK(platform_legacy_rom_describe(small, 12, NULL, 0, NULL, 0) == 11 && !small[11] &&
          small[12] == 'z');
}

/* The boot image lies in whole 8 KiB granules even when its RAM range ends
 * on an odd 4 KiB boundary (4 KiB aligned); the reference firmware
 * describes RAM to EFI only in such granules and must be able to reserve
 * every byte of the image. */
static void test_handoff_granule(void)
{
    add_ram(0, 0x9f000);
    add_ram(0x100000, 0x700000);
    add_ram(0x2000000, 0x2000000);
    add_ram(0x5000000, 0x7000);
    struct boot_info bi = {0};
    uint8_t image[] = {'M', 'Z', 1, 2};
    bi.module_count = 1;
    strcpy(bi.modules[0].name, "bootdisk");
    bi.modules[0].hva = image;
    bi.modules[0].size = sizeof(image);
    struct fw_image fw = {.base = 0x100000,
                          .size = 0x180000,
                          .entry = 0x100000,
                          .global_pointer = 0x101000,
                          .pal_entry = 0x100060};
    struct ia64vmm_handoff h;
    struct ia64_entry_regs regs;
    uint64_t at;
    CHECK(platform_handoff(&bi, &fw, &h, &at, &regs));
    CHECK(h.fw_data_base == 0x2000000 && h.boot_image_size == sizeof(image));
    CHECK(h.boot_image_base >= 0x5000000 && h.boot_image_base % 8192 == 0);
    CHECK(IA64VMM_ALIGN_UP(h.boot_image_base + h.boot_image_size, 8192) <= 0x5006000);
    CHECK(!memcmp(gm_direct_ptr(h.boot_image_base, sizeof(image)), image, sizeof(image)));
}

/* AMD topology: root ports at function 1 behind a reserved dummy host bridge
 * at function 0. The guest must find function 0 (identity only, read only)
 * so it walks to the root port; a reserved function 0 without guest-owned
 * siblings stays absent. */
static void test_pci_presence(void)
{
    static const struct pci_host_ops ops = {fake_pci_read, fake_pci_write, fake_pci_map};
    for (unsigned i = 0; i < 8; ++i) {
        memset(&fake_pci[i], 0, sizeof(fake_pci[i]));
        fake_pci[i].bdf = 0xffff;
    }
    const uint16_t bdfs[] = {0, 8, 9, 16, 256};
    const uint32_t classes[] = {0x060000, 0x060000, 0x060400, 0x0c0330, 0x010601};
    const uint32_t ids[] = {0x14d81022, 0x14da1022, 0x14db1022, 0x15b71022, 0x43f61022};
    for (unsigned i = 0; i < 5; ++i) {
        fake_pci[i].bdf = bdfs[i];
        fake_put(&fake_pci[i], 0, ids[i], 4);
        fake_put(&fake_pci[i], 8, classes[i] << 8 | 0x01, 4);
        fake_put(&fake_pci[i], 4, 0x0007, 2);
        fake_put(&fake_pci[i], 6, 0x0010, 2);
        fake_put(&fake_pci[i], 0x34, 0x40, 1);
    }
    fake_put(&fake_pci[1], 0x0c, 0x00800010, 4); /* 00:01.0: multi-function host bridge */
    fake_put(&fake_pci[1], 0x2c, 0x14da1022, 4);
    fake_put(&fake_pci[1], 0x10, 0xfe000000, 4); /* a register the guest must not see */
    fake_put(&fake_pci[2], 0x0e, 0x01, 1);       /* 00:01.1: root port to bus 1 */
    fake_put(&fake_pci[2], 0x18, 0x00010100, 4);
    platform_pci_set_host(&ops, NULL);
    CHECK(platform_pci_inventory("vmm_media=00:02.0"));
    CHECK(platform_pci_device(0)->owner == PCI_OWNER_VMM &&
          platform_pci_device(1)->owner == PCI_OWNER_VMM);
    CHECK(platform_pci_device(2)->owner == PCI_OWNER_GUEST);
    /* 00:00.0: reserved, no guest sibling: absent. */
    CHECK(platform_pci_read(pci_address(0, 0), 4) == UINT32_MAX);
    /* 00:01.0: identity, header type with the multi-function bit, nothing else. */
    CHECK(platform_pci_read(pci_address(8, 0), 4) == 0x14da1022);
    CHECK(platform_pci_read(pci_address(8, 2), 2) == 0x14da);
    CHECK(platform_pci_read(pci_address(8, 8), 4) == (0x060000u << 8 | 1));
    CHECK(platform_pci_read(pci_address(8, 0x0e), 1) == 0x80);
    CHECK(platform_pci_read(pci_address(8, 0x0c), 4) == 0x00800000);
    CHECK(platform_pci_read(pci_address(8, 0x2c), 4) == 0x14da1022);
    CHECK(platform_pci_read(pci_address(8, 4), 4) == 0);
    CHECK(platform_pci_read(pci_address(8, 0x10), 4) == 0);
    CHECK(platform_pci_read(pci_address(8, 0x34), 1) == 0);
    CHECK(platform_pci_read(pci_address(8, 0x100), 4) == 0);
    unsigned writes = fake_pci[1].writes;
    platform_pci_write(pci_address(8, 4), 0, 2);
    platform_pci_write(pci_address(8, 0x10), 0xffffffff, 4);
    CHECK(fake_pci[1].writes == writes);
    /* 00:01.1 and the AHCI behind it are the guest's as before. */
    CHECK(platform_pci_read(pci_address(9, 0), 4) == 0x14db1022);
    CHECK(platform_pci_read(pci_address(256, 0), 4) == 0x43f61022);
    /* The VMM media function is reserved and has no guest sibling: absent. */
    CHECK(platform_pci_read(pci_address(16, 0), 4) == UINT32_MAX);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    log_set_fb_sink(fb_sink);
    vmm_log_fb_enable(true);
    if (!strcmp(argv[1], "loader"))
        test_loader(argv[2]);
    else if (!strcmp(argv[1], "handoff"))
        test_handoff();
    else if (!strcmp(argv[1], "granule"))
        test_handoff_granule();
    else if (!strcmp(argv[1], "presence"))
        test_pci_presence();
    else if (!strcmp(argv[1], "window"))
        test_window();
    else if (!strcmp(argv[1], "devices"))
        test_devices();
    else if (!strcmp(argv[1], "pci")) {
        test_storage();
        test_pci_boot_media();
        test_pci_ari();
        test_pci();
        test_vga_intel();
        test_legacy_rom();
    } else if (!strcmp(argv[1], "vm"))
        test_vm(argv[2]);
    else
        CHECK(!strcmp(argv[1], "rtc") && platform_rtc_seconds() > 1700000000);
    printf("test-platform: %s passed\n", argv[1]);
    return 0;
}
