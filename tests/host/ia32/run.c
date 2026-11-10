/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/ia32/run.h"
#include "ia64/tlb.h"
#include "ia64/interrupt.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/guest_mem.h"
#include "host/amd64/vmx_ia32.h"
#include "host/amd64/ia32_svm.h"

/* Intel Itanium SDM Vol.2 Ch.9, Vol.4 JMPE. The fake transport exercises
 * exit dispatch independently of privileged VMX/SVM instructions. */
static struct vcpu v;
static uint8_t ram[16384] __attribute__((aligned(4096)));
static unsigned checks, entries, maps;
enum transport_case {
    TRANSPORT_NOP,
    TRANSPORT_DB_BEFORE,
    TRANSPORT_STEP_UNKNOWN,
    TRANSPORT_REP,
    TRANSPORT_JMP_NEXT,
    TRANSPORT_POPA,
    TRANSPORT_REFUSE,
    TRANSPORT_LOAD,
    TRANSPORT_STORE,
    TRANSPORT_PAUSE_LOAD,
    TRANSPORT_SVM_STORE,
    TRANSPORT_UNCERTAIN_LOAD,
    TRANSPORT_FAULT,
    TRANSPORT_X87_HOST
};
static enum transport_case transport_case;
static unsigned fault_backend, fault_vector, native_features;
static bool stale_fp_environment;
static bool host_after_instruction;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%u: %s\n", __LINE__, #x);                                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
struct ia32_container {
    struct ia32_native_state n;
};
static struct ia32_container container;
static bool probe(unsigned cpu, struct ia32_capabilities *c, struct ia32_setup_error *e)
{
    (void)e;
    *c = (struct ia32_capabilities){.backend = IA32_BACKEND_VMX,
                                    .host_cpu = cpu,
                                    .nested_paging = true,
                                    .nested_invalidate = true,
                                    .mtf = true,
                                    .bounded_exit = true,
                                    .legacy_fp = true,
                                    .native_features = native_features,
                                    .ept_execute_only = true};
    return true;
}
static bool create(unsigned cpu, struct ia32_container **c, struct ia32_setup_error *e)
{
    (void)cpu;
    (void)e;
    *c = &container;
    return true;
}
static bool import(struct ia32_container *c, const struct ia32_native_state *n, uint64_t dirty,
                   struct ia32_setup_error *e)
{
    (void)dirty;
    (void)e;
    c->n = *n;
    return true;
}
static struct ia32_mapping mappings[12];
static unsigned mapping_count;
static unsigned device_reads, device_writes;
static uint32_t device_value;
static bool pause_once;
static uint8_t *mapped(uint32_t address)
{
    for (unsigned i = 0; i < mapping_count; ++i)
        if (mappings[i].token.value == (address & ~4095u)) {
            hpa_t pa = mappings[i].hpa + (address & 4095);
            return pa >= 0x100000 && pa < 0x104000 ? ram + pa - 0x100000 : (uint8_t *)(uintptr_t)pa;
        }
    CHECK(false);
    return NULL;
}
static bool map(struct ia32_container *c, const struct ia32_mapping *m, struct ia32_setup_error *e)
{
    (void)c;
    (void)e;
    CHECK(m->hpa && !(m->hpa & 4095));
    CHECK(mapping_count < 12);
    mappings[mapping_count++] = *m;
    ++maps;
    return true;
}
static bool unmap(struct ia32_container *c, ia32_token_t t, uint64_t bytes,
                  struct ia32_setup_error *e)
{
    (void)c;
    (void)t;
    mapping_count = 0;
    (void)bytes;
    (void)e;
    return true;
}
static bool invalidate(struct ia32_container *c, const struct ia32_generations *g,
                       struct ia32_setup_error *e)
{
    (void)c;
    (void)g;
    (void)e;
    return true;
}
static bool reset(struct ia32_container *c, struct ia32_setup_error *e)
{
    (void)c;
    (void)e;
    return true;
}
static void destroy(struct ia32_container *c)
{
    (void)c;
}
static struct ia32_native_exit enter(struct ia32_container *c, struct ia32_native_state *n,
                                     const struct ia32_run_limit *l, struct ia32_run_usage *u)
{
    (void)c;
    CHECK(l->step);
    ++entries;
    uint32_t old = n->eip.value;
    if (stale_fp_environment)
        memset(n->fp.bytes + 6, 0, 18);
    if (transport_case == TRANSPORT_X87_HOST) {
        *u = (struct ia32_run_usage){0};
        if (host_after_instruction)
            n->eip.value += ram[old + 1] == 0xe8 ? 2 : 6;
        return (struct ia32_native_exit){.reason = IA32_EXIT_HOST_IRQ,
            .phase = host_after_instruction ? IA32_PHASE_UNKNOWN : IA32_PHASE_BEFORE,
            .position_valid = true, .eip = n->eip,
            .linear_ip = {(uint32_t)n->cs.base + n->eip.value}};
    }
    if (transport_case == TRANSPORT_FAULT) {
        *u = (struct ia32_run_usage){.instructions_valid = true};
        if (fault_vector == 19)
            n->fp.bytes[24] |= 1; /* Unmasked SSE invalid operation updates MXCSR. */
        if (fault_backend == IA32_BACKEND_VMX) {
            struct vmx_ia32_exit_fields f = {
                .reason = 0, .intr_info = (1u << 31) | (3u << 8) | fault_vector};
            if (fault_vector == 13)
                f.intr_info |= 1u << 11;
            return vmx_ia32_decode_exit(&f, n, false);
        }
        uint8_t vmcb[4096] = {0};
        uint64_t value = 0x40 + fault_vector;
        memcpy(vmcb + VMCB_EXITCODE, &value, 8);
        value = old;
        memcpy(vmcb + VMCB_RIP, &value, 8);
        return amd64_svm_ia32_decode(vmcb, false, false);
    }
    if (transport_case == TRANSPORT_LOAD || transport_case == TRANSPORT_STORE ||
        transport_case == TRANSPORT_PAUSE_LOAD || transport_case == TRANSPORT_SVM_STORE ||
        transport_case == TRANSPORT_UNCERTAIN_LOAD) {
        bool store = transport_case == TRANSPORT_STORE || transport_case == TRANSPORT_SVM_STORE;
        uint8_t *code = mapped((uint32_t)n->cs.base + old);
        CHECK(code[0] == (store ? 0xa3 : 0xa1));
        uint32_t address;
        memcpy(&address, code + 1, 4);
        address += (uint32_t)n->ds.base;
        if (transport_case == TRANSPORT_UNCERTAIN_LOAD)
            return (struct ia32_native_exit){.reason = IA32_EXIT_HOST_IRQ,
                                             .phase = IA32_PHASE_UNKNOWN};
        if (transport_case == TRANSPORT_PAUSE_LOAD && !pause_once) {
            pause_once = true;
            return (struct ia32_native_exit){.reason = IA32_EXIT_HOST_IRQ,
                                             .phase = IA32_PHASE_BEFORE,
                                             .position_valid = true,
                                             .eip = {old},
                                             .linear_ip = {(uint32_t)n->cs.base + old}};
        }
        if (store)
            memcpy(mapped(address), &n->gpr[IA32_EAX], 4);
        else
            memcpy(&n->gpr[IA32_EAX], mapped(address), 4);
        n->eip.value += 5;
        *u = (struct ia32_run_usage){.instructions = 1, .instructions_valid = true};
        if (transport_case == TRANSPORT_SVM_STORE)
            return (struct ia32_native_exit){.reason = IA32_EXIT_STEP,
                                             .phase = IA32_PHASE_UNKNOWN,
                                             .position_valid = true,
                                             .eip = n->eip,
                                             .dirty = IA32_DIRTY_IP};
        return (struct ia32_native_exit){.reason = IA32_EXIT_STEP,
                                         .phase = IA32_PHASE_COMPLETED,
                                         .position_valid = true,
                                         .eip = {old},
                                         .linear_ip = {(uint32_t)n->cs.base + old},
                                         .next_eip = n->eip,
                                         .next_eip_valid = true,
                                         .dirty = IA32_DIRTY_IP | IA32_DIRTY_GPR};
    }
    if (transport_case == TRANSPORT_REFUSE)
        return (struct ia32_native_exit){.reason = IA32_EXIT_ENTRY_FAILURE,
                                         .phase = IA32_PHASE_BEFORE,
                                         .backend = IA32_BACKEND_VMX,
                                         .detail = "test transport refused entry"};
    if (transport_case == TRANSPORT_DB_BEFORE)
        return (struct ia32_native_exit){.reason = IA32_EXIT_EXCEPTION,
                                         .phase = IA32_PHASE_BEFORE,
                                         .backend = IA32_BACKEND_VMX,
                                         .position_valid = true,
                                         .eip = {old},
                                         .linear_ip = {old},
                                         .exception = {.valid = true, .vector = 1}};
    if (transport_case == TRANSPORT_STEP_UNKNOWN)
        return (struct ia32_native_exit){
            .reason = IA32_EXIT_STEP, .phase = IA32_PHASE_UNKNOWN, .backend = IA32_BACKEND_VMX};
    if (transport_case == TRANSPORT_JMP_NEXT) {
        CHECK((ram[old] == 0xeb || ram[old] == 0x74 || ram[old] == 0xe2) && ram[old + 1] == 0);
        if (ram[old] == 0xe2)
            --n->gpr[IA32_ECX];
        n->eip.value += 2;
        *u = (struct ia32_run_usage){.instructions = 1, .instructions_valid = true};
        return (struct ia32_native_exit){.reason = IA32_EXIT_STEP,
                                         .phase = IA32_PHASE_COMPLETED,
                                         .backend = IA32_BACKEND_VMX,
                                         .position_valid = true,
                                         .eip = {old},
                                         .linear_ip = {old},
                                         .next_eip = n->eip,
                                         .next_eip_valid = true,
                                         .dirty = IA32_DIRTY_IP};
    }
    if (transport_case == TRANSPORT_POPA) {
        static const unsigned order[] = {IA32_EDI, IA32_ESI, IA32_EBP, IA32_ESP,
                                         IA32_EBX, IA32_EDX, IA32_ECX, IA32_EAX};
        CHECK(ram[old] == 0x61);
        uint32_t sp = n->gpr[IA32_ESP];
        for (unsigned i = 0; i < 8; ++i)
            if (i != 3)
                memcpy(&n->gpr[order[i]], ram + sp + 4 * i, 4);
        n->gpr[IA32_ESP] = sp + 32;
        ++n->eip.value;
        *u = (struct ia32_run_usage){.instructions = 1, .instructions_valid = true};
        return (struct ia32_native_exit){.reason = IA32_EXIT_STEP,
                                         .phase = IA32_PHASE_COMPLETED,
                                         .backend = IA32_BACKEND_VMX,
                                         .position_valid = true,
                                         .eip = {old},
                                         .linear_ip = {old},
                                         .next_eip = n->eip,
                                         .next_eip_valid = true,
                                         .dirty = IA32_DIRTY_GPR | IA32_DIRTY_IP};
    }
    if (transport_case == TRANSPORT_REP) {
        CHECK(ram[old] == 0xf3 && ram[old + 1] == 0xa4 && n->gpr[IA32_ECX]);
        ram[n->gpr[IA32_EDI]++] = ram[n->gpr[IA32_ESI]++];
        bool complete = !--n->gpr[IA32_ECX];
        if (complete)
            n->eip.value += 2;
        n->eflag = complete ? n->eflag & ~0x10000u : n->eflag | 0x10000;
        *u = (struct ia32_run_usage){.instructions = complete, .instructions_valid = true};
        return (struct ia32_native_exit){
            .reason = IA32_EXIT_STEP,
            .phase = complete ? IA32_PHASE_COMPLETED : IA32_PHASE_PARTIAL,
            .backend = IA32_BACKEND_VMX,
            .position_valid = true,
            .eip = {old},
            .linear_ip = {old},
            .next_eip = n->eip,
            .next_eip_valid = complete,
            .dirty = IA32_DIRTY_GPR | IA32_DIRTY_IP | IA32_DIRTY_FLAGS};
    }
    CHECK(ram[n->eip.value] == 0x90);
    ++n->eip.value;
    *u = (struct ia32_run_usage){.instructions = 1, .instructions_valid = true};
    return (struct ia32_native_exit){.reason = IA32_EXIT_STEP,
                                     .phase = IA32_PHASE_COMPLETED,
                                     .backend = IA32_BACKEND_VMX,
                                     .position_valid = true,
                                     .eip = {old},
                                     .linear_ip = {old},
                                     .next_eip = n->eip,
                                     .next_eip_valid = true,
                                     .dirty = IA32_DIRTY_IP};
}
static const struct ia32_backend_ops backend = {.probe = probe,
                                                .create = create,
                                                .import = import,
                                                .map = map,
                                                .unmap = unmap,
                                                .invalidate = invalidate,
                                                .reset = reset,
                                                .destroy = destroy,
                                                .enter = enter};
