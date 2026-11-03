/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SALE_ENTRY with SAL_INIT and SAL_CHECK, the SAL PMI handler, and the
 * per-processor registrations SAL makes with PAL (SAL 245359-005 §4.1.1,
 * §4.8, §5, §6; SDM Rev 2.3 Vol.2 §11.1.4). SALE_ENTRY
 * runs in physical mode from PALE_INIT/PALE_CHECK: bank 0, PSR.mc = 1,
 * GR16-GR20 PAL's parameters; GR1-GR15, bank 0 GR21-GR31, the predicates,
 * BR0 and BR1 are saved in the min-state area and free. Everything else is
 * the interrupted context's and stays untouched: no other branch, floating-
 * point or application register, and no register stack. Warm and cold
 * boots are the VMM's restart service.
 */
#include "mc.h"
#define SVC_RESET_STATUS 0x8003fffff8000028
#define SVC_RESET 0x8003fffff8000030
#define SVC_PANIC 0x8003fffff8000040
#define SVC_DEBUG 0x8003fffff8000048
.explicit
.text
/* SAL §4.1.1, §5.1, §6.2 and SDM Vol.2 PAL_COPY_PAL: on every processor,
 * PAL_COPY_PAL with copy_option 1 at the PAL block in place (PAL sets the
 * processor's PALE_PMI entrypoint), PAL_MC_REGISTER_MEM with this
 * processor's uncacheable min-state area (LID.id selects it), and
 * PAL_PMI_ENTRYPOINT with SALE_PMI. Physical mode, C calling convention. */
.align 16
.global sal_cpu_init
.proc sal_cpu_init
sal_cpu_init:
    alloc r2 = ar.pfs,0,2,4,0
    ;;
    mov loc0 = r2
    mov loc1 = b0
    mov r28 = 256
    mov out0 = 256
    movl out1 = pal_proc_entry
    mov out2 = 32768
    mov out3 = 1
    ;;
    mov b6 = out1
    ;;
    br.call.sptk.many b0 = b6
    ;;
    cmp.ne p6,p7 = r8,r0
    mov r14 = cr.lid
    ;;
(p6) br.cond.spnt .Lfailed
    extr.u r14 = r14,24,8
    ;;
    cmp.ltu p6,p7 = SAL_MIN_STATE_CPUS-1,r14
    ;;
(p6) br.cond.spnt .Lfailed
    shl r14 = r14,12
    movl r15 = sal_min_state
    ;;
    add r14 = r14,r15
    movl r15 = 0x8000000000000000
    ;;
    or r29 = r14,r15
    mov r28 = 27
    mov r30 = r0
    mov r31 = r0
    movl r14 = .Lregistered
    movl r15 = pal_proc_entry
    ;;
    mov b0 = r14
    mov b6 = r15
    br.sptk.many b6
    ;;
.Lregistered:
    cmp.ne p6,p7 = r8,r0
    ;;
(p6) br.cond.spnt .Lfailed
    mov r28 = 32
    movl r29 = sale_pmi
    mov r30 = r0
    mov r31 = r0
    movl r14 = .Lpmi_registered
    movl r15 = pal_proc_entry
    ;;
    mov b0 = r14
    mov b6 = r15
    br.sptk.many b6
    ;;
.Lpmi_registered:
    cmp.ne p6,p7 = r8,r0
    ;;
(p6) br.cond.spnt .Lfailed
    mov b0 = loc1
    mov ar.pfs = loc0
    ;;
    br.ret.sptk.many b0
    ;;
.Lfailed:
    movl r14 = SVC_PANIC
    movl r15 = 0x47335343
    ;;
    st8 [r14] = r15
    ;;
.Lfailed_halt:
    hint @pause
    br.sptk .Lfailed_halt
    ;;
.endp sal_cpu_init

/* SAL §6.3: no platform PMI source or function exists, so SALE_PMI returns
 * to PALE_PMI through BR0 at once, GR26-GR29 intact. */
.align 256
.global sale_pmi
.proc sale_pmi
sale_pmi:
    br.sptk.many b0
    ;;
