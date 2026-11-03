/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/cache/background.h"
#include "core/vm.h"
#include "core/log_internal.h"
#include "core/smp.h"
#include "core/helper.h"
#include "core/profile.h"
#pragma weak profile_configure
#pragma weak profile_dump
#pragma weak profile_request_dump
#include "engine/tier1/tier1.h"
#include "engine/tier0/tier0.h"
#pragma weak engine_tier1_glue
#pragma weak tier0_set_reclaim_submit
#pragma weak code_background_set
#pragma weak engine_fp_configure
#pragma weak engine_fp_log
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"
#include "platform/platform.h"
#include "ia64vmm/pci.h"
#include "core/dump-va.h"
#include "persist/persist.h"

static struct boot_info *boot;
/* Registered once per run; restart resets its per-boot state in place. */
static struct platform_state platform;
/* fp_stats=1: each vCPU's FP statistics line when its executor stops. */
static bool fp_stats;

/* vCPU0 owns loader observation and boot milestones. */
struct guest_run {
    struct vm_cpu *bsp;
    struct platform_state *platform;
    uint64_t fw_base, fw_size;
    uint64_t start, hz;
    /* One bit per guest RAM page; region offsets keep physical holes sparse. */
    uint8_t *preboot;
    size_t preboot_storage, preboot_offset[GM_MAX_REGIONS];
    /* Opt-in sample_sec=N progress log, per vCPU. */
    uint64_t sample_ticks;
    const char *cmdline; /* for the opt-in exit diagnostics */
    /* Post-EBS slice count, limit and initial timestamp. */
    uint64_t handoff_steps, handoff_limit, handoff_start;
};
/* Per-vCPU sample deadline and previously reported counters. */
struct sample_state {
    uint64_t next, retired, idle, faults;
};
static struct sample_state samples[IA64VMM_MAX_CPUS];

static bool outside_firmware(const struct guest_run *run, uint64_t pa)
{
    return pa < run->fw_base || pa - run->fw_base >= run->fw_size;
}

/* Compilation timing is optional in binaries without engine/engine.c. */
__attribute__((weak)) bool engine_compile_ticks(uint64_t *ticks)
{
    (void)ticks;
    return false;
}
static uint64_t ticks_ms(uint64_t ticks, uint64_t hz)
{
    return ticks / hz * 1000 + ticks % hz * 1000 / hz;
}
/* Cumulative boot timing: exec and idle are disjoint; compile is part of exec. */
static void milestone(struct guest_run *run, const char *name)
{
    uint64_t ip, psr, retired, fault_ip, fault_info, faults, compile = 0;
    unsigned slot;
    const char *fault;
    vcpu_progress(run->bsp->v, &ip, &slot, &psr, &retired, &fault, &fault_ip, &fault_info, &faults);
    bool compiled = engine_compile_ticks(&compile);
    uint64_t wall = hal_counter() - run->start;
    char extra[40] = "";
    if (compiled)
        vmm_snprintf(extra, sizeof(extra), " compile_ms=%llu",
                     (unsigned long long)ticks_ms(compile, run->hz));
    LOGI("milestone %s retired=%llu exec_ms=%llu idle_ms=%llu%s wall_ms=%llu", name,
         (unsigned long long)retired, (unsigned long long)ticks_ms(run->bsp->exec_ticks, run->hz),
         (unsigned long long)ticks_ms(run->bsp->idle_ticks, run->hz), extra,
         (unsigned long long)ticks_ms(wall, run->hz));
    if (profile_request_dump)
        profile_request_dump(name);
}

static bool preboot_init(struct guest_run *run)
{
    size_t bits = 0;
    for (unsigned i = 0; i < gm_region_count(); ++i) {
        const struct gm_region *r = gm_region_at(i);
        run->preboot_offset[i] = bits;
        if (r->kind == GM_RAM || r->kind == GM_FIRMWARE)
            bits += (r->size + 4095) / 4096;
    }
    run->preboot_storage = ((bits + 7) / 8 + 4095) / 4096;
    run->preboot = hal_alloc_pages(run->preboot_storage, HAL_PAGE_ZERO);
    return run->preboot != NULL;
}
static void preboot_record(struct guest_run *run, uint64_t pa)
{
    for (unsigned i = 0; i < gm_region_count(); ++i) {
        const struct gm_region *r = gm_region_at(i);
        if ((r->kind == GM_RAM || r->kind == GM_FIRMWARE) && pa >= r->base &&
            pa - r->base < r->size) {
            size_t bit = run->preboot_offset[i] + (pa - r->base) / 4096;
            run->preboot[bit / 8] |= 1u << (bit % 8);
            return;
        }
    }
}
static bool preboot_used(const struct guest_run *run, const struct ia64vmm_ram_range *range)
{
    for (unsigned i = 0; i < gm_region_count(); ++i) {
        const struct gm_region *r = gm_region_at(i);
        if (r->kind != GM_RAM && r->kind != GM_FIRMWARE)
            continue;
        uint64_t base = r->base > range->base ? r->base : range->base;
        uint64_t end = r->base + r->size < range->base + range->size ? r->base + r->size
                                                                     : range->base + range->size;
        if (base >= end)
            continue;
        size_t first = run->preboot_offset[i] + (base - r->base) / 4096;
        size_t last = run->preboot_offset[i] + (end - r->base + 4095) / 4096;
        for (size_t bit = first; bit < last; ++bit)
            if (run->preboot[bit / 8] & (1u << (bit % 8)))
                return true;
    }
    return false;
}

/* Loader diagnostics, before each engine slice: remember pages executed
 * before ExitBootServices; after it, single-step until execution reaches a
 * loader allocation that is not the loader itself (the kernel entry). */