const struct ia32_backend_ops *hal_ia32_backend(void)
{
    return &backend;
}
bool hal_ia32_backing_hpa(const void *ptr, hpa_t *out)
{
    if ((uintptr_t)ptr < (uintptr_t)ram || (uintptr_t)ptr >= (uintptr_t)ram + sizeof(ram)) {
        *out = (uintptr_t)ptr;
        return true;
    }
    *out = 0x100000 + ((const uint8_t *)ptr - ram);
    return true;
}
static void init(const uint8_t *code, unsigned size)
{
    if (v.native)
        engine_ia32_glue()->detach(&v);
    memset(&v, 0, sizeof(v));
    memset(ram, 0, sizeof(ram));
    memcpy(ram, code, size);
    v.cfg.cpu_model = v.s.cpu_model = IA64_CPU_MADISON;
    v.cfg.engine = ENGINE_INTERP;
    v.s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC | IA64_PSR_IS;
    v.s.rr[0] = 12u << 2;
    v.s.ar[AR_CSD] = UINT64_C(0xc9bfffff00000000);
    v.s.ar[AR_SSD] = UINT64_C(0xc93fffff00000000);
    v.s.gr[24] = v.s.gr[27] = v.s.gr[28] = v.s.gr[29] = v.s.ar[AR_SSD];
    v.s.ar[AR_FCR] = UINT64_C(0x1f800000037f);
    v.s.ar[AR_FSR] = UINT64_C(0x55550000);
    v.s.ar[AR_EFLAG] = 2;
    v.s.ar[AR_CFLG] = (UINT64_C(1) << 41) | 1;
    v.s.cr[CR_IVA] = 0x8000;
    for (unsigned i = 0; i < 4; ++i) {
        ia64_tc_insert(&v, i * 4096, 0, 0x100000 + i * 4096 + 0x661, 12u << 2);
        ia64_tc_insert(&v, i * 4096, 1, 0x100000 + i * 4096 + 0x661, 12u << 2);
    }
    entries = maps = 0;
    device_reads = device_writes = 0;
    pause_once = false;
    transport_case = TRANSPORT_NOP;
    native_features = IA32_NATIVE_SSE3 | IA32_NATIVE_POPCNT;
    stale_fp_environment = false;
    host_after_instruction = false;
}
static struct vcpu_run_exit run(unsigned instructions)
{
    struct vcpu_run_budget b = {
        .ia64_instructions = 1,
        .native = {.deadline = hal_counter() + hal_counter_hz(), .instructions = instructions}};
    return engine_ia32_glue()->run(&v, &b);
}
static void intercepts(void)
{
    static const uint8_t hlt[] = {0x66, 0xf4};
    init(hlt, sizeof(hlt));
    run(1);
    CHECK(!entries && !(v.s.psr & IA64_PSR_IS));
    CHECK(v.s.ip == 0xea00 && v.s.cr[CR_IIP] == 0);
    CHECK(v.s.cr[CR_IIM] == 0xf4 && (v.s.cr[CR_ISR] & 0xffff) == 0x1004);
    CHECK(v.s.cr[CR_IPSR] & IA64_PSR_IS);
    static const uint8_t interrupt[] = {0xcd, 0x80};
    init(interrupt, sizeof(interrupt));
    v.s.gr[IA32_ESP + 8] = 0x2000;
    run(1);
    CHECK(!entries && v.s.ip == 0xeb00 && v.s.cr[CR_IIP] == 2);
    CHECK(v.s.cr[CR_ISR] == 0x800000 && v.s.gr[IA32_ESP + 8] == 0x2000);
    static const uint8_t breakpoint[] = {0xcc};
    init(breakpoint, sizeof(breakpoint));
    run(1);
    CHECK(v.s.ip == 0xe900 && v.s.cr[CR_IIP] == 1 && v.s.cr[CR_ISR] == 0x30000);
}
static void jmpe(void)
{
    static const uint8_t absolute[] = {0x0f, 0xb8, 0x3f, 0x12, 0, 0};
    init(absolute, sizeof(absolute));
    run(1);
    CHECK(!(v.s.psr & IA64_PSR_IS) && v.s.ip == 0x1230 && v.s.gr[1] == 6);
    CHECK(v.s.psr & (UINT64_C(1) << 4));
    init(absolute, sizeof(absolute));
    v.s.psr |= IA64_PSR_DI;
    run(1);
    CHECK(v.s.ip == 0xd400 && v.s.cr[CR_ISR] == 0x40 && v.s.cr[CR_IIP] == 0 && !v.s.gr[1]);
    static const uint8_t indirect[] = {0x0f, 0x00, 0x35, 0x00, 0x20, 0, 0};
    init(indirect, sizeof(indirect));
    ram[0x2000] = 0x4f;
    ram[0x2001] = 0x12;
    run(1);
    CHECK(v.s.ip == 0x1240 && v.s.gr[1] == 7);
    init(indirect, sizeof(indirect));
    v.s.ar[AR_FSR] |= 0x80;
    run(1);
    CHECK(v.s.ip == 0xe900 && v.s.cr[CR_ISR] == 0x100000 && !v.s.gr[1]);
}
static void native(void)
{
    static const uint8_t nops[] = {0x90, 0x90, 0xf4};
    init(nops, sizeof(nops));
    struct vcpu_run_exit e = run(2);
    CHECK(e.stop == VCPU_RUN_YIELD && v.s.ip == 2 && entries == 2 && maps >= 2);
    CHECK((v.s.psr & IA64_PSR_IS) && !v.retired);
    run(1);
    CHECK(v.s.ip == 0xea00 && v.s.cr[CR_IIP] == 2);
}
static void system_flags(void)
{
    static const uint8_t sti[] = {0xfb};
    init(sti, sizeof(sti));
    v.s.ar[AR_CFLG] |= 0x100;
    run(1);
    CHECK(v.s.ip == 0xea00 && v.s.cr[CR_ISR] == 0x24000 && v.s.cr[CR_IIP] == 1);
    CHECK(v.s.cr[CR_IIM] == 2 && (v.s.ar[AR_EFLAG] & 0x200));
    static const uint8_t popf[] = {0x9d};
    init(popf, sizeof(popf));
    v.s.gr[12] = 0x2000;
    ram[0x2000] = 2;
    ram[0x2001] = 1;
    run(1);
    CHECK(v.s.ip == 0xea00 && v.s.cr[CR_ISR] == 0x28000 && v.s.gr[12] == 0x2004);
    static const uint8_t movss[] = {0x8e, 0xd0};
    init(movss, sizeof(movss));
    v.s.gr[8] = 8;
    v.s.gr[31] = UINT64_C(0x0800001700003000);
    uint64_t desc = UINT64_C(0x00cf93000000ffff);
    memcpy(ram + 0x3008, &desc, 8);
    run(1);
    CHECK(v.s.ip == 0xea00 && v.s.cr[CR_ISR] == 0x2c000 && v.s.cr[CR_IIP] == 2);
}
static void gate(void)
{
    static const uint8_t call[] = {0x9a, 0, 0, 0, 0, 8, 0};
    init(call, sizeof(call));
    v.s.gr[31] = UINT64_C(0x0800001700003000);
    v.s.gr[12] = 0x2000;
    uint64_t desc = UINT64_C(0x00008c0000101234);
    memcpy(ram + 0x3008, &desc, 8);
    run(1);
    CHECK(v.s.ip == 0xea00 && v.s.cr[CR_ISR] == 0x10000 && v.s.cr[CR_IIP] == 7);
    CHECK(v.s.cr[CR_IFA] == 8 && v.s.cr[CR_IIM] == desc && v.s.gr[12] == 0x2000);
}
static void assisted_operand_debug(void)
{
    /* Vol.2 7.1, 9.1 and 9.2: operand breakpoints trap after committed
     * effects and combine with SystemFlag and JMPE transition events. */
    static const uint8_t pushf[] = {0x9c};
    init(pushf, sizeof(pushf));
    v.s.gr[12] = 0x2004;
    v.s.psr |= IA64_PSR_DB;
    v.s.dbr[0] = 0x2000;
    v.s.dbr[1] = UINT64_C(0x41ffffffffffffff);
    struct vcpu_run_exit out = run(1);
    CHECK(!entries && out.native_usage.instructions == 1 && v.s.gr[12] == 0x2000);
    CHECK(v.s.cr[CR_ISR] == 0x10010 && v.s.cr[CR_IIP] == 1);
    CHECK(ram[0x2000] == 2 && ram[0x2001] == 0 && (v.s.cr[CR_IPSR] & IA64_PSR_IS));
    static const uint8_t popf[] = {0x9d};
    init(popf, sizeof(popf));
    v.s.gr[12] = 0x2000;
    v.s.ar[AR_CFLG] |= 0x100;
    v.s.psr |= IA64_PSR_DB;
    v.s.dbr[0] = 0x2000;
    v.s.dbr[1] = UINT64_C(0x81ffffffffffffff);
    ram[0x2000] = 2;
    ram[0x2001] = 2;
    out = run(1);
    CHECK(!entries && out.native_usage.instructions == 1 && v.s.gr[12] == 0x2004);
    CHECK(v.s.cr[CR_ISR] == 0x28010 && v.s.cr[CR_IIP] == 1 && v.s.cr[CR_IIM] == 2);
    CHECK(v.s.ar[AR_EFLAG] & 0x200);
    static const uint8_t jmpe_memory[] = {0x0f, 0x00, 0x35, 0x00, 0x20, 0, 0};
    init(jmpe_memory, sizeof(jmpe_memory));
    v.s.psr |= IA64_PSR_DB;
    v.s.dbr[0] = 0x2000;
    v.s.dbr[1] = UINT64_C(0x81ffffffffffffff);
    ram[0x2000] = 0x3f;
    ram[0x2001] = 0x12;
    out = run(1);
    CHECK(!entries && out.native_usage.instructions == 1 && v.s.gr[1] == 7);
    CHECK(v.s.cr[CR_ISR] == 0x10010 && v.s.cr[CR_IIP] == 0x1230);
    CHECK(v.s.ip == 0xe900 && !(v.s.cr[CR_IPSR] & IA64_PSR_IS));
    static const uint8_t movss_memory[] = {0x8e, 0x15, 0x00, 0x20, 0, 0};
    init(movss_memory, sizeof(movss_memory));
    v.s.psr |= IA64_PSR_DB;
    v.s.ar[AR_EFLAG] |= 0x100;
    v.s.dbr[0] = 0x2000;
    v.s.dbr[1] = UINT64_C(0x81ffffffffffffff);
    v.s.gr[31] = UINT64_C(0x0800001700003000);
    uint64_t descriptor = UINT64_C(0x00cf93000000ffff);
    memcpy(ram + 0x3008, &descriptor, sizeof(descriptor));
    ram[0x2000] = 8;
    out = run(1);
    CHECK(!entries && out.native_usage.instructions == 1 && v.s.cr[CR_IIP] == 6);
    CHECK(v.s.cr[CR_ISR] == 0x2c018 && v.s.cr[CR_IIM] == 0x102);
    CHECK(((v.s.gr[17] >> 16) & 0xffff) == 8);
}
static void malformed_and_transport(void)
{
    static const uint8_t bad_lock[] = {0x64, 0xf0, 0x90};
    init(bad_lock, sizeof(bad_lock));
    struct vcpu_run_exit out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_IIP] == 0);
    CHECK(v.s.cr[CR_IIM] == 0x90 && v.s.cr[CR_ISR] == 0x224e);
    uint8_t overlong[15];
    memset(overlong, 0x66, sizeof(overlong));
    init(overlong, sizeof(overlong));
    out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_ISR] == 0xd0000);
    static const uint8_t nop[] = {0x90};
    init(nop, sizeof(nop));
    transport_case = TRANSPORT_STEP_UNKNOWN;
    v.s.psr |= IA64_PSR_ID;
    out = run(1);
    CHECK(out.stop == VCPU_RUN_EXIT && out.reason == VCPU_EXIT_UNIMPLEMENTED);
    CHECK(!out.native_usage.instructions && v.s.ip == 0 && (v.s.psr & IA64_PSR_ID));
    init(nop, sizeof(nop));
    transport_case = TRANSPORT_DB_BEFORE;
    v.s.psr |= IA64_PSR_ID;
    out = run(1);
    CHECK(entries == 1 && !out.native_usage.instructions && v.s.cr[CR_IIP] == 0);
    CHECK(v.s.cr[CR_ISR] == UINT64_C(0x100010000));
    CHECK(v.s.cr[CR_IPSR] & IA64_PSR_ID);
}
static void repetitions(void)
{
    static const uint8_t movs[] = {0xf3, 0xa4};
    init(movs, sizeof(movs));
    transport_case = TRANSPORT_REP;
    v.s.gr[24] = v.s.ar[AR_SSD];
    v.s.gr[9] = 2;
    v.s.gr[14] = 0x2000;
    v.s.gr[15] = 0x2100;
    ram[0x2000] = 0x41;
    ram[0x2001] = 0x42;
    v.s.psr |= IA64_PSR_ID;
    struct vcpu_run_budget budget = {
        .ia64_instructions = 1,
        .native = {.deadline = hal_counter() + hal_counter_hz(), .step = true}};
    struct vcpu_run_exit out = engine_ia32_glue()->run(&v, &budget);
    CHECK(out.stop == VCPU_RUN_YIELD && !out.native_usage.instructions && entries == 1);
    CHECK(v.s.ip == 0 && v.s.gr[9] == 1 && v.s.gr[14] == 0x2001 && v.s.gr[15] == 0x2101);
    CHECK(ram[0x2100] == 0x41 && ram[0x2101] == 0 && (v.s.psr & IA64_PSR_ID));
    out = engine_ia32_glue()->run(&v, &budget);
    CHECK(out.native_usage.instructions == 1 && v.s.ip == 2 && v.s.gr[9] == 0);
    CHECK(ram[0x2101] == 0x42 && !(v.s.psr & IA64_PSR_ID) && !(v.s.ar[AR_EFLAG] & 0x10000));
    init(movs, sizeof(movs));
    transport_case = TRANSPORT_REP;
    v.s.gr[24] = v.s.ar[AR_SSD];
    v.s.gr[9] = 2;
    v.s.gr[14] = 0x2000;
    v.s.gr[15] = 0x2100;
    v.s.ar[AR_EFLAG] |= 0x100;
    v.s.psr |= IA64_PSR_ID;
    out = run(1);
    CHECK(entries == 1 && !out.native_usage.instructions && v.s.cr[CR_IIP] == 0);
    CHECK(v.s.cr[CR_ISR] == 0x10008 && v.s.gr[9] == 1 && (v.s.cr[CR_IPSR] & IA64_PSR_ID));
    static const uint8_t ins[] = {0xf3, 0x6c};
    init(ins, sizeof(ins));
    v.s.gr[24] = v.s.ar[AR_SSD];
    v.s.gr[9] = 2;
    v.s.gr[10] = 2;
    v.s.gr[15] = 0x2000;
    v.s.ar[0] = 0x4000000;
    v.s.ar[AR_EFLAG] |= 0x100;
    v.s.psr |= IA64_PSR_ID;
    ram[0x3002] = 0x5a;
    ia64_tc_insert(&v, 0x4000000, 1, 0x103671, 12u << 2);
    out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_IIP] == 0);
    CHECK(v.s.cr[CR_ISR] == 0x10008 && v.s.gr[9] == 1 && v.s.gr[15] == 0x2001);
    CHECK(ram[0x2000] == 0x5a && (v.s.cr[CR_IPSR] & IA64_PSR_ID));
    CHECK(v.s.ar[AR_EFLAG] & 0x10000);
}
static void segment_side_effects(void)
{
    static const uint8_t movds[] = {0x8e, 0xd8};
    init(movds, sizeof(movds));
    v.s.gr[8] = 8;
    v.s.gr[31] = UINT64_C(0x0800001700003000);
    uint64_t desc = UINT64_C(0x00cf92000000ffff);
    memcpy(ram + 0x3008, &desc, sizeof(desc));
    struct vcpu_run_exit out = run(1);
    CHECK(out.stop == VCPU_RUN_YIELD && out.native_usage.instructions == 1 && !entries);
    CHECK(v.s.ip == 2 && ram[0x300d] == 0x93);
    static const uint8_t lss[] = {0x0f, 0xb2, 0x05, 0x04, 0x20, 0, 0};
    init(lss, sizeof(lss));
    v.s.gr[31] = UINT64_C(0x0800001700003000);
    desc = UINT64_C(0x00cf93000000ffff);
    memcpy(ram + 0x3008, &desc, sizeof(desc));
    ram[0x2004] = 0x34;
    ram[0x2005] = 0x12;
    ram[0x2008] = 8;
    v.s.psr |= UINT64_C(1) << 3;
    out = run(1);
    CHECK(!entries && out.native_usage.instructions == 1 && v.s.ip == 7);
    CHECK((v.s.psr & IA64_PSR_IS) && v.s.gr[8] == 0x1234);
    static const uint8_t retf[] = {0xcb};
    init(retf, sizeof(retf));
    v.s.gr[31] = UINT64_C(0x0800001700003000);
    v.s.gr[12] = 0x2004;
    desc = UINT64_C(0x00cf9b000000ffff);
    memcpy(ram + 0x3008, &desc, sizeof(desc));
    ram[0x2005] = 1;
    ram[0x2008] = 8;
    v.s.psr |= UINT64_C(1) << 3;
    out = run(1);
    CHECK(!entries && out.native_usage.instructions == 1 && v.s.ip == 0x100);
    CHECK((v.s.psr & IA64_PSR_IS) && v.s.gr[12] == 0x200c);
    init(movds, sizeof(movds));
    v.s.gr[8] = 8;
    v.s.gr[31] = UINT64_C(0x0800001700003000);
    desc = UINT64_C(0x00cf92000000ffff);
    memcpy(ram + 0x3008, &desc, sizeof(desc));
    ia64_tc_insert(&v, 0x3000, 1, 0x103621, 12u << 2);
    out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_IIP] == 0);
    CHECK(ram[0x300d] == 0x92 && !(v.s.psr & IA64_PSR_IS));
}
static void branch_completion(void)
{
    static const uint8_t next[] = {0xeb, 0};
    init(next, sizeof(next));
    transport_case = TRANSPORT_JMP_NEXT;
    v.s.psr |= IA64_PSR_TB;
    struct vcpu_run_exit out = run(1);
    CHECK(entries == 1 && out.native_usage.instructions == 1 && v.s.cr[CR_IIP] == 2);
    CHECK(v.s.cr[CR_ISR] == 0x10004);
    static const uint8_t jz[] = {0x74, 0};
    init(jz, sizeof(jz));
    transport_case = TRANSPORT_JMP_NEXT;
    v.s.psr |= IA64_PSR_TB;
    v.s.ar[AR_EFLAG] |= 0x40;
    out = run(1);
    CHECK(entries == 1 && out.native_usage.instructions == 1 && v.s.cr[CR_ISR] == 0x10004);
    init(jz, sizeof(jz));
    transport_case = TRANSPORT_JMP_NEXT;
    v.s.psr |= IA64_PSR_TB;
    out = run(1);
    CHECK(entries == 1 && out.native_usage.instructions == 1 && v.s.ip == 2);
    CHECK(v.s.psr & IA64_PSR_IS);
    static const uint8_t loop[] = {0xe2, 0};
    init(loop, sizeof(loop));
    transport_case = TRANSPORT_JMP_NEXT;
    v.s.psr |= IA64_PSR_TB;
    v.s.gr[9] = 2;
    out = run(1);
    CHECK(entries == 1 && out.native_usage.instructions == 1 && v.s.cr[CR_ISR] == 0x10004);
    CHECK(v.s.gr[9] == 1);
}
static void stack_accesses(void)
{
    static const uint8_t pusha[] = {0x60};
    init(pusha, sizeof(pusha));
    v.s.gr[12] = 0x2001;
    v.s.psr |= UINT64_C(1) << 3;
    struct vcpu_run_exit out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_ISR] == 0x110000);
    CHECK(v.s.gr[12] == 0x2001 && v.s.cr[CR_IIP] == 0);
    static const uint8_t popa[] = {0x61};
    init(popa, sizeof(popa));
    transport_case = TRANSPORT_POPA;
    v.s.gr[12] = 0x2000;
    v.s.psr |= IA64_PSR_DB;
    v.s.dbr[0] = 0x200c;
    v.s.dbr[1] = UINT64_C(0x81ffffffffffffff);
    uint32_t eax = 0x12345678;
    memcpy(ram + 0x201c, &eax, sizeof(eax));
    out = run(1);
    CHECK(entries == 1 && out.native_usage.instructions == 1 && v.s.ip == 1);
    CHECK((v.s.psr & IA64_PSR_IS) && v.s.gr[12] == 0x2020 && v.s.gr[8] == 0x12345678);
    static const uint8_t locked[] = {0xf0, 0x01, 0x00};
    init(locked, sizeof(locked));
    v.s.gr[8] = 0x2000;
    v.s.cr[CR_DCR] = 4;
    ia64_tc_insert(&v, 0x2000, 1, 0x102671, 12u << 2);
    out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_IIP] == 0);
    CHECK(v.s.cr[CR_ISR] == UINT64_C(0x600040000) && v.s.cr[CR_IFA] == 0x2000);
}
static void user_mode(void)
{
    v.s.psr |= UINT64_C(3) << 32;
    v.s.ar[AR_CSD] |= UINT64_C(0x60) << 52;
    v.s.ar[AR_SSD] |= UINT64_C(0x60) << 52;
    v.s.gr[24] = v.s.gr[27] = v.s.gr[28] = v.s.gr[29] = v.s.ar[AR_SSD];
    v.s.gr[17] = 0x13000b;
    ia64_tc_insert(&v, 0, 0, 0x1007e1, 12u << 2);
    ia64_tc_insert(&v, 0, 1, 0x1007e1, 12u << 2);
}
static void mode_and_fetch(void)
{
    static const uint8_t missing_data[] = {0xa1, 0x00, 0x40, 0, 0};
    init(missing_data, sizeof(missing_data));
    struct vcpu_run_exit out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_IIP] == 0);
    CHECK((v.s.cr[CR_ISR] & (UINT64_C(1) << 40)) && !(v.s.cr[CR_ISR] & (UINT64_C(1) << 35)));
    static const uint8_t movds[] = {0x8e, 0xd8};
    init(movds, sizeof(movds));
    v.s.ar[AR_CFLG] &= ~UINT64_C(1);
    v.s.gr[8] = 8;
    out = run(1);
    CHECK(!entries && out.native_usage.instructions == 1);
    CHECK(v.s.ip == 2 && (v.s.psr & IA64_PSR_IS) && (uint16_t)v.s.gr[16] == 8);
    CHECK((uint32_t)v.s.gr[27] == 0x80);
    init(movds, sizeof(movds));
    user_mode();
    v.s.gr[8] = 0x0b;
    v.s.gr[31] = UINT64_C(0x0800001700003000);
    uint64_t user_data = UINT64_C(0x00cff2000000ffff);
    memcpy(ram + 0x3008, &user_data, sizeof(user_data));
    out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_IIP] == 0);
    CHECK((v.s.cr[CR_ISR] & (UINT64_C(1) << 40)) && !(v.s.cr[CR_ISR] & (UINT64_C(1) << 35)));
    CHECK(ram[0x300d] == 0xf2 && !(v.s.psr & IA64_PSR_IS));
    static const uint8_t cli[] = {0xfa};
    init(cli, sizeof(cli));
    user_mode();
    v.s.ar[AR_CFLG] |= UINT64_C(1) << 33;
    v.s.ar[AR_EFLAG] |= 0x80200;
    out = run(1);
    CHECK(!entries && out.native_usage.instructions == 1 && v.s.ip == 1);
    CHECK((v.s.ar[AR_EFLAG] & 0x80200) == 0x200);
    static const uint8_t hlt[] = {0xf4};
    init(hlt, sizeof(hlt));
    user_mode();
    v.s.ar[AR_CFLG] |= UINT64_C(1) << 33;
    v.s.ar[AR_EFLAG] |= 0x180000;
    out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_ISR] == 0xd0000);
    CHECK(v.s.cr[CR_IIP] == 0 && ((v.s.cr[CR_IPSR] >> 32) & 3) == 3);
    init(hlt, sizeof(hlt));
    user_mode();
    v.s.ar[AR_CFLG] |= UINT64_C(1) << 33;
    v.s.ar[AR_EFLAG] |= 0x100000;
    out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_ISR] == 6);
    init(hlt, sizeof(hlt));
    user_mode();
    v.s.ar[AR_CFLG] |= UINT64_C(1) << 32;
    v.s.ar[AR_EFLAG] |= 0x1a0000;
    out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_ISR] == 0xd0000);
}
/* Vol.4 LAR/LSL/VERR/VERW: invisible selectors clear ZF without #GP. */
static void descriptor_queries(void)
{
    static const uint8_t lar[] = {0x0f, 0x02, 0xc1};
    for (unsigned type = 0; type < 32; ++type) {
        init(lar, sizeof(lar));
        v.s.gr[8] = 0x12345678;
        v.s.gr[9] = 8;
        v.s.gr[31] = UINT64_C(0x0800001700003000);
        uint64_t raw = UINT64_C(0x00c0000000001234) | (uint64_t)type << 40;
        memcpy(ram + 0x3008, &raw, 8);
        struct vcpu_run_exit out = run(1);
        bool valid = type >= 16 || type == 1 || type == 2 || type == 3 || type == 4 || type == 5 ||
                     type == 9 || type == 11 || type == 12;
        CHECK(!entries && out.native_usage.instructions == 1 && v.s.ip == 3);
        CHECK(!!(v.s.ar[AR_EFLAG] & 0x40) == valid);
        CHECK((uint32_t)v.s.gr[8] == (valid ? (uint32_t)(raw >> 32) & 0xf0ff00 : 0x12345678));
    }
    static const uint8_t lsl[] = {0x0f, 0x03, 0xc1};
    init(lsl, sizeof(lsl));
    v.s.gr[9] = 8;
    v.s.gr[31] = UINT64_C(0x0800001700003000);
    uint64_t raw = UINT64_C(0x00c0920000001234);
    memcpy(ram + 0x3008, &raw, 8);
    run(1);
    CHECK((v.s.ar[AR_EFLAG] & 0x40) && v.s.gr[8] == 0x1234fff);
    static const uint8_t verr[] = {0x0f, 0x00, 0xe1};
    init(verr, sizeof(verr));
    v.s.gr[9] = 0;
    v.s.ar[AR_EFLAG] |= 0x40;
    run(1);
    CHECK(v.s.ip == 3 && !(v.s.ar[AR_EFLAG] & 0x40));
    init(verr, sizeof(verr));
    v.s.ar[AR_CFLG] &= ~UINT64_C(1);
    run(1);
    CHECK(v.s.ip == 0xea00 && v.s.cr[CR_IIP] == 0);
}
static void virtual_flags(void)
{
    static const uint8_t sti[] = {0xfb};
    init(sti, sizeof(sti));
    user_mode();
    v.s.ar[AR_CFLG] |= UINT64_C(1) << 33;
    run(1);
    CHECK(v.s.ip == 1 && (v.s.ar[AR_EFLAG] & 0x80200) == 0x80000);
    init(sti, sizeof(sti));
    user_mode();
    v.s.ar[AR_CFLG] |= UINT64_C(1) << 33;
    v.s.ar[AR_EFLAG] |= 0x100000;
    run(1);
    CHECK(v.s.cr[CR_ISR] == 0xd0000 && v.s.cr[CR_IIP] == 0);
    static const uint8_t pushf[] = {0x9c};
    init(pushf, sizeof(pushf));
    user_mode();
    ia64_tc_insert(&v, 0x1000, 1, 0x1017e1, 12u << 2);
    v.s.gr[12] = 0x2000;
    v.s.ar[AR_CFLG] |= UINT64_C(1) << 32;
    v.s.ar[AR_EFLAG] |= 0xa0000;
    v.s.ar[AR_CSD] &= ~(UINT64_C(1) << 62);
    v.s.ar[AR_SSD] &= ~(UINT64_C(1) << 62);
    run(1);
    uint16_t pushed;
    memcpy(&pushed, ram + 0x1ffe, 2);
    CHECK(v.s.ip == 1 && v.s.gr[12] == 0x1ffe && (pushed & 0x3200) == 0x3200);
    static const uint8_t popf[] = {0x9d};
    init(popf, sizeof(popf));
    user_mode();
    ia64_tc_insert(&v, 0x2000, 1, 0x1027e1, 12u << 2);
    v.s.gr[12] = 0x2000;
    v.s.ar[AR_CFLG] |= UINT64_C(1) << 32;
    v.s.ar[AR_EFLAG] |= 0x20000;
    v.s.ar[AR_CSD] &= ~(UINT64_C(1) << 62);
    v.s.ar[AR_SSD] &= ~(UINT64_C(1) << 62);
    pushed = 0x240;
    memcpy(ram + 0x2000, &pushed, 2);
    run(1);
    CHECK(v.s.ip == 1 && v.s.gr[12] == 0x2002 && (v.s.ar[AR_EFLAG] & 0x80240) == 0x80040);
}
static void selector_reads(void)
{
    static const uint8_t movss[] = {0x8c, 0xd0};
    init(movss, sizeof(movss));
    v.s.gr[17] = UINT64_C(0x12340000);
    run(1);
    CHECK(!entries && v.s.ip == 2 && v.s.gr[8] == 0x1234);
    static const uint8_t pushcs[] = {0x0e};
    init(pushcs, sizeof(pushcs));
    v.s.gr[17] = 0x1234;
    v.s.gr[12] = 0x2000;
    run(1);
    uint32_t pushed;
    memcpy(&pushed, ram + 0x1ffc, 4);
    CHECK(!entries && v.s.ip == 1 && v.s.gr[12] == 0x1ffc && pushed == 0x1234);
}
static void outer_return(void)
{
    static const uint8_t ret[] = {0xca, 0x08, 0};
    init(ret, sizeof(ret));
    v.s.gr[31] = UINT64_C(0x0800001700003000);
    v.s.gr[12] = 0x2000;
    uint64_t cs = UINT64_C(0x00cffa000000ffff), ss = UINT64_C(0x00cff2000000ffff);
    memcpy(ram + 0x3008, &cs, 8);
    memcpy(ram + 0x3010, &ss, 8);
    uint32_t frame[] = {0x100, 0x0b, 0xaaaaaaaa, 0xbbbbbbbb, 0x2800, 0x13};
    memcpy(ram + 0x2000, frame, sizeof(frame));
    struct vcpu_run_exit out = run(1);
    CHECK(!entries && out.native_usage.instructions == 1 && v.s.ip == 0x100);
    CHECK(((v.s.psr >> 32) & 3) == 3 && v.s.gr[12] == 0x2808);
    CHECK((v.s.gr[17] & 0xffffffff) == 0x13000b);
    CHECK((ram[0x300d] & 1) && (ram[0x3015] & 1));
    CHECK(!(v.s.gr[24] & (UINT64_C(1) << 59)) && !(v.s.gr[27] & (UINT64_C(1) << 59)));
    init(ret, sizeof(ret));
    v.s.gr[31] = UINT64_C(0x0800001700003000);
    v.s.gr[12] = 0x2000;
    memcpy(ram + 0x3008, &cs, 8);
    ss &= ~(UINT64_C(1) << 47);
    memcpy(ram + 0x3010, &ss, 8);
    memcpy(ram + 0x2000, frame, sizeof(frame));
    out = run(1);
    CHECK(!out.native_usage.instructions && v.s.cr[CR_IIP] == 0 && v.s.cr[CR_ISR] == 0xc0010);
    CHECK(v.s.gr[12] == 0x2000 && !(ram[0x300d] & 1));
    static const uint8_t jump[] = {0xea, 0x20, 0, 0, 0, 0x80, 0};
    init(jump, sizeof(jump));
    v.s.ar[AR_CFLG] &= ~UINT64_C(1);
    out = run(1);
    CHECK(out.native_usage.instructions == 1 && v.s.ip == 0x820 && (v.s.gr[17] & 0xffff) == 0x80);
}
static uint64_t device_read(void *opaque, struct vcpu *cpu, uint64_t offset, unsigned size)
{
    (void)opaque;
    (void)cpu;
    CHECK(offset == 0 && size == 4);
    ++device_reads;
    return device_value++;
}
static void device_write(void *opaque, struct vcpu *cpu, uint64_t offset, uint64_t value,
                         unsigned size)
{
    (void)opaque;
    (void)cpu;
    CHECK(offset == 0 && size == 4);
    ++device_writes;
    device_value = value;
}
static void native_memory(void)
{
    static const struct gm_mmio_ops ops = {.read = device_read, .write = device_write};
    struct gm_region region = {.base = 0x200000, .size = 4096, .kind = GM_EMULATED, .ops = &ops};
    CHECK(gm_add_region(&region));
    static const uint8_t load[] = {0xa1, 0x00, 0x20, 0, 0};
    init(load, sizeof(load));
    ia64_tc_insert(&v, 0x2000, 1, 0x200661, 12u << 2);
    device_value = 0x12345678;
    transport_case = TRANSPORT_LOAD;
    struct vcpu_run_exit out = run(1);
    CHECK(out.native_usage.instructions == 1 && v.s.ip == 5 && v.s.gr[8] == 0x12345678);
    CHECK(device_reads == 1 && !device_writes);
    init(load, sizeof(load));
    ia64_tc_insert(&v, 0x2000, 1, 0x200661, 12u << 2);
    device_value = 0x12345678;
    transport_case = TRANSPORT_PAUSE_LOAD;
    out = run(1);
    CHECK(!out.native_usage.instructions && v.s.ip == 0 && device_reads == 1);
    out = run(1);
    CHECK(out.native_usage.instructions == 1 && v.s.ip == 5 && v.s.gr[8] == 0x12345678);
    CHECK(device_reads == 1 && !device_writes);
    init(load, sizeof(load));
    ia64_tc_insert(&v, 0x2000, 1, 0x200661, 12u << 2);
    transport_case = TRANSPORT_UNCERTAIN_LOAD;
    out = run(1);
    CHECK(out.stop == VCPU_RUN_EXIT && out.reason == VCPU_EXIT_UNIMPLEMENTED && device_reads == 1);
    CHECK(!out.native_usage.instructions_valid);
    out = run(1);
    CHECK(out.stop == VCPU_RUN_EXIT && out.reason == VCPU_EXIT_UNIMPLEMENTED && device_reads == 1);
    static const uint8_t store[] = {0xa3, 0x00, 0x20, 0, 0};
    init(store, sizeof(store));
    ia64_tc_insert(&v, 0x2000, 1, 0x200661, 12u << 2);
    v.s.gr[8] = 0x76543210;
    transport_case = TRANSPORT_STORE;
    out = run(1);
    CHECK(out.native_usage.instructions == 1 && v.s.ip == 5 && device_value == 0x76543210);
    CHECK(!device_reads && device_writes == 1);
    init(store, sizeof(store));
    ia64_tc_insert(&v, 0x2000, 1, 0x200661, 12u << 2);
    v.s.gr[8] = 0x12345678;
    transport_case = TRANSPORT_SVM_STORE;
    out = run(1);
    CHECK(out.native_usage.instructions == 1 && v.s.ip == 5 && device_value == 0x12345678);
    CHECK(!device_reads && device_writes == 1);
    static const uint8_t add[] = {0xf0, 0x83, 0x05, 0, 0x20, 0, 0, 1};
    init(add, sizeof(add));
    ia64_tc_insert(&v, 0x2000, 1, 0x200661, 12u << 2);
    device_value = 10;
    out = run(1);
    CHECK(!entries && out.native_usage.instructions == 1 && v.s.ip == sizeof(add));
    CHECK(device_reads == 1 && device_writes == 1 && device_value == 11);
    /* Same linear address, distinct instruction and data translations. */
    static const uint8_t separate[] = {0xa1, 0, 0, 0, 0};
    init(separate, sizeof(separate));
    ia64_tc_insert(&v, 0, 1, 0x102661, 12u << 2);
    uint32_t value = 0x44332211;
    memcpy(ram + 0x2000, &value, 4);
    transport_case = TRANSPORT_LOAD;
    out = run(1);
    CHECK(out.native_usage.instructions == 1 && v.s.ip == 5 && v.s.gr[8] == value);
    CHECK((uint32_t)v.s.ar[AR_CSD] == 0 && (uint32_t)v.s.gr[27] == 0);
}
static void x87_alignment(void)
{
    static const uint8_t fld8[] = {0xdb, 0x2d, 0x08, 0x20, 0, 0};
    init(fld8, sizeof(fld8));
    v.s.psr |= UINT64_C(1) << 3;
    struct vcpu_run_exit out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_ISR] == 0x110000);
    CHECK(v.s.cr[CR_IFA] == 0x2008 && v.s.cr[CR_IIP] == 0);
    static const uint8_t fld4[] = {0xdb, 0x2d, 0x04, 0x20, 0, 0};
    init(fld4, sizeof(fld4));
    user_mode();
    ia64_tc_insert(&v, 0x2000, 1, 0x1027e1, 12u << 2);
    v.s.ar[AR_EFLAG] |= 0x40000;
    v.s.ar[AR_CFLG] |= 0x40000;
    out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_ISR] == 0x110000);
    CHECK(v.s.cr[CR_IFA] == 0x2004 && v.s.cr[CR_IIP] == 0);
    init(fld8, sizeof(fld8));
    user_mode();
    ia64_tc_insert(&v, 0x2000, 1, 0x1027e1, 12u << 2);
    v.s.ar[AR_EFLAG] |= 0x40000;
    v.s.ar[AR_CFLG] |= 0x40000;
    transport_case = TRANSPORT_REFUSE;
    out = run(1);
    CHECK(entries == 1 && out.stop == VCPU_RUN_EXIT && out.reason == VCPU_EXIT_UNIMPLEMENTED);
    CHECK((v.s.psr & IA64_PSR_IS) && v.s.ip == 0 && !out.native_usage.instructions);
}
/* Faults preserve exception status, but do not replay a consumed MMIO read. */
static void native_faults(void)
{
    static const uint8_t div[] = {0xf7, 0x35, 0, 0x20, 0, 0};
    static const uint8_t divss[] = {0xf3, 0x0f, 0x5e, 0x05, 0, 0x20, 0, 0};
    static const uint8_t load[] = {0xa1, 0, 0x20, 0, 0};
    for (unsigned backend_id = IA32_BACKEND_SVM; backend_id <= IA32_BACKEND_VMX; ++backend_id)
        for (unsigned fault = 0; fault < 3; ++fault) {
            const uint8_t *code = fault == 0 ? div : fault == 1 ? divss : load;
            unsigned length = fault == 0 ? sizeof(div) : fault == 1 ? sizeof(divss) : sizeof(load);
            init(code, length);
            ia64_tc_insert(&v, 0x2000, 1, 0x200661, 12u << 2);
            device_value = 0;
            v.s.ar[AR_CFLG] |= UINT64_C(1) << 42;
            v.s.ar[AR_FCR] &= ~(UINT64_C(1) << 39);
            v.s.ar[AR_FIR] = UINT64_C(0x123456712345678);
            v.s.ar[AR_FDR] = UINT64_C(0x789abcdef012);
            stale_fp_environment = true;
            transport_case = TRANSPORT_FAULT;
            fault_backend = backend_id;
            fault_vector = fault == 0 ? 0 : fault == 1 ? 19 : 13;
            struct vcpu_run_exit out = run(1);
            CHECK(out.stop == VCPU_RUN_YIELD && !out.native_usage.instructions);
            CHECK(entries == 1 && device_reads == 1 && !device_writes);
            CHECK(!(v.s.psr & IA64_PSR_IS) && v.s.ip == 0xe900 && v.s.cr[CR_IIP] == 0);
            CHECK(v.s.cr[CR_ISR] == (uint64_t)fault_vector << 16);
            if (fault == 1)
                CHECK(v.s.ar[AR_FSR] & (UINT64_C(1) << 32));
            CHECK(v.s.ar[AR_FIR] == UINT64_C(0x123456712345678) &&
                  v.s.ar[AR_FDR] == UINT64_C(0x789abcdef012));
            CHECK(out.native_exit.phase == IA32_PHASE_BEFORE);
        }
    static const uint8_t reserved[] = {0xf2, 0x0f, 0x52, 0x05, 0, 0x20, 0, 0};
    init(reserved, sizeof(reserved));
    ia64_tc_insert(&v, 0x2000, 1, 0x200661, 12u << 2);
    run(1);
    CHECK(!entries && !device_reads && !device_writes && v.s.ip == 0xea00);
    CHECK(v.s.cr[CR_IIP] == 0);
    static const uint8_t optional[][8] = {
        {0xf2, 0x0f, 0x12, 0x05, 0, 0x20, 0, 0}, /* MOVDDUP */
        {0xf3, 0x0f, 0xb8, 0x05, 0, 0x20, 0, 0}, /* POPCNT */
        {0xdb, 0x0d, 0, 0x20, 0, 0},             /* FISTTP */
    };
    for (unsigned op = 0; op < 3; ++op) {
        init(optional[op], op == 2 ? 6 : 8);
        ia64_tc_insert(&v, 0x2000, 1, 0x200661, 12u << 2);
        native_features = 0;
        run(1);
        CHECK(!entries && !device_reads && !device_writes && v.s.ip == 0xea00);
    }
    /* Integer instructions leave the last non-control x87 environment intact. */
    static const uint8_t nop[] = {0x90};
    init(nop, sizeof(nop));
    v.s.ar[AR_FIR] = UINT64_C(0x123456712345678);
    v.s.ar[AR_FDR] = UINT64_C(0x789abcdef012);
    stale_fp_environment = true;
    struct vcpu_run_exit out = run(1);
    CHECK(out.native_usage.instructions == 1 && v.s.ip == 1);
    CHECK(v.s.ar[AR_FIR] == UINT64_C(0x123456712345678) &&
          v.s.ar[AR_FDR] == UINT64_C(0x789abcdef012));
}
/* Native host exits do not discard the last non-control x87 environment. */
static void fp_host_boundaries(void)
{
    const uint8_t code[][6] = {{0xd9, 0xe8}, {0xd9, 0x1d, 0, 0x20, 0, 0}};
    for (unsigned memory = 0; memory < 2; ++memory)
        for (unsigned after = 0; after < 2; ++after) {
            init(code[memory], memory ? 6 : 2);
            v.s.ar[AR_FIR] = UINT64_C(0x123456712345678);
            v.s.ar[AR_FDR] = UINT64_C(0x789abcdef012);
            v.s.gr[17] = 0x1234;
            v.s.gr[16] = 0x5678;
            stale_fp_environment = true;
            host_after_instruction = after;
            transport_case = TRANSPORT_X87_HOST;
            struct vcpu_run_exit out = run(1);
            CHECK(out.stop == VCPU_RUN_YIELD && out.yield == VCPU_YIELD_KICK);
            CHECK(v.s.ip == (after ? memory ? 6u : 2u : 0u));
            uint64_t fir = after ? ((uint64_t)(memory ? 0x11d : 0x1e8) << 48) |
                                      (UINT64_C(0x1234) << 32)
                                : UINT64_C(0x123456712345678);
            uint64_t fdr = after && memory ? UINT64_C(0x567800002000)
                                          : UINT64_C(0x789abcdef012);
            CHECK(v.s.ar[AR_FIR] == fir && v.s.ar[AR_FDR] == fdr);
        }
}
static void segment_integrity(void)
{
    static const uint8_t cpuid[] = {0x0f, 0xa2};
    init(cpuid, sizeof(cpuid));
    v.s.ar[AR_CSD] = UINT64_C(0xc93fffff00000000); /* Data CS is legal. */
    struct vcpu_run_exit out = run(1);
    CHECK(out.native_usage.instructions == 1 && v.s.ip == 2);
    for (unsigned bad = 0; bad < 3; ++bad) {
        init(cpuid, sizeof(cpuid));
        if (bad == 0)
            v.s.ar[AR_CSD] &= ~(UINT64_C(1) << 52);
        else {
            v.s.ar[AR_CFLG] &= ~UINT64_C(1);
            if (bad == 1) {
                v.s.psr |= UINT64_C(1) << 32;
                ia64_tc_insert(&v, 0, 0, 0x1007e1, 12u << 2);
            } else
                v.s.ar[AR_EFLAG] |= 0x20000;
        }
        out = run(1);
        CHECK(!out.native_usage.instructions && v.s.cr[CR_ISR] == 0xd0000);
        CHECK(v.s.cr[CR_IIP] == 0);
    }
    static const uint8_t pushf[] = {0x9c};
    init(pushf, sizeof(pushf));
    v.s.ar[AR_SSD] |= UINT64_C(3) << 57;
    v.s.gr[12] = 0x2000;
    memset(ram + 0x1ffc, 0x5a, 4);
    out = run(1);
    CHECK(!out.native_usage.instructions && v.s.cr[CR_ISR] == 0xc0000);
    CHECK(v.s.gr[12] == 0x2000 && ram[0x1ffc] == 0x5a && ram[0x1fff] == 0x5a);
    static const uint8_t movds[] = {0x8e, 0xd8};
    init(movds, sizeof(movds));
    v.s.gr[31] = UINT64_C(0x0000001700003000); /* GDTD.P=0. */
    v.s.gr[8] = 8;
    uint64_t desc = UINT64_C(0x00cf92000000ffff);
    memcpy(ram + 0x3008, &desc, 8);
    out = run(1);
    CHECK(!out.native_usage.instructions && v.s.cr[CR_ISR] == 0xd0008);
    CHECK((uint16_t)v.s.gr[16] == 0 && ram[0x300d] == 0x92);
    /* Vol.1 Table 6-2: cached expand-down limits also apply in real mode. */
    for (unsigned large = 0; large < 2; ++large)
        for (unsigned below = 0; below < 2; ++below) {
            uint8_t load[] = {0xa1, 0, below ? 0x10 : 0x20, 0, 0};
            init(load, sizeof(load));
            v.s.ar[AR_CFLG] &= ~UINT64_C(1);
            v.s.gr[27] = ((UINT64_C(0x97) | (large ? 0x400 : 0)) << 52) |
                         (UINT64_C(0x1fff) << 32);
            uint32_t value = 0x87654321;
            memcpy(ram + 0x2000, &value, 4);
            transport_case = TRANSPORT_LOAD;
            out = run(1);
            if (below)
                CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_ISR] == 0xd0000);
            else
                CHECK(entries == 1 && out.native_usage.instructions == 1 && (uint32_t)v.s.gr[8] == value);
        }
    for (unsigned below = 0; below < 2; ++below) {
        init(pushf, sizeof(pushf));
        v.s.ar[AR_CFLG] &= ~UINT64_C(1);
        v.s.ar[AR_SSD] = (UINT64_C(0x497) << 52) | (UINT64_C(0x1fff) << 32);
        v.s.gr[12] = below ? 0x2000 : 0x2004;
        out = run(1);
        if (below)
            CHECK(!out.native_usage.instructions && v.s.cr[CR_ISR] == 0xc0000 &&
                  v.s.gr[12] == 0x2000 && ram[0x1ffc] == 0);
        else
            CHECK(out.native_usage.instructions == 1 && v.s.gr[12] == 0x2000 && ram[0x2000] == 2);
    }
}
static void invalid_task_gates(void)
{
    static const uint8_t call[] = {0x9a, 0, 0, 0, 0, 8, 0};
    init(call, sizeof(call));
    v.s.gr[31] = UINT64_C(0x0800001700003000);
    v.s.gr[12] = 0x2000;
    uint64_t busy = UINT64_C(0x00008b0000101234);
    memcpy(ram + 0x3008, &busy, sizeof(busy));
    struct vcpu_run_exit out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_ISR] == 0xd0008);
    CHECK(v.s.cr[CR_IIP] == 0 && v.s.gr[12] == 0x2000);
    static const uint8_t ldt_call[] = {0x9a, 0, 0, 0, 0, 12, 0};
    init(ldt_call, sizeof(ldt_call));
    v.s.gr[30] = UINT64_C(0x0820001700003000);
    v.s.gr[17] |= UINT64_C(0x10) << 32;
    v.s.gr[12] = 0x2000;
    uint64_t available = UINT64_C(0x0000890000101234);
    memcpy(ram + 0x3008, &available, sizeof(available));
    out = run(1);
    CHECK(!entries && !out.native_usage.instructions && v.s.cr[CR_ISR] == 0xd000c);
    CHECK(v.s.cr[CR_IIP] == 0 && v.s.gr[12] == 0x2000);
}
static void entry_and_budgets(void)
{
    static const uint8_t nop[] = {0x90};
    init(nop, sizeof(nop));
    v.s.psr &= ~IA64_PSR_IS;
    v.s.psr |= IA64_PSR_ID;
    CHECK(ia32_enter(&v, IA32_ENTRY_BRANCH, 0).kind == IA64_STEP_BRANCH);
    CHECK((v.s.psr & IA64_PSR_IS) && !(v.s.psr & IA64_PSR_ID));
    struct vcpu_run_exit out = run(1);
    CHECK(out.native_usage.instructions == 1 && entries == 1 && !v.retired);
    init(nop, sizeof(nop));
    v.s.psr &= ~IA64_PSR_IS;
    v.s.cr[CR_IPSR] = v.s.psr | IA64_PSR_IS | IA64_PSR_ID;
    v.s.cr[CR_IIP] = 0;
    CHECK(ia32_enter(&v, IA32_ENTRY_RFI, 0).kind == IA64_STEP_BRANCH);
    CHECK((v.s.psr & IA64_PSR_IS) && (v.s.psr & IA64_PSR_ID));
    out = run(1);
    CHECK(out.native_usage.instructions == 1 && !(v.s.psr & IA64_PSR_ID));
    init(nop, sizeof(nop));
    struct vcpu_run_budget budget = {.native = {.deadline = hal_counter() + hal_counter_hz()}};
    out = engine_run_bounded(&v, &budget);
    CHECK(out.stop == VCPU_RUN_YIELD && out.yield == VCPU_YIELD_IA64_BUDGET);
    CHECK(!entries && !out.native_usage.instructions && !out.ia64_retired && v.s.ip == 0);
    budget.ia64_instructions = 1;
    budget.native.deadline = hal_counter();
    out = engine_run_bounded(&v, &budget);
    CHECK(out.stop == VCPU_RUN_YIELD && out.yield == VCPU_YIELD_NATIVE_DEADLINE && !entries);
    budget.native.deadline = UINT64_MAX;
    out = engine_run_bounded(&v, &budget);
    CHECK(out.stop == VCPU_RUN_EXIT && out.reason == VCPU_EXIT_UNIMPLEMENTED && !entries);
}
/* Itanium SDM Vol.2 Table 5-6, 3.3.5.2/4 and Disabled FP-Register vector. */
static void interruption_rules(void)
{
    static const uint8_t nop[] = {0x90};
    for (unsigned fault = 0; fault < 4; ++fault) {
        init(nop, sizeof(nop));
        v.s.ip = 3;
        ram[3] = 0x90;
        v.s.psr |= IA64_PSR_DB;
        v.s.ibr[0] = 3;
        v.s.ibr[1] = (UINT64_C(1) << 63) | (UINT64_C(1) << 56) |
                     UINT64_C(0x00ffffffffffffff);
        if (fault == 0)
            ia64_tc_insert(&v, 0, 0, 0x100641, 12u << 2); /* A=0 */
        else if (fault == 1)
            v.s.ar[AR_CSD] = UINT64_C(0x09b0000000000000); /* limit=0 */
        else if (fault == 2)
            ia64_tc_insert(&v, 0, 0, 0x100061, 12u << 2); /* no execute */
        else {
            v.s.ip = v.s.ibr[0] = 0x4003; /* no instruction translation */
        }
        uint64_t ip = v.s.ip;
        struct vcpu_run_exit out = run(1);
        CHECK(!entries && !out.native_usage.instructions && v.s.ip == 0xe900);
        CHECK(v.s.cr[CR_ISR] == UINT64_C(0x100010000) && v.s.cr[CR_IIP] == ip);
    }
    static const uint8_t fp[][3] = {{0x90}, {0xd9, 0xd0}, {0x0f, 0xef, 0xc0},
                                   {0x0f, 0x58, 0xc0}};
    static const unsigned lengths[] = {1, 2, 3, 3};
    for (unsigned op = 0; op < 4; ++op)
        for (unsigned disabled = 0; disabled < 4; ++disabled) {
            init(fp[op], lengths[op]);
            v.s.psr |= (uint64_t)disabled << 18;
            transport_case = TRANSPORT_REFUSE;
            struct vcpu_run_exit out = run(1);
            unsigned code = (disabled & 2) ? 2 : op && (disabled & 1) ? 1 : 0;
            if (code) {
                CHECK(!entries && !out.native_usage.instructions && v.s.ip == 0xd500);
                CHECK(v.s.cr[CR_ISR] == code && v.s.cr[CR_IIP] == 0);
            } else
                CHECK(entries == 1 && out.stop == VCPU_RUN_EXIT && (v.s.psr & IA64_PSR_IS));
        }
    init(nop, sizeof(nop));
    v.s.ip = 3;
    ram[3] = 0x90;
    ia64_tc_insert(&v, 0, 0, 0x100641, 12u << 2);
    run(1);
    CHECK(v.s.ip == 0xa400 && v.s.cr[CR_IIP] == 3 && v.s.cr[CR_IFA] == 0);
    /* A second-page fetch fault reports that page's bundle, not the first IP. */
    init(nop, sizeof(nop));
    v.s.ip = 4095;
    ram[4095] = 0x0f;
    ia64_tc_insert(&v, 4096, 0, 0x101641, 12u << 2);
    run(1);
    CHECK(v.s.ip == 0xa400 && v.s.cr[CR_IIP] == 4095 && v.s.cr[CR_IFA] == 4096);
    static const uint8_t load[] = {0xa1, 0, 0x40, 0, 0};
    init(load, sizeof(load));
    ia64_tc_insert(&v, 0, 0, (UINT64_C(1) << 52) | 0x100661, 12u << 2);
    run(1);
    CHECK(v.s.cr[CR_ISR] == UINT64_C(0x10400000000) && v.s.cr[CR_IFA] == 0x4000);
    static const enum ia64_fault_kind faults[] = {IA64_FAULT_DTLB, IA64_FAULT_KEY_MISS,
        IA64_FAULT_ACCESS_RIGHTS, IA64_FAULT_ACCESS_BIT, IA64_FAULT_DIRTY_BIT};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        init(nop, sizeof(nop));
        ia64_tc_insert(&v, 0, 0, (UINT64_C(1) << 52) | 0x100661, 12u << 2);
        Ia64Instruction insn = {.opcode = IA64_OP_NOP};
        struct ia64_result r = ia64_fault(faults[i], 0x123);
        r.translation_fault = true;
        r.fault_isr = UINT64_C(1) << 34;
        ia64_deliver_fault(&v, &insn, r);
        CHECK(!(v.s.cr[CR_ISR] & (UINT64_C(1) << 43)) && v.s.cr[CR_IFA] == 0x123);
    }
}
int main(void)
{
    struct gm_region r = {.base = 0x100000, .size = sizeof(ram), .kind = GM_RAM, .hva = ram};
    CHECK(gm_add_region(&r));
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    interruption_rules();
    intercepts();
    jmpe();
    native();
    system_flags();
    gate();
    assisted_operand_debug();
    malformed_and_transport();
    repetitions();
    segment_side_effects();
    branch_completion();
    stack_accesses();
    mode_and_fetch();
    descriptor_queries();
    virtual_flags();
    selector_reads();
    outer_return();
    native_memory();
    native_faults();
    fp_host_boundaries();
    segment_integrity();
    x87_alignment();
    invalid_task_gates();
    entry_and_budgets();
    if (v.native)
        engine_ia32_glue()->detach(&v);
    printf("ia32 run: %u checks\n", checks);
    return 0;
}