.endp sale_pmi

.align 16
.global sale_entry
.proc sale_entry
sale_entry:
    /* Without a min-state area (PSP.mn = 0) the OS_MCA and OS_INIT hand-off
     * state (SAL §4.8, §5.3) does not exist and nothing can resume: SAL_INIT
     * warm boots as without an OS_INIT handler (§5.1), SAL_CHECK halts. */
    movl r2 = sal_mc
    cmp.eq p6,p7 = 2,r20
    tbit.z p8,p9 = r18,5
    ;;
(p6) br.cond.sptk .Lsal_init
    cmp.eq p6,p7 = 1,r20
    ;;
(p6) br.cond.sptk .Lsal_check
    movl r3 = 0x47335345
    br.sptk.many .Lpanic
    ;;
.Lsal_init:
    /* SAL §5.1, §9 SAL_SET_VECTORS: a processor entering SAL_INIT while no
     * other processor is in it is the monarch; the others are non-monarchs. */
(p8) br.cond.spnt .Lwarm
    adds r5 = SAL_MC_ACTIVE,r2
    ;;
    fetchadd8.acq r6 = [r5],1
    adds r7 = SAL_MC_OS_INIT,r2
    ;;
    cmp.eq p6,p7 = r6,r0
    ;;
(p7) adds r7 = SAL_MC_HANDLER,r7
    movl r13 = .Lsal_init_return
    movl r14 = .Lsal_init_invalid
    br.sptk.many .Lhandler
    ;;
.Lsal_check:
(p8) movl r3 = 0x47334d53
(p8) br.cond.spnt .Lpanic
    adds r7 = SAL_MC_OS_MCA,r2
    movl r13 = .Lsal_check_return
    movl r14 = .Lsal_check_invalid
    ;;
.Lhandler:
    /* r7: the handler (entry, gp, length_cs); r13 its return address; r14
     * where to go when it is not registered, being changed (an odd
     * generation, sal/mc.h) or fails its checksum. */
    adds r3 = SAL_MC_GENERATION,r2
    mov b1 = r14
    ;;
.Lread:
    ld8.acq r4 = [r3]
    adds r8 = 8,r7
    adds r9 = 16,r7
    ;;
    tbit.nz p6,p0 = r4,0
    ld8.acq r21 = [r7]
    ;;
(p6) br.cond.spnt b1
    ld8.acq r22 = [r8]
    ;;
    ld8.acq r9 = [r9]
    ;;
    ld8 r23 = [r3]
    ;;
    cmp.ne p6,p0 = r4,r23
    ;;
(p6) br.cond.spnt .Lread
    cmp.eq p6,p7 = r21,r0
    ;;
(p6) br.cond.spnt b1
    /* SAL §4.1, §5.1, §9 SAL_SET_VECTORS: with length_cs{32} and a nonzero
     * length the code is checked. The checksum byte is length_cs{47:40},
     * outside the code: the length_cs{31:0} code bytes plus that byte add
     * up to zero modulo 256. The bytes are read through the address as
     * registered, the uncacheable alias when bit 63 is set. */
    tbit.z p6,p7 = r9,32
    extr.u r10 = r9,0,32
    extr.u r11 = r9,40,8
    ;;
(p7) cmp.eq p6,p7 = r10,r0
    mov r12 = r21
    add r15 = r21,r10
    ;;
(p6) br.cond.sptk .Lcall
.Lsum:
    ld1 r8 = [r12],1
    ;;
    add r11 = r11,r8
    cmp.ltu p6,p7 = r12,r15
    ;;
(p6) br.cond.sptk .Lsum
    extr.u r11 = r11,0,8
    ;;
    cmp.ne p6,p7 = r11,r0
    ;;
