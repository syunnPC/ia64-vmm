/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included by main.c so PAL checks share the existing memory/clock harness. */
#include "ia64/pal.h"
#include "ia64vmm/platform.h"

static void pal_args(struct vcpu *v, uint64_t index, uint64_t a, uint64_t b, uint64_t c)
{
    ia64_gr_write(&v->s, 28, index, false);
    unsigned first = index & 256 ? 33 : 29;
    if (index & 256) {
        /* Nonzero GR rotation catches direct physical-array argument reads. */
        v->s.cfm = 8 | (8u << 7) | (1u << 14) | (3u << 18);
        ia64_gr_write(&v->s, 32, index, false);
        v->s.gr[29] = v->s.gr[30] = v->s.gr[31] = UINT64_MAX;
    }
    ia64_gr_write(&v->s, first, a, false);
    ia64_gr_write(&v->s, first + 1, b, false);
    ia64_gr_write(&v->s, first + 2, c, false);
    for (unsigned i = 8; i <= 11; ++i)
        ia64_gr_write(&v->s, i, UINT64_MAX, true);
}
static void pal_expect(struct vcpu *v, uint64_t index, uint64_t a, uint64_t b, uint64_t c,
                       int64_t status, uint64_t x, uint64_t y, uint64_t z)
{
    pal_args(v, index, a, b, c);
    uint64_t frame = v->s.cfm;
    CHECK(ia64_pal_dispatch(v).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.cfm == frame && ia64_gr_read(&v->s, 28) == index);
    unsigned first = index & 256 ? 33 : 29;
    CHECK(ia64_gr_read(&v->s, first) == a && ia64_gr_read(&v->s, first + 1) == b &&
          ia64_gr_read(&v->s, first + 2) == c);
    if ((int64_t)v->s.gr[8] != status || v->s.gr[9] != x || v->s.gr[10] != y || v->s.gr[11] != z)
        fprintf(
            stderr,
            "PAL model=%u index=%llu args=%llx,%llx,%llx got=%llx,%llx,%llx,%llx expected=%llx,%llx,%llx,%llx\n",
            v->cfg.cpu_model, (unsigned long long)index, (unsigned long long)a,
            (unsigned long long)b, (unsigned long long)c, (unsigned long long)v->s.gr[8],
            (unsigned long long)v->s.gr[9], (unsigned long long)v->s.gr[10],
            (unsigned long long)v->s.gr[11], (unsigned long long)status, (unsigned long long)x,
            (unsigned long long)y, (unsigned long long)z);
    CHECK((int64_t)v->s.gr[8] == status);
    CHECK(v->s.gr[9] == x && v->s.gr[10] == y && v->s.gr[11] == z);
    for (unsigned i = 8; i <= 11; ++i)
        CHECK(!ia64_nat_read(&v->s, i));
}
static uint64_t pal_word(struct vcpu *v, uint64_t ptr)
{
    uint64_t value;
    CHECK(ia64_physical_read(v, ptr, 8, &value).kind == IA64_STEP_COMPLETE);
    return value;
}
static void pal_store(struct vcpu *v, uint64_t ptr, uint64_t value)
{
    CHECK(ia64_physical_write(v, ptr, 8, value).kind == IA64_STEP_COMPLETE);
}

