/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/profile.h"
#include "ia64/debug.h"

extern void test_model_tier1(struct vcpu *) __attribute__((weak));
extern void test_pal_event_tier1(struct vcpu *) __attribute__((weak));

static void test_model(const char *image)
{
    test_arch_pmu_registers();
    /* Literal SDM/fork encodings, independent of profile helpers. */
    static const uint64_t cpuid[2][5] = {
        {UINT64_C(0x49656e69756e6547), 0x6c65746e, 0, 0x20000704, 5},
        {UINT64_C(0x49656e69756e6547), 0x6c65746e, 0, 0x1f010504, 1}};
    static const char *const brands[] = {"VMM Montecito-compatible IA-64 CPU 1.60GHz 12MB",
                                         "VMM Madison-compatible IA-64 CPU 1.60GHz 3MB"};
    /* level, type, r9/r10 for each model; status -2 for absent L2-I. */
    static const struct {
        unsigned level, type;
        uint64_t x[2], y[2];
    } caches[] = {{0, 1, {0x01ff06060400, 0x01ff06060400}, {0x310c0c00004000, 0x310c0c00004000}},
                  {0, 2, {0x010106060400, 0x010106060400}, {0x310c0c00004000, 0x310c0c00004000}},
                  {1, 1, {0x07ff07070800, 0}, {0x31110c00100000, 0}},
                  {1, 2, {0x050107070802, 0x050107070803}, {0x310f0c00040000, 0x310f0c00040000}},
                  {2, 2, {0x0e0107070c03, 0x0e0107070c03}, {0x31140c00c00000, 0x31120c00300000}}};
    enum ia64_cpu_model parsed;
    CHECK(ia64_cpu_model_parse("montecito", 9, &parsed) && parsed == IA64_CPU_MONTECITO);
    CHECK(ia64_cpu_model_parse("madison", 7, &parsed) && parsed == IA64_CPU_MADISON);
    CHECK(!ia64_cpu_model_parse("Madison", 7, &parsed));
    CHECK(!ia64_cpu_model_parse("madison-extra", 13, &parsed));
    CHECK(!ia64_cpu_model_parse("", 0, &parsed));
    struct vcpu_config invalid = {.cpu_model = 2};
    CHECK(!vcpu_create(&invalid));
    for (unsigned model = 0; model < 2; ++model) {
        struct vcpu *v = model_vcpu(model);
        Ia64Instruction i = instruction(IA64_OP_MOV_CPUID_INDEXED);
        i.operands.system.destination = 8;
        i.operands.system.register_index = 10;
        for (unsigned n = 0; n < 5; ++n) {
            v->s.gr[10] = n;
            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
            CHECK(v->s.gr[8] == cpuid[model][n]);
        }
        for (unsigned bit = 0; bit < 64; ++bit) {
            i = instruction(IA64_OP_TF_NZ);
            i.operands.integer.immediate = bit;
            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
            CHECK(ia64_pr_read(&v->s, 6) == ((cpuid[model][4] >> bit) & 1));
        }
        i = instruction(IA64_OP_MOV_CPUID_INDEXED);
        i.operands.system.destination = 8;
        i.operands.system.register_index = 10;
        v->s.gr[10] = 5;
        v->s.gr[8] = 123;
        CHECK(ia64_execute(v, &i).fault == IA64_FAULT_RESERVED && v->s.gr[8] == 123);
        for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(caches); ++n) {
            int status = model == 1 && n == 2 ? -2 : 0;
            pal_expect(v, PAL_CACHE_INFO, caches[n].level, caches[n].type, 0, status,
                       caches[n].x[model], caches[n].y[model], 0);
            /* Protection tags encode PA MSB 49 and the model's tag LSB. */
            uint64_t tag = (caches[n].y[model] >> 40) & 255;
            uint64_t prot = ((UINT64_C(0x400c4000) | (tag << 8)) << 32) | 64;
            pal_expect(v, PAL_CACHE_PROT_INFO, caches[n].level, caches[n].type, 0, status,
                       status ? 0 : prot, 0, 0);
            pal_expect(v, PAL_CACHE_SHARED_INFO, caches[n].level, caches[n].type, 0, model ? -1 : 0,
                       model ? 0 : 1, 0, 0);
        }
        for (unsigned set = 0; set < 21; ++set) {
            int status = !set        ? 0
                         : set < 16  ? -2
                         : model     ? (set == 16 ? 0 : -8)
                         : set < 18  ? 1
                         : set == 18 ? 0
                                     : -8;
            uint64_t bits = 0;
            pal_expect(v, PAL_PROC_GET_FEATURES, 0, set, 0, status, bits, bits, bits);
            pal_expect(v, PAL_PROC_SET_FEATURES, UINT64_MAX, set, 0, status, 0, 0, 0);
        }
        pal_expect(v, PAL_BRAND_INFO, 0, ENTRY + 0x9000, 0, 0, strlen(brands[model]), 0, 0);
        CHECK(!strcmp((char *)ram + 0x9000, brands[model]));
        /* Architectural hints retain their base update on both models. */
        i = instruction(IA64_OP_HINT_M);
        i.imm_base_update = true;
        i.operands.system.register_index = 10;
        i.operands.system.immediate = 16;
        v->s.gr[10] = 123;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[10] == 139);
        i = instruction(IA64_OP_BR_IA);
        i.operands.branch.target = 1;
        v->s.br[1] = 0x1234567;
        v->s.cfm = 0;
        struct ia64_state before = v->s;
        struct ia64_result r = ia64_execute(v, &i);
        CHECK(model ? r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_UNIMPLEMENTED &&
                          !strcmp(r.detail,
                                  "IA-32 native backend not implemented (cpu_model=madison)")
                    : r.fault == IA64_FAULT_DISABLED_ISA);
        CHECK(!memcmp(&before, &v->s, sizeof(before)));
        v->s.psr |= IA64_PSR_DI;
        CHECK(ia64_execute(v, &i).fault == IA64_FAULT_DISABLED_ISA);
        v->s.ar[AR_BSPSTORE] = v->s.ar[AR_BSP] + 8;
        CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
        v->s.ar[AR_BSPSTORE] = v->s.ar[AR_BSP];
        v->s.psr = 0;
        i = instruction(IA64_OP_RFI);
        v->s.cr[CR_IPSR] = IA64_PSR_IS;
        before = v->s;
        r = ia64_execute(v, &i);
        CHECK(model ? r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_UNIMPLEMENTED &&
                          !strcmp(r.detail,
                                  "IA-32 native backend not implemented (cpu_model=madison)")
                    : r.fault == IA64_FAULT_DISABLED_ISA);
        CHECK(!memcmp(&before, &v->s, sizeof(before)));
        if (!model) {
            ia64_deliver_fault(v, &i, r);
            CHECK(v->s.ip == 0x5400 && (v->s.cr[CR_ISR] & 255) == 0x40);
        }
        v->s.psr = IA64_PSR_DB;
        v->s.dbr[0] = 0x2000;
        v->s.dbr[1] = (UINT64_C(1) << 63) | (UINT64_C(1) << 56) | 0xffffff;
        CHECK((ia64_debug_data(v, 0x100f, 8, IA64_ACCESS_READ, 0, false, false).fault ==
               IA64_FAULT_DEBUG) == (model == 1));
        CHECK(ia64_debug_data(v, 0x1008, 8, IA64_ACCESS_READ, 0, false, false).kind ==
              IA64_STEP_COMPLETE);
        vcpu_destroy(v);
    }
    /* Actual decoded bundles through every engine. No destination, base,
     * AR, memory, NaT, or privilege effect can precede the absent-op fault. */
    /* Also freeze common PAL buffer/procedure outputs on both models. */
    for (unsigned model = 0; model < 2; ++model) {
        struct vcpu *v = model_vcpu(model);
        uint64_t buf = ENTRY + 0x9000;
        pal_expect(v, PAL_BUS_GET_FEATURES, 0, 0, 0, 0, 0, 0, 0);
        pal_expect(v, PAL_BUS_SET_FEATURES, 0, 0, 0, 0, 0, 0, 0);
        pal_expect(v, PAL_BUS_GET_FEATURES, 0, 0, 0, 0, 0, 0, 0);
        pal_expect(v, PAL_CACHE_FLUSH, 3, 0, 0, 0, 0, 0, 0);
        pal_expect(v, PAL_COPY_PAL, buf, 4096, 0, 0, 0, 0, 0);
        pal_expect(v, PAL_PERF_MON_INFO, buf, 0, 0, 0, 0x08123004, 0, 0);
        static const uint64_t masks[16] = {0x3fff, 0, 0, 0, 0x3ffff, 0, 0, 0,
                                           0xf0,   0, 0, 0, 0xf0,    0, 0, 0};
        for (unsigned n = 0; n < 16; ++n)
            CHECK(pal_word(v, buf + 8 * n) == masks[n]);
        pal_expect(v, PAL_HALT_INFO, buf, 0, 0, 0, 0, 0, 0);
        static const uint64_t states[8] = {UINT64_C(0x3000000000000000), 0, 0, 0, 0, 0, 0, 0};
        for (unsigned n = 0; n < 8; ++n)
            CHECK(pal_word(v, buf + 8 * n) == states[n]);
        pal_expect(v, PAL_HALT, 1, 0, 0, -1, 0, 0, 0);
        CHECK(!v->requested_exit);
        v->requested_exit = 0;
        pal_expect(v, PAL_HALT_LIGHT, 0, 0, 0, 0, 0, 0, 0);
        CHECK(v->requested_exit);
        v->requested_exit = 0;
        /* Every unsupported procedure in the static/stacked number space. */
        for (unsigned n = 0; n < 512; ++n) {
            bool supported = (n >= 1 && n <= 32) || n == 34 || (n >= 37 && n <= 39) ||
                             (n >= 41 && n <= 43) || (n >= 256 && n <= 258) || n == 261 || n == 274;
            if (!supported)
                pal_expect(v, n, 0, 0, 0, -1, 0, 0, 0);
        }
        vcpu_destroy(v);
    }
    for (unsigned model = 0; model < 2; ++model)
        for (unsigned count = 1; count <= 4; ++count)
            for (unsigned index = 0; index < count; ++index) {
                struct vcpu_config topology = {
                    .index = index, .cpu_count = count, .cpu_model = model, .itc_icount = true};
                struct vcpu *cpu = vcpu_create(&topology);
                CHECK(cpu);
                unsigned width = model || count == 1 ? 1 : 2;
                unsigned socket = index / width, cores = count - socket * width;
                if (cores > width)
                    cores = width;
                uint64_t info = cores | (UINT64_C(1) << 16) | ((uint64_t)cores << 32) |
                                ((uint64_t)socket << 48);
                pal_expect(cpu, PAL_LOGICAL_TO_PHYSICAL, UINT64_MAX, 0, 0, model ? -1 : 0,
                           model ? 0 : info, model ? 0 : (uint64_t)(index % width) << 32,
                           model ? 0 : index);
                for (unsigned logical = 0; logical <= cores; ++logical)
                    pal_expect(cpu, PAL_LOGICAL_TO_PHYSICAL, logical, 0, 0,
                               model              ? -1
                               : logical == cores ? -2
                                                  : 0,
                               model || logical == cores ? 0 : info,
                               model || logical == cores ? 0 : (uint64_t)logical << 32,
                               model || logical == cores ? 0 : socket * width + logical);
                pal_expect(cpu, PAL_BRAND_INFO, 17, 0, 0, 0,
                           (model ? UINT64_C(3145728) : UINT64_C(12582912)) * cores, 0, 0);
                vcpu_destroy(cpu);
            }
    load_image(image);
    if (selected_engine == ENGINE_TIER1) {
        CHECK(test_model_tier1);
        struct vcpu *v = model_vcpu(IA64_CPU_MADISON);
        test_model_tier1(v);
        vcpu_destroy(v);
        CHECK(test_pal_event_tier1);
        v = model_vcpu(IA64_CPU_MONTECITO);
        test_pal_event_tier1(v);
        vcpu_destroy(v);
    }
    for (unsigned n = 0; n < 5; ++n) {
        struct vcpu *v = model_vcpu(IA64_CPU_MADISON);
        v->s.ip = ENTRY + 16 * n;
        v->s.slot = n >= 3 ? 2 : 0;
        v->s.gr[8] = 123;
        v->s.gr[9] = 456;
        v->s.gr[10] = ENTRY + 0x8000;
        v->s.ar[AR_CSD] = 789;
        v->s.ar[AR_CCV] = 987;
        /* Even NaT/address/privilege failures lose to model absence. */
        v->s.nat[10] = true;
        v->s.psr = UINT64_C(3) << 32;
        uint8_t bytes[16];
        memset(bytes, 0x5a, sizeof(bytes));
        CHECK(gm_write(ENTRY + 0x8000, bytes, 16));
        struct ia64_state before = v->s;
        if (selected_engine == ENGINE_TIER1)
            CHECK(engine_vcpu_attach(v));
        struct vcpu_exit out = engine_run(v, 32);
        CHECK(out.reason == VCPU_EXIT_BAD_ACCESS || out.reason == VCPU_EXIT_DEBUG);
        CHECK(v->fault.kind == IA64_FAULT_ILLEGAL && v->fault.ip == ENTRY + 16 * n &&
              v->fault.slot == (n >= 3 ? 2u : 0u));
        CHECK(v->retired == 0 && !memcmp(before.gr, v->s.gr, sizeof(before.gr)));
        CHECK(!memcmp(before.nat, v->s.nat, sizeof(before.nat)));
        CHECK(v->s.ar[AR_CSD] == 789 && v->s.ar[AR_CCV] == 987);
        CHECK(!memcmp(ram + 0x8000, bytes, 16));
        engine_vcpu_detach(v);
        vcpu_destroy(v);
    }
    for (unsigned n = 3; n < 5; ++n)
        for (unsigned cpl = 0; cpl <= 3; cpl += 3) {
            struct vcpu *v = model_vcpu(IA64_CPU_MONTECITO);
            v->s.ip = ENTRY + 16 * n;
            v->s.slot = 2;
            v->s.psr = (uint64_t)cpl << 32;
            struct ia64_state before = v->s;
            if (selected_engine == ENGINE_TIER1)
                CHECK(engine_vcpu_attach(v));
            engine_run(v, 1);
            CHECK(v->fault.kind == (cpl ? IA64_FAULT_PRIVILEGED : IA64_FAULT_VIRTUALIZATION));
            CHECK(v->fault.ip == ENTRY + 16 * n && v->fault.slot == 2 && !v->retired);
            CHECK(v->s.ip == (cpl ? 0x5400 : 0x6100));
            CHECK(!memcmp(before.gr, v->s.gr, sizeof(before.gr)));
            CHECK(!memcmp(before.ar, v->s.ar, sizeof(before.ar)));
            engine_vcpu_detach(v);
            vcpu_destroy(v);
        }
    /* Synthetic decoder flags exercise preparation priority even when an
     * encoding's reserved bit is classified by a future decoder revision. */
    for (unsigned n = 0; n < 3; ++n) {
        struct vcpu *v = model_vcpu(IA64_CPU_MADISON);
        const struct ia64_bundle *bundle;
        CHECK(code_get_bundle(v, ENTRY + 16 * n, &bundle).kind == IA64_STEP_COMPLETE);
        Ia64Instruction i = bundle->insn[0];
        i.reserved_field = true;
        struct ia64_state before = v->s;
        CHECK(ia64_execute(v, &i).fault == IA64_FAULT_RESERVED);
        i.placement_illegal = true;
        CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
        CHECK(!memcmp(&before, &v->s, sizeof(before)));
        vcpu_destroy(v);
    }
    for (unsigned model = 0; model < 2; ++model)
        for (unsigned n = 5; n < 7; ++n) {
            struct vcpu *v = model_vcpu(model);
            v->s.ip = ENTRY + 16 * n;
            v->s.slot = 2;
            v->s.br[1] = 0x1234567;
            v->s.cr[CR_IPSR] = IA64_PSR_IS;
            v->s.cr[CR_IIP] = 0x2345678;
            struct ia64_state before = v->s;
            if (selected_engine == ENGINE_TIER1)
                CHECK(engine_vcpu_attach(v));
            struct vcpu_exit out = engine_run(v, 1);
            if (model) {
                CHECK(out.reason == VCPU_EXIT_UNIMPLEMENTED && out.ip == ENTRY + 16 * n &&
                      out.slot == 2);
                CHECK(out.detail &&
                      !strcmp(out.detail,
                              "IA-32 native backend not implemented (cpu_model=madison)"));
                CHECK(!memcmp(&before, &v->s, sizeof(before)));
            } else {
                CHECK(v->fault.kind == IA64_FAULT_DISABLED_ISA && v->fault.ip == ENTRY + 16 * n);
                CHECK((v->s.cr[CR_ISR] & 255) == 0x40);
            }
            CHECK(!v->retired);
            engine_vcpu_detach(v);
            vcpu_destroy(v);
        }
}
