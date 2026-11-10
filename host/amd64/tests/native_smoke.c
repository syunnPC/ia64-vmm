/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Ring-0 native transport smoke image; no IA-64 firmware is required. */
#include "host/amd64/amd64.h"
#include "host/amd64/ia32.h"
#include "ia64vmm/bootinfo.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

uint32_t platform_pci_host_read(uint16_t bdf, unsigned offset, unsigned size)
{
    (void)bdf;
    (void)offset;
    (void)size;
    hal_panic("native smoke requires an inactive IOMMU");
}
void platform_pci_host_write(uint16_t bdf, unsigned offset, uint32_t value, unsigned size)
{
    (void)bdf;
    (void)offset;
    (void)value;
    (void)size;
    hal_panic("native smoke requires an inactive IOMMU");
}

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            hal_panic("native smoke: " #x);                                                        \
    } while (0)
static struct ia32_native_state initial(void)
{
    struct ia32_native_state s = {0};
    s.cs = (struct ia32_segment){.limit = UINT32_MAX, .selector = 8, .attrib = 0xc9b};
    s.ss = s.es = s.ds = s.fs = s.gs =
        (struct ia32_segment){.limit = UINT32_MAX, .selector = 16, .attrib = 0xc93};
    s.ldtr.unusable = s.tr.unusable = true;
    s.fs = s.gs = (struct ia32_segment){.unusable = true};
    s.cflg = (UINT64_C(1) << 41) | 1;
    s.eflag = 0x202;
    s.dr6 = 0xffff0ff0;
    s.dr7 = 0x400;
    s.eip.value = 0x1000;
    s.gpr[IA32_ESP] = 0x2ff0;
    s.fp.bytes[0] = 0x7f;
    s.fp.bytes[1] = 3;
    s.fp.bytes[24] = 0x80;
    s.fp.bytes[25] = 0x1f;
    return s;
}
static struct ia32_native_exit enter(const struct ia32_backend_ops *ops, struct ia32_container *c,
                                     struct ia32_native_state *s, bool step, uint64_t deadline)
{
    struct ia32_run_limit limit = {.deadline = deadline, .step = step, .svm_step_safe = true};
    struct ia32_run_usage usage;
    struct ia32_native_exit e = {0};
    for (unsigned i = 0; i < 8; ++i) {
        e = ops->enter(c, s, &limit, &usage);
        LOGI("native smoke exit: reason=%u raw=%llx phase=%u eip=%x eax=%x flags=%x detail=%s",
             e.reason, (unsigned long long)e.raw_reason, e.phase, s->eip.value, s->gpr[IA32_EAX],
             s->eflag, e.detail ? e.detail : "");
        if (e.reason != IA32_EXIT_HOST_IRQ && e.reason != IA32_EXIT_HOST_NMI)
            return e;
    }
    hal_panic("native smoke: too many unrelated host exits");
}
void vmm_main(const struct boot_info *bi)
{
    (void)bi;
    const struct ia32_backend_ops *ops = hal_ia32_backend();
    struct ia32_capabilities caps;
    struct ia32_setup_error error = {0};
    CHECK(ops);
    if (!ops->probe(hal_cpu_index(), &caps, &error)) {
        LOGE("native smoke probe refused: %s raw=%llx", error.detail,
             (unsigned long long)error.raw);
        hal_shutdown(HAL_SHUTDOWN_POWER_OFF, 2);
    }
    LOGI("native smoke backend=%u mtf=%u nested=%u", caps.backend, caps.mtf, caps.nested_paging);
    struct ia32_container *c;
    if (!ops->create(hal_cpu_index(), &c, &error)) {
        LOGE("native smoke create refused: %s", error.detail);
        hal_shutdown(HAL_SHUTDOWN_POWER_OFF, 2);
    }
    uint8_t *code = hal_alloc_pages(1, HAL_PAGE_ZERO);
    uint32_t *data = hal_alloc_pages(1, HAL_PAGE_ZERO);
    CHECK(code && data);
    const uint8_t program[] = {0x90, 0xb8, 0x78, 0x56, 0x34, 0x12, 0x83, 0xc0, 0x05, 0xa3,
                               0x00, 0x20, 0x00, 0x00, 0x0f, 0xa2, 0xfa, 0xeb, 0xfe};
    memcpy(code, program, sizeof(program));
    const uint8_t fp_program[] = {0xd9, 0xe8, 0xd9, 0x1d, 0x04, 0x20, 0x00, 0x00};
    memcpy(code + 0x100, fp_program, sizeof(fp_program));
    struct ia32_mapping m = {.token = {0x1000},
                             .hpa = (uintptr_t)code - amd64_hhdm,
                             .rights = IA32_MAP_READ | IA32_MAP_EXEC};
    CHECK(ops->map(c, &m, &error));
    m.token.value = 0x2000;
    m.hpa = (uintptr_t)data - amd64_hhdm;
    m.rights = IA32_MAP_READ | IA32_MAP_WRITE;
    CHECK(ops->map(c, &m, &error));
    struct ia32_native_state s = initial();
    if (!ops->import(c, &s, IA32_DIRTY_ALL, &error)) {
        LOGE("native smoke import refused: %s", error.detail);
        hal_shutdown(HAL_SHUTDOWN_POWER_OFF, 2);
    }
    const uint32_t eips[] = {0x1001, 0x1006, 0x1009, 0x100e};
    for (unsigned i = 0; i < 4; ++i) {
        struct ia32_native_exit e = enter(ops, c, &s, true, hal_counter() + hal_counter_hz());
        CHECK(e.reason == IA32_EXIT_STEP && s.eip.value == eips[i]);
        CHECK(!(s.eflag & 0x100));
    }
    CHECK(s.gpr[IA32_EAX] == 0x1234567d && data[0] == 0x1234567d);
    CHECK(!s.tr.attrib && s.tr.unusable && !s.ldtr.attrib && s.ldtr.unusable);
    CHECK(!s.fs.attrib && s.fs.unusable && !s.gs.attrib && s.gs.unusable);
    LOGI("native smoke stepped NOP, MOV, ADD and store passed");
    struct ia32_native_exit e = enter(ops, c, &s, false, hal_counter() + hal_counter_hz());
    CHECK(e.reason == IA32_EXIT_CPUID && s.eip.value == 0x100e && e.next_eip_valid);
    CHECK(s.gpr[IA32_EAX] == 0x1234567d);
    LOGI("native smoke CPUID interception preserves input registers passed");
    ia32_eip_t loop = e.next_eip;
    s.eip.value = 0x1100;
    CHECK(ops->import(c, &s, IA32_DIRTY_IP, &error));
    e = enter(ops, c, &s, true, hal_counter() + hal_counter_hz());
    CHECK(e.reason == IA32_EXIT_STEP && s.eip.value == 0x1102);
    uint16_t fsw;
    uint32_t fip, fdp;
    memcpy(&fsw, s.fp.bytes + 2, sizeof(fsw));
    memcpy(&fip, s.fp.bytes + 8, sizeof(fip));
    CHECK(((fsw >> 11) & 7) == 7 && s.fp.bytes[4] == 0x80 && fip == 0x1100);
    e = enter(ops, c, &s, true, hal_counter() + hal_counter_hz());
    CHECK(e.reason == IA32_EXIT_STEP && s.eip.value == 0x1108 && data[1] == 0x3f800000);
    memcpy(&fsw, s.fp.bytes + 2, sizeof(fsw));
    memcpy(&fip, s.fp.bytes + 8, sizeof(fip));
    memcpy(&fdp, s.fp.bytes + 16, sizeof(fdp));
    CHECK(((fsw >> 11) & 7) == 0 && !s.fp.bytes[4] && fip == 0x1102 && fdp == 0x2004);
    LOGI("native smoke x87 FLD1/FSTP, TOP, tags and legacy pointers across entries passed");
    struct ia32_native_state protected = s;
    const uint8_t vm86_program[] = {0x90, 0xb8, 0x34, 0x12, 0x83, 0xc0, 0x05, 0xa3, 0x0c,
                                    0x00, 0xd9, 0xe8, 0xd9, 0x1e, 0x10, 0x00, 0x36, 0xd9,
                                    0x06, 0x14, 0x00, 0xd9, 0x1e, 0x18, 0x00};
    memcpy(code + 0x180, vm86_program, sizeof(vm86_program));
    data[5] = 0x40000000;
    s = initial();
    s.eflag |= 1u << 17;
    s.cflg |= 1;
    s.cpl = 3;
    s.cs =
        (struct ia32_segment){.base = 0x1000, .limit = 0xffff, .selector = 0x100, .attrib = 0xf3};
    s.ss = s.es = s.ds = s.fs = s.gs =
        (struct ia32_segment){.base = 0x2000, .limit = 0xffff, .selector = 0x200, .attrib = 0xf3};
    s.eip.value = 0x180;
    s.gpr[IA32_ESP] = 0xff0;
    s.gdtr.limit = UINT32_MAX;
    CHECK(ops->import(c, &s, IA32_DIRTY_ALL, &error));
    const uint32_t vm86_eip[] = {0x181, 0x184, 0x187, 0x18a, 0x18c, 0x190, 0x195, 0x199};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(vm86_eip); ++i) {
        e = enter(ops, c, &s, true, hal_counter() + hal_counter_hz());
        CHECK(e.reason == IA32_EXIT_STEP && s.eip.value == vm86_eip[i]);
        CHECK((s.eflag & (1u << 17)) && !(e.hardware_eflag & (1u << 17)) && s.cpl == 3);
        CHECK(s.cs.selector == 0x100 && s.ss.selector == 0x200 && s.cs.attrib == 0xf3 &&
              s.gdtr.limit == UINT32_MAX);
    }
    CHECK((data[3] & 0xffff) == 0x1239 && data[4] == 0x3f800000 && data[6] == 0x40000000);
    CHECK((s.fp.bytes[12] | s.fp.bytes[13] << 8) == 0x100);
    CHECK((s.fp.bytes[20] | s.fp.bytes[21] << 8) == 0x200);
    memcpy(&fip, s.fp.bytes + 8, sizeof(fip));
    memcpy(&fdp, s.fp.bytes + 16, sizeof(fdp));
    LOGI("native smoke VM86 x87 FIP=%x FDP=%x", fip, fdp);
    CHECK(fip == 0x195 && fdp == 0x18);
    LOGI("native smoke VM86 private execution, cached state and x87 selectors passed");
    s = protected;
    s.eip = loop;
    CHECK(s.eip.value == 0x1010 && ops->import(c, &s, IA32_DIRTY_ALL, &error));
    uint64_t start = hal_counter(), deadline = start + hal_counter_hz() / 100;
    e = enter(ops, c, &s, false, deadline);
    CHECK(e.reason == IA32_EXIT_DEADLINE && !(s.eflag & 0x200));
    CHECK(hal_counter() - start < hal_counter_hz());
    CHECK(s.eip.value == 0x1011);
    LOGI("native smoke CLI loop deadline with guest IF=0 passed");
    CHECK(ops->reset(c, &error));
    ops->destroy(c);
    hal_free_pages(code, 1);
    hal_free_pages(data, 1);
    LOGI("native smoke PASSED");
    hal_shutdown(HAL_SHUTDOWN_POWER_OFF, 0);
}