static void test_pal_results_model(unsigned model)
{
    struct vcpu *v = model_vcpu(model);
    uint64_t buf = ENTRY + 0x9000;
    struct {
        unsigned index;
        uint64_t a, b, c;
        int64_t status;
        uint64_t x, y, z;
    } cases[] = {
        {PAL_VERSION, 0, 0, 0, 0, PAL_VERSION_VALUE, PAL_VERSION_VALUE, 0},
        {PAL_RSE_INFO, 0, 0, 0, 0, 96, 0, 0},
        {PAL_FREQ_BASE, 0, 0, 0, 0, 100000000, 0, 0},
        {PAL_FREQ_RATIOS, 0, 0, 0, 0, UINT64_C(0x1000000001), UINT64_C(0x400000001),
         UINT64_C(0x400000001)},
        {PAL_VM_SUMMARY, 0, 0, 0, 0,
         model == IA64_CPU_MADISON ? UINT64_C(0x02043f3f080f1865) : UINT64_C(0x02041f1f080f1865),
         0x183c, 0},
        {PAL_VM_INFO, 0, 1, 0, 0, UINT64_C(0x202001), UINT64_C(0x1000), 0},
        {PAL_VM_INFO, 0, 2, 0, 0, UINT64_C(0x202001), UINT64_C(0x1000), 0},
        {PAL_VM_INFO, 1, 1, 0, 0, UINT64_C(0x500808001), IA64_PAGE_SIZE_MASK, 0},
        {PAL_VM_INFO, 1, 2, 0, 0, UINT64_C(0x500808001), IA64_PAGE_SIZE_MASK, 0},
        {PAL_VM_INFO, 2, 1, 0, -2, 0, 0, 0},
        {PAL_VM_INFO, 0, 0, 0, -2, 0, 0, 0},
        {PAL_VM_INFO, 0, 3, 0, -2, 0, 0, 0},
        {PAL_VM_PAGE_SIZE, 0, 0, 0, 0, IA64_PAGE_SIZE_MASK, IA64_PAGE_SIZE_MASK, 0},
        {PAL_PTCE_INFO, 0, 0, 0, 0, 0, UINT64_C(0x100000001), 0},
        {PAL_MEM_ATTRIB, 0, 0, 0, 0, 0x71, 0, 0},
        {PAL_CACHE_SUMMARY, 0, 0, 0, 0, 3, 5, 0},
        {PAL_CACHE_INIT, 0, 3, 1, 0, 0, 0, 0},
        {PAL_CACHE_INIT, UINT64_MAX, UINT64_MAX, UINT64_MAX, 0, 0, 0, 0},
        {PAL_CACHE_INIT, 3, 1, 0, -2, 0, 0, 0},
        {PAL_CACHE_INIT, 0, 0, 0, -2, 0, 0, 0},
        {PAL_CACHE_INIT, 0, 1, 2, -2, 0, 0, 0},
        {PAL_CACHE_LINE_INIT, ENTRY, UINT64_MAX, 0, 0, 0, 0, 0},
        {PAL_CACHE_LINE_INIT, UINT64_C(1) << 63, 0, 0, -2, 0, 0, 0},
        {PAL_CACHE_LINE_INIT, UINT64_C(1) << 50, 0, 0, -2, 0, 0, 0},
        {PAL_CACHE_LINE_INIT, ENTRY, 0, 1, -2, 0, 0, 0},
        {PAL_PREFETCH_VIS, 0, 0, 0, 1, 0, 0, 0},
        {PAL_PREFETCH_VIS, 1, 0, 0, 1, 0, 0, 0},
        {PAL_PREFETCH_VIS, 2, 0, 0, -2, 0, 0, 0},
        {PAL_DEBUG_INFO, 0, 0, 0, 0, 4, 4, 0},
        {PAL_FIXED_ADDR, 0, 0, 0, 0, 0, 0, 0},
        {PAL_LOGICAL_TO_PHYSICAL, UINT64_MAX, 0, 0, 0, UINT64_C(0x100010001), 0, 0},
        {PAL_LOGICAL_TO_PHYSICAL, 0, 0, 0, 0, UINT64_C(0x100010001), 0, 0},
        {PAL_LOGICAL_TO_PHYSICAL, 1, 0, 0, -2, 0, 0, 0},
        {PAL_REGISTER_INFO, 0, 0, 0, 0, UINT64_C(0x11117f2f00ff), 7, 0},
        {PAL_REGISTER_INFO, 1, 0, 0, 0, 0, 0, 0},
        {PAL_REGISTER_INFO, 2, 0, 0, 0, 0x3fb0107, 0x307ff, 0},
        {PAL_REGISTER_INFO, 3, 0, 0, 0, 0, 2, 0},
        {PAL_REGISTER_INFO, 4, 0, 0, -2, 0, 0, 0},
        {PAL_COPY_INFO, 0, 0, 0, 0, 4096, 4096, 0},
        {PAL_COPY_INFO, 1, 0, 0, -2, 0, 0, 0},
        {PAL_COPY_INFO, 2, 0, 0, -2, 0, 0, 0},
        /* Pal.h PAL_PLATFORM_INFO: 32-bit controller and processor counts. */
        {PAL_COPY_INFO, 0, 1, 0, 0, 4096, 4096, 0},
        {PAL_COPY_INFO, 0, UINT64_C(0x100000001), 512, 0, 4096, 4096, 0},
        {PAL_MC_CLEAR_LOG, 0, 0, 0, 0, 0, 0, 0},
        {PAL_MC_DRAIN, 0, 0, 0, 0, 0, 0, 0},
        {PAL_MC_DYNAMIC_STATE, 0, 0, 0, -1, 0, 0, 0},
        {PAL_MC_DYNAMIC_STATE, 1, buf, 0, -1, 0, 0, 0},
        {PAL_MC_DYNAMIC_STATE, 8, 0, 0, -2, 0, 0, 0},
        {PAL_MC_DYNAMIC_STATE, 1, buf + 1, 0, -2, 0, 0, 0},
        {PAL_MC_ERROR_INFO, 0, 0, 0, -6, 0, 0, 0},
        {PAL_MC_ERROR_INFO, 1, 0, 0, -6, 0, 0, 0},
        {PAL_MC_ERROR_INFO, 2, 0x100, 0, -6, 0, 0, 0},
        {PAL_MC_ERROR_INFO, 2, 0x100, 4, -6, 0, 0, 0},
        {PAL_MC_ERROR_INFO, 2, 0x100, 5, -2, 0, 0, 0},
        {PAL_MC_ERROR_INFO, 2, 0x300, 0, -2, 0, 0, 0},
        {PAL_MC_ERROR_INFO, 2, 0x101, 0, -2, 0, 0, 0},
        {PAL_MC_ERROR_INFO, 3, 0, 0, -2, 0, 0, 0},
        {PAL_MC_RESUME, 0, 0, 0, -2, 0, 0, 0},
        {PAL_MC_REGISTER_MEM, 0, 0, 0, -2, 0, 0, 0},
        {PAL_MEM_FOR_TEST, 0, 0, 0, 0, 0, 1, 0},
        {PAL_TEST_PROC, ENTRY, 4096, 1, 0, 4, 0, 0},
        {PAL_TEST_PROC, ENTRY, 4096, 0, -2, 0, 0, 0},
        {PAL_TEST_PROC, ENTRY, 4096, 0x10001, -2, 0, 0, 0},
        {PAL_TEST_PROC, UINT64_C(1) << 63, 4096, 1, -2, 0, 0, 0},
        {PAL_VM_TR_READ, model == IA64_CPU_MADISON ? 64 : 32, 0, buf, -2, 0, 0, 0},
        {PAL_VM_TR_READ, 0, 2, buf, -2, 0, 0, 0},
        {PAL_VM_TR_READ, 0, 0, buf + 1, -2, 0, 0, 0},
        {PAL_BRAND_INFO, 16, 0, 0, 0, 1600000000, 0, 0},
        {PAL_BRAND_INFO, 17, 0, 0, 0, 12 * 1024 * 1024, 0, 0},
        {PAL_BRAND_INFO, 18, 0, 0, 0, 400000000, 0, 0},
        {PAL_BRAND_INFO, 19, 0, 0, -2, 0, 0, 0},
        {PAL_BRAND_INFO, 0, 0, 0, -2, 0, 0, 0},
    };
    if (model == IA64_CPU_MADISON)
        for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(cases); ++i) {
            if (cases[i].index == PAL_VERSION && !cases[i].status)
                cases[i].x = cases[i].y = UINT64_C(0x0000057301000573);
            if (cases[i].index == PAL_CACHE_SUMMARY)
                cases[i].y = 4;
            if (cases[i].index == PAL_LOGICAL_TO_PHYSICAL) {
                cases[i].status = -1;
                cases[i].x = cases[i].y = cases[i].z = 0;
            }
            if (cases[i].index == PAL_BRAND_INFO && cases[i].a == 17)
                cases[i].x = 3145728;
        }
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(cases); ++i)
        pal_expect(v, cases[i].index, cases[i].a, cases[i].b, cases[i].c, cases[i].status,
                   cases[i].x, cases[i].y, cases[i].z);
    const unsigned reserved[] = {
        PAL_VERSION,    PAL_RSE_INFO,    PAL_VM_SUMMARY,   PAL_VM_PAGE_SIZE,     PAL_PTCE_INFO,
        PAL_FREQ_BASE,  PAL_FREQ_RATIOS, PAL_MEM_ATTRIB,   PAL_CACHE_SUMMARY,    PAL_DEBUG_INFO,
        PAL_FIXED_ADDR, PAL_MC_DRAIN,    PAL_MC_CLEAR_LOG, PAL_BUS_GET_FEATURES, PAL_HALT_LIGHT};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(reserved); ++i)
        for (unsigned arg = 0; arg < 3; ++arg)
            pal_expect(v, reserved[i], arg == 0, arg == 1, arg == 2, -2, 0, 0, 0);

    pal_expect(v, PAL_MC_EXPECTED, 1, 0, 0, 0, 0, 0, 0);
    CHECK(v->s.pal.mc_expected);
    pal_expect(v, PAL_MC_EXPECTED, 0, 0, 0, 0, 1, 0, 0);
    CHECK(!v->s.pal.mc_expected);
    pal_expect(v, PAL_MC_EXPECTED, 2, 0, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_MC_REGISTER_MEM, buf | (UINT64_C(1) << 63), 0, 0, 0, 0, 0, 0);
    CHECK(v->s.pal.mc_save_addr == (buf | (UINT64_C(1) << 63)));
    pal_expect(v, PAL_MC_REGISTER_MEM, (buf + 8) | (UINT64_C(1) << 63), 0, 0, -2, 0, 0, 0);
    CHECK(v->s.pal.mc_save_addr == (buf | (UINT64_C(1) << 63)));
    /* SDM Vol.2 PAL_MC_REGISTER_MEM: UC address, 512-byte aligned, the
     * whole 4 KB min-state area implemented; size 0/4 KB; reserved 0. */
    {
        const uint64_t uc = UINT64_C(1) << 63, top = UINT64_C(1) << 50;
        /* a = address, b = size in KB (only 4 or "not passed" = 0 match
         * the required 4 KB), c = reserved; req_size (r9) only on a size
         * mismatch. */
        const struct {
            uint64_t a, b, c;
            int64_t status;
            uint64_t req;
        } mc[] = {
            {uc | (top - 4096), 0, 0, 0, 0},
            {uc | (top - 3584), 0, 0, -2, 0},
            {uc | (top - 512), 0, 0, -2, 0},
            {uc | top, 0, 0, -2, 0},
            {uc | (UINT64_C(1) << 62), 0, 0, -2, 0},
            {top - 4096, 0, 0, -2, 0},
            {uc | buf, 4, 0, 0, 0},
            {uc | (buf + 512), 8, 0, -2, 4},
            {uc | (buf + 512), 1, 0, -2, 4},
            {uc | (buf + 512), 0, 1, -2, 0},
            {uc | (buf + 256), 8, 0, -2, 0},
        };
        uint64_t saved = v->s.pal.mc_save_addr;
        for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(mc); ++i) {
            pal_expect(v, PAL_MC_REGISTER_MEM, mc[i].a, mc[i].b, mc[i].c, mc[i].status, mc[i].req,
                       0, 0);
            if (!mc[i].status)
                saved = mc[i].a;
            CHECK(v->s.pal.mc_save_addr == saved);
        }
        pal_expect(v, PAL_MC_REGISTER_MEM, buf | uc, 0, 0, 0, 0, 0, 0);
    }
    pal_expect(v, PAL_PMI_ENTRYPOINT, buf, 0, 0, 0, 0, 0, 0);
    CHECK(v->s.pal.pmi_entry == buf);
    pal_expect(v, PAL_PMI_ENTRYPOINT, buf + 16, 0, 0, -2, 0, 0, 0);
    pal_args(v, PAL_MC_RESUME, 0, buf | (UINT64_C(1) << 63), 0);
    CHECK(ia64_pal_dispatch(v).exit == VCPU_EXIT_UNIMPLEMENTED && v->s.gr[8] == UINT64_MAX);
    pal_args(v, PAL_VM_TR_READ, 0, 0, buf);
    CHECK(ia64_pal_dispatch(v).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0 && v->s.gr[9] == 0);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(pal_word(v, buf + 8 * i) == 0);
    /* MMIO TR service shares the portal formatter, but accepts only RAM. */
    v->s.tr[1][3] = (struct ia64_tlb_entry){.pte = 0x12300661,
                                            .itir = 24u << 2,
                                            .va = 0x2000000,
                                            .rid = 0x1234,
                                            .page_shift = 24,
                                            .valid = true};
    ia64_pal_tr_select(v, 0x103);
    ia64_pal_tr_read(v, buf | (UINT64_C(1) << 63));
    ia64_pal_complete_services(v);
    v->requested_exit = 0;
    CHECK(ia64_pal_tr_status(v) == 0 && ia64_pal_tr_valid(v) == 15);
    CHECK(pal_word(v, buf) == 0x12300661 && pal_word(v, buf + 8) == (24u << 2));
    CHECK(pal_word(v, buf + 16) == 0x2000001 && pal_word(v, buf + 24) == 0x123460);
    for (unsigned i = 0; i < 5; ++i) {
        const uint64_t invalid[] = {buf + 1, ENTRY + RAM_SIZE - 8, ENTRY + 0x20000,
                                    IA64VMM_SERVICE_BASE, UINT64_C(1) << 50};
        ia64_pal_tr_read(v, invalid[i]);
        ia64_pal_complete_services(v);
        v->requested_exit = 0;
        CHECK((int64_t)ia64_pal_tr_status(v) == -2 && !ia64_pal_tr_valid(v));
        CHECK(pal_word(v, buf) == 0x12300661);
    }
    ia64_pal_tr_select(v, 0x203);
    ia64_pal_tr_read(v, buf);
    ia64_pal_complete_services(v);
    v->requested_exit = 0;
    CHECK((int64_t)ia64_pal_tr_status(v) == -2 && pal_word(v, buf) == 0x12300661);
    /* Verify all firmware-supported LIDs, including the table's last entry. */
    for (unsigned i = 0; i < IA64VMM_MAX_CPUS; ++i) {
        struct vcpu_config cfg = {.index = i, .itc_icount = true};
        struct vcpu *cpu = vcpu_create(&cfg);
        CHECK(cpu && cpu->s.cr[CR_LID] == IA64VMM_CPU_LID(i));
        vcpu_destroy(cpu);
    }
    struct vcpu_config invalid_cpu = {.index = IA64VMM_MAX_CPUS};
    CHECK(!vcpu_create(&invalid_cpu));
    invalid_cpu.index = 1;
    invalid_cpu.lid = 1u << 16;
    CHECK(!vcpu_create(&invalid_cpu));
    pal_expect(v, PAL_PLATFORM_ADDR, 0, IA64VMM_PIB_BASE, 0, 0, 0, 0, 0);
    pal_expect(v, PAL_PLATFORM_ADDR, 1, IA64VMM_LEGACY_IO_BASE, 0, 0, 0, 0, 0);
    CHECK(v->s.pal.interrupt_block_addr == IA64VMM_PIB_BASE &&
          v->s.pal.io_block_addr == IA64VMM_LEGACY_IO_BASE);
    pal_expect(v, PAL_PLATFORM_ADDR, 1, IA64VMM_LEGACY_IO_BASE + 1, 0, -1, 0, 0, 0);
    pal_expect(v, PAL_PLATFORM_ADDR, 2, 0, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_PLATFORM_ADDR, 0, UINT64_C(1) << 50, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_PLATFORM_ADDR, 0, (UINT64_C(1) << 50) - (UINT64_C(2) << 20), 0, -1, 0, 0, 0);
    pal_expect(v, PAL_PLATFORM_ADDR, 0, UINT64_C(0x80000000ff000000), 0, -1, 0, 0, 0);
    v->cfg.index = 5;
    pal_expect(v, PAL_FIXED_ADDR, 0, 0, 0, 0, 5, 0, 0);
    pal_expect(v, PAL_LOGICAL_TO_PHYSICAL, 0, 0, 0, model ? -1 : 0,
               model ? 0 : UINT64_C(0x5000100010001), 0, model ? 0 : 5);
    vcpu_destroy(v);
}