(p6) br.cond.spnt b1
.Lcall:
    /* SAL §4.8, §5.3: GR1 the handler's gp, GR8 PAL_PROC, GR9 SAL_PROC,
     * GR10 the SAL gp, GR11 0 (INIT: no rendezvous or crash dump switch;
     * MCA: no rendezvous required), GR12 the return address; GR16-GR20 as
     * PAL left them. */
    adds r8 = SAL_MC_PAL_PROC,r2
    adds r9 = SAL_MC_SAL_PROC,r2
    adds r10 = SAL_MC_SAL_GP,r2
    mov b0 = r21
    ;;
    ld8 r8 = [r8]
    ld8 r9 = [r9]
    ld8 r10 = [r10]
    mov r1 = r22
    mov r11 = r0
    mov r12 = r13
    br.sptk.many b0
    ;;
.Lsal_init_return:
    /* SAL §5.4: GR8 0 resumes through PAL_MC_RESUME with GR22 and GR10;
     * -1 warm boots. The processor leaves SAL_INIT here. */
    movl r2 = sal_mc
    ;;
    adds r5 = SAL_MC_ACTIVE,r2
    ;;
    fetchadd8.rel r6 = [r5],-1
    cmp.eq p6,p7 = r8,r0
    ;;
(p6) br.cond.sptk .Lresume
    cmp.eq p6,p7 = -1,r8
    ;;
(p6) br.cond.sptk .Lwarm
    movl r3 = 0x47334952
    mov r18 = r8
    br.sptk.many .Lpanic
    ;;
.Lsal_init_invalid:
    /* SAL §5.1, §5.5: no valid OS_INIT handler: warm boot. */
    adds r5 = SAL_MC_ACTIVE,r2
    ;;
    fetchadd8.rel r6 = [r5],-1
    br.sptk.many .Lwarm
    ;;
.Lsal_check_return:
    /* SAL §4.8.1: GR8 0 corrected (resume), -1 warm boot, -2 cold boot,
     * -3 halt. */
    cmp.eq p6,p7 = r8,r0
    ;;
(p6) br.cond.sptk .Lresume
    cmp.eq p6,p7 = -1,r8
    ;;
(p6) br.cond.sptk .Lwarm
    cmp.eq p6,p7 = -2,r8
    ;;
(p6) br.cond.sptk .Lcold
    movl r3 = 0x47334d48
    mov r18 = r8
    br.sptk.many .Lpanic
    ;;
.Lsal_check_invalid:
    /* SAL §4.1: without a valid OS_MCA SAL halts or reboots; it halts. */
    movl r3 = 0x47334d4e
    br.sptk.many .Lpanic
    ;;
.Lresume:
    /* PAL_MC_RESUME(set_cmci 0, GR22, GR10), only BR0 and BR1 used
     * (SDM Vol.2 §11.3.3); it returns only on an error. */
    mov r28 = 26
    mov r29 = r0
    mov r30 = r22
    mov r31 = r10
    movl r14 = .Lresume_failed
    movl r15 = pal_proc_entry
    ;;
    mov b0 = r14
    mov b1 = r15
    br.sptk.many b1
    ;;
.Lresume_failed:
    movl r3 = 0x4733524d
    mov r18 = r8
    br.sptk.many .Lpanic
    ;;
.Lwarm:
    mov r4 = 1
    br.sptk.many .Lreset
    ;;
.Lcold:
    mov r4 = 0
    ;;
.Lreset:
    movl r5 = SVC_RESET_STATUS
    movl r6 = SVC_RESET
    ;;
    st8 [r5] = r0
    ;;
    st8 [r6] = r4
    br.sptk.many .Lhalt
    ;;
.Lpanic:
    /* r3: the reason. The debug line carries r18: the processor state
     * parameter, or the GR8 status an OS handler or PAL_MC_RESUME returned. */
    movl r5 = SVC_DEBUG
    movl r6 = SVC_PANIC
    ;;
    st8 [r5] = r18
    ;;
    st8 [r6] = r3
    ;;
.Lhalt:
    hint @pause
    br.sptk .Lhalt
    ;;
.endp sale_entry

.data
.align 8
.global sal_mc
sal_mc:
    .skip SAL_MC_GENERATION+8
.bss
.align 4096
.global sal_min_state
sal_min_state:
    .skip SAL_MIN_STATE_CPUS*4096
