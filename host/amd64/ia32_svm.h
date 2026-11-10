/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_AMD64_IA32_SVM_H
#define IA64VMM_AMD64_IA32_SVM_H
#include "host/amd64/ia32.h"
#include "host/amd64/svm.h"

/* Assembly-owned GPRs; RAX/RSP/RIP are in the VMCB. */
struct amd64_svm_ia32_gprs {
    uint64_t rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t dr[4];
};
_Static_assert(sizeof(struct amd64_svm_ia32_gprs) == 144 &&
                   offsetof(struct amd64_svm_ia32_gprs, dr) == 112,
               "native SVM assembly GPR layout");
struct amd64_svm_ia32_requirements {
    struct ia32_capabilities caps;
    unsigned physical_bits;
    uint64_t pat_flags[3]; /* WB/UC/WC nested leaf bits; UINT64_MAX if absent */
    bool rdpru, tsc_aux, invlpgb;
};
bool amd64_svm_ia32_requirements(struct amd64_svm_ia32_requirements *, struct ia32_setup_error *);
void amd64_svm_ia32_controls(uint8_t *vmcb, const struct svm_vmcb_params *);
bool amd64_svm_ia32_state_valid(const struct ia32_native_state *, const char **detail);
void amd64_svm_ia32_put(uint8_t *vmcb, struct amd64_svm_ia32_gprs *,
                        const struct ia32_native_state *);
/* Virtual CFLG/KRs and IDTR remain in *state; private CRs/MSRs never export. */
void amd64_svm_ia32_get(const uint8_t *vmcb, const struct amd64_svm_ia32_gprs *,
                        struct ia32_native_state *state);
struct ia32_native_exit amd64_svm_ia32_decode(const uint8_t *vmcb, bool nrip, bool decode_assists);
uint64_t amd64_svm_ia32_lock(void);
void amd64_svm_ia32_unlock(uint64_t flags);
void amd64_svm_ia32_run(uint8_t *vmcb, uint8_t *host, struct amd64_svm_ia32_gprs *,
                        struct amd64_ia32_fp_owner *, struct ia32_native_state *, bool tsc_aux);
void amd64_svm_ia32_vmrun(uint64_t vmcb_pa, uint64_t host_pa, struct amd64_svm_ia32_gprs *,
                          struct amd64_ia32_fp_owner *, struct ia32_fp_image *);

/* FSAVE's 28-byte legacy environment and eight ten-byte ST(i) values. */
struct amd64_ia32_fsave {
    uint8_t bytes[108];
};
void amd64_ia32_fp_pack(const struct ia32_fp_image *, struct amd64_ia32_fsave *);
void amd64_ia32_fp_unpack(const struct amd64_ia32_fsave *, struct ia32_fp_image *);
bool amd64_ia32_fp_prepare(struct amd64_ia32_fp_owner *, const struct ia32_fp_image *,
                           struct ia32_setup_error *);
bool amd64_ia32_fp_host_admit(struct amd64_ia32_fp_owner *, struct ia32_setup_error *);
uint64_t amd64_ia32_fp_host_inuse(void);
void amd64_ia32_fp_complete(struct amd64_ia32_fp_owner *, struct ia32_fp_image *);
void amd64_ia32_fp_roundtrip(struct amd64_ia32_fp_owner *, struct ia32_fp_image *);
#endif