static void test_pal_results(void)
{
    test_pal_results_model(IA64_CPU_MONTECITO);
    test_pal_results_model(IA64_CPU_MADISON);
}

static void test_pal_features(void)
{
    struct vcpu *v = new_vcpu();
    pal_expect(v, PAL_BUS_GET_FEATURES, 0, 0, 0, 0, PAL_BUS_FEATURES, PAL_BUS_FEATURES,
               PAL_BUS_FEATURES);
    pal_expect(v, PAL_BUS_SET_FEATURES, UINT64_MAX, 0, 0, 0, 0, 0, 0);
    pal_expect(v, PAL_BUS_SET_FEATURES, 0, 0, 0, 0, 0, 0, 0);
    pal_expect(v, PAL_BUS_SET_FEATURES, UINT64_MAX, 1, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_BUS_GET_FEATURES, 0, 0, 0, 0, PAL_BUS_FEATURES, 0, PAL_BUS_FEATURES);
    pal_expect(v, PAL_PROC_GET_FEATURES, 0, 0, 0, 0, 0, 0, 0);
    pal_expect(v, PAL_PROC_GET_FEATURES, 0, 18, 0, 0, PAL_PROC_FEATURES, PAL_PROC_FEATURES,
               PAL_PROC_FEATURES);
    pal_expect(v, PAL_PROC_SET_FEATURES, 0, 18, 0, 0, 0, 0, 0);
    pal_expect(v, PAL_PROC_GET_FEATURES, 0, 18, 0, 0, PAL_PROC_FEATURES, 0, PAL_PROC_FEATURES);
    pal_expect(v, PAL_PROC_SET_FEATURES, UINT64_MAX, 18, 0, 0, 0, 0, 0);
    pal_expect(v, PAL_PROC_GET_FEATURES, 0, 18, 0, 0, PAL_PROC_FEATURES, PAL_PROC_FEATURES,
               PAL_PROC_FEATURES);
    for (unsigned set = 1; set <= 20; ++set) {
        int64_t status = set < 16 ? -2 : set < 18 ? 1 : set == 18 ? 0 : -8;
        uint64_t features = set == 18 ? PAL_PROC_FEATURES : 0;
        pal_expect(v, PAL_PROC_GET_FEATURES, 0, set, 0, status, features, features, features);
        pal_expect(v, PAL_PROC_SET_FEATURES, UINT64_MAX, set, 0, status, 0, 0, 0);
    }
    pal_expect(v, PAL_PROC_GET_FEATURES, 1, 18, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_PROC_SET_FEATURES, 0, 18, 1, -2, 0, 0, 0);
    pal_args(v, PAL_RSE_INFO, 0, 0, 0);
    v->s.nat[29] = true;
    CHECK(ia64_pal_dispatch(v).fault == IA64_FAULT_NAT && v->s.gr[8] == UINT64_MAX);
    pal_args(v, PAL_RSE_INFO, 0, 0, 0);
    v->s.nat[28] = true;
    CHECK(ia64_pal_dispatch(v).fault == IA64_FAULT_NAT && v->s.gr[8] == UINT64_MAX);
    pal_args(v, PAL_HALT_INFO, ENTRY + 0x9000, 0, 0);
    ia64_gr_write(&v->s, 33, ENTRY + 0x9000, true);
    CHECK(ia64_pal_dispatch(v).fault == IA64_FAULT_NAT && v->s.gr[8] == UINT64_MAX);
    /* A NaT index cannot select the stacked convention or its frame check. */
    pal_args(v, PAL_HALT_INFO, 0, 0, 0);
    v->s.cfm = 0;
    ia64_gr_write(&v->s, 28, PAL_HALT_INFO, true);
    CHECK(ia64_pal_dispatch(v).fault == IA64_FAULT_NAT && v->s.gr[8] == UINT64_MAX);
    pal_args(v, PAL_HALT_INFO, ENTRY + 0x9000, 0, 0);
    v->s.cfm = 3;
    CHECK(ia64_pal_dispatch(v).fault == IA64_FAULT_ILLEGAL && v->s.gr[8] == UINT64_MAX);
    uint64_t log_before = log_get_ring()->write_pos;
    pal_expect(v, 255, 0, 0, 0, -1, 0, 0, 0);
    CHECK(log_get_ring()->write_pos > log_before);
    vcpu_reset(v, NULL);
    pal_expect(v, PAL_BUS_GET_FEATURES, 0, 0, 0, 0, PAL_BUS_FEATURES, PAL_BUS_FEATURES,
               PAL_BUS_FEATURES);
    pal_expect(v, PAL_PROC_GET_FEATURES, 0, 18, 0, 0, PAL_PROC_FEATURES, PAL_PROC_FEATURES,
               PAL_PROC_FEATURES);
    vcpu_destroy(v);
}