static void observe_loader(struct guest_run *run)
{
    struct vcpu *v = run->bsp->v;
    struct platform_state *platform = run->platform;
    if (!platform->boot_services_exited) {
        uint64_t pa = 0;
        if (vcpu_instruction_physical_address(v, &pa) && outside_firmware(run, pa))
            preboot_record(run, pa);
    }
    if (!platform->handoff_pending)
        return;
    unsigned slot;
    uint64_t ip = vcpu_instruction_pointer(v, &slot);
    uint64_t pa = 0;
    bool candidate = vcpu_instruction_physical_address(v, &pa);
    if (candidate && !platform->loader_resumed && outside_firmware(run, pa)) {
        /* A loader can relocate without updating LoadedImage.
         * EBS always returns to its caller before kernel entry. */
        platform->loader_resumed = true;
        for (unsigned i = 0; i < platform->loader_range_count; ++i) {
            const struct ia64vmm_ram_range *r = &platform->loader_ranges[i];
            if (pa >= r->base && pa - r->base < r->size &&
                platform->image_range_count < IA64VMM_ARRAY_SIZE(platform->image_ranges))
                platform->image_ranges[platform->image_range_count++] = *r;
        }
        LOGI("EFI loader resumed ip=%llx slot=%u", (unsigned long long)ip, slot);
        milestone(run, "loader_resumed");
    }
    for (unsigned i = 0; i < platform->image_range_count; ++i) {
        const struct ia64vmm_ram_range *r = &platform->image_ranges[i];
        if (pa >= r->base && pa - r->base < r->size)
            candidate = false;
    }
    for (unsigned i = 0; i < platform->loader_range_count; ++i) {
        const struct ia64vmm_ram_range *r = &platform->loader_ranges[i];
        if (candidate && pa >= r->base && pa - r->base < r->size && outside_firmware(run, pa)) {
            LOGI("EFI loader done ip=%llx slot=%u allocation=%llx+%llx", (unsigned long long)ip,
                 slot, (unsigned long long)r->base, (unsigned long long)r->size);
            milestone(run, "kernel_entry");
            platform->handoff_pending = false;
            break;
        }
    }
}
/* The diagnostic exit the platform requested when ExitBootServices succeeded. */
static void observe_exit_boot_services(struct guest_run *run)
{
    struct platform_state *platform = run->platform;
    if (!run->handoff_limit)
        platform->handoff_pending = false;
    /* Loader modules can execute from LoaderCode allocations outside the
     * original image and EBS return descriptor. Exclude allocations
     * observed executing at budget boundaries before exit as well. */
    for (unsigned i = 0; run->handoff_limit && i < platform->loader_range_count; ++i) {
        const struct ia64vmm_ram_range *r = &platform->loader_ranges[i];
        if (!preboot_used(run, r))
            continue;
        if (platform->image_range_count == IA64VMM_ARRAY_SIZE(platform->image_ranges)) {
            platform->preboot_overflow = true;
            break;
        }
        platform->image_ranges[platform->image_range_count++] = *r;
        LOGI("EFI pre-exit code allocation base=%llx size=%llx", (unsigned long long)r->base,
             (unsigned long long)r->size);
    }
    if (run->handoff_limit && platform->preboot_overflow) {
        LOGW("EFI entry observation capacity exhausted");
        platform->handoff_pending = false;
    }
    unsigned slot;
    uint64_t ip = vcpu_instruction_pointer(run->bsp->v, &slot);
    LOGI("EFI first IP after ExitBootServices ip=%llx slot=%u", (unsigned long long)ip, slot);
    milestone(run, "ebs");
}
/* Periodic per-vCPU progress without reading guest memory. */
static void sample_progress(struct guest_run *run, struct vm_cpu *c)
{
    struct sample_state *at = &samples[c->index];
    uint64_t now = hal_counter();
    if (!run->sample_ticks || now < at->next)
        return;
    uint64_t ip, psr, retired, fault_ip, fault_info, faults;
    unsigned slot;
    const char *fault;
    vcpu_progress(c->v, &ip, &slot, &psr, &retired, &fault, &fault_ip, &fault_info, &faults);
    char who[16] = "", reserve[80] = "";
    struct hal_memory_stats memory;
    if (hal_memory_get_stats(&memory))
        vmm_snprintf(reserve, sizeof(reserve), " reserve_used_bytes=%llu reserve_peak_bytes=%llu",
                     (unsigned long long)memory.used_bytes, (unsigned long long)memory.peak_bytes);
    if (vm_current()->count > 1)
        vmm_snprintf(who, sizeof(who), "vCPU%u ", c->index);
    LOGI(
        "%ssample t=%llus ip=%llx:%u cpl=%u psr.i=%u psr=%llx retired=%llu delta=%llu idle_ms=%llu "
        "faults+%llu last_fault=%s@%llx info=%llx%s",
        who, (unsigned long long)((now - run->start) / run->hz), (unsigned long long)ip, slot,
        (unsigned)((psr >> 32) & 3), (unsigned)((psr >> 14) & 1), (unsigned long long)psr,
        (unsigned long long)retired, (unsigned long long)(retired - at->retired),
        (unsigned long long)((c->idle_ticks - at->idle) * 1000 / run->hz),
        (unsigned long long)(faults - at->faults), fault ? fault : "none",
        (unsigned long long)fault_ip, (unsigned long long)fault_info, reserve);
    at->retired = retired;
    at->idle = c->idle_ticks;
    at->faults = faults;
    at->next = now + run->sample_ticks;
}
/* Opt-in dump_va=<hex va>:<bytes>[,...]: at guest exit, print guest
 * memory through the BSP's translations. Each range is at most 64 KiB. */
