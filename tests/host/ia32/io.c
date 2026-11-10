/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/ia32/run.h"
#include "ia64/ia32/access.h"
#include "ia64/tlb.h"
#include "ia64vmm/guest_mem.h"

/* SDM Vol.2 10.7.1/10.7.2.1/10.7.3: expanded ports, 4-port
 * boundaries, IOPL before bitmap, and mandatory translated port accesses. */
extern struct ia64_result ia32_io(struct vcpu *, struct ia32_native_state *,
                                  const struct ia32_decoded *, const uint8_t *,
                                  struct ia32_interruption *);
static unsigned checks, reads, writes, destination_writes, destination_width;
static uint64_t destination_value;
static bool remap_destination;
static unsigned widths[8];
static uint64_t offsets[8], values[8];
static struct vcpu v;
static struct ia32_native_state n;
static struct ia32_interruption record;
static uint8_t ram[16384] __attribute__((aligned(4096)));
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint64_t read_port(void *opaque, struct vcpu *cpu, uint64_t off, unsigned size)
{
    (void)opaque;
    CHECK(cpu == &v);
    CHECK(reads < 8);
    offsets[reads] = off;
    widths[reads++] = size;
    if (remap_destination)
        ia64_tc_insert(&v, 0x2000, 1, 0x3047e1, 12u << 2);
    return UINT64_C(0x44332211);
}
static void write_port(void *opaque, struct vcpu *cpu, uint64_t off, uint64_t value, unsigned size)
{
    (void)opaque;
    CHECK(cpu == &v);
    CHECK(writes < 8);
    offsets[writes] = off;
    widths[writes] = size;
    values[writes++] = value;
}
static void write_destination(void *opaque, struct vcpu *cpu, uint64_t off, uint64_t value,
                               unsigned size)
{
    (void)opaque;
    CHECK(cpu == &v && off == 0);
    ++destination_writes;
    destination_width = size;
    destination_value = value;
}
static void init(void)
{
    memset(&v, 0, sizeof(v));
    memset(&n, 0, sizeof(n));
    memset(ram, 0, sizeof(ram));
    record = (struct ia32_interruption){0};
    reads = writes = destination_writes = destination_width = 0;
    destination_value = 0;
    remap_destination = false;
    v.cfg.cpu_model = v.s.cpu_model = IA64_CPU_MADISON;
    v.s.psr = IA64_PSR_DT | IA64_PSR_IC | IA64_PSR_IS;
    v.s.rr[0] = 12u << 2;
    n.kr0 = 0x4000000;
    n.cs.attrib = 0x400;
    n.ds = n.es = (struct ia32_segment){.base = 0, .limit = 0xffffffff, .attrib = 0xc93};
    n.tr = (struct ia32_segment){.base = 0x1000, .limit = 0x2fff, .attrib = 0x89};
    n.eflag = 2;
    n.gpr[IA32_EAX] = 0xaabbccdd;
    n.gpr[IA32_EDX] = 2;
    for (unsigned i = 0; i < 4; ++i) {
        ia64_tc_insert(&v, i * 4096, 1, 0x100000 + i * 4096 + 0x7e1, 12u << 2);
        ia64_tc_insert(&v, 0x4000000 + i * 4096, 1, 0x200000 + i * 4096 + 0x7f1, 12u << 2);
    }
    ram[0x1066] = 0x80;
}
static struct ia64_result execute(const uint8_t *bytes, unsigned size)
{
    struct ia32_decoded d;
    CHECK(ia32_decode(bytes, size, &n, &d) == IA32_DECODE_OK && d.kind == IA32_INSN_IO);
    record = (struct ia32_interruption){0};
    return ia32_io(&v, &n, &d, bytes, &record);
}
#define EXEC(...) execute((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))
static void scalar(void)
{
    init();
    CHECK(EXEC(0xe4, 2).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.gpr[IA32_EAX] == 0xaabbcc11 && n.eip.value == 2);
    CHECK(reads == 1 && offsets[0] == 2 && widths[0] == 1);
    init();
    CHECK(EXEC(0x66, 0xed).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.gpr[IA32_EAX] == 0xaabb2211 && reads == 1 && widths[0] == 2);
    init();
    n.gpr[IA32_EDX] = 0;
    CHECK(EXEC(0xef).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(writes == 1 && values[0] == 0xaabbccdd && widths[0] == 4 && n.eip.value == 1);
    init();
    n.gpr[IA32_EDX] = 3;
    CHECK(EXEC(0xef).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(writes == 3 && offsets[0] == 3 && offsets[1] == 0x1004 && offsets[2] == 0x1006);
    CHECK(widths[0] == 1 && widths[1] == 2 && widths[2] == 1);
    CHECK(values[0] == 0xdd && values[1] == 0xbbcc && values[2] == 0xaa);
    init();
    n.gpr[IA32_EDX] = 3;
    ia64_tc_insert(&v, 0x4001000, 1, 0x201000 + 0x7f1 - 1, 12u << 2);
    CHECK(EXEC(0xed).kind == IA64_STEP_FAULT && reads == 0 && n.eip.value == 0);
    init();
    n.gpr[IA32_EDX] = 0xffff;
    ia64_tc_insert(&v, 0x7fff000, 1, 0x2027f1, 12u << 2);
    ia64_tc_insert(&v, 0x8000000, 1, 0x2037f1, 12u << 2);
    CHECK(EXEC(0x66, 0xed).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(reads == 2 && offsets[0] == 0x2fff && offsets[1] == 0x3000);
    CHECK(n.gpr[IA32_EAX] == 0xaabb1111);
    init();
    ia64_tc_insert(&v, 0x4000000, 1, 0x2007e1, 12u << 2);
    CHECK(EXEC(0xef).kind == IA64_STEP_EXIT && !writes && !n.eip.value);
}
static void permissions(void)
{
    init();
    n.cpl = 3;
    CHECK(EXEC(0xec).kind == IA64_STEP_COMPLETE && record.valid && record.isr == 0xd0000);
    CHECK(reads == 0 && n.eip.value == 0);
    init();
    n.cpl = 3;
    n.cflg = 0x40;
    CHECK(EXEC(0xec).kind == IA64_STEP_COMPLETE && !record.valid && reads == 1);
    init();
    n.cpl = 3;
    n.cflg = 0x40;
    ram[0x1080] = 4;
    CHECK(EXEC(0xec).kind == IA64_STEP_COMPLETE && record.valid && record.isr == 0xd0000);
    CHECK(reads == 0 && n.eip.value == 0);
    init();
    n.cpl = 3;
    n.cflg = 0x40;
    n.tr.limit = 0x66;
    CHECK(EXEC(0xec).kind == IA64_STEP_COMPLETE && record.valid && !reads);
    init();
    v.s.psr &= ~IA64_PSR_DT;
    CHECK(EXEC(0xec).kind == IA64_STEP_COMPLETE && record.valid && record.isr == 0xd0000);
    init();
    v.s.psr |= UINT64_C(1) << 3;
    n.gpr[IA32_EDX] = 1;
    CHECK(EXEC(0xed).kind == IA64_STEP_COMPLETE && record.valid && record.isr == 0x110000);
    CHECK(record.ifa == 0x4000001 && !reads);
    init();
    n.cpl = 3;
    n.cflg = 0x40;
    n.gpr[IA32_EDX] = 7;
    ram[0x1081] = 1;
    CHECK(EXEC(0x66, 0xed).kind == IA64_STEP_COMPLETE && record.valid && !reads);
    init();
    n.cpl = 3;
    n.cflg = 0x40;
    n.tr.base = 0x4000;
    struct ia64_result r = EXEC(0xec);
    CHECK(r.kind == IA64_STEP_FAULT && (r.fault_isr & (UINT64_C(1) << 40)) && !reads);
    CHECK(!(r.fault_isr & (UINT64_C(1) << 35)));
    init();
    n.cpl = 3;
    n.cflg = 0x40;
    ia64_tc_insert(&v, 0x1000, 1, 0x101661, 12u << 2);
    r = EXEC(0xec);
    CHECK(r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_ACCESS_RIGHTS && !reads);
    CHECK((r.fault_isr & (UINT64_C(1) << 40)) && !(r.fault_isr & (UINT64_C(1) << 35)));
    init();
    n.kr0 = UINT64_C(0xc000000004000000);
    n.gpr[IA32_EDX] = 1;
    v.s.psr |= UINT64_C(1) << 3;
    ia64_tc_insert(&v, n.kr0, 1, 0x2007f1, 12u << 2);
    CHECK(EXEC(0xed).kind == IA64_STEP_COMPLETE && record.valid && !reads);
    CHECK(record.ifa == UINT64_C(0xc000000004000001));
}
static void tss_types(void)
{
    /* Vol.1 Table 6-4 ignores cached TSS.type. Vol.2 10.7.3 checks
     * the system/present bits, limits and bitmap for every type. */
    for (unsigned type = 0; type < 16; ++type)
        for (unsigned failure = 0; failure < 6; ++failure) {
            init();
            n.cpl = 3;
            n.cflg = 0x41;
            n.tr.attrib = 0x80 | type;
            if (failure == 1)
                n.tr.attrib |= 0x10;
            else if (failure == 2)
                n.tr.attrib &= ~0x80u;
            else if (failure == 3)
                n.tr.limit = 0x66;
            else if (failure == 4)
                n.tr.limit = 0x7f;
            else if (failure == 5)
                ram[0x1080] = 4;
            CHECK(EXEC(0xec).kind == IA64_STEP_COMPLETE);
            if (!failure) {
                CHECK(!record.valid && reads == 1 && n.eip.value == 1);
                CHECK(n.gpr[IA32_EAX] == 0xaabbcc11);
            } else {
                CHECK(record.valid && record.isr == 0xd0000 && !reads && !n.eip.value);
                CHECK(n.gpr[IA32_EAX] == 0xaabbccdd);
            }
        }
}
static void real_expand_down(void)
{
    /* Vol.1 Table 6-2: data E/B select bounds in the cached segment,
     * including real mode. VM86 rejects expand-down caches separately. */
    const unsigned references[] = {IA32_SEG_ES, IA32_SEG_SS, IA32_SEG_DS, IA32_SEG_FS, IA32_SEG_GS};
    for (unsigned r = 0; r < sizeof(references) / sizeof(references[0]); ++r)
        for (unsigned big = 0; big < 2; ++big) {
            init();
            unsigned id = references[r];
            struct ia32_segment *segment = (struct ia32_segment *)ia32_segment(&n, id);
            *segment = (struct ia32_segment){.limit = 0x1fff, .attrib = big ? 0x497 : 0x97};
            uint8_t bytes[] = {0x12, 0x34};
            CHECK(ia32_operand_bytes(&v, &n, id, 0x2000, 2, true, bytes, &record).kind ==
                  IA64_STEP_COMPLETE);
            CHECK(!record.valid && ram[0x2000] == 0x12 && ram[0x2001] == 0x34);
            CHECK(ia32_operand_bytes(&v, &n, id, 0x1fff, 2, true, bytes, &record).kind ==
                  IA64_STEP_COMPLETE);
            CHECK(record.valid && record.isr == (id == IA32_SEG_SS ? 0xc0000 : 0xd0000));
            CHECK(!ram[0x1fff] && ram[0x2000] == 0x12);
            record = (struct ia32_interruption){0};
            ia64_tc_insert(&v, 0xf000, 1, 0x1037e1, 12u << 2);
            CHECK(ia32_operand_bytes(&v, &n, id, 0xffff, 1, true, bytes, &record).kind ==
                  IA64_STEP_COMPLETE);
            CHECK(!record.valid && ram[0x3fff] == 0x12);
            ia64_tc_insert(&v, 0x10000, 1, 0x1037e1, 12u << 2);
            CHECK(ia32_operand_bytes(&v, &n, id, 0xffff, 2, true, bytes, &record).kind ==
                  IA64_STEP_COMPLETE);
            CHECK(record.valid == !big);
            CHECK(ram[0x3000] == (big ? 0x34 : 0));
            record = (struct ia32_interruption){0};
            CHECK(ia32_operand_bytes(&v, &n, id, 0x10000, 1, true, bytes, &record).kind ==
                  IA64_STEP_COMPLETE);
            CHECK(record.valid == !big && ram[0x3000] == (big ? 0x12 : 0));
            record = (struct ia32_interruption){0};
            ia64_tc_insert(&v, 0xfffff000, 1, 0x1037e1, 12u << 2);
            CHECK(ia32_operand_bytes(&v, &n, id, UINT32_MAX, 1, true, bytes, &record).kind ==
                  IA64_STEP_COMPLETE);
            CHECK(record.valid == !big);
            record = (struct ia32_interruption){0};
            CHECK(ia32_operand_bytes(&v, &n, id, UINT32_MAX, 2, true, bytes, &record).kind ==
                  IA64_STEP_COMPLETE);
            CHECK(record.valid && record.isr == (id == IA32_SEG_SS ? 0xc0000 : 0xd0000));
            CHECK(!ram[0]);
        }
}
static void strings(void)
{
    init();
    n.gpr[IA32_EDI] = 0x2000;
    CHECK(EXEC(0x6c).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(reads == 1 && ram[0x2000] == 0x11 && n.gpr[IA32_EDI] == 0x2001 && n.eip.value == 1);
    init();
    n.gpr[IA32_ESI] = 0x2000;
    ram[0x2000] = 0x78;
    n.eflag |= 0x400;
    CHECK(EXEC(0x6e).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(writes == 1 && values[0] == 0x78 && n.gpr[IA32_ESI] == 0x1fff);
    init();
    n.gpr[IA32_EDI] = 0x2000;
    n.gpr[IA32_ECX] = 2;
    n.eflag |= 0x10000;
    CHECK(EXEC(0xf3, 0x6c).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(reads == 1 && n.gpr[IA32_ECX] == 1 && n.gpr[IA32_EDI] == 0x2001 && n.eip.value == 0);
    CHECK(n.eflag & 0x10000);
    CHECK(EXEC(0xf3, 0x6c).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(reads == 2 && n.gpr[IA32_ECX] == 0 && n.eip.value == 2);
    CHECK(!(n.eflag & 0x10000));
    init();
    n.gpr[IA32_EDI] = 0xffffffff;
    CHECK(EXEC(0xf3, 0x6c).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(!reads && n.gpr[IA32_EDI] == 0xffffffff && n.eip.value == 2);
    init();
    n.gpr[IA32_EDI] = 0x4000;
    CHECK(EXEC(0x6c).kind == IA64_STEP_FAULT && !reads && n.eip.value == 0);
    init();
    n.gpr[IA32_EDI] = 0x1234ffff;
    CHECK(EXEC(0x67, 0x6c).kind == IA64_STEP_FAULT && !reads);
    n.gpr[IA32_EDI] = 0x12342000;
    CHECK(EXEC(0x67, 0x6c).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(n.gpr[IA32_EDI] == 0x12342001);
    init();
    n.gpr[IA32_ESI] = 0x2000;
    n.fs = n.ds;
    n.fs.base = 0x1000;
    ram[0x3000] = 0x56;
    CHECK(EXEC(0x64, 0x6e).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(writes == 1 && values[0] == 0x56);
    init();
    n.gpr[IA32_EDI] = 0x2000;
    n.gpr[IA32_ECX] = 2;
    n.eflag |= 0x100;
    CHECK(EXEC(0xf3, 0x6c).kind == IA64_STEP_COMPLETE && record.valid);
    CHECK(record.isr == 0x10008 && record.iip == 0 && n.gpr[IA32_ECX] == 1 && reads == 1);
    CHECK(n.eflag & 0x10000);
    init();
    v.s.psr |= IA64_PSR_DB;
    v.s.dbr[0] = 0x4000002;
    v.s.dbr[1] = UINT64_C(0x81ffffffffffffff);
    CHECK(EXEC(0xec).kind == IA64_STEP_COMPLETE && record.valid);
    CHECK(record.isr == 0x10010 && record.iip == 1 && reads == 1);
}
static void destination_checks(void)
{
    /* INS checks the complete destination before the port read. MMIO is
     * a supported destination and retains the instruction's width. */
    for (unsigned width = 1; width <= 4; width *= 2) {
        init();
        n.gpr[IA32_EDX] = 0;
        n.gpr[IA32_EDI] = 0x2000;
        ia64_tc_insert(&v, 0x2000, 1, 0x3007e1, 12u << 2);
        struct ia64_result result = width == 1   ? EXEC(0x6c)
                                      : width == 2 ? EXEC(0x66, 0x6d)
                                                   : EXEC(0x6d);
        CHECK(result.kind == IA64_STEP_COMPLETE && !record.valid);
        CHECK(reads == 1 && widths[0] == width && destination_writes == 1);
        CHECK(destination_width == width &&
              destination_value == (width == 1 ? 0x11 : width == 2 ? 0x2211 : 0x44332211));
        CHECK(n.gpr[IA32_EDI] == 0x2000 + width);
    }
    init();
    n.gpr[IA32_EDX] = 0;
    n.gpr[IA32_EDI] = 0x2000;
    ia64_tc_insert(&v, 0x2000, 1, 0x3007e1, 12u << 2);
    remap_destination = true;
    CHECK(EXEC(0x6d).kind == IA64_STEP_COMPLETE && !record.valid);
    CHECK(reads == 1 && destination_writes == 1 && destination_width == 4);
    CHECK(destination_value == 0x44332211 && n.gpr[IA32_EDI] == 0x2004 && n.eip.value == 1);
    init();
    n.gpr[IA32_EDI] = 0x2000;
    ia64_tc_insert(&v, 0x2000, 1, 0x3047e1, 12u << 2);
    CHECK(EXEC(0x6c).kind == IA64_STEP_EXIT && !reads && !destination_writes);
    CHECK(n.gpr[IA32_EDI] == 0x2000 && !n.eip.value);
    init();
    n.gpr[IA32_EDX] = 0;
    n.gpr[IA32_EDI] = 0x2000;
    ia64_tc_insert(&v, 0x2000, 1, 0x3057e1, 12u << 2);
    CHECK(EXEC(0x6d).kind == IA64_STEP_EXIT && !reads && !destination_writes);
    init();
    n.gpr[IA32_EDI] = 0x2fff;
    ia64_tc_insert(&v, 0x2000, 1, 0x3007e1, 12u << 2);
    ia64_tc_insert(&v, 0x3000, 1, 0x3047e1, 12u << 2);
    CHECK(EXEC(0x66, 0x6d).kind == IA64_STEP_EXIT && !reads && !destination_writes);
    init();
    n.gpr[IA32_EDI] = 0x2001;
    v.s.psr |= UINT64_C(1) << 3;
    CHECK(EXEC(0x6d).kind == IA64_STEP_COMPLETE && record.valid && record.isr == 0x110000);
    CHECK(!reads && !destination_writes);
    init();
    n.es.attrib &= ~1u;
    n.gpr[IA32_EDI] = 0x2000;
    CHECK(EXEC(0x6c).kind == IA64_STEP_COMPLETE && record.valid && record.isr == 0xd0000);
    CHECK(!reads && !destination_writes);
    init();
    n.ss = (struct ia32_segment){.limit = 0xffffffff, .attrib = 0x4b3};
    uint8_t bytes[] = {0x12, 0x34};
    CHECK(ia32_operand_bytes(&v, &n, IA32_SEG_SS, 0x2000, 2, true, bytes, &record).kind ==
          IA64_STEP_COMPLETE);
    CHECK(record.valid && record.isr == 0xc0000 && !ram[0x2000] && !ram[0x2001]);
    init();
    n.cpl = 3;
    n.cflg = 0x40;
    n.tr.attrib = 0x99;
    CHECK(EXEC(0xec).kind == IA64_STEP_COMPLETE && record.valid && !reads);
}
int main(void)
{
    const struct gm_mmio_ops ops = {.read = read_port, .write = write_port};
    const struct gm_mmio_ops destination_ops = {.write = write_destination};
    const struct gm_mmio_ops readonly_ops = {.read = read_port};
    CHECK(gm_add_region(
        &(struct gm_region){.base = 0x100000, .size = sizeof(ram), .kind = GM_RAM, .hva = ram}));
    CHECK(gm_add_region(
        &(struct gm_region){.base = 0x200000, .size = 16384, .kind = GM_EMULATED, .ops = &ops}));
    CHECK(gm_add_region(&(struct gm_region){
        .base = 0x300000, .size = 4096, .kind = GM_EMULATED, .ops = &destination_ops}));
    CHECK(gm_add_region(&(struct gm_region){
        .base = 0x304000, .size = 4096, .kind = GM_EMULATED, .ops = &readonly_ops}));
    CHECK(gm_add_region(&(struct gm_region){
        .base = 0x305000, .size = 4096, .kind = GM_DIRECT_MMIO, .hva = ram + 1}));
    scalar();
    permissions();
    tss_types();
    real_expand_down();
    strings();
    destination_checks();
    printf("ia32 io: %u checks passed\n", checks);
    return 0;
}
