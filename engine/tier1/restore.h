/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_TIER1_RESTORE_H
#define IA64VMM_TIER1_RESTORE_H
#include "engine/tier1/codegen.h"

/* Append bounded C-ABI leaves taking (struct t1_exec *, struct vcpu *).
 * They preserve callee-saved registers; flags are scratch. Entry reads homes
 * through entry_bof and writes only frame_in; exits/checkpoints restore homes
 * from captured values. No absolute addresses; offset zero selects C fallback. */
void t1_restore_emit(const struct t1_mir *m, struct t1_emitted *out);
#endif
