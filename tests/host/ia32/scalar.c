/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Intel SDM Vol.2 instruction operation/flag definitions; device protocol
 * observations are independent of the arithmetic implementation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/ia32/scalar.h"
#include "ia64/ia32/access.h"
#include "ia64/memory.h"
#include "tests/host/test.h"

static struct vcpu v;
static uint64_t device;
static unsigned reads, writes, begins, ends, prepared, fault_at;
static bool locked, fail_read, fail_write;
static uint32_t expected_offset;
static uint64_t checks;
void ia64_device_rmw_begin(void)
{
    CHECK(!locked);
    locked = true;
    ++begins;
}
void ia64_device_rmw_end(void)
{
    CHECK(locked);
    locked = false;
    ++ends;
}
struct ia64_result ia32_operand_prepare(struct vcpu *cpu, const struct ia32_native_state *n,
                                        unsigned seg, uint32_t off, unsigned size, bool write,
                                        unsigned alignment, struct ia32_interruption *r)
{
    (void)n;
    (void)seg;
    (void)size;
    (void)write;
    (void)alignment;
    CHECK(cpu == &v && !locked && off == expected_offset);
    if (++prepared == fault_at)
        r->valid = true;
    return ia64_complete();
}
struct ia64_result ia32_operand(struct vcpu *cpu, const struct ia32_native_state *n, unsigned seg,
                                uint32_t off, unsigned size, bool write, uint64_t *value,
                                struct ia32_interruption *r)
{
    (void)n;
    (void)seg;
    (void)r;
    CHECK(cpu == &v && locked && off == expected_offset);
    uint64_t mask = size == 8 ? UINT64_MAX : (UINT64_C(1) << (8 * size)) - 1;
    if (write) {
        ++writes;
        if (fail_write)
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, 0, "test device refusal");
        device = *value & mask;
    } else {
        ++reads;
        if (fail_read)
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, 0, "test read refusal");
        *value = device & mask;
    }
    return ia64_complete();
}
static struct ia32_native_state state(void)
{
    struct ia32_native_state n = {.eflag = 0x10202, .eip = {0x1234}, .cpl = 3};
    n.cs.attrib = 0xc9b;
    n.gpr[IA32_EDI] = 0x100;
    return n;
}
static struct ia32_decoded decode(const uint8_t *bytes, unsigned count,
                                  const struct ia32_native_state *n)
{
    struct ia32_decoded d;
    enum ia32_decode_status status = ia32_decode(bytes, count, n, &d);
    if (status != IA32_DECODE_OK || d.length != count)
        fprintf(stderr, "scalar fixture decode status=%u opcode=%02x %02x length=%u/%u\n", status,
                bytes[0], bytes[1], d.length, count);
    CHECK(status == IA32_DECODE_OK && d.length == count);
    return d;
}
static struct ia64_result run(struct ia32_native_state *n, const uint8_t *bytes, unsigned count,
                              struct ia32_interruption *r, bool *handled)
{
    struct ia32_decoded d = decode(bytes, count, n);
    expected_offset = d.offset;
    prepared = reads = writes = begins = ends = 0;
    *r = (struct ia32_interruption){0};
    struct ia64_result result = ia32_scalar(&v, n, &d, bytes, handled, r);
    CHECK(!locked && begins == ends);
    return result;
}
static unsigned status(uint8_t value)
{
    unsigned bits = 0;
    for (unsigned i = 0; i < 8; ++i)
        bits += (value >> i) & 1;
    return (value ? 0 : 0x40) | (value & 0x80) | ((bits & 1) ? 0 : 4);
}
int main(void)
{
    struct ia32_interruption r;
    bool handled;
    const uint8_t arithmetic[][3] = {
        {0xf0, 0x00, 0x07}, {0xf0, 0x28, 0x07}, {0xf0, 0x10, 0x07}, {0xf0, 0x18, 0x07}};
    for (unsigned operation = 0; operation < 4; ++operation)
        for (unsigned carry = 0; carry <= (operation >= 2); ++carry)
            for (unsigned a = 0; a < 256; ++a)
                for (unsigned b = 0; b < 256; ++b) {
                    struct ia32_native_state n = state();
                    n.eflag |= carry;
                    n.gpr[IA32_EAX] = 0xabcdef00 | b;
                    device = a;
                    CHECK(run(&n, arithmetic[operation], 3, &r, &handled).kind ==
                          IA64_STEP_COMPLETE);
                    bool subtract = operation & 1;
                    int mathematical =
                        subtract ? (int)a - (int)b - (int)carry : (int)a + (int)b + (int)carry;
                    uint8_t result = mathematical;
                    int signed_result = subtract ? (int)(int8_t)a - (int)(int8_t)b - (int)carry
                                                 : (int)(int8_t)a + (int)(int8_t)b + (int)carry;
                    int nibble = subtract ? (int)(a & 15) - (int)(b & 15) - (int)carry
                                          : (int)(a & 15) + (int)(b & 15) + (int)carry;
                    unsigned flags = status(result) | (mathematical < 0 || mathematical > 255) |
                                     ((nibble < 0 || nibble > 15) ? 0x10 : 0) |
                                     ((signed_result < -128 || signed_result > 127) ? 0x800 : 0);
                    CHECK(handled && !r.valid && device == result && (n.eflag & 0x8d5) == flags &&
                          reads == 1 && writes == 1 && begins == 1);
                    CHECK(n.eip.value == 0x1234 &&
                          (n.eflag & ~0x8d5u) == (state().eflag & ~0x8d5u));
                    ++checks;
                }
    const uint8_t logic[][2] = {{0x08, 0x07}, {0x20, 0x07}, {0x30, 0x07}, {0x38, 0x07}};
    for (unsigned operation = 0; operation < 4; ++operation)
        for (unsigned a = 0; a < 256; ++a)
            for (unsigned b = 0; b < 256; ++b) {
                struct ia32_native_state n = state();
                n.eflag |= 0x811;
                n.gpr[0] = b;
                device = a;
                CHECK(run(&n, logic[operation], 2, &r, &handled).kind == IA64_STEP_COMPLETE);
                unsigned value = operation == 0   ? a | b
                                 : operation == 1 ? a & b
                                 : operation == 2 ? a ^ b
                                                  : (a - b) & 255;
                CHECK(handled && reads == 1 && writes == (operation != 3));
                CHECK(device == (operation == 3 ? a : value));
                if (operation != 3)
                    CHECK((n.eflag & 0x8c5) == status(value));
                else
                    CHECK(!!(n.eflag & 0x40) == (a == b));
                ++checks;
            }
    struct ia32_native_state n = state();
    const uint8_t arpl[] = {0x63, 0x0f};
    for (unsigned source = 0; source < 4; ++source)
        for (unsigned destination = 0; destination < 4; ++destination) {
            n = state();
            n.gpr[1] = 0x12345670 | source;
            n.eflag |= 0x8d5;
            device = 0xfff0 | destination;
            CHECK(run(&n, arpl, sizeof(arpl), &r, &handled).kind == IA64_STEP_COMPLETE);
            CHECK(handled && device == (0xfff0 | (source > destination ? source : destination)));
            CHECK(writes == (source > destination) && !!(n.eflag & 0x40) == (source > destination));
            CHECK((n.eflag & ~0x40u) == ((state().eflag | 0x8d5) & ~0x40u));
            ++checks;
        }
    const uint8_t xchg[] = {0x87, 0x07};
    n = state();
    n.gpr[IA32_EAX] = 0x12345678;
    device = 0x87654321;
    CHECK(run(&n, xchg, sizeof(xchg), &r, &handled).kind == IA64_STEP_COMPLETE);
    CHECK(n.gpr[0] == 0x87654321 && device == 0x12345678 && n.eflag == state().eflag);
    const uint8_t cmpxchg[] = {0xf0, 0x0f, 0xb1, 0x0f};
    n = state();
    n.gpr[0] = 4;
    n.gpr[1] = 8;
    device = 5;
    CHECK(run(&n, cmpxchg, sizeof(cmpxchg), &r, &handled).kind == IA64_STEP_COMPLETE);
    CHECK(n.gpr[0] == 5 && device == 5 && writes == 1 && !(n.eflag & 0x40));
    n.gpr[0] = 5;
    CHECK(run(&n, cmpxchg, sizeof(cmpxchg), &r, &handled).kind == IA64_STEP_COMPLETE);
    CHECK(device == 8 && (n.eflag & 0x40));
    const uint8_t xadd[] = {0xf0, 0x0f, 0xc1, 0x0f};
    n = state();
    n.gpr[1] = 7;
    device = 9;
    CHECK(run(&n, xadd, sizeof(xadd), &r, &handled).kind == IA64_STEP_COMPLETE);
    CHECK(n.gpr[1] == 9 && device == 16);
    const uint8_t inc[] = {0xff, 0x07};
    n = state();
    n.eflag |= 1;
    device = UINT32_MAX;
    CHECK(run(&n, inc, sizeof(inc), &r, &handled).kind == IA64_STEP_COMPLETE);
    CHECK(!device && (n.eflag & 1));
    const uint8_t high[] = {0x86, 0x27};
    n = state();
    n.gpr[0] = 0x5566abcd;
    device = 0x12;
    CHECK(run(&n, high, sizeof(high), &r, &handled).kind == IA64_STEP_COMPLETE);
    CHECK(n.gpr[0] == 0x556612cd && device == 0xab);
    const uint8_t sign_imm[] = {0x83, 0x07, 0xff};
    n = state();
    device = 0x80000000;
    CHECK(run(&n, sign_imm, sizeof(sign_imm), &r, &handled).kind == IA64_STEP_COMPLETE);
    CHECK(device == 0x7fffffff && (n.eflag & 0x8d5) == 0x805);
    for (unsigned operation = 0; operation < 4; ++operation)
        for (unsigned a = 0; a < 256; ++a) {
            const uint8_t byte_ops[][2] = {{0xfe, 0x07}, {0xfe, 0x0f}, {0xf6, 0x17}, {0xf6, 0x1f}};
            n = state();
            n.eflag |= 0x8d5;
            device = a;
            CHECK(run(&n, byte_ops[operation], 2, &r, &handled).kind == IA64_STEP_COMPLETE);
            unsigned value = operation == 0   ? (a + 1) & 255
                             : operation == 1 ? (a - 1) & 255
                             : operation == 2 ? a ^ 255
                                              : -a & 255;
            CHECK(device == value && reads == 1 && writes == 1);
            if (operation == 2)
                CHECK(n.eflag == (state().eflag | 0x8d5));
            else {
                CHECK((n.eflag & 0xc4) == status(value));
                CHECK((n.eflag & 1) == (operation < 2 ? 1u : a != 0));
                CHECK(!!(n.eflag & 0x800) == (operation == 0 ? a == 127 : a == 128));
            }
            ++checks;
        }
    const uint32_t edges[] = {0,      1,       15,         16,         0x7f,
                              0x80,   0xff,    0x100,      0x7fff,     0x8000,
                              0xffff, 0x10000, 0x7fffffff, 0x80000000, UINT32_MAX};
    for (unsigned width = 2; width <= 4; width += 2)
        for (unsigned operation = 0; operation < 4; ++operation)
            for (unsigned carry = 0; carry <= (operation >= 2); ++carry)
                for (unsigned ai = 0; ai < sizeof(edges) / sizeof(edges[0]); ++ai)
                    for (unsigned bi = 0; bi < sizeof(edges) / sizeof(edges[0]); ++bi) {
                        uint64_t m = width == 2 ? 0xffff : UINT32_MAX;
                        uint64_t a = edges[ai] & m, b = edges[bi] & m;
                        uint8_t bytes[] = {0x66, (uint8_t)(arithmetic[operation][1] | 1), 0x07};
                        n = state();
                        n.gpr[0] = b;
                        n.eflag |= carry;
                        device = a;
                        CHECK(run(&n, bytes + (width == 4), 3 - (width == 4), &r, &handled).kind ==
                              IA64_STEP_COMPLETE);
                        int64_t mathematical =
                            operation & 1 ? (int64_t)a - b - carry : (int64_t)a + b + carry;
                        int64_t sa = width == 2 ? (int16_t)a : (int32_t)a;
                        int64_t sb = width == 2 ? (int16_t)b : (int32_t)b;
                        int64_t signed_value = operation & 1 ? sa - sb - carry : sa + sb + carry;
                        int64_t max_signed = (INT64_C(1) << (width * 8 - 1)) - 1;
                        uint64_t value = mathematical & m;
                        CHECK(device == value &&
                              (n.eflag & 1) == (mathematical < 0 || mathematical > (int64_t)m));
                        CHECK(!!(n.eflag & 0x800) ==
                              (signed_value < -max_signed - 1 || signed_value > max_signed));
                        CHECK(!!(n.eflag & 0x40) == !value &&
                              !!(n.eflag & 0x80) == !!(value >> (width * 8 - 1)));
                        ++checks;
                    }
    const uint8_t cmp8[] = {0xf0, 0x0f, 0xc7, 0x0f};
    n = state();
    device = UINT64_C(0x1234567887654321);
    CHECK(run(&n, cmp8, sizeof(cmp8), &r, &handled).kind == IA64_STEP_COMPLETE);
    CHECK(n.gpr[0] == 0x87654321 && n.gpr[2] == 0x12345678 && !(n.eflag & 0x40));
    n.gpr[1] = 0x11223344;
    n.gpr[3] = 0x55667788;
    CHECK(run(&n, cmp8, sizeof(cmp8), &r, &handled).kind == IA64_STEP_COMPLETE);
    CHECK(device == UINT64_C(0x1122334455667788) && (n.eflag & 0x40) && writes == 1);
    const uint8_t bts[] = {0xf0, 0x0f, 0xab, 0x0f}, btr[] = {0xf0, 0x0f, 0xb3, 0x0f};
    n = state();
    n.gpr[1] = 40;
    device = 0;
    CHECK(run(&n, bts, sizeof(bts), &r, &handled).kind == IA64_STEP_COMPLETE);
    CHECK(device == 0x100 && !(n.eflag & 1) && expected_offset == 0x104);
    n.gpr[1] = UINT32_MAX;
    device = 0x80000000;
    CHECK(run(&n, btr, sizeof(btr), &r, &handled).kind == IA64_STEP_COMPLETE);
    CHECK(!device && (n.eflag & 1) && expected_offset == 0xfc);
    for (unsigned group = 0; group < 8; ++group)
        for (unsigned count = 0; count < 32; ++count)
            for (unsigned carry = 0; carry < 2; ++carry)
                for (unsigned a = 0; a < 256; ++a) {
                    if (group == 6)
                        continue;
                    uint8_t bytes[] = {0xc0, (uint8_t)(group * 8 + 7), count};
                    n = state();
                    n.eflag |= carry;
                    device = a;
                    CHECK(run(&n, bytes, sizeof(bytes), &r, &handled).kind == IA64_STEP_COMPLETE);
                    unsigned turns = group <= 1 ? count % 8 : group <= 3 ? count % 9 : count;
                    unsigned result = a, cf = carry;
                    for (unsigned i = 0; i < turns; ++i) {
                        unsigned old = result, old_cf = cf;
                        if (group == 0 || group == 2 || group == 4 || group == 6) {
                            cf = old >> 7;
                            result = ((old << 1) & 255) | (group == 0   ? cf
                                                           : group == 2 ? old_cf
                                                                        : 0);
                        } else {
                            cf = old & 1;
                            result = (old >> 1) | (group == 1   ? cf << 7
                                                   : group == 3 ? old_cf << 7
                                                   : group == 7 ? old & 0x80
                                                                : 0);
                        }
                    }
                    if (count && !turns && group <= 1)
                        cf = group == 0 ? result & 1 : result >> 7;
                    CHECK(device == result && reads == 1 && writes == 1);
                    if (group < 4 || count <= 8 || group == 7)
                        CHECK((n.eflag & 1) == cf);
                    if (count == 1) {
                        bool of = group == 0 || group == 2 || group == 4 || group == 6
                                      ? ((result >> 7) ^ cf) & 1
                                  : group == 5 ? a >> 7
                                  : group == 7 ? false
                                               : ((result >> 7) ^ (result >> 6)) & 1;
                        CHECK(((n.eflag >> 11) & 1) == of);
                    }
                    if (group >= 4 && count)
                        CHECK((n.eflag & 0xc4) == status(result));
                    ++checks;
                }
    for (unsigned width = 2; width <= 4; width += 2)
        for (unsigned right = 0; right < 2; ++right)
            for (unsigned use_cl = 0; use_cl < 2; ++use_cl)
                for (unsigned count = 0; count <= (width == 2 ? 16u : 31u); ++count)
                    for (unsigned ai = 0; ai < sizeof(edges) / sizeof(edges[0]); ++ai) {
                        uint64_t m = width == 2 ? 0xffff : UINT32_MAX;
                        unsigned bits = width * 8;
                        uint64_t original = edges[ai] & m, value = original,
                                 source = 0xa5c36978 & m;
                        unsigned cf = 1;
                        for (unsigned i = 0; i < count; ++i) {
                            cf = right ? value & 1 : value >> (bits - 1);
                            value = right ? (value >> 1) | ((source & 1) << (bits - 1))
                                          : ((value << 1) | (source >> (bits - 1))) & m;
                            source = right ? source >> 1 : (source << 1) & m;
                        }
                        uint8_t bytes[] = {0x66, 0x0f, (uint8_t)(0xa4 | (right * 8) | use_cl), 0x17,
                                           count};
                        n = state();
                        n.eflag |= 0x811;
                        n.gpr[2] = 0xa5c36978;
                        n.gpr[1] = count;
                        device = original;
                        CHECK(run(&n, bytes + (width == 4), 5 - (width == 4) - use_cl, &r, &handled)
                                  .kind == IA64_STEP_COMPLETE);
                        CHECK(device == value && (n.eflag & 1) == cf);
                        if (!count)
                            CHECK(n.eflag == (state().eflag | 0x811));
                        else {
                            CHECK((n.eflag & 0x44) ==
                                  ((value ? 0 : 0x40) | (status(value & 255) & 4)));
                            CHECK(!!(n.eflag & 0x80) == !!(value >> (bits - 1)));
                            if (count == 1)
                                CHECK(!!(n.eflag & 0x800) == !!((original ^ value) >> (bits - 1)));
                        }
                        ++checks;
                    }
    fault_at = 2;
    n = state();
    device = 10;
    struct ia32_native_state prior = n;
    CHECK(run(&n, xchg, sizeof(xchg), &r, &handled).kind == IA64_STEP_COMPLETE);
    CHECK(handled && r.valid && !reads && !writes && !begins && !memcmp(&n, &prior, sizeof(n)));
    fault_at = 0;
    fail_write = true;
    CHECK(run(&n, xchg, sizeof(xchg), &r, &handled).kind == IA64_STEP_EXIT);
    CHECK(reads == 1 && writes == 1 && begins == ends && !memcmp(&n, &prior, sizeof(n)));
    fail_write = false;
    fail_read = true;
    CHECK(run(&n, xchg, sizeof(xchg), &r, &handled).kind == IA64_STEP_EXIT);
    CHECK(reads == 1 && !writes && begins == ends && !memcmp(&n, &prior, sizeof(n)));
    fail_read = false;
    const uint8_t unsupported[] = {0x0f, 0xaf, 0x07};
    CHECK(run(&n, unsupported, sizeof(unsupported), &r, &handled).kind == IA64_STEP_COMPLETE);
    CHECK(!handled && !reads && !writes && !begins && !memcmp(&n, &prior, sizeof(n)));
    printf(
        "IA-32 scalar MMIO: %llu exhaustive arithmetic cases, RMW cycles and fault atomicity passed\n",
        (unsigned long long)checks);
    return 0;
}
