/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Intel SDM Vol.2 scalar arithmetic, bit and RMW instruction operations. */
#include "ia64/ia32/scalar.h"
#include "ia64/ia32/access.h"
#include "ia64/memory.h"

#define STATUS UINT32_C(0x8d5)
enum operation {
    ALU,
    INCDEC,
    NEGNOT,
    EXCHANGE,
    BIT,
    COMPARE_EXCHANGE,
    COMPARE8,
    ADD_EXCHANGE,
    SHIFT,
    DOUBLE_SHIFT,
    ADJUST_RPL
};
struct scalar {
    enum operation operation;
    unsigned group, bytes, reg, count;
    uint32_t offset;
    uint64_t source;
    bool write;
};
static uint64_t mask(unsigned bytes)
{
    return bytes == 8 ? UINT64_MAX : (UINT64_C(1) << (8 * bytes)) - 1;
}
static uint64_t reg_read(const struct ia32_native_state *n, unsigned reg, unsigned bytes)
{
    return bytes == 1 && reg >= 4 ? (n->gpr[reg - 4] >> 8) & 255 : n->gpr[reg] & mask(bytes);
}
static void reg_write(struct ia32_native_state *n, unsigned reg, unsigned bytes, uint64_t value)
{
    if (bytes == 1 && reg >= 4)
        n->gpr[reg - 4] = (n->gpr[reg - 4] & ~0xff00u) | ((value & 255) << 8);
    else
        n->gpr[reg] = (n->gpr[reg] & ~(uint32_t)mask(bytes)) | (uint32_t)(value & mask(bytes));
}
static uint64_t immediate(const uint8_t *bytes, unsigned length, unsigned count)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= (uint64_t)bytes[length - count + i] << (8 * i);
    return value;
}
bool ia32_scalar_supported(const struct ia32_decoded *d)
{
    if (!d || !d->operand_memory || !d->has_modrm)
        return false;
    unsigned op = d->opcode, group = (d->modrm >> 3) & 7;
    if (op == 0x0fc7 && group == 1)
        return d->access_width == 8;
    if (d->access_width != 1 && d->access_width != 2 && d->access_width != 4)
        return false;
    return (op <= 0x39 && (op & 7) <= 1) || op == 0x80 || op == 0x81 || op == 0x83 || op == 0x86 ||
           op == 0x87 || ((op == 0xfe || op == 0xff) && group <= 1) ||
           ((op == 0xf6 || op == 0xf7) && (group == 2 || group == 3)) ||
           ((op == 0xc0 || op == 0xc1 || (op >= 0xd0 && op <= 0xd3)) && group != 6) ||
           op == 0x0fa3 || op == 0x0fab || op == 0x0fb3 || op == 0x0fbb ||
           (op == 0x0fba && group >= 4) || op == 0x0fb0 || op == 0x0fb1 || op == 0x0fc0 ||
           op == 0x0fc1 || op == 0x0fa4 || op == 0x0fa5 || op == 0x0fac || op == 0x0fad ||
           (op == 0x63 && d->access_width == 2);
}
static struct scalar plan(const struct ia32_native_state *n, const struct ia32_decoded *d,
                          const uint8_t *bytes)
{
    unsigned op = d->opcode, bits = 8 * d->access_width;
    struct scalar p = {.operation = ALU,
                       .bytes = d->access_width,
                       .reg = (d->modrm >> 3) & 7,
                       .offset = d->offset,
                       .write = true};
    p.source = reg_read(n, p.reg, p.bytes > 4 ? 4 : p.bytes);
    if (op <= 0x39)
        p.group = (op >> 3) & 7;
    else if (op == 0x80 || op == 0x81 || op == 0x83) {
        p.group = p.reg;
        p.source = immediate(bytes, d->length, op == 0x81 ? p.bytes : 1);
        if (op == 0x83)
            p.source = (uint64_t)(int64_t)(int8_t)p.source & mask(p.bytes);
    } else if (op == 0x63)
        p.operation = ADJUST_RPL;
    else if (op == 0x86 || op == 0x87)
        p.operation = EXCHANGE;
    else if (op == 0xfe || op == 0xff) {
        p.operation = INCDEC;
        p.group = p.reg;
    } else if (op == 0xf6 || op == 0xf7) {
        p.operation = NEGNOT;
        p.group = p.reg;
    } else if (op == 0x0fb0 || op == 0x0fb1)
        p.operation = COMPARE_EXCHANGE;
    else if (op == 0x0fc7)
        p.operation = COMPARE8;
    else if (op == 0x0fc0 || op == 0x0fc1)
        p.operation = ADD_EXCHANGE;
    else if (op == 0x0fa4 || op == 0x0fa5 || op == 0x0fac || op == 0x0fad) {
        p.operation = DOUBLE_SHIFT;
        p.group = (op & 8) != 0;
        p.count = (op & 1) ? n->gpr[IA32_ECX] & 31 : immediate(bytes, d->length, 1) & 31;
    } else if (op == 0xc0 || op == 0xc1 || (op >= 0xd0 && op <= 0xd3)) {
        p.operation = SHIFT;
        p.group = p.reg;
        p.count = (op == 0xc0 || op == 0xc1) ? immediate(bytes, d->length, 1) & 31
                  : op <= 0xd1               ? 1
                                             : n->gpr[IA32_ECX] & 31;
    } else {
        p.operation = BIT;
        p.group = op == 0x0fba ? p.reg - 4 : (op >> 3) & 3;
        if (op == 0x0fba)
            p.count = immediate(bytes, d->length, 1) & (bits - 1);
        else {
            p.count = (unsigned)p.source & (bits - 1);
        }
        p.write = p.group != 0;
    }
    if (p.operation == ALU && p.group == 7)
        p.write = false;
    return p;
}
static uint32_t szp(uint64_t value, unsigned bytes)
{
    unsigned parity = (unsigned)value & 255;
    parity ^= parity >> 4;
    parity ^= parity >> 2;
    parity ^= parity >> 1;
    return (!value ? 0x40 : 0) | ((value >> (8 * bytes - 1)) ? 0x80 : 0) | (!(parity & 1) ? 4 : 0);
}
static uint64_t arithmetic(struct ia32_native_state *n, uint64_t a, uint64_t b, unsigned bytes,
                           bool subtract, unsigned carry)
{
    uint64_t m = mask(bytes), sign = UINT64_C(1) << (8 * bytes - 1);
    uint64_t full = subtract ? a - b - carry : a + b + carry, result = full & m;
    bool cf = subtract ? a < b + carry : full > m;
    bool of = ((subtract ? a ^ b : ~(a ^ b)) & (a ^ result) & sign) != 0;
    n->eflag = (n->eflag & ~STATUS) | szp(result, bytes) | cf | (of ? 0x800 : 0) |
               (((a ^ b ^ result) & 16) ? 0x10 : 0);
    return result;
}
static uint64_t alu(struct ia32_native_state *n, uint64_t a, uint64_t b, unsigned bytes,
                    unsigned group)
{
    unsigned carry = n->eflag & 1;
    switch (group) {
    case 0:
        return arithmetic(n, a, b, bytes, false, 0);
    case 2:
        return arithmetic(n, a, b, bytes, false, carry);
    case 3:
        return arithmetic(n, a, b, bytes, true, carry);
    case 5:
    case 7:
        return arithmetic(n, a, b, bytes, true, 0);
    default: {
        uint64_t result = group == 1 ? a | b : group == 4 ? a & b : a ^ b;
        n->eflag = (n->eflag & ~STATUS) | szp(result, bytes);
        return result;
    }
    }
}
static uint64_t shift(struct ia32_native_state *n, uint64_t a, struct scalar p)
{
    unsigned bits = 8 * p.bytes, count = p.count, group = p.group;
    uint64_t m = mask(p.bytes), sign = UINT64_C(1) << (bits - 1), result = a;
    bool cf = false, of = false;
    if (group <= 1)
        count %= bits;
    else if (group <= 3)
        count %= bits + 1;
    if (!count) {
        /* ROL/ROR full operand cycles still publish the final shifted
         * bit when the masked count was nonzero. */
        if (p.count && group <= 1)
            n->eflag = (n->eflag & ~1u) | (group == 0 ? a & 1 : !!(a & sign));
        return a;
    }
    switch (group) {
    case 0:
        result = ((a << count) | (a >> (bits - count))) & m;
        cf = result & 1;
        of = !!(result & sign) != cf;
        break;
    case 1:
        result = ((a >> count) | (a << (bits - count))) & m;
        cf = result & sign;
        of = !!(result & sign) != !!(result & (sign >> 1));
        break;
    case 2:
    case 3: {
        uint64_t wide = a | ((uint64_t)(n->eflag & 1) << bits);
        unsigned total = bits + 1;
        wide = group == 2 ? (wide << count) | (wide >> (total - count))
                          : (wide >> count) | (wide << (total - count));
        result = wide & m;
        cf = (wide >> bits) & 1;
        of = group == 2 ? !!(result & sign) != cf : !!(result & sign) != !!(result & (sign >> 1));
        break;
    }
    case 4:
    case 6:
        result = count < bits ? (a << count) & m : 0;
        cf = count <= bits ? (a >> (bits - count)) & 1 : 0;
        of = !!(result & sign) != cf;
        break;
    case 5:
        result = count < bits ? a >> count : 0;
        cf = count <= bits ? (a >> (count - 1)) & 1 : 0;
        of = a & sign;
        break;
    default:
        cf = count < bits ? (a >> (count - 1)) & 1 : !!(a & sign);
        result = count >= bits ? ((a & sign) ? m : 0)
                               : (a >> count) | ((a & sign) ? m ^ (m >> count) : 0);
        break;
    }
    n->eflag = (n->eflag & ~1u) | cf;
    if (p.count == 1)
        n->eflag = (n->eflag & ~0x800u) | (of ? 0x800 : 0);
    if (group >= 4)
        n->eflag = (n->eflag & ~0xc4u) | szp(result, p.bytes);
    return result;
}
struct ia64_result ia32_scalar(struct vcpu *v, struct ia32_native_state *n,
                               const struct ia32_decoded *d, const uint8_t *bytes, bool *handled,
                               struct ia32_interruption *record)
{
    *handled = ia32_scalar_supported(d);
    if (!*handled)
        return ia64_complete();
    struct scalar p = plan(n, d, bytes);
    struct ia64_result result =
        ia32_operand_prepare(v, n, d->segment, p.offset, p.bytes, false, p.bytes, record);
    if (result.kind != IA64_STEP_COMPLETE || record->valid)
        return result;
    if (p.write) {
        result = ia32_operand_prepare(v, n, d->segment, p.offset, p.bytes, true, p.bytes, record);
        if (result.kind != IA64_STEP_COMPLETE || record->valid)
            return result;
    }
    struct ia32_native_state candidate = *n;
    uint64_t a = 0, value = 0;
    ia64_device_rmw_begin();
    result = ia32_operand(v, n, d->segment, p.offset, p.bytes, false, &a, record);
    if (result.kind != IA64_STEP_COMPLETE || record->valid)
        goto done;
    switch (p.operation) {
    case ALU:
        value = alu(&candidate, a, p.source, p.bytes, p.group);
        break;
    case ADJUST_RPL:
        p.write = (p.source & 3) > (a & 3);
        value = (a & ~UINT64_C(3)) | (p.source & 3);
        candidate.eflag = (candidate.eflag & ~0x40u) | (p.write ? 0x40 : 0);
        break;
    case INCDEC: {
        unsigned cf = candidate.eflag & 1;
        value = arithmetic(&candidate, a, 1, p.bytes, p.group != 0, 0);
        candidate.eflag = (candidate.eflag & ~1u) | cf;
        break;
    }
    case NEGNOT:
        value = p.group == 2 ? ~a & mask(p.bytes) : arithmetic(&candidate, 0, a, p.bytes, true, 0);
        break;
    case EXCHANGE:
        value = p.source;
        reg_write(&candidate, p.reg, p.bytes, a);
        break;
    case BIT:
        candidate.eflag = (candidate.eflag & ~1u) | ((a >> p.count) & 1);
        value = p.group == 1   ? a | (UINT64_C(1) << p.count)
                : p.group == 2 ? a & ~(UINT64_C(1) << p.count)
                               : a ^ (UINT64_C(1) << p.count);
        break;
    case COMPARE_EXCHANGE: {
        uint64_t accumulator = reg_read(n, IA32_EAX, p.bytes);
        arithmetic(&candidate, accumulator, a, p.bytes, true, 0);
        value = accumulator == a ? p.source : a;
        if (accumulator != a)
            reg_write(&candidate, IA32_EAX, p.bytes, a);
        break;
    }
    case COMPARE8: {
        uint64_t accumulator = n->gpr[IA32_EAX] | (uint64_t)n->gpr[IA32_EDX] << 32;
        bool equal = accumulator == a;
        candidate.eflag = (candidate.eflag & ~0x40u) | (equal ? 0x40 : 0);
        value = equal ? n->gpr[IA32_EBX] | (uint64_t)n->gpr[IA32_ECX] << 32 : a;
        if (!equal) {
            candidate.gpr[IA32_EAX] = a;
            candidate.gpr[IA32_EDX] = a >> 32;
        }
        break;
    }
    case ADD_EXCHANGE:
        value = arithmetic(&candidate, a, p.source, p.bytes, false, 0);
        reg_write(&candidate, p.reg, p.bytes, a);
        break;
    case SHIFT:
        value = shift(&candidate, a, p);
        break;
    case DOUBLE_SHIFT: {
        value = a;
        if (p.count) {
            unsigned bits = 8 * p.bytes;
            uint64_t pair = p.group ? (p.source << bits) | a : (a << bits) | p.source;
            value = (p.group ? pair >> p.count : pair << p.count >> bits) & mask(p.bytes);
            bool cf = p.count <= bits
                          ? (p.group ? (a >> (p.count - 1)) & 1 : (a >> (bits - p.count)) & 1)
                          : 0;
            candidate.eflag = (candidate.eflag & ~0xc5u) | szp(value, p.bytes) | cf;
            if (p.count == 1) {
                bool of = ((a ^ value) >> (bits - 1)) & 1;
                candidate.eflag = (candidate.eflag & ~0x800u) | (of ? 0x800 : 0);
            }
        }
        break;
    }
    }
    if (p.write) {
        result = ia32_operand(v, n, d->segment, p.offset, p.bytes, true, &value, record);
        if (result.kind != IA64_STEP_COMPLETE || record->valid)
            goto done;
    }
    *n = candidate;
done:
    ia64_device_rmw_end();
    return result;
}
