/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_SAL_H
#define VMM_SAL_H
#include "../acpi/tables.h"
#include "../ap.h"
#include "mc.h"
#define SAL_TABLE_CAPACITY 8192u
struct sal_call_result {
    int64_t status;
    uint64_t v0, v1, v2;
};
struct sal_vector {
    uint64_t entry[2], gp[2], size[2];
};
struct sal_state {
    uint64_t ecam_size, pal_entry;
    volatile uint8_t *ecam;
    uint32_t bus_first, bus_last;
    struct sal_vector vectors[3];
    /* SAL §9: rendezvous, wakeup and CPE parameters apply system-wide. */
    uint64_t mc_params[3][4];
    /* OS_BOOT_RENDEZ consumer (ap.S); NULL when it cannot be reached. */
    volatile struct ap_wake_block *ap;
    /* OS_MCA/OS_INIT consumer (sal/entry.S); NULL when it cannot be reached. */
    volatile struct sal_mc *mc;
    bool ecam_available;
};
struct sal_memory {
    uint64_t base, size;
    uint8_t attribute, usage;
};
struct sal_tr {
    uint64_t address, itir;
    uint8_t kind, index;
};
struct sal_system_table {
    uint8_t bytes[SAL_TABLE_CAPACITY];
    size_t used;
};
void sal_init(struct sal_state *state, const struct ia64vmm_handoff *handoff, uint64_t pal);
bool sal_ecam(const struct sal_state *state, uint64_t address, uint64_t width, uint64_t mode,
              uint64_t *offset);
struct sal_call_result sal_dispatch(struct sal_state *state, uint64_t index, uint64_t a, uint64_t b,
                                    uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g);
bool sal_build(struct sal_system_table *table, uint64_t base, uint64_t pal, uint64_t sal,
               uint64_t gp, unsigned cpus, const struct sal_memory *memory, size_t memory_count,
               const struct sal_tr *trs, size_t tr_count);
#endif
