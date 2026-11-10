/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_AMD64_IA32_NATIVE_H
#define IA64VMM_AMD64_IA32_NATIVE_H
#include "host/amd64/ia32.h"
#include "host/amd64/simd.h"

struct amd64_ia32_map {
    uint64_t root;
    /* Enough tables for the complete 4 GiB native linear space. Sparse
     * higher token mappings consume the same bounded pool. */
    void *pages[2054];
    unsigned count, physical_bits;
    enum ia32_backend_kind backend;
    bool execute_only;
};
/* Intel SDM 26.5.2: MTF outranks NMI/INTR. AMD APM 8.5/15.7:
 * TF trap recognition/interception likewise precedes NMI/INTR. With a
 * qualified step, no shadow/event injection and no vectoring in progress,
 * normal host exits therefore precede the instruction's completed boundary. */
static inline void amd64_ia32_step_host_phase(struct ia32_native_exit *e, bool qualified_step)
{
    if (qualified_step && !e->vectoring.valid &&
        (e->reason == IA32_EXIT_HOST_IRQ || e->reason == IA32_EXIT_HOST_NMI ||
         e->reason == IA32_EXIT_DEADLINE))
        e->phase = IA32_PHASE_BEFORE;
}
/* Both nested formats use the same 4 KiB table address field. Retain
 * empty tables for reuse and visit allocated tables rather than each page
 * in a potentially large absent range. No large leaves are installed. */
static inline void amd64_ia32_clear_tables(uint64_t *table, unsigned level, uint64_t base,
                                           uint64_t begin, uint64_t end, uint64_t hhdm)
{
    uint64_t span = UINT64_C(1) << (12 + 9 * (level - 1));
    for (unsigned i = 0; i < 512; ++i) {
        uint64_t at = base + i * span;
        if (at >= end)
            break;
        if (at + span <= begin || !table[i])
            continue;
        if (level == 1)
            table[i] = 0;
        else {
            uint64_t *next = (void *)(uintptr_t)(hhdm + (table[i] & UINT64_C(0x000ffffffffff000)));
            amd64_ia32_clear_tables(next, level - 1, at, begin, end, hhdm);
        }
    }
}
bool amd64_ia32_map_init(struct amd64_ia32_map *, enum ia32_backend_kind, bool execute_only,
                         unsigned physical_bits, struct ia32_setup_error *);
bool amd64_ia32_map_set(struct amd64_ia32_map *, const struct ia32_mapping *,
                        struct ia32_setup_error *);
bool amd64_ia32_map_unset(struct amd64_ia32_map *, ia32_token_t, uint64_t bytes,
                          struct ia32_setup_error *);
void amd64_ia32_map_drop(struct amd64_ia32_map *);
/* Pure leaf encoder; zero rights means an absent leaf. */
bool amd64_ia32_map_leaf(enum ia32_backend_kind, bool execute_only, unsigned physical_bits,
                         const struct ia32_mapping *, uint64_t *leaf);
bool amd64_ia32_fp_prepare(struct amd64_ia32_fp_owner *, const struct ia32_fp_image *,
                           struct ia32_setup_error *);
bool amd64_ia32_fp_host_admit(struct amd64_ia32_fp_owner *, struct ia32_setup_error *);
void amd64_ia32_fp_complete(struct amd64_ia32_fp_owner *, struct ia32_fp_image *);
#endif