static void test_pal_cache_and_buffers(void)
{
    struct vcpu *v = new_vcpu();
    uint64_t buf = ENTRY + 0x9000;
    struct {
        unsigned level, type;
        uint64_t info, size;
    } caches[] = {
        {0, 1, UINT64_C(0x1ff06060400), UINT64_C(0x310c0c00004000)},
        {0, 2, UINT64_C(0x10106060400), UINT64_C(0x310c0c00004000)},
        {1, 1, UINT64_C(0x7ff07070800), UINT64_C(0x31110c00100000)},
        {1, 2, UINT64_C(0x50107070802), UINT64_C(0x310f0c00040000)},
        {2, 2, UINT64_C(0xe0107070c03), UINT64_C(0x31140c00c00000)},
    };
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(caches); ++i) {
        pal_expect(v, PAL_CACHE_INFO, caches[i].level, caches[i].type, 0, 0, caches[i].info,
                   caches[i].size, 0);
        /* Protection method zero, 64 data bits, tag range from the profile. */
        uint64_t tag_lsb = (caches[i].size >> 40) & 255;
        pal_expect(v, PAL_CACHE_PROT_INFO, caches[i].level, caches[i].type, 0, 0,
                   64 | (((UINT64_C(1) << 30) | (tag_lsb << 8) | (UINT64_C(49) << 14)) << 32), 0,
                   0);
        pal_expect(v, PAL_CACHE_SHARED_INFO, caches[i].level, caches[i].type, 0, 0, 1, 0, 0);
        pal_expect(v, PAL_CACHE_SHARED_INFO, caches[i].level, caches[i].type, 1, -2, 0, 0, 0);
    }
    pal_expect(v, PAL_CACHE_INFO, 2, 1, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_CACHE_INFO, 3, 2, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_CACHE_INFO, 0, 0, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_CACHE_INFO, 0, 3, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_CACHE_PROT_INFO, 0, 1, 1, -2, 0, 0, 0);

    pal_expect(v, PAL_PERF_MON_INFO, buf, 0, 0, 0, 0x08123004, 0, 0);
    for (unsigned i = 0; i < 16; ++i) {
        uint64_t expected = i == 0 ? 0x3fff : i == 4 ? 0x3ffff : i == 8 || i == 12 ? 0xf0 : 0;
        CHECK(pal_word(v, buf + 8 * i) == expected);
    }
    pal_expect(v, PAL_PERF_MON_INFO, 0, 0, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_PERF_MON_INFO, buf + 1, 0, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_HALT_INFO, buf, 0, 0, 0, 0, 0, 0);
    CHECK(pal_word(v, buf) == UINT64_C(0x3000000000000000));
    CHECK(pal_word(v, buf + 8) == 0);
    for (unsigned i = 2; i < 8; ++i)
        CHECK(pal_word(v, buf + i * 8) == 0);
    pal_expect(v, PAL_HALT_INFO, buf + 1, 0, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_HALT_INFO, buf, 1, 0, -2, 0, 0, 0);
    static const char brand[] = "VMM Montecito-compatible IA-64 CPU 1.60GHz 12MB";
    pal_expect(v, PAL_BRAND_INFO, 0, buf, 0, 0, sizeof(brand) - 1, 0, 0);
    CHECK(!memcmp(ram + (buf - ENTRY), brand, sizeof(brand)));
    pal_expect(v, PAL_BRAND_INFO, 0, buf, 1, -2, 0, 0, 0);
    pal_args(v, PAL_PERF_MON_INFO, ENTRY + RAM_SIZE, 0, 0);
    CHECK(ia64_pal_dispatch(v).exit == VCPU_EXIT_BAD_ACCESS && v->s.gr[8] == UINT64_MAX);
    pal_store(v, ENTRY + RAM_SIZE - 8, 0x1234);
    pal_args(v, PAL_PERF_MON_INFO, ENTRY + RAM_SIZE - 8, 0, 0);
    CHECK(ia64_pal_dispatch(v).exit == VCPU_EXIT_BAD_ACCESS);
    CHECK(pal_word(v, ENTRY + RAM_SIZE - 8) == 0x1234);
    pal_args(v, PAL_HALT_INFO, buf, 0, 0);
    v->s.psr = IA64_PSR_DT;
    CHECK(ia64_pal_dispatch(v).fault == IA64_FAULT_NESTED_DTLB);
    v->s.psr = 0;
    pal_args(v, PAL_BRAND_INFO, 0, ENTRY + RAM_SIZE - 1, 0);
    CHECK(ia64_pal_dispatch(v).exit == VCPU_EXIT_BAD_ACCESS);

    const struct ia64_bundle *bundle;
    uint64_t nop = UINT64_C(1) << 27;
    bundle_write(ENTRY, 1, nop, nop, nop);
    CHECK(code_get_bundle(v, ENTRY, &bundle).kind == IA64_STEP_COMPLETE);
    /* Simulate an external writer bypassing the CPU/VMM store helpers. */
    ram[0] = 1;
    memset(ram + 1, 0, 15);
    CHECK(code_get_bundle(v, ENTRY, &bundle).kind == IA64_STEP_COMPLETE &&
          bundle->insn[0].opcode == IA64_OP_NOP);
    pal_expect(v, PAL_CACHE_FLUSH, 2, 0, 0, 0, 0, 0, 0);
    CHECK(code_get_bundle(v, ENTRY, &bundle).kind == IA64_STEP_COMPLETE &&
          bundle->insn[0].opcode == IA64_OP_NOP);
    pal_expect(v, PAL_CACHE_FLUSH, 1, 0, 0, 0, 0, 0, 0);
    CHECK(code_get_bundle(v, ENTRY, &bundle).kind == IA64_STEP_COMPLETE &&
          bundle->insn[0].opcode == IA64_OP_BREAK);
    for (unsigned type = 1; type <= 4; ++type)
        for (unsigned op = 0; op <= 3; ++op)
            pal_expect(v, PAL_CACHE_FLUSH, type, op, UINT64_MAX, 0, 0, 0, 0);
    pal_expect(v, PAL_CACHE_FLUSH, 0, 0, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_CACHE_FLUSH, 5, 0, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_CACHE_FLUSH, 1, 4, 0, -2, 0, 0, 0);
    vcpu_destroy(v);
}

