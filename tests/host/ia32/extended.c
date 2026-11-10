/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/ia32/extended.h"
#include "ia64/ia32/run.h"
#include "ia64/tlb.h"
#include "ia64vmm/guest_mem.h"

/* Itanium SDM Vol.4 ENTER, FX state, MASKMOVQ and BT/POP encodings;
 * unchanged ENTER semantics are clarified by Intel SDM Vol.2A ENTER. */
static unsigned checks;
static struct vcpu v;
static struct ia32_native_state n;
static struct ia32_decoded d;
static struct ia32_interruption record;
static uint8_t ram[65536] __attribute__((aligned(4096)));
static uint8_t device[4096];
static unsigned transactions;
static char transaction_kind[16];
static uint64_t transaction_offset[16];
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%u: %s\n", __LINE__, #x);                                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint64_t get(const uint8_t *p, unsigned bytes)
{
    uint64_t x = 0;
    for (unsigned i = 0; i < bytes; ++i)
        x |= (uint64_t)p[i] << (8 * i);
    return x;
}
static void put(uint8_t *p, uint64_t x, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = x >> (8 * i);
}
static uint64_t device_read(void *opaque, struct vcpu *cpu, uint64_t offset, unsigned size)
{
    (void)opaque;
    (void)cpu;
    CHECK(transactions < 16 && offset + size <= sizeof(device));
    transaction_kind[transactions] = 'R';
    transaction_offset[transactions++] = offset;
    return get(device + offset, size);
}
static void device_write(void *opaque, struct vcpu *cpu, uint64_t offset, uint64_t value,
                         unsigned size)
{
    (void)opaque;
    (void)cpu;
    CHECK(transactions < 16 && offset + size <= sizeof(device));
    transaction_kind[transactions] = 'W';
    transaction_offset[transactions++] = offset;
    put(device + offset, value, size);
}
static void init(void)
{
    if (v.native)
        engine_ia32_glue()->detach(&v);
    memset(&v, 0, sizeof(v));
    memset(&n, 0, sizeof(n));
    memset(ram, 0xcc, sizeof(ram));
    record = (struct ia32_interruption){0};
    v.cfg.cpu_model = v.s.cpu_model = IA64_CPU_MADISON;
    v.s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC | IA64_PSR_IS;
    v.s.rr[0] = 12u << 2;
    n.cs.attrib = 0x400;
    n.ds = n.es = n.ss = n.fs = (struct ia32_segment){.limit = 65535, .attrib = 0xc93};
    n.cflg = (UINT64_C(1) << 41) | 1;
    n.eip.value = 0x100;
    n.eflag = 0x10002;
    n.gpr[IA32_ESP] = 0x4000;
    n.gpr[IA32_EBP] = 0x5000;
    n.gpr[IA32_EDI] = 0x3000;
    put(n.fp.bytes, 0x37f, 2);
    put(n.fp.bytes + 24, 0x1f80, 4);
    for (unsigned i = 0; i < 16; ++i)
        ia64_tc_insert(&v, i * 4096, 1, 0x100000 + i * 4096 + 0x661, 12u << 2);
    struct ia32_setup_error error;
    CHECK(engine_ia32_glue()->attach(&v, 0, &error));
}
static void decode(const uint8_t *p, unsigned bytes)
{
    CHECK(ia32_decode(p, bytes, &n, &d) == IA32_DECODE_OK);
}
static struct ia64_result execute(const uint8_t *p, unsigned bytes)
{
    decode(p, bytes);
    bool handled;
    record = (struct ia32_interruption){0};
    struct ia64_result r = ia32_extended(&v, &n, &d, p, &handled, &record);
    CHECK(handled);
    return r;
}
#define DECODE(...) decode((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))
#define EXEC(...) execute((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))
static void bit_and_pop(void)
{
    init();
    n.gpr[IA32_EAX] = 0x3000;
    n.gpr[IA32_ECX] = 63;
    DECODE(0x0f, 0xa3, 0x08);
    CHECK(d.offset == 0x3004 && d.access_width == 4 && d.access_kind == IA32_ACCESS_READ);
    n.gpr[IA32_ECX] = (uint32_t)-1;
    DECODE(0x0f, 0xab, 0x08);
    CHECK(d.offset == 0x2ffc && d.access_width == 4 && d.access_kind == IA32_ACCESS_RMW);
    n.gpr[IA32_ECX] = (uint32_t)-33;
    DECODE(0x0f, 0xb3, 0x08);
    CHECK(d.offset == 0x2ff8);
    n.gpr[IA32_ECX] = 0x1234ffff;
    DECODE(0x66, 0x0f, 0xbb, 0x08);
    CHECK(d.offset == 0x2ffe && d.access_width == 2);
    DECODE(0x0f, 0xba, 0x20, 0xff);
    CHECK(d.offset == 0x3000 && d.access_width == 4);
    n.gpr[IA32_EBX] = 0;
    DECODE(0x67, 0x66, 0x0f, 0xa3, 0x0f);
    CHECK(d.offset == 0xfffe);
    DECODE(0x8f, 0x44, 0x24, 0x10);
    CHECK(d.has_sib && d.sib == 0x24 && d.offset == 0x4014 && d.segment == IA32_SEG_SS);
    n.gpr[IA32_ECX] = 3;
    DECODE(0x8f, 0x44, 0x8c, 0x10);
    CHECK(d.offset == 0x4020);
    n.ss.attrib &= ~0x400;
    n.gpr[IA32_ESP] = 0x1234fffe;
    DECODE(0x8f, 0x04, 0x24);
    CHECK(d.offset == 0x12340002);
    n.gpr[IA32_EBX] = 0x1234fff0;
    n.gpr[IA32_EAX] = 0x1234;
    DECODE(0x67, 0x64, 0xd7);
    CHECK(d.offset == 0x24 && d.segment == IA32_SEG_FS && d.access_width == 1);
    DECODE(0x62, 0xf1, 0x7c, 0x48, 0x58, 0xc1);
    CHECK(d.kind == IA32_INSN_NATIVE && d.length == 6 && (d.flags & IA32_INSN_SSE));
}
static void enter(void)
{
    init();
    CHECK(EXEC(0xc8, 0x10, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.gpr[IA32_EBP] == 0x3ffc && n.gpr[IA32_ESP] == 0x3fec);
    CHECK(get(ram + 0x3ffc, 4) == 0x5000 && n.eip.value == 0x104 && !(n.eflag & 0x10000));
    init();
    put(ram + 0x4ffc, 0x12345678, 4);
    CHECK(EXEC(0xc8, 0x10, 0, 2).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.gpr[IA32_EBP] == 0x3ffc && n.gpr[IA32_ESP] == 0x3fe4);
    CHECK(get(ram + 0x3ffc, 4) == 0x5000 && get(ram + 0x3ff8, 4) == 0x12345678);
    CHECK(get(ram + 0x3ff4, 4) == 0x3ffc);
    init();
    n.gpr[IA32_EBP] = 0x4000;
    CHECK(EXEC(0xc8, 0, 0, 2).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(get(ram + 0x3ff8, 4) == 0x4000); /* Display read forwards the first push. */
    init();
    n.gpr[IA32_EBP] = 0x12345000;
    CHECK(EXEC(0x66, 0xc8, 0x10, 0, 1).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.gpr[IA32_EBP] == 0x12343ffe && n.gpr[IA32_ESP] == 0x3fec);
    CHECK(get(ram + 0x3ffc, 2) == 0x3ffe && get(ram + 0x3ffe, 2) == 0x5000);
    init();
    n.ss.attrib &= ~0x400;
    n.gpr[IA32_ESP] = 0x12344000;
    CHECK(EXEC(0xc8, 0x10, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.gpr[IA32_EBP] == 0x12343ffc && n.gpr[IA32_ESP] == 0x12343fec);
    init();
    n.ss.limit = 0x3fff;
    CHECK(EXEC(0xc8, 0xff, 0xff, 0).kind == IA64_STEP_COMPLETE && record.valid);
    CHECK(record.isr == 0xc0000 && n.gpr[IA32_ESP] == 0x4000 && n.gpr[IA32_EBP] == 0x5000);
    CHECK(get(ram + 0x3ffc, 4) == 0xcccccccc);
    init();
    v.s.psr |= IA64_PSR_DB;
    v.s.dbr[0] = 0x3ffc;
    v.s.dbr[1] = UINT64_C(0x41ffffffffffffff);
    CHECK(EXEC(0xc8, 0, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(ia32_operand_breakpoints(&v) == 1);
}
static void fxstate(void)
{
    init();
    n.fp.bytes[4] = 0x24;
    put(n.fp.bytes + 8, 0x12345678, 4);
    for (unsigned i = 0; i < 10; ++i)
        n.fp.bytes[32 + i] = i + 1;
    for (unsigned i = 0; i < 16; ++i)
        n.fp.bytes[160 + i] = 0xa0 + i;
    CHECK(EXEC(0x0f, 0xae, 0x05, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(get(ram + 0x3000, 2) == 0x37f && ram[0x3004] == 0x24);
    CHECK(get(ram + 0x3008, 4) == 0x12345678 && ram[0x3029] == 10 && ram[0x30af] == 0xaf);
    CHECK(ram[0x3120] == 0xcc && ram[0x31ff] == 0xcc && n.fp.bytes[4] == 0x24);
    init();
    memset(ram + 0x3000, 0, 512);
    put(ram + 0x3000, 0x37f, 2);
    put(ram + 0x3002, 0x1800, 2);
    ram[0x3004] = 0x80;
    put(ram + 0x3018, 0x1f80, 4);
    ram[0x3020] = 0x5a;
    ram[0x30a0] = 0xa5;
    CHECK(EXEC(0x0f, 0xae, 0x0d, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(get(n.fp.bytes + 2, 2) == 0x1800 && n.fp.bytes[4] == 0x80);
    CHECK(n.fp.bytes[32] == 0x5a && n.fp.bytes[160] == 0xa5);
    init();
    memset(ram + 0x3000, 0, 512);
    put(ram + 0x3018, 0x10000, 4);
    struct ia32_fp_image old = n.fp;
    CHECK(EXEC(0x0f, 0xae, 0x0d, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && record.valid);
    CHECK(record.isr == 0xd0000 && !memcmp(&old, &n.fp, sizeof(old)) && n.eip.value == 0x100);
    init();
    CHECK(EXEC(0x0f, 0xae, 0x05, 8, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && record.valid);
    CHECK(record.isr == 0xd0000 && ram[0x3008] == 0xcc);
    init();
    n.cflg |= 8;
    CHECK(EXEC(0x0f, 0xae, 0x05, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && record.valid);
    CHECK(record.isr == 0x70000);
    init();
    ia64_tc_insert(&v, 0x4000, 1, 0x104660, 12u << 2);
    CHECK(EXEC(0x0f, 0xae, 0x05, 0, 0x3f, 0, 0).kind == IA64_STEP_FAULT);
    CHECK(ram[0x3f00] == 0xcc && n.eip.value == 0x100);
}
static void maskmov(void)
{
    init();
    put(n.fp.bytes + 32, UINT64_C(0x0807060504030201), 8);
    put(n.fp.bytes + 48, UINT64_C(0x8000000080000080), 8);
    CHECK(EXEC(0x0f, 0xf7, 0xc1).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(ram[0x3000] == 1 && ram[0x3001] == 0xcc && ram[0x3003] == 4 && ram[0x3007] == 8);
    CHECK(n.fp.bytes[4] == 0xff && !(get(n.fp.bytes + 2, 2) & 0x3800));
    init();
    put(n.fp.bytes + 2, 0x1800, 2); /* MM0/MM1 occupy logical ST5/ST6 at TOP3. */
    put(n.fp.bytes + 32 + 5 * 16, UINT64_C(0x8877665544332211), 8);
    put(n.fp.bytes + 32 + 6 * 16, UINT64_C(0x8080808080808080), 8);
    CHECK(EXEC(0x0f, 0xf7, 0xc1).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(get(ram + 0x3000, 8) == UINT64_C(0x8877665544332211));
    CHECK(get(n.fp.bytes + 32, 8) == UINT64_C(0x8877665544332211));
    init();
    n.gpr[IA32_EDI] = 0x12343000;
    n.fs.base = 0x1000;
    put(n.fp.bytes + 48, UINT64_C(0x8080808080808080), 8);
    CHECK(EXEC(0x67, 0x64, 0x0f, 0xf7, 0xc1).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(get(ram + 0x4000, 8) == 0);
    init();
    for (unsigned i = 0; i < 16; ++i) {
        n.fp.bytes[160 + i] = i;
        n.fp.bytes[176 + i] = 0x80;
    }
    CHECK(EXEC(0x66, 0x0f, 0xf7, 0xc1).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(ram[0x3000] == 0 && ram[0x300f] == 15 && n.fp.bytes[4] == 0);
    init();
    n.ds.limit = 0x3003;
    CHECK(EXEC(0x0f, 0xf7, 0xc1).kind == IA64_STEP_COMPLETE && record.valid);
    CHECK(record.isr == 0xd0000 && n.fp.bytes[4] == 0);
    init();
    n.gpr[IA32_EDI] = 0x3ffc;
    put(n.fp.bytes + 48, UINT64_C(0x8080808080808080), 8);
    ia64_tc_insert(&v, 0x4000, 1, 0x104660, 12u << 2);
    CHECK(EXEC(0x0f, 0xf7, 0xc1).kind == IA64_STEP_FAULT);
    CHECK(get(ram + 0x3ffc, 4) == 0xcccccccc && n.fp.bytes[4] == 0);
}
static void x87_environment(void)
{
    init();
    n.fp.bytes[4] = 1;
    put(n.fp.bytes + 8, 0x12345678, 4);
    put(n.fp.bytes + 12, 0x00b3, 2);
    put(n.fp.bytes + 16, 0x87654321, 4);
    put(n.fp.bytes + 20, 0x00cb, 2);
    put(n.fp.bytes + 6, 0x345, 2);
    CHECK(EXEC(0xd9, 0x35, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(get(ram + 0x3000, 2) == 0x37f && get(ram + 0x3008, 2) == 0xfffd);
    CHECK(get(ram + 0x300c, 4) == 0x12345678 && get(ram + 0x3010, 4) == 0x034500b3);
    CHECK(get(ram + 0x3014, 4) == 0x87654321 && get(ram + 0x3018, 2) == 0xcb);
    init();
    memset(ram + 0x3000, 0, 28);
    put(ram + 0x3000, 0x37f, 2);
    put(ram + 0x3004, 0x1800, 2);
    put(ram + 0x3008, 0xfffc, 2);
    put(ram + 0x300c, 0x11223344, 4);
    put(ram + 0x3010, 0x051200ab, 4);
    put(ram + 0x3014, 0x55667788, 4);
    put(ram + 0x3018, 0xcd, 2);
    n.fp.bytes[32] = 0x5a; /* Logical ST0 before TOP changes is physical R0. */
    CHECK(EXEC(0xd9, 0x25, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(get(n.fp.bytes + 2, 2) == 0x1800 && n.fp.bytes[4] == 1);
    CHECK(get(n.fp.bytes + 8, 4) == 0x11223344 && get(n.fp.bytes + 12, 2) == 0xab);
    CHECK(get(n.fp.bytes + 6, 2) == 0x512 && n.fp.bytes[32 + 5 * 16] == 0x5a);
    init();
    n.cflg &= ~UINT64_C(1);
    put(n.fp.bytes + 8, 0x56, 4);
    put(n.fp.bytes + 12, 0x1234, 2);
    put(n.fp.bytes + 16, 0x67, 4);
    put(n.fp.bytes + 20, 0x2345, 2);
    put(n.fp.bytes + 6, 0x5a3, 2);
    CHECK(EXEC(0x66, 0xd9, 0x35, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(get(ram + 0x3006, 2) == 0x2396 && get(ram + 0x3008, 2) == 0x15a3);
    CHECK(get(ram + 0x300a, 2) == 0x34b7 && get(ram + 0x300c, 2) == 0x2000);
    init();
    n.fp.bytes[4] = 1;
    n.fp.bytes[32] = 0x6b;
    n.fp.bytes[160] = 0xa5;
    CHECK(EXEC(0xdd, 0x35, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(ram[0x301c] == 0x6b && get(n.fp.bytes, 2) == 0x37f && !n.fp.bytes[4]);
    CHECK(n.fp.bytes[160] == 0xa5 && !get(n.fp.bytes + 8, 4));
    init();
    memset(ram + 0x3000, 0, 108);
    put(ram + 0x3000, 0x37f, 2);
    put(ram + 0x3004, 0x1000, 2);
    put(ram + 0x3008, 0xffcf, 2);
    ram[0x301c] = 0x7c;
    CHECK(EXEC(0xdd, 0x25, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(get(n.fp.bytes + 2, 2) == 0x1000 && n.fp.bytes[4] == 4 && n.fp.bytes[32] == 0x7c);
    init();
    put(n.fp.bytes + 2, 0x1200, 2);
    n.gpr[IA32_EAX] = 0x56781234;
    CHECK(EXEC(0xdf, 0xe0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.gpr[IA32_EAX] == 0x56781200);
    init();
    put(n.fp.bytes + 2, 0x80, 2);
    CHECK(EXEC(0xd9, 0x35, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(EXEC(0xd9, 0x25, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    init();
    n.fp.bytes[2] = 0x80;
    CHECK(EXEC(0xd9, 0x25, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && record.valid);
    CHECK(record.isr == 0x100000 && n.eip.value == 0x100);
    init();
    put(n.fp.bytes + 2, 0xffff, 2);
    put(n.fp.bytes + 8, 0x12345678, 4);
    put(n.fp.bytes + 12, 0xb3, 2);
    CHECK(EXEC(0xdb, 0xe2).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(get(n.fp.bytes + 2, 2) == 0x7f00 && get(n.fp.bytes + 8, 4) == 0x12345678);
    CHECK(get(n.fp.bytes + 12, 2) == 0xb3);
    init();
    put(n.fp.bytes + 2, 0x1880, 2);
    n.fp.bytes[4] = 0xff;
    n.fp.bytes[32 + 5 * 16] = 0xaa;
    n.fp.bytes[160] = 0xa5;
    put(n.fp.bytes + 8, 0x12345678, 4);
    CHECK(EXEC(0xdb, 0xe3).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(get(n.fp.bytes, 2) == 0x37f && !get(n.fp.bytes + 2, 2) && !n.fp.bytes[4]);
    CHECK(!get(n.fp.bytes + 8, 4) && n.fp.bytes[32] == 0xaa && n.fp.bytes[160] == 0xa5);
}
static struct ia64_result admission(const uint8_t *bytes, unsigned size)
{
    decode(bytes, size);
    record = (struct ia32_interruption){0};
    return ia32_extended_check(&v, &n, &d, &record);
}
#define ADMIT(...) admission((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))
static void admission_checks(void)
{
    init();
    n.cflg |= 8;
    CHECK(ADMIT(0xdf, 0xe0).kind == IA64_STEP_COMPLETE && record.valid);
    CHECK(record.isr == 0x70000 && n.eip.value == 0x100);
    init();
    n.fp.bytes[2] = 0x80;
    CHECK(ADMIT(0xd8, 0xc0).kind == IA64_STEP_COMPLETE && record.valid);
    CHECK(record.isr == 0x100000);
    CHECK(ADMIT(0xd9, 0x35, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    init();
    n.cflg |= 8;
    CHECK(ADMIT(0x9b).kind == IA64_STEP_COMPLETE && !record.valid);
    n.cflg |= 2;
    CHECK(ADMIT(0x9b).kind == IA64_STEP_COMPLETE && record.valid && record.isr == 0x70000);
    init();
    n.fp.bytes[2] = 0x80;
    CHECK(ADMIT(0x0f, 0x77).kind == IA64_STEP_COMPLETE && record.valid && record.isr == 0x100000);
    init();
    n.cflg |= 8;
    CHECK(ADMIT(0x0f, 0x58, 0xc1).kind == IA64_STEP_COMPLETE && record.valid &&
          record.isr == 0x70000);
    init();
    CHECK(ADMIT(0x0f, 0x58, 0x05, 8, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && record.valid);
    CHECK(record.isr == 0xd0000 && n.eip.value == 0x100);
    CHECK(ADMIT(0x0f, 0x10, 0x05, 8, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(ADMIT(0x66, 0x0f, 0x6f, 0x05, 8, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && record.valid);
    CHECK(ADMIT(0xf3, 0x0f, 0x6f, 0x05, 8, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(ADMIT(0x0f, 0xe7, 0x05, 4, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && record.valid);
    init();
    n.fp.bytes[2] = 0x80;
    CHECK(ADMIT(0x0f, 0x2a, 0x05, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
}
static void extended_boundaries(void)
{
    init();
    CHECK(EXEC(0xc8, 0, 0, 0xff).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.gpr[IA32_EBP] == 0x3ffc && n.gpr[IA32_ESP] == 0x3f80);
    CHECK(get(ram + 0x3f80, 4) == 0x3ffc);
    init();
    ia64_tc_insert(&v, 0x1000, 1, 0x101660, 12u << 2);
    CHECK(EXEC(0xc8, 0x00, 0x20, 0).kind == IA64_STEP_FAULT);
    CHECK(get(ram + 0x3ffc, 4) == 0xcccccccc && n.gpr[IA32_ESP] == 0x4000);
    init();
    n.cflg &= ~(UINT64_C(1) << 41);
    memset(ram + 0x3000, 0, 512);
    put(ram + 0x3000, 0x37f, 2);
    put(ram + 0x3018, 0x10000, 4);
    n.fp.bytes[160] = 0x5a;
    CHECK(EXEC(0x0f, 0xae, 0x0d, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.fp.bytes[160] == 0x5a && get(n.fp.bytes + 24, 4) == 0x1f80);
    init();
    v.s.psr |= IA64_PSR_DB;
    v.s.dbr[0] = 0x3000;
    v.s.dbr[1] = UINT64_C(0x41ffffffffffffff);
    CHECK(EXEC(0x0f, 0xae, 0x05, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(ia32_operand_breakpoints(&v) == 1);
    init();
    v.s.psr |= IA64_PSR_DB;
    v.s.dbr[0] = 0x31ff;
    v.s.dbr[1] = UINT64_C(0x41ffffffffffffff);
    CHECK(EXEC(0x0f, 0xae, 0x05, 0, 0x30, 0, 0).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(!ia32_operand_breakpoints(&v));
    init();
    v.s.psr |= IA64_PSR_DB;
    v.s.dbr[0] = 0x3001;
    v.s.dbr[1] = UINT64_C(0x41ffffffffffffff);
    n.fp.bytes[48] = 0x80;
    CHECK(EXEC(0x0f, 0xf7, 0xc1).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(!ia32_operand_breakpoints(&v));
    init();
    v.s.psr |= IA64_PSR_DB;
    v.s.dbr[0] = 0x3000;
    v.s.dbr[1] = UINT64_C(0x41ffffffffffffff);
    n.fp.bytes[48] = 0x80;
    CHECK(EXEC(0x0f, 0xf7, 0xc1).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(ia32_operand_breakpoints(&v) == 1);
    init();
    transactions = 0;
    memset(device, 0, sizeof(device));
    ia64_tc_insert(&v, 0x6000, 1, 0x200671, 12u << 2);
    n.gpr[IA32_ESP] = 0x6100;
    n.gpr[IA32_EBP] = 0x6200;
    put(device + 0x1fc, 0x12345678, 4);
    CHECK(EXEC(0xc8, 0x10, 0, 2).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(transactions == 4 && !memcmp(transaction_kind, "WRWW", 4));
    CHECK(transaction_offset[0] == 0xfc && transaction_offset[1] == 0x1fc);
    CHECK(get(device + 0xf8, 4) == 0x12345678 && get(device + 0xf4, 4) == 0x60fc);
    init();
    transactions = 0;
    ia64_tc_insert(&v, 0x6000, 1, 0x200671, 12u << 2);
    ia64_tc_insert(&v, 0x4000, 1, 0x104660, 12u << 2);
    n.gpr[IA32_ESP] = 0x6100;
    CHECK(EXEC(0xc8, 0, 0x20, 0).kind == IA64_STEP_FAULT);
    CHECK(!transactions && n.gpr[IA32_ESP] == 0x6100);
}
int main(void)
{
    CHECK(gm_add_region(
        &(struct gm_region){.base = 0x100000, .size = sizeof(ram), .hva = ram, .kind = GM_RAM}));
    static const struct gm_mmio_ops ops = {.read = device_read, .write = device_write};
    CHECK(gm_add_region(&(struct gm_region){
        .base = 0x200000, .size = sizeof(device), .ops = &ops, .kind = GM_EMULATED}));
    bit_and_pop();
    enter();
    fxstate();
    maskmov();
    x87_environment();
    admission_checks();
    extended_boundaries();
    if (v.native)
        engine_ia32_glue()->detach(&v);
    printf("ia32 extended: %u checks passed\n", checks);
    return 0;
}
