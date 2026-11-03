/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PALE_INIT and PALE_CHECK (SDM Rev 2.3 Vol.2 §11.3, §11.4).
 * The VMM-private state service saves the entry capture before PAL scratch
 * use, including both interruption levels and both GR banks. It covers
 * the frame and enforces lazy RSE mode at the engine boundary. */
#define SVC_XR0 0x8003fffff80000f0
#define SVC_EXPECTED 0x8003fffff80000f8
#define SVC_EVENT 0x8003fffff8000110
#define SVC_STATE_SAVE 0x8003fffff8000120
/* Processor State Parameter (Figure 11-11, Tables 11-7 and 11-12). */
#define PSP_VALID 0xbdf20000            /* gr b1 fp pr br rr tr dr pc cr rs: untouched */
#define PSP_SAVED 0x42000020            /* b0 ar mn: valid through the min-state area */
#define PSP_INIT 0x10180                /* in co ci */
#define PSP_CO 0x80
#define PSP_CI 0x100
#define PSP_EX 0x80000
#define PSP_TC 0x1000000000000000
.explicit
.text
.align 16
.global pale_init, pale_check
.proc pale_init
pale_init:
    mov r24 = 2                         /* SALE_ENTRY function INIT (Table 11-13) */
    br.sptk.many .Lsave
    ;;
.endp pale_init
.align 16
.proc pale_check
pale_check:
    mov r24 = 1                         /* SALE_ENTRY function MACHINE CHECK */
    ;;
.Lsave:
    mov r25 = pr
    movl r26 = SVC_XR0
    ;;
    ld8 r27 = [r26]
    ;;
    cmp.eq p6,p7 = r27,r0
    ;;
(p6) br.cond.spnt .Lunregistered
    movl r26 = SVC_STATE_SAVE
    ;;
    st8 [r26] = r27
    ;;
.Lparameters:
    /* r24 the function, r25 the predicates, r27 XR0. INIT: Table 11-12. A
     * machine check is isolated (Table 11-7); a TR overlap is a TLB check.
     * A purge's TLB check with IIP, IPSR and IFS collected into a min-state
     * area is continuable (co): the purge completed and the saved state is
     * all valid. sy, pi and pm stay 0: IIP names the instruction after the
     * purge, not the one responsible. Only bank 0 GR16-GR31 are used from
     * here. */
    movl r26 = SVC_EVENT
    movl r28 = SVC_EXPECTED
    ;;
    ld8 r26 = [r26]
    ld8 r28 = [r28]
    movl r18 = PSP_VALID
    ;;
    cmp.eq p6,p7 = 2,r24
    tbit.nz p8,p0 = r26,9
    cmp.ne p9,p0 = r27,r0
    cmp.ne p10,p0 = r0,r0
    cmp.ne p11,p0 = r0,r0
    extr.u r30 = r26,8,1                /* COLLECTED */
    extr.u r31 = r26,10,1               /* TLB_PURGE */
    ;;
    and r30 = r30,r31
    ;;
(p9) cmp.ne p11,p0 = r30,r0
(p6) movl r29 = PSP_INIT
(p7) cmp.ne p10,p0 = r28,r0
    ;;
(p7) movl r29 = PSP_CI
    ;;
    or r18 = r18,r29
    movl r29 = PSP_TC
    ;;
(p8) or r18 = r18,r29
    movl r29 = PSP_SAVED
    ;;
(p9) or r18 = r18,r29
    movl r29 = PSP_EX
    ;;
(p10) or r18 = r18,r29
    movl r29 = PSP_CO
    ;;
(p11) or r18 = r18,r29
    mov r16 = r0
    mov r17 = r27
    ;;
(p9) adds r16 = 0x200,r27               /* first location for SAL */
    mov r19 = r0                        /* no rendezvous return */
    mov r20 = r24
    mov pr = r25,-1
    ;;
    br.sptk.many sale_entry
    ;;
.Lunregistered:
    /* No min-state area: bank 0 GR16-GR20 carry the parameters, RSC.mode
     * becomes 0 and cover saves CFM in IFS; beyond PAL's GR24-GR31 nothing
     * else changes. */
    mov r26 = ar.rsc
    ;;
    dep r26 = r0,r26,0,2
    ;;
    mov ar.rsc = r26
    ;;
    cover
    ;;
    br.sptk.many .Lparameters
    ;;
.endp pale_check