static void dump_virtual(struct guest_run *run)
{
    const char *value;
    size_t length, i = 0;
    if (!run->cmdline || !platform_cmdline_value(run->cmdline, "dump_va", &value, &length))
        return;
    struct vcpu *v = run->bsp->v;
    while (i < length) {
        uint64_t va, bytes;
        const char *error;
        if (!vmm_dump_va_range(value, length, &i, &va, &bytes, &error)) {
            LOGW("dump_va: %s", error);
            continue;
        }
        for (uint64_t offset = 0, span = bytes ? (va & 7) + bytes : 0; offset < span; offset += 8) {
            uint64_t at = (va & ~UINT64_C(7)) + offset;
            uint64_t word;
            if (vcpu_read_virtual(v, at, &word))
                LOGI("dump_va %016llx: %016llx", (unsigned long long)at, (unsigned long long)word);
            else
                LOGI("dump_va %016llx: untranslated", (unsigned long long)at);
        }
    }
}
/* Report post-EBS observation cost, including bounded or interrupted runs. */
static void handoff_end(struct guest_run *run, const char *how)
{
    uint64_t ticks = run->handoff_steps ? hal_counter() - run->handoff_start : 0;
    LOGI("EFI handoff observation %s: %llu single-step slices in %llu ms (handoff_steps=%llu)", how,
         (unsigned long long)run->handoff_steps, (unsigned long long)(ticks * 1000 / run->hz),
         (unsigned long long)run->handoff_limit);
}
static void handoff_step(struct guest_run *run)
{
    struct platform_state *platform = run->platform;
    if (!platform->handoff_pending)
        return;
    if (run->handoff_steps >= run->handoff_limit) {
        LOGW("EFI kernel entry not recognised within handoff_steps=%llu: observation stopped, "
             "normal budgets resume",
             (unsigned long long)run->handoff_limit);
        platform->handoff_pending = false;
        handoff_end(run, "stopped at its bound");
        return;
    }
    if (!run->handoff_steps)
        run->handoff_start = hal_counter();
    ++run->handoff_steps;
}
/* port_trace=N[:FIRST-LAST]: N decimal (at most 9999), ports hex. */
static bool port_trace_option(struct platform_state *p, const char *value, size_t length)
{
    unsigned number = 0, first = 0, last = 0xffff;
    size_t i = 0;
    for (; i < length && value[i] != ':'; ++i) {
        if (value[i] < '0' || value[i] > '9' || i == 4)
            return false;
        number = number * 10 + (unsigned)(value[i] - '0');
    }
    if (!i)
        return false;
    if (i < length) {
        unsigned *bound = &first;
        unsigned digits = 0;
        first = last = 0;
        for (++i; i < length; ++i) {
            char c = value[i];
            if (c == '-' && bound == &first && digits) {
                bound = &last;
                digits = 0;
                continue;
            }
            unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                         : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                                                : 16;
            if (d == 16 || ++digits > 4)
                return false;
            *bound = *bound << 4 | d;
        }
        if (bound != &last || !digits || last < first)
            return false;
    }
    p->port_trace = number;
    p->port_trace_first = (uint16_t)first;
    p->port_trace_last = (uint16_t)last;
    return true;
}
/* The guest-specific steps of every executor loop (core/smp.c). */
static void exec_sample(void *ctx, struct vm_cpu *c)
{
    sample_progress(ctx, c);
}
static uint64_t exec_slice(void *ctx, struct vm_cpu *c)
{
    struct guest_run *run = ctx;
    struct platform_state *platform = run->platform;
    /* APs are held until the OS wakes them, after ExitBootServices. */
    if (c->index || !run->handoff_limit)
        return VM_SLICE_BUDGET;
    bool observing = platform->handoff_pending;
    observe_loader(run);
    if (observing && !platform->handoff_pending)
        handoff_end(run, "ended");
    handoff_step(run);
    return platform->handoff_pending ? 1 : VM_SLICE_BUDGET;
}
static bool exec_debug_exit(void *ctx, struct vm_cpu *c)
{
    struct guest_run *run = ctx;
    struct platform_state *platform = run->platform;
    /* Only the platform's one-shot EBS request; other DEBUG exits (e.g. a
     * lockstep divergence) still end the run. */
    if (c->index || !platform->ebs_exit_requested)
        return false;
    platform->ebs_exit_requested = false;
    observe_exit_boot_services(run);
    if (run->handoff_limit && !platform->handoff_pending)
        handoff_end(run, "ended");
    return true;
}
static void exec_stopped(void *ctx, struct vm_cpu *c)
{
    struct guest_run *run = ctx;
    if (fp_stats && engine_fp_log)
        engine_fp_log(c->v);
    if (c->index) {
        /* An AP's per-vCPU counters; the milestone lines stay vCPU 0's. */
        uint64_t ip, psr, retired, fault_ip, fault_info, faults;
        unsigned slot;
        const char *fault;
        vcpu_progress(c->v, &ip, &slot, &psr, &retired, &fault, &fault_ip, &fault_info, &faults);
        LOGI("vCPU%u executor stopped retired=%llu exec_ms=%llu idle_ms=%llu wall_ms=%llu",
             c->index, (unsigned long long)retired,
             (unsigned long long)ticks_ms(c->exec_ticks, run->hz),
             (unsigned long long)ticks_ms(c->idle_ticks, run->hz),
             (unsigned long long)ticks_ms(hal_counter() - run->start, run->hz));
        return;
    }
    milestone(run, "exit");
    if (run->platform->handoff_pending)
        handoff_end(run, "unfinished at exit");
}
static const struct vm_exec_ops exec_ops = {
    .sample = exec_sample,
    .slice = exec_slice,
    .debug_exit = exec_debug_exit,
    .stopped = exec_stopped,
};

/* Undoes the VM setup in the teardown order: routes (unbind waits for in-flight senders), then the vCPUs, then the
 * engines and devices. */
static void vm_cleanup(struct vm *vm, unsigned bound, enum engine_kind engine,
                       struct platform_state *platform, bool restart)
{
    helper_shutdown();
    if (code_background_set)
        code_background_set(NULL, NULL);
    if (tier0_set_reclaim_submit)
        tier0_set_reclaim_submit(NULL);
    for (unsigned i = 0; i < bound; ++i)
        platform_irq_unbind(i);
    for (unsigned i = 0; vm && i < vm->count; ++i) {
        struct vcpu *v = vm->cpu[i].v;
        if (!v)
            continue;
        engine_vcpu_detach(v);
        vcpu_destroy(v);
        vm->cpu[i].v = NULL;
    }
    vm_destroy(vm);
    /* Tier1 is configured by engine=tier1 and by engine=lockstep tier1=on;
     * engine_tier1_destroy does nothing otherwise. */
    engine_tier1_destroy();
    if (engine != ENGINE_INTERP)
        tier0_destroy();
    if (!restart)
        platform_devices_cleanup(platform);
}

static void request_reclaim(void)
{
    helper_post(HELPER_RECLAIM, 0);
}

