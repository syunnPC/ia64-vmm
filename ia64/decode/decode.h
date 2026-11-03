/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Ported from the syunnPC/qemu-system-ia64 QEMU target, commit 5f67e29.
 * QEMU dependencies removed for ia64-vmm.
 *
 * TCG-independent IA-64 instruction decoder interface.
 */

#ifndef TARGET_IA64_DECODE_DECODE_H
#define TARGET_IA64_DECODE_DECODE_H

#include "ia64/decode/operand.h"

Ia64Instruction ia64_decode_insn(IA64SlotUnit unit, uint64_t raw, uint64_t address, uint8_t slot);
void ia64_apply_mlx_long_fixup(uint8_t template_code, const uint64_t slots[IA64_BUNDLE_SLOTS],
                               int slot, Ia64Instruction *insn, bool *skip_x_slot);

int64_t ia64_sign_extend(uint64_t value, unsigned width);
static inline uint64_t ia64_low_mask(uint64_t len)
{
    return len >= 64 ? UINT64_MAX : ((1ULL << len) - 1);
}

static inline uint64_t ia64_bitfield_effective_len(uint64_t pos, uint64_t len)
{
    pos &= 0x3f;
    return len > 64 - pos ? 64 - pos : len;
}

static inline uint64_t ia64_deposit_mask(uint64_t pos, uint64_t len)
{
    len = ia64_bitfield_effective_len(pos, len);
    return ia64_low_mask(len) << (pos & 0x3f);
}

bool ia64_opcode_is_store(Ia64Opcode opcode);
bool ia64_opcode_is_release_store(Ia64Opcode opcode);
bool ia64_opcode_is_load(Ia64Opcode opcode);

#endif /* TARGET_IA64_DECODE_DECODE_H */
