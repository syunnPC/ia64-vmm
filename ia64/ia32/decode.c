/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/ia32/decode.h"

struct decoder {
    const uint8_t *bytes;
    size_t available;
    unsigned pos;
    enum ia32_decode_status status;
    struct ia32_decoded d;
    const struct ia32_native_state *n;
    bool operand_override, address_override, segment_override;
    uint8_t repeat;
};
static bool take(struct decoder *c, unsigned count, uint32_t *value)
{
    if (c->pos + count > 15) {
        c->d.overlong = true;
        c->status = IA32_DECODE_ILLEGAL;
        return false;
    }
    if (c->pos + count > c->available) {
        c->status = IA32_DECODE_INCOMPLETE;
        return false;
    }
    *value = 0;
    for (unsigned i = 0; i < count; ++i)
        *value |= (uint32_t)c->bytes[c->pos++] << (8 * i);
    return true;
}
static bool byte(struct decoder *c, uint8_t *value)
{
    uint32_t v;
    if (!take(c, 1, &v))
        return false;
    *value = v;
    return true;
}
static bool illegal(struct decoder *c)
{
    c->status = IA32_DECODE_ILLEGAL;
    return false;
}
static bool unsupported(struct decoder *c)
{
    c->status = IA32_DECODE_UNSUPPORTED;
    return false;
}
static void access(struct decoder *c, enum ia32_access_kind kind, unsigned width)
{
    c->d.access_kind = kind;
    c->d.access_width = width;
    if (kind == IA32_ACCESS_RMW)
        c->d.flags |= IA32_INSN_RMW;
}
static void stack(struct decoder *c)
{
    c->d.flags |= IA32_INSN_STACK | IA32_INSN_MEMORY;
}
static bool modrm(struct decoder *c)
{
    uint8_t m;
    if (!byte(c, &m))
        return false;
    c->d.has_modrm = true;
    c->d.modrm = m;
    unsigned mod = m >> 6, rm = m & 7;
    if (mod == 3)
        return true;
    c->d.operand_memory = true;
    c->d.flags |= IA32_INSN_MEMORY;
    unsigned displacement = mod == 1 ? 1 : mod == 2 ? c->d.address_bits / 8 : 0;
    uint32_t base = 0, disp;
    bool ss = false;
    if (c->d.address_bits == 16) {
        static const unsigned a[8] = {IA32_EBX, IA32_EBX, IA32_EBP, IA32_EBP,
                                      IA32_ESI, IA32_EDI, IA32_EBP, IA32_EBX};
        static const unsigned b[4] = {IA32_ESI, IA32_EDI, IA32_ESI, IA32_EDI};
        if (!mod && rm == 6)
            displacement = 2;
        else {
            base = c->n->gpr[a[rm]];
            if (rm < 4)
                base += c->n->gpr[b[rm]];
            ss = rm == 2 || rm == 3 || rm == 6;
        }
    } else {
        unsigned reg = rm;
        if (rm == 4) {
            uint8_t sib;
            if (!byte(c, &sib))
                return false;
            c->d.has_sib = true;
            c->d.sib = sib;
            unsigned index = (sib >> 3) & 7;
            if (index != 4)
                base += c->n->gpr[index] << (sib >> 6);
            reg = sib & 7;
        }
        if (!mod && reg == 5)
            displacement = 4;
        else {
            base += c->n->gpr[reg];
            ss = reg == IA32_ESP || reg == IA32_EBP;
        }
    }
    if (!take(c, displacement, &disp))
        return false;
    if (displacement == 1)
        disp = (uint32_t)(int32_t)(int8_t)disp;
    c->d.offset = base + disp;
    if (c->d.address_bits == 16)
        c->d.offset &= 0xffff;
    if (!c->segment_override)
        c->d.segment = ss ? IA32_SEG_SS : IA32_SEG_DS;
    return true;
}
static unsigned group(const struct decoder *c)
{
    return (c->d.modrm >> 3) & 7;
}
static bool memory_only(struct decoder *c)
{
    return c->d.operand_memory || illegal(c);
}
static bool x87(struct decoder *c, uint8_t op)
{
    if (!modrm(c))
        return false;
    c->d.flags |= IA32_INSN_FP;
    unsigned reg = group(c), rm = c->d.modrm & 7;
    if (c->d.operand_memory) {
        /* Memory forms from the Vol.4 x87 instruction encoding tables. */
        static const uint8_t valid[8] = {0xff, 0xfd, 0xff, 0xaf, 0xff, 0xdf, 0xff, 0xff};
        if (!(valid[op - 0xd8] & (1u << reg)))
            return illegal(c);
        if ((op == 0xdb || op == 0xdd || op == 0xdf) && reg == 1)
            c->d.native_features |= IA32_NATIVE_SSE3; /* FISTTP */
        enum ia32_access_kind kind = IA32_ACCESS_READ;
        unsigned width = op == 0xdc || op == 0xdd ? 8 : op == 0xde ? 2 : 4;
        if ((op == 0xd9 || op == 0xdb || op == 0xdd || op == 0xdf) &&
            (reg == 1 || reg == 2 || reg == 3 || reg == 6 || reg == 7))
            kind = IA32_ACCESS_WRITE;
        if (op == 0xd9) {
            if (reg == 4 || reg == 6)
                width = c->d.operand_bits == 32 ? 28 : 14;
            else if (reg == 5 || reg == 7)
                width = 2;
        } else if (op == 0xdb && (reg == 5 || reg == 7))
            width = 10;
        else if (op == 0xdd) {
            if (reg == 4 || reg == 6)
                width = c->d.operand_bits == 32 ? 108 : 94;
            else if (reg == 7)
                width = 2;
        } else if (op == 0xdf)
            width = reg == 4 || reg == 6 ? 10 : reg == 5 || reg == 7 ? 8 : 2;
        access(c, kind, width);
        return true;
    }
    switch (op) {
    case 0xd8:
        return true;
    case 0xd9:
        return reg <= 1 || (reg == 2 && !rm) ||
               (reg == 4 && (rm == 0 || rm == 1 || rm == 4 || rm == 5)) || (reg == 5 && rm != 7) ||
               reg >= 6 || illegal(c);
    case 0xda:
        return reg <= 3 || (reg == 5 && rm == 1) || illegal(c);
    case 0xdb:
        if (reg == 5 || reg == 6)
            c->d.flags |= IA32_INSN_WRITE_FLAGS;
        return reg <= 3 || (reg == 4 && (rm == 2 || rm == 3)) || reg == 5 || reg == 6 || illegal(c);
    case 0xdc:
        return reg == 0 || reg == 1 || reg >= 4 || illegal(c);
    case 0xdd:
        return reg == 0 || (reg >= 2 && reg <= 5) || illegal(c);
    case 0xde:
        return reg == 0 || reg == 1 || (reg == 3 && rm == 1) || reg >= 4 || illegal(c);
    case 0xdf:
        if (reg == 5 || reg == 6)
            c->d.flags |= IA32_INSN_WRITE_FLAGS;
        return (reg == 4 && rm == 0) || reg == 5 || reg == 6 || illegal(c);
    }
    return illegal(c);
}
static bool vector(struct decoder *c, uint8_t op)
{
    /* Intel SDM Vol.2D Table A-3: prefix-selected opcode slots. */
    enum { NONE = 1, P66 = 2, PF3 = 4, PF2 = 8 };
    unsigned prefixes = NONE | P66;
    switch (op) {
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x2a:
    case 0x2c:
    case 0x2d:
    case 0x51:
    case 0x58:
    case 0x59:
    case 0x5a:
    case 0x5c:
    case 0x5d:
    case 0x5e:
    case 0x5f:
    case 0x70:
    case 0xc2:
        prefixes = NONE | P66 | PF3 | PF2;
        break;
    case 0x16:
    case 0x5b:
    case 0x6f:
    case 0x7e:
    case 0x7f:
        prefixes = NONE | P66 | PF3;
        break;
    case 0x52:
    case 0x53:
        prefixes = NONE | PF3;
        break;
    case 0x6c:
    case 0x6d:
        prefixes = P66;
        break;
    case 0x7c:
    case 0x7d:
    case 0xd0:
        prefixes = P66 | PF2;
        break;
    case 0xd6:
    case 0xe6:
        prefixes = P66 | PF3 | PF2;
        break;
    case 0xf0:
        prefixes = PF2;
        break;
    }
    if (c->operand_override && c->repeat)
        return unsupported(c);
    unsigned prefix = c->repeat == 0xf2 ? PF2 : c->repeat == 0xf3 ? PF3 :
                      c->operand_override ? P66 : NONE;
    if (!(prefixes & prefix))
        return illegal(c);
    unsigned width = 8;
    bool scalar = c->repeat != 0;
    bool sse = (op >= 0x10 && op <= 0x2f) || (op >= 0x50 && op <= 0x5f) || op == 0xc2 ||
               op == 0xc6 || c->operand_override || c->repeat;
    if (!modrm(c))
        return false;
    c->d.flags |= sse ? IA32_INSN_SSE : IA32_INSN_MMX;
    if ((op == 0x12 && c->repeat) || (op == 0x16 && c->repeat == 0xf3) ||
        op == 0x7c || op == 0x7d || op == 0xd0 || op == 0xf0)
        c->d.native_features |= IA32_NATIVE_SSE3;
    if (sse)
        width = scalar ? (c->repeat == 0xf2 ? 8 : 4) : 16;
    switch (op) {
    case 0x12:
    case 0x13:
    case 0x16:
    case 0x17:
        width = c->repeat == 0xf3 && (op == 0x12 || op == 0x16) ? 16 : 8;
        if ((op == 0x13 || op == 0x17 || (c->operand_override && (op == 0x12 || op == 0x16))) &&
            !memory_only(c))
            return false;
        break;
    case 0x2a:
        width = scalar ? 4 : 8;
        if (!scalar && !c->d.operand_memory)
            c->d.flags |= IA32_INSN_MMX;
        break;
    case 0x2c:
    case 0x2d:
        width = scalar ? (c->repeat == 0xf2 ? 8 : 4) : c->operand_override ? 16 : 8;
        if (!scalar)
            c->d.flags |= IA32_INSN_MMX;
        break;
    case 0x2e:
    case 0x2f:
        width = c->operand_override ? 8 : 4;
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        break;
    case 0x50:
    case 0xd7:
    case 0xc5:
        if (c->d.operand_memory)
            return illegal(c);
        break;
    case 0x6e:
    case 0x7e:
        width = op == 0x7e && c->repeat == 0xf3 ? 8 : 4;
        break;
    case 0x5a:
        width = scalar ? (c->repeat == 0xf2 ? 8 : 4) : c->operand_override ? 16 : 8;
        break;
    case 0x60:
    case 0x61:
    case 0x62:
        width = sse ? 16 : 4;
        break;
    case 0x5b:
    case 0x6f:
    case 0x7f:
    case 0x70:
    case 0x7c:
    case 0x7d:
    case 0xd0:
        width = sse ? 16 : 8;
        break;
    case 0xd6:
        width = c->operand_override ? 8 : 0;
        if (c->repeat) {
            if (c->d.operand_memory)
                return illegal(c);
            c->d.flags |= IA32_INSN_MMX;
        }
        break;
    case 0xe6:
        width = c->repeat == 0xf3 ? 8 : 16;
        break;
    case 0xf0:
        width = 16;
        if (!memory_only(c))
            return false;
        break;
    case 0xc4:
        width = 2;
        break;
    case 0x71:
    case 0x72:
    case 0x73:
        if (c->d.operand_memory ||
            (group(c) != 2 && group(c) != 6 && !(op != 0x73 && group(c) == 4) &&
             !(op == 0x73 && c->operand_override && (group(c) == 3 || group(c) == 7))))
            return illegal(c);
        break;
    case 0xf7:
        if (c->d.operand_memory)
            return illegal(c);
        c->d.flags |= IA32_INSN_MEMORY;
        access(c, IA32_ACCESS_UNKNOWN, 0);
        return true;
    }
    if ((op == 0x2b || op == 0xe7) && !memory_only(c))
        return false;
    bool store = op == 0x11 || op == 0x13 || op == 0x17 || op == 0x29 || op == 0x2b ||
                 (op == 0x7e && c->repeat != 0xf3) || op == 0x7f || op == 0xe7 ||
                 (op == 0xd6 && c->operand_override);
    access(c, store ? IA32_ACCESS_WRITE : IA32_ACCESS_READ, width);
    if (op == 0x70 || (op >= 0x71 && op <= 0x73) || op == 0xc2 || op == 0xc4 || op == 0xc5 ||
        op == 0xc6) {
        uint32_t imm;
        return take(c, 1, &imm);
    }
    return true;
}
static bool two_byte(struct decoder *c)
{
    uint8_t op;
    uint32_t imm;
    unsigned width = c->d.operand_bits / 8;
    if (!byte(c, &op))
        return false;
    c->d.opcode = 0x0f00 | op;
    switch (op) {
    case 0x00:
        if (!modrm(c))
            return false;
        access(c, IA32_ACCESS_READ, 2);
        if (group(c) == 6) {
            c->d.kind = IA32_INSN_JMPE;
            c->d.operand = c->n->gpr[c->d.modrm & 7];
            if (width == 2)
                c->d.operand &= 0xffff;
            access(c, IA32_ACCESS_READ, width);
        } else if (group(c) < 4)
            c->d.kind = IA32_INSN_INTERCEPT;
        else if (group(c) == 7)
            return illegal(c);
        else
            c->d.flags |= IA32_INSN_WRITE_FLAGS;
        return true;
    case 0x01:
        if (!modrm(c))
            return false;
        if (!c->d.operand_memory && c->d.modrm == 0xd0)
            return true; /* XGETBV: extent known; native availability decides execution. */
        if (!c->d.operand_memory && c->d.modrm == 0xf9) {
            c->d.kind = IA32_INSN_CLOCK;
            return true;
        }
        if (group(c) == 5 || (!c->d.operand_memory && group(c) != 4 && group(c) != 6))
            return illegal(c);
        c->d.kind = IA32_INSN_INTERCEPT;
        return true;
    case 0x02:
    case 0x03:
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, IA32_ACCESS_READ, 2);
        return modrm(c);
    case 0x06:
    case 0x08:
    case 0x09:
    case 0x0b:
    case 0x30:
    case 0x32:
    case 0x34:
    case 0x35:
    case 0xaa:
        c->d.kind = IA32_INSN_INTERCEPT;
        return true;
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x23:
        if (!modrm(c))
            return false;
        if (c->d.operand_memory || ((op == 0x20 || op == 0x22) && group(c) != 0 && group(c) != 2 &&
                                    group(c) != 3 && group(c) != 4))
            return illegal(c);
        c->d.kind = op == 0x20 ? IA32_INSN_CONTROL_READ : IA32_INSN_INTERCEPT;
        return true;
    case 0x31:
    case 0x33:
        c->d.kind = IA32_INSN_CLOCK;
        return true;
    case 0x18:
        if (!modrm(c))
            return false;
        if (group(c) > 3 || !memory_only(c))
            return illegal(c);
        /* A hint is not an ordinary load and cannot inherit its fault policy. */
        c->d.flags &= ~IA32_INSN_MEMORY;
        return true;
    case 0x1e:
    case 0x1f:
        if (!modrm(c))
            return false;
        c->d.flags &= ~IA32_INSN_MEMORY;
        return true;
    case 0x38:
    case 0x3a: {
        uint8_t third;
        if (!byte(c, &third) || !modrm(c))
            return false;
        c->d.flags |= IA32_INSN_SSE;
        access(c, IA32_ACCESS_UNKNOWN, 0);
        return op == 0x38 || take(c, 1, &imm);
    }
    case 0x77:
        c->d.flags |= IA32_INSN_MMX;
        return true;
    case 0xa0:
    case 0xa8:
        stack(c);
        return true;
    case 0xa1:
    case 0xa9:
        stack(c);
        c->d.flags |= IA32_INSN_WRITE_SEGMENT;
        c->d.kind = IA32_INSN_SEGMENT;
        return true;
    case 0xa2:
        c->d.kind = IA32_INSN_CPUID;
        return true;
    case 0xa3:
    case 0xab:
    case 0xb3:
    case 0xbb:
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, op == 0xa3 ? IA32_ACCESS_READ : IA32_ACCESS_RMW, width);
        if (!modrm(c))
            return false;
        if (c->d.operand_memory) {
            int32_t index = c->d.operand_bits == 16 ? (int16_t)c->n->gpr[group(c)]
                                                    : (int32_t)c->n->gpr[group(c)];
            int32_t words = index / c->d.operand_bits;
            if (index < 0 && index % c->d.operand_bits)
                --words;
            c->d.offset += (uint32_t)(words * (int32_t)width);
            if (c->d.address_bits == 16)
                c->d.offset &= 0xffff;
        }
        return true;
    case 0xa4:
    case 0xa5:
    case 0xac:
    case 0xad:
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, IA32_ACCESS_RMW, width);
        return modrm(c) && ((op & 1) || take(c, 1, &imm));
    case 0xaf:
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, IA32_ACCESS_READ, width);
        return modrm(c);
    case 0xae:
        if (!modrm(c))
            return false;
        if (!c->d.operand_memory)
            return c->d.modrm == 0xe8 || c->d.modrm == 0xf0 || c->d.modrm == 0xf8 || unsupported(c);
        if (group(c) > 3) {
            c->d.flags |= IA32_INSN_SSE;
            access(c, IA32_ACCESS_UNKNOWN, 0);
            return true;
        }
        if (c->operand_override || c->repeat)
            return illegal(c); /* NP encodings: FXSAVE/FXRSTOR/LDMXCSR/STMXCSR. */
        c->d.flags |= IA32_INSN_SSE;
        if (group(c) < 2)
            c->d.flags |= IA32_INSN_FP;
        access(c, group(c) == 0 || group(c) == 3 ? IA32_ACCESS_WRITE : IA32_ACCESS_READ,
               group(c) < 2 ? 0 : 4); /* FXSAVE/FXRSTOR have noncontiguous effects. */
        return true;
    case 0xb0:
    case 0xb1:
    case 0xc0:
    case 0xc1:
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, IA32_ACCESS_RMW, op & 1 ? width : 1);
        return modrm(c);
    case 0xb2:
    case 0xb4:
    case 0xb5:
        c->d.kind = IA32_INSN_SEGMENT;
        c->d.flags |= IA32_INSN_WRITE_SEGMENT;
        access(c, IA32_ACCESS_READ, width + 2);
        return modrm(c) && memory_only(c);
    case 0xb6:
    case 0xb7:
    case 0xbe:
    case 0xbf:
        access(c, IA32_ACCESS_READ, op & 1 ? 2 : 1);
        return modrm(c);
    case 0xb8:
        if (c->repeat == 0xf3) {
            c->d.flags |= IA32_INSN_WRITE_FLAGS;
            c->d.native_features |= IA32_NATIVE_POPCNT;
            access(c, IA32_ACCESS_READ, width);
            return modrm(c); /* User policy permits native POPCNT. */
        }
        if (c->repeat)
            return unsupported(c);
        c->d.kind = IA32_INSN_JMPE;
        return take(c, width, &c->d.operand);
    case 0xba:
        if (!modrm(c))
            return false;
        if (group(c) < 4)
            return illegal(c);
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, group(c) == 4 ? IA32_ACCESS_READ : IA32_ACCESS_RMW, width);
        return take(c, 1, &imm);
    case 0xbc:
    case 0xbd:
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, IA32_ACCESS_READ, width);
        return modrm(c);
    case 0xc7:
        if (!modrm(c))
            return false;
        if (!c->d.operand_memory && (group(c) == 6 || group(c) == 7)) {
            c->d.flags |= IA32_INSN_WRITE_FLAGS;
            return true;
        }
        if (group(c) != 1 || !memory_only(c))
            return illegal(c);
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, IA32_ACCESS_RMW, 8);
        return true;
    default:
        break;
    }
    if (op >= 0x40 && op <= 0x4f) {
        access(c, IA32_ACCESS_READ, width);
        return modrm(c);
    }
    if (op >= 0x80 && op <= 0x8f) {
        c->d.flags |= IA32_INSN_BRANCH;
        return take(c, width, &imm);
    }
    if (op >= 0x90 && op <= 0x9f) {
        access(c, IA32_ACCESS_WRITE, 1);
        return modrm(c);
    }
    if (op >= 0xc8 && op <= 0xcf)
        return true;
    switch (op) {
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
    case 0x2c:
    case 0x2d:
    case 0x2e:
    case 0x2f:
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
    case 0x58:
    case 0x59:
    case 0x5a:
    case 0x5b:
    case 0x5c:
    case 0x5d:
    case 0x5e:
    case 0x5f:
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x63:
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67:
    case 0x68:
    case 0x69:
    case 0x6a:
    case 0x6b:
    case 0x6c:
    case 0x6d:
    case 0x6e:
    case 0x6f:
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x7c:
    case 0x7d:
    case 0x7e:
    case 0x7f:
    case 0xc2:
    case 0xc4:
    case 0xc5:
    case 0xc6:
    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xd3:
    case 0xd4:
    case 0xd5:
    case 0xd6:
    case 0xd7:
    case 0xd8:
    case 0xd9:
    case 0xda:
    case 0xdb:
    case 0xdc:
    case 0xdd:
    case 0xde:
    case 0xdf:
    case 0xe0:
    case 0xe1:
    case 0xe2:
    case 0xe3:
    case 0xe4:
    case 0xe5:
    case 0xe6:
    case 0xe7:
    case 0xe8:
    case 0xe9:
    case 0xea:
    case 0xeb:
    case 0xec:
    case 0xed:
    case 0xee:
    case 0xef:
    case 0xf0:
    case 0xf1:
    case 0xf2:
    case 0xf3:
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7:
    case 0xf8:
    case 0xf9:
    case 0xfa:
    case 0xfb:
    case 0xfc:
    case 0xfd:
    case 0xfe:
        return vector(c, op);
    default:
        return unsupported(c);
    }
}
static bool vex(struct decoder *c, uint8_t op, uint8_t first)
{
    unsigned map = 1;
    uint8_t last = first, insn;
    uint32_t imm;
    if (op == 0xc4 || op == 0x62) {
        map = first & (op == 0x62 ? 7 : 31);
        if (!byte(c, &last))
            return false;
        if (op == 0x62 && !byte(c, &last))
            return false;
    }
    if (map < 1 || map > 3)
        return unsupported(c);
    if (!byte(c, &insn))
        return false;
    c->d.opcode = 0x0f00 | insn;
    c->d.flags |= IA32_INSN_SSE;
    if (map == 1 && insn == 0x77)
        return true;
    if (!modrm(c))
        return false;
    access(c, IA32_ACCESS_UNKNOWN, 0);
    bool immediate = map == 3 || (map == 1 && (insn == 0x70 || (insn >= 0x71 && insn <= 0x73) ||
                                               insn == 0xc2 || (insn >= 0xc4 && insn <= 0xc6)));
    (void)last;
    return !immediate || take(c, 1, &imm);
}
static bool one_byte(struct decoder *c, uint8_t op)
{
    uint32_t imm;
    unsigned width = c->d.operand_bits / 8;
    if (op == 0x0f)
        return two_byte(c);
    if (op < 0x40) {
        unsigned low = op & 7;
        if (low < 6) {
            c->d.flags |= IA32_INSN_WRITE_FLAGS;
            if (low >= 4)
                return take(c, low == 4 ? 1 : width, &imm);
            access(c, (op & 2) || op >= 0x38 ? IA32_ACCESS_READ : IA32_ACCESS_RMW,
                   op & 1 ? width : 1);
            return modrm(c);
        }
        if (op == 0x27 || op == 0x2f || op == 0x37 || op == 0x3f) {
            c->d.flags |= IA32_INSN_WRITE_FLAGS;
            return true;
        }
        stack(c);
        if (low == 7) {
            c->d.flags |= IA32_INSN_WRITE_SEGMENT;
            c->d.kind = op == 0x17 ? IA32_INSN_SYSTEM_FLAG : IA32_INSN_SEGMENT;
        }
        return true;
    }
    if (op < 0x60) {
        if (op < 0x50)
            c->d.flags |= IA32_INSN_WRITE_FLAGS;
        else
            stack(c);
        return true;
    }
    if (op >= 0x70 && op <= 0x7f) {
        c->d.flags |= IA32_INSN_BRANCH;
        return take(c, 1, &imm);
    }
    if (op >= 0xb0 && op <= 0xbf)
        return take(c, op < 0xb8 ? 1 : width, &imm);
    if (op >= 0xd8 && op <= 0xdf)
        return x87(c, op);
    switch (op) {
    case 0x60:
    case 0x61:
    case 0xc9:
        stack(c);
        return true;
    case 0x62: {
        uint8_t second;
        if (!byte(c, &second))
            return false;
        if ((second >> 6) == 3)
            return vex(c, op, second);
        --c->pos;
        access(c, IA32_ACCESS_READ, width * 2);
        return modrm(c) && memory_only(c);
    }
    case 0x63:
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, IA32_ACCESS_RMW, 2);
        return modrm(c);
    case 0x68:
    case 0x6a:
        stack(c);
        return take(c, op == 0x68 ? width : 1, &imm);
    case 0x69:
    case 0x6b:
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, IA32_ACCESS_READ, width);
        return modrm(c) && take(c, op == 0x69 ? width : 1, &imm);
    case 0x6c:
    case 0x6d:
    case 0x6e:
    case 0x6f:
        c->d.kind = IA32_INSN_IO;
        c->d.flags |= IA32_INSN_MEMORY | IA32_INSN_STRING;
        access(c, op & 2 ? IA32_ACCESS_READ : IA32_ACCESS_WRITE, op & 1 ? width : 1);
        return true;
    case 0x80:
    case 0x81:
    case 0x83:
        if (!modrm(c))
            return false;
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, group(c) == 7 ? IA32_ACCESS_READ : IA32_ACCESS_RMW, op == 0x80 ? 1 : width);
        return take(c, op == 0x81 ? width : 1, &imm);
    case 0x84:
    case 0x85:
    case 0x86:
    case 0x87:
        if (op < 0x86)
            c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, op < 0x86 ? IA32_ACCESS_READ : IA32_ACCESS_RMW, op & 1 ? width : 1);
        return modrm(c);
    case 0x88:
    case 0x89:
    case 0x8a:
    case 0x8b:
        access(c, op & 2 ? IA32_ACCESS_READ : IA32_ACCESS_WRITE, op & 1 ? width : 1);
        return modrm(c);
    case 0x8c:
    case 0x8e:
        if (!modrm(c))
            return false;
        if (group(c) >= 6 || (op == 0x8e && group(c) == 1))
            return illegal(c);
        access(c, op == 0x8c ? IA32_ACCESS_WRITE : IA32_ACCESS_READ, 2);
        if (op == 0x8e) {
            c->d.flags |= IA32_INSN_WRITE_SEGMENT;
            c->d.kind = group(c) == 2 ? IA32_INSN_SYSTEM_FLAG : IA32_INSN_SEGMENT;
        }
        return true;
    case 0x8d:
        if (!modrm(c) || !memory_only(c))
            return false;
        c->d.flags &= ~IA32_INSN_MEMORY;
        return true;
    case 0x8f:
        if (!modrm(c))
            return false;
        if (group(c))
            return illegal(c);
        if (c->d.operand_memory && c->d.has_sib && (c->d.sib & 7) == IA32_ESP) {
            uint32_t old = c->n->gpr[IA32_ESP], next = old + width;
            if (!(c->n->ss.attrib & 0x400))
                next = (old & 0xffff0000) | (next & 0xffff);
            c->d.offset += next - old;
        }
        stack(c);
        access(c, IA32_ACCESS_WRITE, width);
        return true;
    case 0x90:
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97:
    case 0x98:
    case 0x99:
    case 0x9f:
        return true;
    case 0x9a:
    case 0xea:
        c->d.kind = IA32_INSN_FAR;
        c->d.flags |= IA32_INSN_BRANCH;
        if (op == 0x9a)
            stack(c);
        return take(c, width, &imm) && take(c, 2, &imm);
    case 0x9b:
        c->d.flags |= IA32_INSN_FP;
        return true;
    case 0x9c:
    case 0x9d:
        stack(c);
        if (op == 0x9d) {
            c->d.kind = IA32_INSN_SYSTEM_FLAG;
            c->d.flags |= IA32_INSN_WRITE_FLAGS;
        }
        return true;
    case 0x9e:
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        return true;
    case 0xa0:
    case 0xa1:
    case 0xa2:
    case 0xa3:
        c->d.flags |= IA32_INSN_MEMORY;
        c->d.operand_memory = true;
        access(c, op & 2 ? IA32_ACCESS_WRITE : IA32_ACCESS_READ, op & 1 ? width : 1);
        return take(c, c->d.address_bits / 8, &c->d.offset);
    case 0xa4:
    case 0xa5:
    case 0xa6:
    case 0xa7:
    case 0xaa:
    case 0xab:
    case 0xac:
    case 0xad:
    case 0xae:
    case 0xaf:
        c->d.flags |= IA32_INSN_MEMORY | IA32_INSN_STRING;
        if (op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf)
            c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, IA32_ACCESS_UNKNOWN, op & 1 ? width : 1);
        return true;
    case 0xa8:
    case 0xa9:
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        return take(c, op & 1 ? width : 1, &imm);
    case 0xc0:
    case 0xc1:
    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xd3:
        if (!modrm(c))
            return false;
        if (group(c) == 6)
            return illegal(c);
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, IA32_ACCESS_RMW, op & 1 ? width : 1);
        return op >= 0xd0 || take(c, 1, &imm);
    case 0xc2:
    case 0xc3:
    case 0xca:
    case 0xcb:
        stack(c);
        c->d.flags |= IA32_INSN_BRANCH;
        if (op >= 0xca)
            c->d.kind = IA32_INSN_FAR;
        return (op & 1) || take(c, 2, &imm);
    case 0xc4:
    case 0xc5: {
        uint8_t second;
        if (!byte(c, &second))
            return false;
        if ((second >> 6) == 3)
            return vex(c, op, second);
        --c->pos;
        c->d.kind = IA32_INSN_SEGMENT;
        c->d.flags |= IA32_INSN_WRITE_SEGMENT;
        access(c, IA32_ACCESS_READ, width + 2);
        return modrm(c) && memory_only(c);
    }
    case 0xc6:
    case 0xc7:
        if (!modrm(c))
            return false;
        if (group(c))
            return illegal(c);
        access(c, IA32_ACCESS_WRITE, op & 1 ? width : 1);
        return take(c, op & 1 ? width : 1, &imm);
    case 0xc8:
        stack(c);
        return take(c, 2, &imm) && take(c, 1, &imm);
    case 0xcc:
    case 0xcd:
    case 0xce:
        c->d.kind = op == 0xcc ? IA32_INSN_INT3 : op == 0xcd ? IA32_INSN_INT : IA32_INSN_INTO;
        c->d.vector = op == 0xce ? 4 : 3;
        return op != 0xcd || byte(c, &c->d.vector);
    case 0xcf:
    case 0xf1:
    case 0xf4:
        c->d.kind = IA32_INSN_INTERCEPT;
        return true;
    case 0xd4:
    case 0xd5:
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        return take(c, 1, &imm);
    case 0xd7:
        c->d.flags |= IA32_INSN_MEMORY;
        c->d.operand_memory = true;
        c->d.offset = c->n->gpr[IA32_EBX] + (c->n->gpr[IA32_EAX] & 0xff);
        if (c->d.address_bits == 16)
            c->d.offset &= 0xffff;
        access(c, IA32_ACCESS_READ, 1);
        return true;
    case 0xe0:
    case 0xe1:
    case 0xe2:
    case 0xe3:
    case 0xeb:
        c->d.flags |= IA32_INSN_BRANCH;
        return take(c, 1, &imm);
    case 0xe4:
    case 0xe5:
    case 0xe6:
    case 0xe7:
        c->d.kind = IA32_INSN_IO;
        return take(c, 1, &imm);
    case 0xe8:
    case 0xe9:
        c->d.flags |= IA32_INSN_BRANCH;
        if (op == 0xe8)
            stack(c);
        return take(c, width, &imm);
    case 0xec:
    case 0xed:
    case 0xee:
    case 0xef:
        c->d.kind = IA32_INSN_IO;
        return true;
    case 0xf5:
    case 0xf8:
    case 0xf9:
    case 0xfc:
    case 0xfd:
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        return true;
    case 0xfa:
    case 0xfb:
        c->d.kind = IA32_INSN_SYSTEM_FLAG;
        c->d.flags |= IA32_INSN_WRITE_FLAGS;
        return true;
    case 0xf6:
    case 0xf7:
        if (!modrm(c))
            return false;
        if (group(c) == 1)
            return illegal(c);
        if (group(c) != 2)
            c->d.flags |= IA32_INSN_WRITE_FLAGS;
        access(c, group(c) == 2 || group(c) == 3 ? IA32_ACCESS_RMW : IA32_ACCESS_READ,
               op & 1 ? width : 1);
        return group(c) != 0 || take(c, op & 1 ? width : 1, &imm);
    case 0xfe:
    case 0xff:
        if (!modrm(c))
            return false;
        if (group(c) > (op == 0xfe ? 1u : 6u))
            return illegal(c);
        if (group(c) < 2) {
            c->d.flags |= IA32_INSN_WRITE_FLAGS;
            access(c, IA32_ACCESS_RMW, op == 0xfe ? 1 : width);
        } else {
            access(c, IA32_ACCESS_READ, width);
            if (group(c) == 2 || group(c) == 3 || group(c) == 6)
                stack(c);
            if (group(c) != 6)
                c->d.flags |= IA32_INSN_BRANCH;
            if (group(c) == 3 || group(c) == 5) {
                if (!memory_only(c))
                    return false;
                c->d.kind = IA32_INSN_FAR;
                c->d.access_width = width + 2;
            }
        }
        return true;
    default:
        return unsupported(c);
    }
}
static bool lockable(const struct ia32_decoded *d)
{
    unsigned op = d->opcode, reg = (d->modrm >> 3) & 7;
    if (!d->operand_memory)
        return false;
    if (op <= 0x31 && (op & 6) == 0)
        return true;
    if (op == 0x80 || op == 0x81 || op == 0x83)
        return reg < 7;
    if (op == 0x86 || op == 0x87 || op == 0x0fab || op == 0x0fb3 || op == 0x0fbb || op == 0x0fb0 ||
        op == 0x0fb1 || op == 0x0fc0 || op == 0x0fc1)
        return true;
    if (op == 0xf6 || op == 0xf7)
        return reg == 2 || reg == 3;
    if (op == 0xfe || op == 0xff)
        return reg < 2;
    return (op == 0x0fba && reg >= 5) || (op == 0x0fc7 && reg == 1);
}
enum ia32_decode_status ia32_decode(const uint8_t *bytes, size_t available,
                                    const struct ia32_native_state *n, struct ia32_decoded *out)
{
    struct decoder c = {.bytes = bytes,
                        .available = available,
                        .n = n,
                        .d = {.kind = IA32_INSN_NATIVE, .segment = IA32_SEG_DS}};
    uint8_t op;
    *out = (struct ia32_decoded){0};
    for (;;) {
        if (!byte(&c, &op)) {
            if (c.status == IA32_DECODE_ILLEGAL)
                goto result;
            return c.status;
        }
        switch (op) {
        case 0x66:
            c.operand_override = true;
            continue;
        case 0x67:
            c.address_override = true;
            continue;
        case 0xf0:
            c.d.flags |= IA32_INSN_LOCK;
            continue;
        case 0xf2:
        case 0xf3:
            c.repeat = op;
            c.d.flags |= IA32_INSN_REP;
            continue;
        case 0x26:
            c.d.segment = IA32_SEG_ES;
            break;
        case 0x2e:
            c.d.segment = IA32_SEG_CS;
            break;
        case 0x36:
            c.d.segment = IA32_SEG_SS;
            break;
        case 0x3e:
            c.d.segment = IA32_SEG_DS;
            break;
        case 0x64:
            c.d.segment = IA32_SEG_FS;
            break;
        case 0x65:
            c.d.segment = IA32_SEG_GS;
            break;
        default:
            goto opcode;
        }
        c.segment_override = true;
    }
opcode:
    c.d.prefix_length = c.pos - 1;
    bool wide = (n->cs.attrib & 0x400) != 0;
    c.d.operand_bits = wide != c.operand_override ? 32 : 16;
    c.d.address_bits = wide != c.address_override ? 32 : 16;
    c.d.opcode = op;
    c.d.intercept_code = (c.d.operand_bits == 32 ? 2 : 0) | (c.d.address_bits == 32 ? 4 : 0) |
                         (c.d.flags & IA32_INSN_LOCK ? 8 : 0) |
                         (c.repeat == 0xf3   ? 0x10
                          : c.repeat == 0xf2 ? 0x20
                                             : 0) |
                         (c.segment_override ? 0x40 | c.d.segment << 7 : 0) |
                         c.d.prefix_length << 12;
    if (!one_byte(&c, op)) {
        if (c.status == IA32_DECODE_ILLEGAL)
            goto result;
        return c.status;
    }
    if ((c.d.flags & IA32_INSN_LOCK) && !lockable(&c.d))
        c.status = IA32_DECODE_ILLEGAL;
result:
    if (c.status == IA32_DECODE_ILLEGAL)
        c.d.kind = IA32_INSN_INTERCEPT;
    c.d.length = c.pos;
    unsigned bytes_in_iim = c.pos - c.d.prefix_length;
    if (bytes_in_iim > 8)
        bytes_in_iim = 8;
    for (unsigned i = 0; i < bytes_in_iim; ++i)
        c.d.iim |= (uint64_t)bytes[c.d.prefix_length + i] << (8 * i);
    *out = c.d;
    return c.status;
}