/* FP options take 0 or 1 (include/ia64vmm/engine.h). */
static bool fp_configure(const char *cmdline)
{
    static const struct {
        const char *key;
        unsigned flag;
        bool set;
    } options[] = {
        {"fp_approx", ENGINE_FP_APPROX, true},
        {"fp_host", ENGINE_FP_SOFT, false},
        {"fp_check", ENGINE_FP_CHECK, true},
        {"fp_stats", ENGINE_FP_CLASSIFY, true},
        {"fp_experimental", ENGINE_FP_EXPERIMENTAL, true},
    };
    unsigned flags = 0;
    for (unsigned i = 0; i < sizeof(options) / sizeof(*options); ++i) {
        const char *value;
        size_t length;
        if (!platform_cmdline_value(cmdline, options[i].key, &value, &length))
            continue;
        if (length != 1 || (*value != '0' && *value != '1')) {
            LOGE("%s takes 0 or 1", options[i].key);
            return false;
        }
        if ((*value == '1') == options[i].set)
            flags |= options[i].flag;
    }
    fp_stats = flags & ENGINE_FP_CLASSIFY;
    if (flags & ENGINE_FP_EXPERIMENTAL) {
        LOGI("fp_experimental=1: direct FP expansions have incomplete exception proofs");
        if (flags & ENGINE_FP_APPROX)
            LOGI("fp_approx=1: FP arithmetic may differ from IA-64 precision");
    }
    if (engine_fp_configure)
        engine_fp_configure(flags);
    return true;
}

static int guest_boot(const struct boot_info *bi, const struct boot_module *module,
                      enum ia64_cpu_model cpu_model, unsigned boot, uint64_t run_start,
                      bool *restart);

int vm_run_guest(const struct boot_info *bi)
{
#ifndef IA64VMM_TIER1_CHECKED
    const char *replay_value;
    size_t replay_length;
    if (platform_cmdline_value(bi->cmdline, "tier1_check_every", &replay_value, &replay_length) ||
        platform_cmdline_value(bi->cmdline, "tier1_check_after", &replay_value, &replay_length)) {
        LOGE("Tier1 replay options require a TIER1_CHECKED=1 build");
        return VMM_EXIT_SETUP;
    }
#endif
    enum ia64_cpu_model cpu_model = IA64_CPU_MONTECITO;
    const char *value;
    size_t length;
    if (platform_cmdline_value(bi->cmdline, "cpu_model", &value, &length) &&
        !ia64_cpu_model_parse(value, length, &cpu_model)) {
        char bad_model[1024];
        size_t n = length < sizeof(bad_model) - 1 ? length : sizeof(bad_model) - 1;
        memcpy(bad_model, value, n);
        bad_model[n] = 0;
        LOGE("unknown cpu_model=%s", bad_model);
        return VMM_EXIT_SETUP;
    }
    LOGI("cpu_model=%s", ia64_cpu_model_name(cpu_model));
    if (!fp_configure(bi->cmdline))
        return VMM_EXIT_SETUP;
    if (!platform_check_window(bi))
        return VMM_EXIT_SETUP;
    /* display=vbios: once, on the management CPU, after the PCI
     * inventory and before any guest state exists. */
    bool display_ready = platform_vbios_post(bi);
    {
        /* Hold the display outcome before starting or stopping setup. */
        const char *display;
        size_t display_length;
        if (platform_cmdline_value(bi->cmdline, "display", &display, &display_length) &&
            display_length == 5 && !strncmp(display, "vbios", 5))
            hal_checkpoint("vbios");
    }
    if (!display_ready)
        return VMM_EXIT_SETUP;
    const struct boot_module *module = NULL;
    for (unsigned i = 0; i < bi->module_count; ++i)
        if (!strncmp(bi->modules[i].name, "firmware", sizeof(bi->modules[i].name)))
            module = &bi->modules[i];
    if (!module || module->size > SIZE_MAX)
        return VMM_EXIT_SETUP;
    /* timeout_sec counts from the first guest boot across restarts. */
    uint64_t run_start = hal_counter();
    for (unsigned boot = 0;; ++boot) {
        bool restart = false;
        int code = guest_boot(bi, module, cpu_model, boot, run_start, &restart);
        if (restart)
            continue;
        /* The guest run ended and control returned to the VMM: the one
         * write of the EFI variables. */
        nvram_save();
        return code;
    }
}

/* One guest boot: firmware entry to a restart or the end of the run.
 * Boot 0 registers the platform; later boots reset it. */
