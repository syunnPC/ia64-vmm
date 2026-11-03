/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_PAL_H
#define IA64VMM_IA64_PAL_H
#include <stdint.h>
#include <stdbool.h>

/* Intel SDM Vol.2, PAL procedure table. Bit 8 selects stacked arguments. */
enum ia64_pal_index {
    PAL_CACHE_FLUSH = 1,
    PAL_CACHE_INFO,
    PAL_CACHE_INIT,
    PAL_CACHE_SUMMARY,
    PAL_MEM_ATTRIB,
    PAL_PTCE_INFO,
    PAL_VM_INFO,
    PAL_VM_SUMMARY,
    PAL_BUS_GET_FEATURES,
    PAL_BUS_SET_FEATURES,
    PAL_DEBUG_INFO,
    PAL_FIXED_ADDR,
    PAL_FREQ_BASE,
    PAL_FREQ_RATIOS,
    PAL_PERF_MON_INFO,
    PAL_PLATFORM_ADDR,
    PAL_PROC_GET_FEATURES,
    PAL_PROC_SET_FEATURES,
    PAL_RSE_INFO,
    PAL_VERSION,
    PAL_MC_CLEAR_LOG,
    PAL_MC_DRAIN,
    PAL_MC_EXPECTED,
    PAL_MC_DYNAMIC_STATE,
    PAL_MC_ERROR_INFO,
    PAL_MC_RESUME,
    PAL_MC_REGISTER_MEM,
    PAL_HALT,
    PAL_HALT_LIGHT,
    PAL_COPY_INFO,
    PAL_CACHE_LINE_INIT,
    PAL_PMI_ENTRYPOINT,
    PAL_VM_PAGE_SIZE = 34,
    PAL_MEM_FOR_TEST = 37,
    PAL_CACHE_PROT_INFO,
    PAL_REGISTER_INFO,
    PAL_PREFETCH_VIS = 41,
    PAL_LOGICAL_TO_PHYSICAL,
    PAL_CACHE_SHARED_INFO,
    PAL_COPY_PAL = 256,
    PAL_HALT_INFO,
    PAL_TEST_PROC,
    PAL_VM_TR_READ = 261,
    PAL_BRAND_INFO = 274,
    /* Current SDM name; retain the older name for existing callers. */
    PAL_TEST_INFO = PAL_MEM_FOR_TEST,
    PAL_ENTER_IA_32_ENV = 33,
};
/* No bus parking/lock masking, instruction-cache coherence filtering,
 * exclusive prefetch or hardware threading controls are implemented. */
#define PAL_BUS_FEATURES UINT64_C(0)
#define PAL_PROC_FEATURES UINT64_C(0)
#define PAL_COPY_SIZE UINT64_C(4096)
#define PAL_VERSION_VALUE UINT64_C(0x0000096801000968)

/* Kept inside architectural state so reset discards copies and registrations.
 * Feature-control slots remain zero while those controls are unavailable. */
struct ia64_pal_state {
    uint64_t copy_addr, interrupt_block_addr, io_block_addr, mc_save_addr, pmi_entry;
    uint64_t bus_disabled, proc_disabled;
    bool copy_valid, mc_expected;
};
struct vcpu;
struct ia64_result;
bool ia64_pal_entry_matches(const struct vcpu *v, uint64_t ip);
/* Procedure helper, also usable with a constructed CFM before br.call exists. */
struct ia64_result ia64_pal_dispatch(struct vcpu *v);
/* Stacked portal completion uses the same real RSE transition as br.ret. */
bool ia64_pal_stacked_ready(const struct vcpu *v);
struct ia64_result ia64_pal_return_frame(struct vcpu *v);
struct ia64_result ia64_pal_break(struct vcpu *v);
/* SDM Vol.2 Fig.4-5 / PAL_VM_TR_READ; shared by the portal and MMIO service. */
void ia64_pal_capture_state(struct vcpu *v);
bool ia64_pal_tr_snapshot(const struct vcpu *v, uint64_t index, uint64_t side, uint64_t words[4],
                          uint64_t *valid);
#endif