static unsigned pal_hook_calls;
static bool pal_accept_hook(struct vcpu *v, uint64_t imm, uint64_t ip)
{
    (void)v;
    (void)imm;
    (void)ip;
    ++pal_hook_calls;
    return true;
}
static Ia64Instruction pal_break_insn(uint64_t ip)
{
    Ia64Instruction insn = instruction(IA64_OP_BREAK);
    insn.unit = IA64_UNIT_M;
    insn.slot = 0;
    insn.address = ip;
    insn.operands.system.immediate = 0x100000;
    return insn;
}
static void test_pal_portal_and_copy(void)
{
    struct vcpu *v = new_vcpu();
    uint64_t copy = ENTRY + 0x7000;
    Ia64Instruction insn = pal_break_insn(ENTRY);
    v->cfg.pal_entry = ENTRY;
    pal_args(v, PAL_HALT_LIGHT, 0, 0, 0);
    insn.qp = 1;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && !v->requested_exit &&
          v->s.gr[8] == UINT64_MAX);
    insn.qp = 0;
    engine_set_break_hook(pal_accept_hook);
    pal_hook_calls = 0;
    pal_args(v, PAL_RSE_INFO, 0, 0, 0);
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.gr[9] == 96 && !pal_hook_calls);
    insn.address = ENTRY + 16;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_BREAK && !pal_hook_calls);
    insn.address = ENTRY;
    insn.unit = IA64_UNIT_I;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_BREAK && !pal_hook_calls);
    insn.unit = IA64_UNIT_M;
    insn.slot = 1;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_BREAK);
    insn.slot = 0;
    insn.operands.system.immediate = 0x100001;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && pal_hook_calls == 1);
    engine_set_break_hook(NULL);
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_BREAK);
    insn = pal_break_insn(ENTRY);
    v->cfg.pal_entry = 0;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_BREAK);
    v->cfg.pal_entry = ENTRY;

    const struct ia64_bundle *bundle;
    uint64_t nop = UINT64_C(1) << 27;
    bundle_write(copy, 1, nop, nop, nop);
    CHECK(code_get_bundle(v, copy, &bundle).kind == IA64_STEP_COMPLETE &&
          bundle->insn[0].opcode == IA64_OP_NOP);
    pal_args(v, PAL_COPY_PAL, copy, 4096, 0);
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_BRANCH);
    CHECK(v->s.pal.copy_valid && v->s.gr[8] == 0);
    pal_expect(v, PAL_COPY_PAL, copy | (UINT64_C(1) << 63), 4096, 0, 0, 0, 0, 0);
    CHECK(v->s.pal.copy_valid && v->s.pal.copy_addr == copy);
    CHECK(pal_word(v, copy) == UINT64_C(0x000002000000000a));
    CHECK(pal_word(v, copy + 8) == UINT64_C(0x0004000000000200));
    CHECK(pal_word(v, copy + 16) == UINT64_C(0x0000000100000011));
    CHECK(pal_word(v, copy + 24) == UINT64_C(0x0080000800000200));
    CHECK(code_get_bundle(v, copy, &bundle).kind == IA64_STEP_COMPLETE);
    CHECK(bundle->insn[0].opcode == IA64_OP_BREAK &&
          bundle->insn[0].operands.system.immediate == 0x100000);
    pal_args(v, PAL_FREQ_BASE, 0, 0, 0);
    v->s.ip = copy;
    v->s.slot = 0;
    v->s.br[0] = ENTRY + 0x5000;
    CHECK(code_get_bundle(v, copy + 16, &bundle).kind == IA64_STEP_COMPLETE);
    CHECK(bundle->insn[2].opcode == IA64_OP_BR_INDIRECT);
    struct vcpu_exit copy_out = engine_run(v, 6);
    CHECK(copy_out.reason == VCPU_EXIT_BUDGET && v->s.ip == v->s.br[0] && v->s.slot == 0);
    CHECK(v->s.gr[9] == 100000000);
    pal_args(v, PAL_RSE_INFO, 0, 0, 0);
    v->s.ip = copy | (UINT64_C(1) << 63);
    v->s.slot = 0;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.gr[9] == 96);

    uint64_t old_word = pal_word(v, copy + 4096);
    pal_expect(v, PAL_COPY_PAL, copy + 4096, 4096, 1, 0, 0, 0, 0);
    CHECK(pal_word(v, copy + 4096) == old_word);
    CHECK(!ia64_pal_entry_matches(v, copy) && ia64_pal_entry_matches(v, copy + 4096));
    pal_expect(v, PAL_COPY_PAL, copy, 4095, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_COPY_PAL, copy + 8, 4096, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_COPY_PAL, copy, 4096, 2, -2, 0, 0, 0);
    pal_expect(v, PAL_COPY_PAL, UINT64_C(1) << 50, 4096, 0, -2, 0, 0, 0);
    pal_expect(v, PAL_COPY_PAL, copy, UINT64_MAX, 0, -2, 0, 0, 0);
    pal_args(v, PAL_COPY_PAL, ENTRY + RAM_SIZE, 4096, 0);
    CHECK(ia64_pal_dispatch(v).exit == VCPU_EXIT_BAD_ACCESS);
    CHECK(v->s.pal.copy_addr == copy + 4096);
    struct vcpu *other = new_vcpu();
    CHECK(!ia64_pal_entry_matches(other, copy + 4096));
    pal_expect(other, PAL_COPY_PAL, copy + 4096, 4096, 1, 0, 0, 0, 0);
    CHECK(ia64_pal_entry_matches(other, copy + 4096));
    vcpu_reset(v, NULL);
    CHECK(!v->s.pal.copy_valid && !ia64_pal_entry_matches(v, copy + 4096));
    CHECK(!v->s.pal.mc_save_addr && !v->s.pal.pmi_entry && !v->s.pal.mc_expected);
    /* Retain the original entry while reset discards every copy. */
    CHECK(ia64_pal_entry_matches(v, ENTRY));
    vcpu_destroy(other);
    vcpu_destroy(v);
}