static int guest_boot(const struct boot_info *bi, const struct boot_module *module,
                      enum ia64_cpu_model cpu_model, unsigned boot, uint64_t run_start,
                      bool *restart)
{
    const char *value;
    size_t length;
    struct fw_image fw;
    struct ia64vmm_handoff handoff;
    struct ia64_entry_regs regs;
    uint64_t handoff_gpa;
    if (!fw_load(module->hva, module->size, 0, &fw))
        return VMM_EXIT_SETUP;
    unsigned count = hal_vcpu_count(), limit = IA64VMM_VCPU_LIMIT;
    if (!count || count > limit) {
        LOGE("vcpus=%u: the %s firmware supports 1..%u vCPUs", count, "new", limit);
        return VMM_EXIT_SETUP;
    }
    if (!platform_handoff_model(bi, &fw, cpu_model, &handoff, &handoff_gpa, &regs))
        return VMM_EXIT_SETUP;
    struct vcpu_config cfg = {.engine = ENGINE_INTERP,
                              .itc_hz = IA64VMM_ITC_HZ,
                              .pal_entry = fw.pal_entry,
                              .pale_init = fw.pale_init,
                              .pale_check = fw.pale_check,
                              .cpu_model = cpu_model,
                              .cores_per_socket = ia64_model_socket_width(cpu_model, count),
                              .cpu_count = count};
    if (platform_cmdline_value(bi->cmdline, "engine", &value, &length)) {
        if (length == 5 && !strncmp(value, "tier0", length))
            cfg.engine = ENGINE_TIER0;
        else if (length == 5 && !strncmp(value, "tier1", length))
            cfg.engine = ENGINE_TIER1;
        else if (length == 8 && !strncmp(value, "lockstep", length))
            cfg.engine = ENGINE_LOCKSTEP;
        else if (length != 6 || strncmp(value, "interp", length))
            return VMM_EXIT_SETUP;
    }
    /* interp, Tier0 and Tier0 lockstep share concurrent code caches, as does
     * Tier1. Serialization remains a diagnostic. */
    bool serialize = false;
    if (platform_cmdline_value(bi->cmdline, "smp_serialize", &value, &length)) {
        if (length != 1 || (*value != '0' && *value != '1')) {
            LOGE("smp_serialize must be 0 or 1");
            return VMM_EXIT_SETUP;
        }
        serialize = *value == '1';
    }
    /* Concurrent Tier1 compiles only in the background. */
    bool tier1_replay = cfg.engine == ENGINE_LOCKSTEP &&
                        platform_cmdline_value(bi->cmdline, "tier1", &value, &length) &&
                        length == 2 && !strncmp(value, "on", length);
    if (count > 1 && (cfg.engine == ENGINE_TIER1 || tier1_replay) &&
        platform_cmdline_value(bi->cmdline, "helpers", &value, &length) && length == 3 &&
        !strncmp(value, "off", length)) {
        LOGE("%s with vcpus=%u needs background compilation: helpers=off is a setup error",
             tier1_replay ? "engine=lockstep tier1=on" : "engine=tier1", count);
        return VMM_EXIT_SETUP;
    }
    enum code_validity_mode validity = CODE_VALIDITY_COMPARE_ALWAYS;
    uint64_t snapshot_bytes = CODE_SNAPSHOT_DEFAULT_BYTES;
    bool explicit_snapshot_budget = false;
    if (platform_cmdline_value(bi->cmdline, "code_validity", &value, &length) &&
        !code_validity_parse_mode(value, length, &validity)) {
        LOGE("invalid code_validity mode");
        return VMM_EXIT_SETUP;
    }
    if (count > 1 && validity == CODE_VALIDITY_NONE) {
        LOGE("code_validity=none with multiple vCPUs is a setup error");
        return VMM_EXIT_SETUP;
    }
    if (platform_cmdline_value(bi->cmdline, "code_snapshot_mib", &value, &length)) {
        explicit_snapshot_budget = true;
        uint64_t mib = 0;
        if (!length)
            return VMM_EXIT_SETUP;
        for (size_t i = 0; i < length; ++i) {
            if (value[i] < '0' || value[i] > '9' ||
                mib > (UINT64_MAX - (unsigned)(value[i] - '0')) / 10)
                return VMM_EXIT_SETUP;
            mib = mib * 10 + (unsigned)(value[i] - '0');
        }
        if (!mib || mib > UINT64_MAX / 1048576)
            return VMM_EXIT_SETUP;
        snapshot_bytes = mib * 1048576;
    }
    if (!code_validity_configure(validity, snapshot_bytes))
        return VMM_EXIT_SETUP;
    /* The effective mode and what it checks;
     * code_validity_configure runs hw-dirty as compare-always. */
    static const char *const validity_unit[] = {
        [CODE_VALIDITY_COMPARE_ALWAYS] = "compare-always, unit=bundle, once per stay on its pages",
        [CODE_VALIDITY_EPOCH_COMPARE] =
            "epoch-compare, unit=bundle, only after a storage-interrupt "
            "epoch bump: DMA completed by polling or by other devices "
            "is not detected",
        [CODE_VALIDITY_NONE] = "none, no comparison: DMA into code is not detected",
    };
    if (cfg.engine != ENGINE_INTERP && !tier0_configure_cmdline(bi->cmdline))
        return VMM_EXIT_SETUP;
    /* engine=tier1: Tier0 as the outer loop with Tier1;
     * engine=lockstep tier1=on: the same, every Tier1 execution checked. */
    if (cfg.engine == ENGINE_TIER1 && !engine_tier1_configure(bi->cmdline, validity)) {
        tier0_destroy();
        return VMM_EXIT_SETUP;
    }
    if (platform_cmdline_value(bi->cmdline, "tier1", &value, &length)) {
        bool on = length == 2 && !strncmp(value, "on", length);
        if ((!on && (length != 3 || strncmp(value, "off", length))) ||
            cfg.engine != ENGINE_LOCKSTEP) {
            LOGE("tier1= takes on or off, with engine=lockstep (checked Tier1 lockstep)");
            if (cfg.engine != ENGINE_INTERP)
                tier0_destroy();
            return VMM_EXIT_SETUP;
        }
        if (on && !engine_tier1_lockstep_configure(bi->cmdline, validity)) {
            tier0_destroy();
            return VMM_EXIT_SETUP;
        }
    }
    uint64_t halt_retry_us = 1000;
    if (platform_cmdline_value(bi->cmdline, "halt_retry_us", &value, &length)) {
        halt_retry_us = 0;
        if (!length)
            return VMM_EXIT_SETUP;
        for (size_t i = 0; i < length; ++i) {
            if (value[i] < '0' || value[i] > '9' || halt_retry_us > 1000000)
                return VMM_EXIT_SETUP;
            halt_retry_us = halt_retry_us * 10 + (unsigned)(value[i] - '0');
        }
        if (halt_retry_us > 1000000)
            return VMM_EXIT_SETUP;
    }
    uint64_t timeout = 0;
    if (platform_cmdline_value(bi->cmdline, "timeout_sec", &value, &length)) {
        if (!length)
            return VMM_EXIT_SETUP;
        for (size_t i = 0; i < length; ++i) {
            if (value[i] < '0' || value[i] > '9' ||
                timeout > (UINT64_MAX - (unsigned)(value[i] - '0')) / 10)
                return VMM_EXIT_SETUP;
            timeout = timeout * 10 + (unsigned)(value[i] - '0');
        }
    }
    if (boot)
        platform_devices_restart(&platform);
    else {
        if (!platform_devices_init(&platform)) {
            platform_devices_cleanup(&platform);
            return VMM_EXIT_SETUP;
        }
        /* EFI variables of an earlier VMM run; the
         * slot pair follows the firmware format. */
        nvram_load(1, platform.nvram);
    }
    /* INTx lines are prepared with the IOSAPIC during device mapping;
     * publish those versioned route facts before firmware reads the handoff. */
    if (!platform_pci_handoff(&handoff) ||
        !platform_ram_write(handoff_gpa, &handoff, sizeof(handoff))) {
        platform_devices_cleanup(&platform);
        return VMM_EXIT_SETUP;
    }
    /* The remaining options, before any vCPU exists; trace= applies to
     * every vCPU once created. */
    bool traced = false;
    unsigned trace_count = 0;
    if (platform_cmdline_value(bi->cmdline, "trace", &value, &length)) {
        unsigned count = 0;
        bool valid = length != 0;
        for (size_t i = 0; i < length && valid; ++i) {
            valid = value[i] >= '0' && value[i] <= '9';
            if (valid) {
                count = count * 10 + (unsigned)(value[i] - '0');
                valid = count <= 65536;
            }
        }
        if (!valid) {
            platform_devices_cleanup(&platform);
            return VMM_EXIT_SETUP;
        }
        traced = true;
        trace_count = count;
    }
    if (profile_configure && !profile_configure(bi->cmdline)) {
        LOGE("profile_hz takes 1..100000, profile_sec 1..86400");
        platform_devices_cleanup(&platform);
        return VMM_EXIT_SETUP;
    }
    uint64_t sample_sec = 0;
    if (platform_cmdline_value(bi->cmdline, "sample_sec", &value, &length) &&
        !(length == 3 && !strncmp(value, "off", 3))) {
        bool valid = length != 0;
        for (size_t i = 0; i < length && valid; ++i) {
            valid = value[i] >= '0' && value[i] <= '9';
            if (valid)
                sample_sec = sample_sec * 10 + (unsigned)(value[i] - '0');
            valid = valid && sample_sec <= 3600;
        }
        if (!valid || !sample_sec) {
            platform_devices_cleanup(&platform);
            return VMM_EXIT_SETUP;
        }
    }
    uint64_t handoff_steps = 0;
    if (platform_cmdline_value(bi->cmdline, "handoff_steps", &value, &length)) {
        bool valid = length != 0;
        handoff_steps = 0;
        for (size_t i = 0; i < length && valid; ++i) {
            valid = value[i] >= '0' && value[i] <= '9' && handoff_steps <= UINT64_C(100000000000);
            if (valid)
                handoff_steps = handoff_steps * 10 + (unsigned)(value[i] - '0');
        }
        if (!valid) {
            platform_devices_cleanup(&platform);
            return VMM_EXIT_SETUP;
        }
    }
    static const char *const fault_options[] = {"stop_fault", "log_faults"};
    for (unsigned o = 0; o < 2; ++o) {
        if (!platform_cmdline_value(bi->cmdline, fault_options[o], &value, &length))
            continue;
        unsigned number = 0;
        bool valid = length != 0 && length <= 4;
        for (size_t i = 0; i < length && valid; ++i) {
            valid = value[i] >= '0' && value[i] <= '9';
            if (valid)
                number = number * 10 + (unsigned)(value[i] - '0');
        }
        if (!valid) {
            platform_devices_cleanup(&platform);
            return VMM_EXIT_SETUP;
        }
        if (o)
            vcpu_set_log_faults(number);
        else
            vcpu_set_stop_fault(number);
    }
    if (platform_cmdline_value(bi->cmdline, "break_ip", &value, &length)) {
        size_t i = 0;
        bool valid = length != 0;
        while (valid && i < length) {
            uint64_t ip = 0;
            unsigned count = 1, digits = 0;
            for (; i < length && value[i] != ':' && value[i] != '+' && valid; ++i, ++digits) {
                char c = value[i];
                unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                             : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                                                    : 16;
                valid = d < 16 && digits < 16;
                ip = ip << 4 | d;
            }
            if (valid && i < length && value[i] == ':') {
                count = 0;
                for (++i; i < length && value[i] != '+' && valid; ++i) {
                    valid = value[i] >= '0' && value[i] <= '9' && count < 1000;
                    count = count * 10 + (unsigned)(value[i] - '0');
                }
            }
            valid = valid && digits && count;
            if (valid)
                vcpu_set_break_ip(ip, count);
            if (i < length)
                ++i;
        }
        if (!valid) {
            LOGE("break_ip takes HEX[:N][+HEX[:N]...]");
            platform_devices_cleanup(&platform);
            return VMM_EXIT_SETUP;
        }
    }
    if (platform_cmdline_value(bi->cmdline, "break_mem", &value, &length)) {
        uint64_t pa = 0;
        bool valid = length && length <= 16;
        for (size_t i = 0; i < length && valid; ++i) {
            char c = value[i];
            unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                         : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                                                : 16;
            valid = d < 16;
            pa = pa << 4 | d;
        }
        if (!valid) {
            LOGE("break_mem takes a hex guest physical address");
            platform_devices_cleanup(&platform);
            return VMM_EXIT_SETUP;
        }
        vcpu_set_break_mem(pa);
    }
    if (platform_cmdline_value(bi->cmdline, "port_trace", &value, &length) &&
        !port_trace_option(&platform, value, length)) {
        platform_devices_cleanup(&platform);
        return VMM_EXIT_SETUP;
    }
    /* The VM (its ITC origin first), then vCPU i with LID i: vCPU 0 starts
     * with the handoff registers, AP i with its entry register set and is
     * held until its first wake. */
    if (!vm_purge_configure(bi->cmdline))
        return VMM_EXIT_SETUP;
    struct vm *vm = vm_create(count, &platform, serialize);
    if (!vm) {
        platform_devices_cleanup(&platform);
        return VMM_EXIT_SETUP;
    }
    if (count > 1)
        LOGI("guest SMP: %u vCPUs, smp_serialize=%u", count, (unsigned)serialize);
    for (unsigned i = 0; i < count; ++i) {
        struct vcpu_config config = cfg;
        config.index = i;
        config.lid = IA64VMM_CPU_LID(i);
        struct vcpu *v = vcpu_create(&config);
        if (!v) {
            vm_cleanup(vm, 0, cfg.engine, &platform, false);
            return VMM_EXIT_SETUP;
        }
        if (!engine_vcpu_attach(v)) {
            vcpu_destroy(v);
            vm_cleanup(vm, 0, cfg.engine, &platform, false);
            return VMM_EXIT_SETUP;
        }
        vm->cpu[i].v = v;
        vm->cpu[i].host_cpu = hal_vcpu_cpu(i);
        if (i) {
            struct ia64_entry_regs ap;
            platform_entry_regs(&handoff, handoff_gpa, fw.entry, fw.global_pointer, i, &ap);
            vcpu_reset(v, &ap);
        } else
            vcpu_reset(v, &regs);
        if (traced && !ia64_set_trace(v, trace_count)) {
            vm_cleanup(vm, 0, cfg.engine, &platform, false);
            return VMM_EXIT_SETUP;
        }
    }
    bool helpers = true;
    if (platform_cmdline_value(bi->cmdline, "helpers", &value, &length)) {
        if (length == 3 && !strncmp(value, "off", length))
            helpers = false;
        else if (!(length == 2 && !strncmp(value, "on", length))) {
            LOGE("helpers= takes on or off");
            vm_cleanup(vm, 0, cfg.engine, &platform, false);
            return VMM_EXIT_SETUP;
        }
    }
    const struct t1_engine_glue *glue = engine_tier1_glue ? engine_tier1_glue() : NULL;
    if (hal_vcpu_cpu(0) != hal_cpu_index() &&
        helper_start(helpers, cfg.itc_icount, glue && glue->background && vm->cpu[0].v->upper)) {
        if (glue && glue->background && vm->cpu[0].v->upper)
            glue->background(helper_submit_t1, request_reclaim);
        code_background_set(helper_quiesce, helper_resume);
        if (tier0_set_reclaim_submit)
            tier0_set_reclaim_submit(request_reclaim);
        LOGI("helper CPUs=%u; %s work", hal_helper_count(),
             hal_helper_count() ? "background" : "management-step");
    }
    if (count > 1 && vm->cpu[0].v->upper && !(glue && glue->config && glue->config()->submit)) {
        LOGE("engine=tier1 with vcpus=%u: background compilation did not start", count);
        vm_cleanup(vm, 0, cfg.engine, &platform, false);
        return VMM_EXIT_SETUP;
    }
    /* Fixed arenas and hostwin pools exist. Budget sizing also reserves
     * the lazy decoded pages and compile/check workspaces before execution. */
    struct hal_memory_stats memory;
    if (hal_memory_get_stats(&memory)) {
        uint64_t free_bytes = memory.total_bytes - memory.used_bytes;
        if (!explicit_snapshot_budget)
            snapshot_bytes = code_validity_reserve_budget(free_bytes, memory.total_bytes);
        engine_tier1_memory_budget(free_bytes, snapshot_bytes, memory.total_bytes);
        LOGI("VMM reserve total_bytes=%llu used_bytes=%llu free_bytes=%llu",
             (unsigned long long)memory.total_bytes, (unsigned long long)memory.used_bytes,
             (unsigned long long)free_bytes);
    }
    LOGI("code snapshot budget_bytes=%llu policy=%s", (unsigned long long)snapshot_bytes,
         explicit_snapshot_budget ? "user" : "default");
    LOGI(
        "code validity: mode=%s; snapshot budget %llu MiB",
        validity_unit[validity == CODE_VALIDITY_HW_DIRTY ? CODE_VALIDITY_COMPARE_ALWAYS : validity],
        (unsigned long long)(snapshot_bytes >> 20));
    uint64_t start = hal_counter(), hz = hal_counter_hz();
    if (!hz) {
        vm_cleanup(vm, 0, cfg.engine, &platform, false);
        return VMM_EXIT_SETUP;
    }
    static struct guest_run run;
    run = (struct guest_run){.bsp = &vm->cpu[0],
                             .platform = &platform,
                             .fw_base = fw.base,
                             .fw_size = fw.size,
                             .start = start,
                             .hz = hz,
                             .handoff_limit = handoff_steps,
                             .sample_ticks = sample_sec * hz,
                             .cmdline = bi->cmdline};
    for (unsigned i = 0; i < count; ++i)
        samples[i] = (struct sample_state){.next = start + sample_sec * hz};
    /* Every route before any executor starts: the BSP's first IPI always
     * finds its target. */
    for (unsigned i = 0; i < count; ++i)
        if (!platform_irq_bind(vm->cpu[i].v, i, IA64VMM_CPU_LID(i), vm->cpu[i].host_cpu)) {
            vm_cleanup(vm, i, cfg.engine, &platform, false);
            return VMM_EXIT_SETUP;
        }
    if (run.handoff_limit && !preboot_init(&run)) {
        for (unsigned i = 0; i < count; ++i)
            platform_irq_unbind(i);
        vm_cleanup(vm, 0, cfg.engine, &platform, false);
        return VMM_EXIT_SETUP;
    }
    struct vm_exec exec = {.ops = &exec_ops,
                           .ctx = &run,
                           .start = run_start,
                           .hz = hz,
                           .timeout = timeout,
                           .halt_retry_us = halt_retry_us};
    uint64_t watchdog =
        timeout && timeout <= (UINT64_MAX - run_start) / hz ? run_start + timeout * hz : UINT64_MAX;
    vmm_log_fb_enable(false);
    int code = vm_run(vm, &exec, watchdog);
    /* A reset request that restarts the guest: no
     * further DMA into the RAM the next boot reuses. */
    *restart = vm_restart_decided(vm);
    if (*restart)
        platform_pci_quiesce();
    if (profile_dump)
        profile_dump("exit");
    /* All executors stopped: AP translations and captures are stable. */
    dump_virtual(&run);
    /* A host-console scroll redraws its RAM grid over the guest GOP. Keep
     * the successful final image through statistics and shutdown logging,
     * and the guest's display across a restart. */
    if (!*restart && (code != VMM_EXIT_SUCCESS || !platform.guest_gop_owned))
        vmm_log_fb_enable(true);
    platform_msi_dump();
    platform_irq_dump();
    for (unsigned i = 0; i < count; ++i)
        platform_irq_unbind(i);
    code_validity_dump_stats();
    engine_dump_stats();
    if (hal_memory_get_stats(&memory))
        LOGI("VMM reserve total_bytes=%llu used_bytes=%llu peak_bytes=%llu",
             (unsigned long long)memory.total_bytes, (unsigned long long)memory.used_bytes,
             (unsigned long long)memory.peak_bytes);
    if (run.preboot)
        hal_free_pages(run.preboot, run.preboot_storage);
    vm_cleanup(vm, 0, cfg.engine, &platform, *restart);
    if (!*restart)
        return code;
    if (!platform_pci_reset_functions()) {
        *restart = false;
        platform_devices_cleanup(&platform);
        return VMM_EXIT_FAILURE;
    }
    static const char *const types[] = {"cold", "warm", "shutdown", "platform"};
    uint64_t type = platform.reset_type;
    LOGI("guest restart %u source=%s type=%s status=%llx", boot + 1,
         platform_reset_source_name(platform.reset_source), type < 4 ? types[type] : "?",
         (unsigned long long)platform.reset_status);
    return code;
}

