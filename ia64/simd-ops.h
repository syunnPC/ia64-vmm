/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Ported from the syunnPC/qemu-system-ia64 QEMU target, commit 5f67e29.
 * QEMU dependencies removed for ia64-vmm.
 *
 * Pure IA-64 packed-integer operations.
 */

#ifndef TARGET_IA64_ARCH_SIMD_OPS_H
#define TARGET_IA64_ARCH_SIMD_OPS_H

#include <stdbool.h>
#include <stdint.h>

uint64_t ia64_simd_pavg_value(uint32_t op_sel, uint64_t a, uint64_t b);
uint64_t ia64_simd_pcmp_value(uint32_t op_sel, uint64_t a, uint64_t b);
uint64_t ia64_simd_pminmax_value(uint32_t op_sel, uint64_t a, uint64_t b);
uint64_t ia64_simd_pmpy_value(uint32_t op_sel, uint64_t a, uint64_t b, uint32_t shift);
uint64_t ia64_simd_psad1_value(uint64_t a, uint64_t b);
uint64_t ia64_simd_pshladd2_value(uint64_t a, uint64_t b, uint32_t count);
uint64_t ia64_simd_mux_value(uint32_t op_sel, uint64_t value, uint32_t imm);
uint64_t ia64_simd_mix_value(uint32_t op_sel, uint64_t a, uint64_t b);
uint64_t ia64_simd_unpack_value(uint32_t op_sel, uint64_t a, uint64_t b);
uint64_t ia64_simd_pack_value(uint32_t op_sel, uint64_t a, uint64_t b);
uint64_t ia64_simd_czx_value(uint32_t op_sel, uint64_t value);

#endif /* TARGET_IA64_ARCH_SIMD_OPS_H */
