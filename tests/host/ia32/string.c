/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Itanium SDM Rev.2.3 Vol.4 MOVS/STOS/LODS/CMPS/SCAS/REP,
 * Vol.2 Table 10-3 RF and IA-32 data-side references. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/ia32/string.h"
#include "ia64/tlb.h"
#include "ia64vmm/guest_mem.h"

static struct vcpu v;
static struct ia32_native_state n;
static struct ia32_interruption record;
static unsigned checks, reads, writes, events;
static struct {
    bool write;
    unsigned size;
    uint64_t offset, value;
} log_access[16];
static uint8_t ram[16384] __attribute__((aligned(4096)));
static uint8_t instructions[16384] __attribute__((aligned(4096)));
static uint8_t device[16384];
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint64_t read_device(void *unused, struct vcpu *cpu, uint64_t off, unsigned size)
{
    (void)unused;
    CHECK(cpu == &v && events < 16 && off + size <= sizeof(device));
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= (uint64_t)device[off + i] << (8 * i);
    log_access[events].write = false;
    log_access[events].size = size;
    log_access[events].offset = off;
    log_access[events++].value = value;
    ++reads;
    return value;
}
static void write_device(void *unused, struct vcpu *cpu, uint64_t off, uint64_t value,
                         unsigned size)
{
    (void)unused;
    CHECK(cpu == &v && events < 16 && off + size <= sizeof(device));
    log_access[events].write = true;
    log_access[events].size = size;
    log_access[events].offset = off;
    log_access[events++].value = value;
    for (unsigned i = 0; i < size; ++i)
        device[off + i] = value >> (8 * i);
    ++writes;
}
static void init(void)
{
    memset(&v, 0, sizeof(v));
    memset(&n, 0, sizeof(n));
    memset(ram, 0, sizeof(ram));
    memset(instructions, 0xcc, sizeof(instructions));
    memset(device, 0, sizeof(device));
    v.cfg.cpu_model = v.s.cpu_model = IA64_CPU_MADISON;
    v.s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC | IA64_PSR_IS;
    v.s.rr[0] = 12u << 2;
    n.cs = (struct ia32_segment){.limit = UINT32_MAX, .attrib = 0xc9b};
    n.es = n.ss = n.ds = n.fs = n.gs = (struct ia32_segment){.limit = UINT32_MAX, .attrib = 0xc93};
    n.cflg = 1;
    n.eflag = 0x202;
    n.gpr[IA32_ESI] = 0x100;
    n.gpr[IA32_EDI] = 0x200;
    n.gpr[IA32_EAX] = 0x87654321;
    for (unsigned i = 0; i < 4; ++i) {
        ia64_tc_insert(&v, i * 4096, 1, 0x100000 + i * 4096 + 0x7e1, 12u << 2);
        ia64_tc_insert(&v, i * 4096, 0, 0x200000 + i * 4096 + 0x7e1, 12u << 2);
    }
    ia64_tc_insert(&v, 0x4000, 1, 0x3007f1, 12u << 2);
    ia64_tc_insert(&v, 0x5000, 1, 0x3017f1, 12u << 2);
    reads = writes = events = 0;
}
static struct ia64_result execute(const uint8_t *bytes, unsigned size)
{
    struct ia32_decoded decoded;
    CHECK(ia32_decode(bytes, size, &n, &decoded) == IA32_DECODE_OK);
    CHECK(decoded.flags & IA32_INSN_STRING);
    record = (struct ia32_interruption){0};
    return ia32_string(&v, &n, &decoded, &record);
}
#define EXEC(...) execute((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))
static void basic(void)
{
    init();
    ram[0x100] = 0x9a;
    CHECK(EXEC(0xa4).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(ram[0x200] == 0x9a && n.gpr[IA32_ESI] == 0x101 && n.gpr[IA32_EDI] == 0x201);
    CHECK(n.eip.value == 1 && n.eflag == 0x202 && n.gpr[IA32_ECX] == 0);
    init();
    n.eflag |= 0x400;
    n.gpr[IA32_ECX] = 0x12345678;
    CHECK(EXEC(0xab).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(!memcmp(ram + 0x200, (uint8_t[]){0x21, 0x43, 0x65, 0x87}, 4));
    CHECK(n.gpr[IA32_ESI] == 0x100 && n.gpr[IA32_EDI] == 0x1fc && n.gpr[IA32_ECX] == 0x12345678);
    init();
    ram[0x100] = 0x11;
    ram[0x101] = 0x22;
    CHECK(EXEC(0x66, 0xad).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.gpr[IA32_EAX] == 0x87652211 && n.gpr[IA32_ESI] == 0x102 && n.gpr[IA32_EDI] == 0x200);
    init();
    n.fs.base = 0x1000;
    ram[0x1100] = 0x56;
    CHECK(EXEC(0x64, 0xa4).kind == IA64_STEP_COMPLETE && ram[0x200] == 0x56);
    init();
    n.ds.limit = 0;
    n.es.limit = 0;
    n.gpr[IA32_ECX] = 0;
    n.eflag |= 0x10000;
    CHECK(EXEC(0xf3, 0xa5).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.eip.value == 2 && !(n.eflag & 0x10000) && !events);
    CHECK(n.gpr[IA32_ESI] == 0x100 && n.gpr[IA32_EDI] == 0x200);
}
static void repeats(void)
{
    init();
    ram[0x100] = 0x11;
    ram[0x101] = 0x22;
    n.gpr[IA32_ECX] = 2;
    CHECK(EXEC(0xf3, 0xa4).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.eip.value == 0 && n.gpr[IA32_ECX] == 1 && (n.eflag & 0x10000));
    CHECK(ram[0x200] == 0x11 && ram[0x201] == 0);
    CHECK(EXEC(0xf3, 0xa4).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.eip.value == 2 && n.gpr[IA32_ECX] == 0 && !(n.eflag & 0x10000));
    CHECK(ram[0x201] == 0x22);
    init();
    ia64_tc_insert(&v, 0xf000, 1, 0x1037e1, 12u << 2);
    ram[0x3fff] = 0xab;
    ram[0] = 0xcd;
    n.gpr[IA32_ESI] = 0x1234ffff;
    n.gpr[IA32_EDI] = 0x56780010;
    n.gpr[IA32_ECX] = 0xabcd0002;
    CHECK(EXEC(0x67, 0xf3, 0xa4).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.gpr[IA32_ESI] == 0x12340000 && n.gpr[IA32_EDI] == 0x56780011 &&
          n.gpr[IA32_ECX] == 0xabcd0001);
    CHECK(EXEC(0x67, 0xf3, 0xa4).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.gpr[IA32_ESI] == 0x12340001 && n.gpr[IA32_EDI] == 0x56780012 &&
          n.gpr[IA32_ECX] == 0xabcd0000);
    CHECK(ram[0x10] == 0xab && ram[0x11] == 0xcd && n.eip.value == 3);
    init();
    n.eflag |= 0x400;
    n.gpr[IA32_EDI] = 0x56780000;
    CHECK(EXEC(0x67, 0xaa).kind == IA64_STEP_COMPLETE && n.gpr[IA32_EDI] == 0x5678ffff);
    init();
    ram[0x100] = ram[0x200] = 1;
    ram[0x101] = 3;
    ram[0x201] = 4;
    n.gpr[IA32_ECX] = 3;
    CHECK(EXEC(0xf3, 0xa6).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.eip.value == 0 && n.gpr[IA32_ECX] == 2 && (n.eflag & 0x40));
    CHECK(EXEC(0xf3, 0xa6).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.eip.value == 2 && n.gpr[IA32_ECX] == 1 && !(n.eflag & (0x40 | 0x10000)));
    init();
    n.gpr[IA32_EAX] = 7;
    ram[0x200] = 2;
    ram[0x201] = 7;
    n.gpr[IA32_ECX] = 3;
    CHECK(EXEC(0xf2, 0xae).kind == IA64_STEP_COMPLETE && n.eip.value == 0 && !(n.eflag & 0x40));
    CHECK(EXEC(0xf2, 0xae).kind == IA64_STEP_COMPLETE && n.eip.value == 2 && (n.eflag & 0x40));
    CHECK(n.gpr[IA32_ECX] == 1 && n.gpr[IA32_EDI] == 0x202);
}
static uint32_t expected_sub(unsigned a, unsigned b)
{
    unsigned result = (a - b) & 255, bits = 0;
    for (unsigned i = 0; i < 8; ++i)
        bits += (result >> i) & 1;
    int signed_a = a >= 128 ? (int)a - 256 : (int)a;
    int signed_b = b >= 128 ? (int)b - 256 : (int)b;
    int difference = signed_a - signed_b;
    return (a < b ? 1 : 0) | (!(bits & 1) ? 4 : 0) | ((a & 15) < (b & 15) ? 16 : 0) |
           (!result ? 64 : 0) | (result >= 128 ? 128 : 0) |
           (difference < -128 || difference > 127 ? 2048 : 0);
}
static void flags(void)
{
    init();
    for (unsigned a = 0; a < 256; ++a)
        for (unsigned b = 0; b < 256; ++b) {
            ram[0x100] = a;
            ram[0x200] = b;
            n.gpr[IA32_ESI] = 0x100;
            n.gpr[IA32_EDI] = 0x200;
            n.eip.value = 0;
            n.eflag = 0x10602;
            CHECK(EXEC(0xa6).kind == IA64_STEP_COMPLETE && !record.valid);
            CHECK(n.eflag == ((0x10602u & ~(0x8d5u | 0x10000u)) | expected_sub(a, b)));
        }
    init();
    n.gpr[IA32_EAX] = 0x80000000;
    memset(ram + 0x200, 0, 4);
    ram[0x200] = 1;
    CHECK(EXEC(0xaf).kind == IA64_STEP_COMPLETE && (n.eflag & 0x8d5) == 0x814);
    init();
    n.gpr[IA32_EAX] = 0x8000;
    ram[0x200] = 1;
    CHECK(EXEC(0x66, 0xaf).kind == IA64_STEP_COMPLETE && (n.eflag & 0x8d5) == 0x814);
}
static void devices_and_faults(void)
{
    init();
    n.gpr[IA32_ESI] = 0x4000;
    device[0] = 0x7a;
    CHECK(EXEC(0xa4).kind == IA64_STEP_COMPLETE && ram[0x200] == 0x7a);
    CHECK(reads == 1 && writes == 0 && log_access[0].size == 1);
    init();
    n.gpr[IA32_EDI] = 0x4000;
    ram[0x100] = 0x5b;
    CHECK(EXEC(0xa4).kind == IA64_STEP_COMPLETE && device[0] == 0x5b);
    CHECK(reads == 0 && writes == 1);
    init();
    n.gpr[IA32_ESI] = 0x4000;
    n.gpr[IA32_EDI] = 0x5000;
    memcpy(device, (uint8_t[]){0x78, 0x56, 0x34, 0x12}, 4);
    CHECK(EXEC(0xa5).kind == IA64_STEP_COMPLETE && reads == 1 && writes == 1);
    CHECK(!log_access[0].write && log_access[1].write && log_access[1].value == 0x12345678);
    init();
    n.gpr[IA32_ESI] = 0x4000;
    n.gpr[IA32_EDI] = 0x7000;
    n.gpr[IA32_ECX] = 2;
    struct ia32_native_state before = n;
    CHECK(EXEC(0xf3, 0xa4).kind == IA64_STEP_FAULT && !events);
    before.eflag |= 0x10000;
    CHECK(!memcmp(&before, &n, sizeof(n)));
    init();
    n.gpr[IA32_ESI] = 0x4fff;
    n.gpr[IA32_EDI] = 0x2000;
    n.gpr[IA32_ECX] = 2;
    ia64_tc_insert(&v, 0x5000, 1, 0x3017f0, 12u << 2);
    device[0xfff] = 0x11;
    device[0x1000] = 0x22;
    CHECK(EXEC(0xf3, 0xa4).kind == IA64_STEP_COMPLETE && reads == 1 && ram[0x2000] == 0x11);
    CHECK(EXEC(0xf3, 0xa4).kind == IA64_STEP_FAULT && reads == 1 && n.gpr[IA32_ECX] == 1);
    CHECK(n.eip.value == 0 && n.gpr[IA32_ESI] == 0x5000 && n.gpr[IA32_EDI] == 0x2001 &&
          (n.eflag & 0x10000));
    ia64_tc_insert(&v, 0x5000, 1, 0x3017f1, 12u << 2);
    CHECK(EXEC(0xf3, 0xa4).kind == IA64_STEP_COMPLETE && reads == 2 && ram[0x2001] == 0x22);
    CHECK(n.gpr[IA32_ECX] == 0 && n.eip.value == 2 && !(n.eflag & 0x10000));
    init();
    n.gpr[IA32_ESI] = 0xfff;
    n.gpr[IA32_EDI] = 0x2000;
    ia64_tc_insert(&v, 0x1000, 1, 0x3007f1, 12u << 2);
    ram[0xfff] = 0x12;
    device[0] = 0x34;
    CHECK(EXEC(0x66, 0xa5).kind == IA64_STEP_COMPLETE && reads == 1);
    CHECK(ram[0x2000] == 0x12 && ram[0x2001] == 0x34 && log_access[0].size == 1);
    init();
    n.gpr[IA32_ESI] = 0x4000;
    n.gpr[IA32_EDI] = 0x200;
    n.es.limit = 0;
    CHECK(EXEC(0xa4).kind == IA64_STEP_COMPLETE && record.valid && !reads);
    CHECK(n.gpr[IA32_ESI] == 0x4000 && n.gpr[IA32_EDI] == 0x200);
}
static void vm86_and_debug(void)
{
    init();
    n.eflag |= 0x20000;
    n.cpl = 3;
    n.cs.attrib = 0xfb;
    n.cs.base = n.es.base = 0x1000;
    n.es.attrib = 0xf3;
    n.es.limit = n.cs.limit = 0xffff;
    n.gpr[IA32_ESI] = 0x10;
    n.gpr[IA32_EDI] = 0x20;
    ram[0x1010] = 0x5a;
    CHECK(EXEC(0x2e, 0xa4).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(ram[0x1020] == 0x5a && instructions[0x1020] == 0xcc);
    CHECK(n.gpr[IA32_ESI] == 0x11 && n.gpr[IA32_EDI] == 0x21);
    init();
    v.s.psr |= IA64_PSR_DB;
    v.s.dbr[0] = 0x100;
    v.s.dbr[1] = UINT64_C(0x81ffffffffffffff);
    n.gpr[IA32_ECX] = 2;
    CHECK(EXEC(0xf3, 0xa4).kind == IA64_STEP_COMPLETE && record.valid);
    CHECK(record.phase == IA32_PHASE_COMPLETED && record.isr == 0x10010 && record.iip == 0 &&
          (n.eflag & 0x10000));
    init();
    n.eflag |= 0x100;
    n.gpr[IA32_ECX] = 2;
    CHECK(EXEC(0xf3, 0xaa).kind == IA64_STEP_COMPLETE && record.valid && record.isr == 0x10008);
    CHECK(EXEC(0xf3, 0xaa).kind == IA64_STEP_COMPLETE && !record.valid && n.eip.value == 2);
}
int main(void)
{
    const struct gm_mmio_ops ops = {.read = read_device, .write = write_device};
    CHECK(gm_add_region(
        &(struct gm_region){.base = 0x100000, .size = sizeof(ram), .kind = GM_RAM, .hva = ram}));
    CHECK(gm_add_region(&(struct gm_region){
        .base = 0x200000, .size = sizeof(instructions), .kind = GM_RAM, .hva = instructions}));
    CHECK(gm_add_region(&(struct gm_region){
        .base = 0x300000, .size = sizeof(device), .kind = GM_EMULATED, .ops = &ops}));
    basic();
    repeats();
    flags();
    devices_and_faults();
    vm86_and_debug();
    printf(
        "ia32 string: %u checks, one-iteration string/REP state, flags, faults, MMIO and VM86 passed\n",
        checks);
    return 0;
}