/* Diagnostic guest_ram_fill=XX: every guest RAM page holds the byte XX
 * before the firmware is loaded, as a machine's RAM holds what the previous
 * boot left; physical RAM is not necessarily zeroed. */
static bool fill_guest_ram(void)
{
    const char *value;
    size_t length;
    if (!platform_cmdline_value(boot->cmdline, "guest_ram_fill", &value, &length))
        return true;
    unsigned byte = 0;
    for (size_t i = 0; i < length; ++i) {
        char c = value[i];
        unsigned digit = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                         : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                                                : 16;
        if (digit > 15 || length > 2) {
            LOGE("guest_ram_fill takes one hex byte");
            return false;
        }
        byte = byte * 16 + digit;
    }
    if (!length) {
        LOGE("guest_ram_fill takes one hex byte");
        return false;
    }
    uint64_t total = 0;
    for (unsigned i = 0; i < gm_region_count(); ++i) {
        const struct gm_region *r = gm_region_at(i);
        if (r->kind != GM_RAM)
            continue;
        memset(r->hva, (int)byte, r->size);
        total += r->size;
    }
    LOGW("guest_ram_fill: %llu MiB of guest RAM filled with %02x",
         (unsigned long long)(total >> 20), byte);
    return true;
}

static bool build_guest_map(void)
{
    for (unsigned i = 0; i < boot->mem_range_count; ++i) {
        const struct boot_mem_range *m = &boot->mem[i];
        if (m->type != BOOT_MEM_USABLE)
            continue;
        if (m->size > UINT64_MAX - m->base || m->base > UINT64_MAX - (IA64VMM_PAGE_SIZE - 1))
            return false;
        uint64_t base = IA64VMM_ALIGN_UP(m->base, IA64VMM_PAGE_SIZE);
        uint64_t end = IA64VMM_ALIGN_DOWN(m->base + m->size, IA64VMM_PAGE_SIZE);
        if (end <= base || end - base < IA64VMM_PAGE_SIZE)
            continue;
        if (!m->hva)
            return false;
        struct gm_region r = {
            .base = base,
            .size = end - base,
            .kind = GM_RAM,
            .name = "ram",
            .hva = m->hva + (base - m->base),
        };
        if (!gm_add_region(&r))
            return false;
    }
    if (boot->fb.format != BOOT_FB_NONE) {
        struct gm_region r = {
            .base = boot->fb.base,
            .size = boot->fb.size,
            .kind = GM_DIRECT_MMIO,
            .name = "gop-fb",
            .hva = boot->fb.hva,
        };
        if (!gm_add_region(&r))
            return false;
    }
    return true;
}

