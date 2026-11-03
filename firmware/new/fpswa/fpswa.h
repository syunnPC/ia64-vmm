/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FW_FPSWA_H
#define FW_FPSWA_H
#include <stdint.h>

/* Intel 245415-001 §7.2: masks refer to physical registers, spill format. */
struct fpswa_fr {
    uint64_t significand, sign_exponent;
};
struct fpswa_state {
    uint64_t mask_low, mask_high;
    struct fpswa_fr *low_preserved, *low_volatile;
    struct fpswa_fr *high_preserved, *high_volatile;
};
struct fpswa_ret {
    uint64_t status, err1, err2, err3;
};
typedef struct fpswa_ret (*fpswa_fn)(uint64_t, void *, uint64_t *, uint64_t *, uint64_t *,
                                     uint64_t *, uint64_t *, void *);
struct fpswa_interface {
    uint32_t revision, reserved;
    fpswa_fn fpswa;
};
#define FPSWA_REVISION 0x00010000u
#define FPSWA_GUID {0xc41b6531, 0x97b9, 0x11d3, {0x9a, 0x29, 0, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}
/* Private assembly/C boundary. live[] is captured before any compiler code
 * can use volatile FRs. Changes to omitted registers go back through live[]. */
struct fpswa_call {
    uint64_t trap_type;
    const uint64_t *bundle;
    uint64_t *ipsr, *fpsr, *isr, *preds, *ifs;
    struct fpswa_state *state;
    struct fpswa_fr live[128];
};
_Static_assert(sizeof(struct fpswa_state) == 48, "FP_STATE ABI");
_Static_assert(sizeof(struct fpswa_ret) == 32, "r8-r11 return ABI");
_Static_assert(sizeof(struct fpswa_interface) == 16, "FPSWA_INTERFACE ABI");
struct fpswa_ret fpswa_emulate(struct fpswa_call *call);
struct fpswa_ret fpswa_emulation_entry(uint64_t, void *, uint64_t *, uint64_t *, uint64_t *,
                                       uint64_t *, uint64_t *, void *);
#endif