static void test_pal_halt(void)
{
    struct vcpu *v = new_vcpu();
    vcpu_raise_external(v, 0x80);
    v->in_service[3] = UINT64_C(1) << 63;
    CHECK(vcpu_halt_wake_pending(v) && !ia64_external_pending(v));
    v->s.cr[CR_TPR] = 0x80;
    CHECK(!vcpu_halt_wake_pending(v));
    v->s.cr[CR_TPR] = 1u << 16;
    CHECK(!vcpu_halt_wake_pending(v));
    v->s.cr[CR_TPR] = 0;
    v->s.cr[CR_IRR0 + 2] = 0;
    v->in_service[3] = 0;
    uint64_t nop = UINT64_C(1) << 27, brk = UINT64_C(0x1000000000);
    bundle_write(ENTRY, 1, brk, nop, nop);
    v->cfg.pal_entry = ENTRY;
    pal_args(v, PAL_HALT_LIGHT, 0, 0, 0);
    struct vcpu_exit out = engine_run(v, 1);
    CHECK(out.reason == VCPU_EXIT_HALT && out.ip == ENTRY && out.slot == 0);
    CHECK(v->retired == 1 && v->s.ip == ENTRY && v->s.slot == 1);
    CHECK(v->s.gr[8] == 0 && !v->s.nat[8] && v->requested_exit == 0);
    CHECK(engine_run(v, 2).reason == VCPU_EXIT_BUDGET && v->retired == 3 && v->s.ip == ENTRY + 16);
    v->s.ip = ENTRY;
    v->s.slot = 0;
    pal_args(v, PAL_HALT_LIGHT, 1, 0, 0);
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && (int64_t)v->s.gr[8] == -2);
    v->s.ip = ENTRY + 16;
    v->s.slot = 0;
    bundle_write(ENTRY + 16, 1, brk, nop, nop);
    /* Outside the PAL entry, BREAK faults through the IVT with IIM = imm. */
    uint64_t iva = ENTRY + 0x8000; /* 32 KiB aligned, inside the test RAM */
    bundle_write(iva + 0x2c00, 1, nop, nop, nop);
    v->s.cr[CR_IVA] = iva;
    v->s.psr |= IA64_PSR_IC;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
    CHECK((v->s.ip & ~UINT64_C(15)) == iva + 0x2c00);
    CHECK(v->s.cr[CR_IIM] == 0x100000 && v->s.cr[CR_IIP] == ENTRY + 16);
    v->s.psr &= ~IA64_PSR_IC;

    /* HALT states 1..7 are unavailable; only HALT_LIGHT is reported. */
    for (unsigned state = 0; state < 9; ++state) {
        pal_expect(v, PAL_HALT, state, ENTRY + 0x9000, 0, -1, 0, 0, 0);
        CHECK(!v->requested_exit);
    }
    vcpu_destroy(v);
}

static void test_pal(void)
{
    test_pal_results();
    test_pal_features();
    test_pal_cache_and_buffers();
    test_pal_portal_and_copy();
    test_pal_halt();
}