void vmm_main(const struct boot_info *bi)
{
    if (!bi || bi->version != BOOT_INFO_VERSION || bi->mem_range_count > BOOT_MAX_MEM_RANGES ||
        bi->module_count > BOOT_MAX_MODULES)
        hal_shutdown(HAL_SHUTDOWN_POWER_OFF, VMM_EXIT_SETUP);
    boot = hal_alloc_pages(IA64VMM_ALIGN_UP(sizeof(*boot), 4096) / 4096, HAL_PAGE_ZERO);
    if (!boot)
        hal_shutdown(HAL_SHUTDOWN_POWER_OFF, VMM_EXIT_SETUP);
    *boot = *bi;
    LOGI("IA-64 VMM on %s (%u host CPU)", boot->host_name, boot->host_cpu_count);
    static const char *const types[] = {
        "usable",     "reserved", "acpi-reclaimable", "acpi-nvs", "bad",
        "bootloader", "vmm",      "framebuffer",
    };
    for (unsigned i = 0; i < boot->mem_range_count; ++i) {
        const struct boot_mem_range *m = &boot->mem[i];
        LOGI("host mem %016llx size %016llx %s hva=%p", (unsigned long long)m->base,
             (unsigned long long)m->size,
             (unsigned)m->type < IA64VMM_ARRAY_SIZE(types) ? types[m->type] : "unknown",
             (void *)m->hva);
    }
    for (unsigned i = 0; i < boot->module_count; ++i) {
        const struct boot_module *m = &boot->modules[i];
        LOGI("module %s hpa=%016llx size=%llu", m->name, (unsigned long long)m->hpa,
             (unsigned long long)m->size);
    }
    LOGI("framebuffer %ux%u bpp=%u pitch=%u base=%016llx", boot->fb.width, boot->fb.height,
         boot->fb.bpp, boot->fb.pitch, (unsigned long long)boot->fb.base);
    int code = VMM_EXIT_SETUP;
    if (platform_check_window(boot) && build_guest_map() && fill_guest_ram()) {
        for (unsigned i = 0; i < gm_region_count(); ++i) {
            const struct gm_region *r = gm_region_at(i);
            LOGI("guest region %s base=%016llx size=%016llx kind=%u", r->name,
                 (unsigned long long)r->base, (unsigned long long)r->size, (unsigned)r->kind);
        }
        code = vm_run_guest(boot);
    } else {
        LOGE("failed to build guest memory map");
    }
    log_flush_guest_text();
    hal_shutdown(HAL_SHUTDOWN_POWER_OFF, code);
}
