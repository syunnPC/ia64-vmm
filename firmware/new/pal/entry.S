/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Rev 2.3 Vol.2 §11.10.2, tables 11-59/60/61. Static procedures
 * never alloc, call, touch stacked registers, change GP, SP or RSE state.
 * Geometry/encodings follow ia64/pal.c's selected VMM profile, originally
 * ported from target/ia64/arch/pal.c (fork 5f67e29, GPL-2.0-or-later).
 * All branches/data are relative to this block, including copied entries.
 * The processor's hardware resources (XR0, the PAL_MC_EXPECTED indicator,
 * the PALE_PMI and SAL PMI entrypoints, the PAL-based interruption last
 * entered) are per-processor service registers (include/ia64vmm/platform.h),
 * so every copy of this block sees the same state.
 */
#define SVC_PANIC 0x8003fffff8000040
#define SVC_DEBUG 0x8003fffff8000048
#define SVC_XR0 0x8003fffff80000f0
#define SVC_EXPECTED 0x8003fffff80000f8
#define SVC_PALE_PMI 0x8003fffff8000100
#define SVC_SALE_PMI 0x8003fffff8000108
#define SVC_EVENT 0x8003fffff8000110
#define SVC_STATE_RESUME 0x8003fffff8000128
#define SVC_PMI_VECTOR 0x8003fffff8000118
.explicit
.section .pal,"awx",@progbits
.align 16
.global pal_proc_entry, __firmware_native_pal
__firmware_native_pal:
.proc pal_proc_entry
pal_proc_entry:
    /* SDM Vol.2 §11.10.2 and NaT Consumption, Vol.3 mov-to-BR:
     * match the VMM PAL's consumption-fault policy for NaT arguments.
     * A compare of a NaT clears both predicates; it cannot validate zero.
     * b6 is scratch and is touched only on the faulting path. */
    .macro CONSUME reg
    tnat.nz p6,p7 = \reg
    ;;
(p6) mov b6 = \reg
    ;;
    .endm
    CONSUME r28
    cmp.eq p6,p7 = 33,r28
    ;;
(p6) br.cond.sptk .Lia32_unsupported
    ;;
    tbit.nz p6,p7 = r28,8
    ;;
(p6) br.cond.sptk .Lstacked
    ;;
    br.sptk .Ldispatch
    ;;
.Lstacked:
    alloc r14 = ar.pfs,4,0,0,0
    ;;
    mov r29 = r33
    mov r30 = r34
    mov r31 = r35
    ;;
.Ldispatch:
    CONSUME r29
    CONSUME r30
    CONSUME r31
    mov r8 = 0
    mov r9 = 0
    mov r10 = 0
    mov r11 = 0
    ;;
    .macro SELECT index,handler
    mov r16 = \index
    ;;
    cmp.eq p6,p7 = r16,r28
    ;;
(p6) br.cond.sptk \handler
    ;;
    .endm
    SELECT 1,.Lflush
    SELECT 2,.Lcache
    SELECT 3,.Linit
    SELECT 4,.Lsummary
    SELECT 5,.Lattrib
    SELECT 6,.Lptce
    SELECT 7,.Lvm_info
    SELECT 8,.Lvm_summary
    SELECT 9,.Lbus_get
    SELECT 10,.Lbus_set
    SELECT 11,.Ldebug
    SELECT 12,.Lfixed
    SELECT 13,.Lbase
    SELECT 14,.Lratios
    SELECT 15,.Lperf
    SELECT 16,.Lplatform
    SELECT 17,.Lproc_get
    SELECT 18,.Lproc_set
    SELECT 19,.Lrse
    SELECT 20,.Lversion
    SELECT 21,.Lmc_empty
    SELECT 22,.Lmc_empty
    SELECT 23,.Lmc_expected
    SELECT 24,.Lmc_dynamic
    SELECT 25,.Lmc_error
    SELECT 26,.Lmc_resume
    SELECT 27,.Lmc_register
    SELECT 28,.Lunsupported
    SELECT 29,.Lidle
    SELECT 30,.Lcopy_info
    SELECT 31,.Lline_init
    SELECT 32,.Lpmi
    SELECT 34,.Lpage_size
    SELECT 37,.Ltest_info
    SELECT 38,.Lcache
    SELECT 39,.Lregister
    SELECT 41,.Lprefetch
    SELECT 42,.Ltopology
    SELECT 43,.Lcache
    SELECT 256,.Lcopy
    SELECT 257,.Lhalt_info
    SELECT 258,.Ltest
    SELECT 261,.Ltr
    SELECT 274,.Lbrand
    br.sptk .Lunsupported
    ;;
    .macro ZERO
    or r16 = r29,r30
    ;;
    or r16 = r16,r31
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    .endm
    .macro BCZERO
    or r16 = r30,r31
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    .endm
    .macro RETURN x=0,y=0,z=0
    movl r9 = \x
    movl r10 = \y
    movl r11 = \z
    ;;
    br.sptk .Lreturn
    ;;
    .endm
    /* Read the guest's immutable IA-64 CPUID, including on copied PAL
     * entries. Never infer the model from the AMD64 firmware host. */
    .macro MADISON
    mov r16 = 3
    ;;
    mov r16 = cpuid[r16]
    ;;
    extr.u r16 = r16,24,8
    ;;
    cmp.eq p6,p7 = 31,r16
    ;;
    .endm
.Lsummary:
    ZERO
    MADISON
(p6) br.cond.sptk .Lsummary_madison
    ;;
    RETURN 3,5
.Lsummary_madison:
    RETURN 3,4
.Lattrib:
    ZERO
    RETURN 0x71
.Lptce:
    ZERO
    RETURN 0,0x100000001
.Lvm_info:
    cmp.ltu p6,p7 = 1,r29
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.ne p6,p7 = r31,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    add r16 = -1,r30
    ;;
    cmp.ltu p6,p7 = 1,r16
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.eq p6,p7 = r29,r0
    ;;
(p6) br.cond.sptk .Lvm_info_l1
    ;;
    RETURN 0x500808001,0x155557000
.Lvm_info_l1:
    RETURN 0x202001,0x1000
.Lvm_summary:
    ZERO
    MADISON
(p6) br.cond.sptk .Lvm_summary_madison
    ;;
    RETURN 0x02041f1f080f1865,0x183c
.Lvm_summary_madison:
    RETURN 0x02043f3f080f1865,0x183c
.Lpage_size:
    ZERO
    RETURN 0x155557000,0x155557000
.Lrse:
    ZERO
    RETURN 96
.Lversion:
    ZERO
    MADISON
(p6) br.cond.sptk .Lversion_madison
    ;;
    RETURN 0x0000096801000968,0x0000096801000968
.Lversion_madison:
    RETURN 0x0000057301000573,0x0000057301000573
.Lbase:
    ZERO
    RETURN 100000000
.Lratios:
    ZERO
    RETURN 0x1000000001,0x400000001,0x400000001
.Ldebug:
    ZERO
    RETURN 4,4
.Lfixed:
    ZERO
    mov r9 = cr.lid
    ;;
    extr.u r9 = r9,24,8
    ;;
    br.sptk .Lreturn
    ;;
    /* One/two populated Montecito cores per socket. The count is copied
     * with PAL and addressed relatively, including virtual PAL calls. */
    .macro SOCKET
.Lsocket_ip\@:
    mov r16 = ip
    ;;
    movl r17 = pal_cpu_count-.Lsocket_ip\@
    ;;
    add r16 = r17,r16
    ;;
    ld8 r17 = [r16]
    mov r18 = cr.lid
    ;;
    extr.u r18 = r18,24,8
    ;;
    cmp.ltu p6,p7 = 1,r17
    ;;
(p6) shr.u r19 = r18,1
    ;;
(p7) mov r19 = r18
(p6) and r20 = -2,r18
    ;;
(p7) mov r20 = r18
    ;;
    sub r17 = r17,r20
    sub r18 = r18,r20
    ;;
    cmp.ltu p6,p7 = 2,r17
    ;;
(p6) mov r17 = 2
    ;;
    .endm
.Ltopology:
    MADISON
(p6) br.cond.sptk .Lunsupported
    ;;
    BCZERO
    SOCKET
    cmp.eq p6,p7 = -1,r29
    ;;
(p6) mov r29 = r18
    ;;
    cmp.geu p6,p7 = r29,r17
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    mov r9 = 0x10000
    shl r10 = r29,32
    add r11 = r20,r29
    ;;
    or r9 = r9,r17
    ;;
    dep r9 = r17,r9,32,16
    ;;
    dep r9 = r19,r9,48,16
    ;;
    br.sptk .Lreturn
    ;;
.Lbus_get:
    ZERO
    /* No bus parking or lock-masking facility exists in the virtual bus. */
    br.sptk .Lreturn
    ;;
.Lbus_set:
    BCZERO
    br.sptk .Lreturn
    ;;
.Lproc_get:
    or r16 = r29,r31
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.eq p6,p7 = r30,r0
    ;;
(p6) br.cond.sptk .Lproc_recovery
    ;;
.Lproc_set:
    cmp.ne p6,p7 = r31,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.eq p6,p7 = r30,r0
    ;;
(p6) br.cond.sptk .Lreturn
    ;;
    cmp.gtu p6,p7 = 16,r30
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    MADISON
(p6) br.cond.sptk .Lproc_madison
    ;;
    cmp.ltu p6,p7 = 18,r30
    ;;
(p6) br.cond.dpnt .Lbeyond
    ;;
    cmp.ne p6,p7 = 18,r30
    ;;
(p6) br.cond.dpnt .Lpositive
    ;;
    /* Set 18 is recognized, but coherence filtering, exclusive prefetch
     * and hardware threading are unavailable. SET ignores absent bits. */
    br.sptk .Lreturn
    ;;
.Lproc_madison:
    cmp.ne p6,p7 = 16,r30
    ;;
(p6) br.cond.sptk .Lbeyond
    ;;
    br.sptk .Lreturn
    ;;
.Lproc_recovery:
    /* Table 11-112: XIP/XPSR/XFS are read-only; XR1-XR3 are absent. */
    RETURN 0x20000000000,0x20000000000
.Lregister:
    BCZERO
    cmp.ltu p6,p7 = 3,r29
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.eq p6,p7 = 0,r29
    ;;
(p6) movl r9 = 0x11117f2f00ff
(p6) mov r10 = 7
    ;;
    cmp.eq p6,p7 = 2,r29
    ;;
(p6) movl r9 = 0x3fb0107
(p6) movl r10 = 0x307ff
    ;;
    cmp.eq p6,p7 = 3,r29
    ;;
(p6) mov r10 = 2
    ;;
    br.sptk .Lreturn
    ;;
.Lcache:
    MADISON
(p7) br.cond.sptk .Lcache_common
    ;;
    cmp.eq p6,p7 = 43,r28
    ;;
(p6) br.cond.sptk .Lunsupported
    ;;
    cmp.eq p6,p7 = 1,r29
    ;;
(p6) cmp.ne p6,p7 = 2,r30
    ;;
(p6) br.cond.sptk .Linvalid
    ;;
.Lcache_common:
    cmp.ne p6,p7 = r31,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.ltu p6,p7 = 2,r29
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    add r16 = -1,r30
    ;;
    cmp.ltu p6,p7 = 1,r16
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.eq p6,p7 = 2,r29
    ;;
(p6) cmp.ne p6,p7 = 2,r30
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.eq p6,p7 = 43,r28
    ;;
(p6) br.cond.sptk .Lshared
    ;;
    mov r17 = 12
    movl r9 = 0x010106060400
    movl r10 = 0x310c0c00004000
    ;;
    cmp.eq p6,p7 = 1,r30
    ;;
(p6) movl r9 = 0x01ff06060400
    ;;
    cmp.eq p6,p7 = 1,r29
    ;;
(p6) br.cond.sptk .Lcache_l2
    ;;
    cmp.eq p6,p7 = 2,r29
    ;;
(p6) movl r9 = 0x0e0107070c03
(p6) movl r10 = 0x31140c00c00000
(p6) mov r17 = 20
    ;;
    MADISON
(p6) cmp.eq p6,p7 = 2,r29
    ;;
(p6) movl r10 = 0x31120c00300000
(p6) mov r17 = 18
    ;;
    br.sptk .Lcache_prot
    ;;
.Lcache_l2:
    movl r9 = 0x050107070802
    movl r10 = 0x310f0c00040000
    mov r17 = 15
    ;;
    MADISON
(p6) movl r9 = 0x050107070803
    ;;
    cmp.eq p6,p7 = 1,r30
    ;;
(p6) movl r9 = 0x07ff07070800
(p6) movl r10 = 0x31110c00100000
(p6) mov r17 = 17
    ;;
.Lcache_prot:
    cmp.ne p6,p7 = 38,r28
    ;;
(p6) br.cond.sptk .Lreturn
    ;;
    movl r16 = 0x400c4000
    ;;
    dep r16 = r17,r16,8,6
    ;;
    shl r16 = r16,32
    mov r9 = 64
    mov r10 = 0
    ;;
    or r9 = r9,r16
    ;;
    br.sptk .Lreturn
    ;;
.Lshared:
    SOCKET
    shl r10 = r18,32
    mov r9 = 1
    mov r11 = cr.lid
    ;;
    extr.u r11 = r11,24,8
    ;;
    br.sptk .Lreturn
    ;;
.Lidle:
    ZERO
    /* SDM PAL_HALT_LIGHT: light-halt wake ignores PSR.i. Keep instruction
     * translation active, and use the UC physical alias with data DT off. */
    mov r17 = psr
    ;;
    rsm psr.i | psr.dt
    ;;
    srlz.d
    ;;
    movl r16 = 0x8003fffff8000038
    ;;
    st8 [r16] = r0
    ;;
    mov psr.l = r17
    ;;
    srlz.d
    ;;
    br.sptk .Lreturn
    ;;
.Lprefetch:
    BCZERO
    cmp.ltu p6,p7 = 1,r29
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    br.sptk .Lpositive
    ;;
.Lcopy_info:
    /* Pal.h PAL_PLATFORM_INFO counts and SAL min-state size do not change
     * this fixed-size image. SDM Rev 2.3 defines only copy_type zero. */
    cmp.ne p6,p7 = r29,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    RETURN 32768,8192
.Lcopy:
    movl r16 = 0x8000000000000000
    ;;
    andcm r17 = r29,r16
    movl r18 = 0x4000000000000
    ;;
    cmp.geu p6,p7 = r17,r18
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    sub r18 = r18,r17
    movl r19 = 32768
    ;;
    cmp.gtu p6,p7 = r30,r18
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.ltu p6,p7 = r30,r19
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    extr.u r18 = r17,0,13
    ;;
    cmp.ne p6,p7 = r18,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.ltu p6,p7 = 1,r31
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    /* SDM Vol.2 PAL_COPY_PAL: the PALE_PMI at the target becomes the
     * processor's, whether this call copies (0) or not (1). */
    movl r20 = pale_pmi-pal_proc_entry
    cmp.eq p6,p7 = 1,r31
    ;;
    add r21 = r20,r29
(p6) br.cond.sptk .Lcopy_done
    ;;
    .align 16
.Lcopy_ip:
    mov r18 = ip
    ;;
    add r18 = pal_proc_entry-.Lcopy_ip,r18
    ;;
    andcm r18 = r18,r16
    ;;
    /* A PAL copy must not overwrite the code currently executing. */
    add r20 = r17,r19
    ;;
    cmp.geu p6,p7 = r18,r20
    ;;
(p6) br.cond.sptk .Lcopy_loop
    ;;
    add r20 = r18,r19
    ;;
    cmp.ltu p6,p7 = r17,r20
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
.Lcopy_loop:
    ld8 r20 = [r18],8
    ;;
    st8 [r29] = r20,8
    add r19 = -8,r19
    ;;
    cmp.ne p6,p7 = r19,r0
    ;;
(p6) br.cond.sptk .Lcopy_loop
    ;;
    mf
    ;;
    sync.i
    ;;
    srlz.i
    ;;
.Lcopy_done:
    movl r20 = SVC_PALE_PMI
    ;;
    st8 [r20] = r21
    br.sptk .Lreturn
    ;;
.Lhalt_info:
    BCZERO
    and r16 = 7,r29
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    /* Only light halt is implemented; no fabricated watt/latency estimates. */
    movl r16 = 0x3000000000000000
    mov r17 = 7
    ;;
    st8 [r29] = r16,8
    ;;
.Lhalt_zero:
    st8 [r29] = r0,8
    add r17 = -1,r17
    ;;
    cmp.ne p6,p7 = r17,r0
    ;;
(p6) br.cond.sptk .Lhalt_zero
    ;;
    br.sptk .Lreturn
    ;;
.Lperf:
    BCZERO
    and r16 = 7,r29
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.eq p6,p7 = r29,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    mov r17 = 0
    ;;
.Lperf_loop:
    mov r16 = 0
    ;;
    cmp.eq p6,p7 = 0,r17
    ;;
(p6) movl r16 = 0x3fff
    ;;
    cmp.eq p6,p7 = 4,r17
    ;;
(p6) movl r16 = 0x3ffff
    ;;
    cmp.eq p6,p7 = 8,r17
    ;;
(p6) mov r16 = 0xf0
    ;;
    cmp.eq p6,p7 = 12,r17
    ;;
(p6) mov r16 = 0xf0
    ;;
    st8 [r29] = r16,8
    add r17 = 1,r17
    ;;
    cmp.gtu p6,p7 = 16,r17
    ;;
(p6) br.cond.sptk .Lperf_loop
    ;;
    RETURN 0x08123004
.Lbrand:
    cmp.ne p6,p7 = r31,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.eq p6,p7 = 16,r29
    ;;
(p6) br.cond.sptk .Lbrand_proc
    ;;
    cmp.eq p6,p7 = 17,r29
    ;;
(p6) br.cond.sptk .Lbrand_cache
    ;;
    cmp.eq p6,p7 = 18,r29
    ;;
(p6) br.cond.sptk .Lbrand_bus
    ;;
    cmp.ne p6,p7 = r29,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.eq p6,p7 = r30,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    .align 16
.Lbrand_ip:
    MADISON
(p6) movl r17 = .Lbrand_madison-.Lbrand_address
(p7) movl r17 = .Lbrand_text-.Lbrand_address
    ;;
    .align 16
.Lbrand_address:
    mov r16 = ip
    ;;
    add r16 = r17,r16
    ;;
.Lbrand_loop:
    ld1 r17 = [r16],1
    ;;
    st1 [r30] = r17,1
    ;;
    cmp.ne p6,p7 = r17,r0
    ;;
(p6) br.cond.sptk .Lbrand_loop
    ;;
    MADISON
(p6) br.cond.sptk .Lbrand_length_madison
    ;;
    RETURN .Lbrand_end-.Lbrand_text-1
.Lbrand_length_madison:
    RETURN .Lbrand_madison_end-.Lbrand_madison-1
.Lbrand_proc:
    RETURN 1600000000
.Lbrand_cache:
    MADISON
(p6) br.cond.sptk .Lbrand_cache_madison
    ;;
    SOCKET
    cmp.eq p6,p7 = 2,r17
    ;;
(p6) br.cond.sptk .Lbrand_cache_two
    ;;
    RETURN 12582912
.Lbrand_cache_two:
    RETURN 25165824
.Lbrand_cache_madison:
    RETURN 3145728
.Lbrand_bus:
    RETURN 400000000
.Ltest_info:
    BCZERO
    cmp.ltu p6,p7 = 1,r29
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    RETURN 0,1
.Lmc_empty:
    ZERO
    mf
    ;;
    br.sptk .Lreturn
    ;;
.Lmc_expected:
    BCZERO
    cmp.ltu p6,p7 = 1,r29
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    movl r16 = SVC_EXPECTED
    ;;
    ld8 r9 = [r16]
    ;;
    st8 [r16] = r29
    ;;
    br.sptk .Lreturn
    ;;
.Lmc_error:
    cmp.gtu p6,p7 = 2,r29
    ;;
(p6) br.cond.sptk .Lno_info
    ;;
    cmp.ne p6,p7 = 2,r29
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    shr.u r16 = r30,48
    extr.u r17 = r30,0,8
    ;;
    or r16 = r16,r17
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    extr.u r16 = r30,8,40
    ;;
    add r17 = -1,r16
    cmp.eq p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    and r17 = r16,r17
    ;;
    cmp.ne p6,p7 = r17,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    and r16 = 7,r31
    ;;
    cmp.ltu p6,p7 = 4,r16
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    br.sptk .Lno_info
    ;;
.Lmc_dynamic:
    cmp.ltu p6,p7 = 1,r29
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.ne p6,p7 = r31,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.ne p6,p7 = r29,r0
    ;;
(p7) br.cond.sptk .Lunsupported
    ;;
    and r16 = 7,r30
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    br.sptk .Lunsupported
    ;;
.Lmc_register:
    cmp.ne p6,p7 = r31,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    tbit.z p6,p7 = r29,63
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    extr.u r16 = r29,0,9
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    extr.u r16 = r29,50,13
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    /* The whole 4 KB area must lie below the 2^50 physical limit. */
    movl r17 = 0x0003fffffffff000
    ;;
    extr.u r18 = r29,0,50
    ;;
    cmp.ltu p6,p7 = r17,r18
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    /* Size in KB: 0 (not passed) or 4; otherwise report req_size 4. */
    and r16 = -5,r30
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) mov r9 = 4
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    /* XR0 (SDM Vol.2 §11.3.1.1). */
    movl r16 = SVC_XR0
    ;;
    st8 [r16] = r29
    ;;
    br.sptk .Lreturn
    ;;
.Lplatform:
    /* SDM explicitly permits processors without block-pointer relocation
     * to report unimplemented; this platform has fixed MMIO windows. */
    cmp.ltu p6,p7 = 1,r29
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.ne p6,p7 = r31,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    movl r16 = 0x8000000000000000
    ;;
    andcm r17 = r30,r16
    ;;
    shr.u r16 = r17,50
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    movl r16 = 0x00000000fee00000
    ;;
    cmp.ne p6,p7 = r29,r0
    ;;
(p6) movl r16 = 0x0003fffffc000000
    ;;
    cmp.ne p6,p7 = r16,r17
    ;;
(p6) br.cond.dpnt .Lunsupported
    ;;
    br.sptk .Lreturn
    ;;
.Lflush:
    add r16 = -1,r29
    ;;
    cmp.ltu p6,p7 = 3,r16
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    and r16 = -4,r30
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    br.sptk .Lcache_sync
    ;;
.Linit:
    cmp.eq p6,p7 = -1,r29
    ;;
(p6) br.cond.sptk .Lcache_sync
    ;;
    cmp.ltu p6,p7 = 2,r29
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    add r16 = -1,r30
    ;;
    cmp.ltu p6,p7 = 2,r16
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.ltu p6,p7 = 1,r31
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    br.sptk .Lcache_sync
    ;;
.Lline_init:
    shr.u r16 = r29,50
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.ne p6,p7 = r31,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    /* SDM Vol.2 p.2:377 operates without backing memory. There are no
     * cache arrays to seed in this model; serialize without RAM/MMIO stores. */
.Lcache_sync:
    mf
    ;;
    sync.i
    ;;
    srlz.i
    ;;
    br.sptk .Lreturn
    ;;
.Lpmi:
    BCZERO
    extr.u r16 = r29,0,8
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    shr.u r16 = r29,50
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    /* The SAL PMI entrypoint register (SDM Vol.2 §11.5.1). */
    movl r16 = SVC_SALE_PMI
    ;;
    st8 [r16] = r29
    ;;
    br.sptk .Lreturn
    ;;
.Ltr:
    MADISON
    mov r18 = 32
    ;;
(p6) mov r18 = 64
    ;;
    cmp.geu p6,p7 = r29,r18
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.ltu p6,p7 = 1,r30
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    and r16 = 7,r31
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    shl r16 = r30,8
    movl r17 = 0x8003fffff80000b8
    ;;
    or r16 = r16,r29
    ;;
    st8 [r17] = r16,8
    ;;
    st8 [r17] = r31,8
    ;;
    ld8 r8 = [r17],8
    ;;
    ld8 r9 = [r17]
    ;;
    br.sptk .Lreturn
    ;;
.Ltest:
    shr.u r16 = r29,50
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    shr.u r16 = r31,8
    tbit.z p6,p7 = r31,0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    /* SDM Fig.11-44: test_phase is bits 63:56, buffer_size is 55:0. */
    extr.u r16 = r30,56,8
    ;;
    cmp.ltu p6,p7 = 1,r16
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    dep r30 = r0,r30,56,8
    ;;
    movl r16 = 0x0004000000000000
    ;;
    sub r16 = r16,r29
    ;;
    cmp.ltu p6,p7 = r16,r30
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    mov r16 = psr
    ;;
    tbit.nz p6,p7 = r16,17
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    /* TEST_PROC evicts TCs but preserves architected translation registers. */
    ptc.e r0
    ;;
    srlz.d
    ;;
    srlz.i
    ;;
    RETURN 4
    ;;
.Lmc_resume:
    cmp.ltu p6,p7 = 1,r29
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    cmp.ltu p6,p7 = 1,r31
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    tbit.z p6,p7 = r30,63
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    extr.u r16 = r30,0,9
    extr.u r17 = r30,50,13
    ;;
    or r16 = r16,r17
    ;;
    cmp.ne p6,p7 = r16,r0
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    /* The area restrictions of PAL_MC_REGISTER_MEM: 4 KB below 2^50. */
    movl r17 = 0x0003fffffffff000
    ;;
    extr.u r18 = r30,0,50
    ;;
    cmp.ltu p6,p7 = r17,r18
    ;;
(p6) br.cond.dpnt .Linvalid
    ;;
    /* set_cmci: a CMC interrupt through this processor's own PIB entry
     * (SDM Vol.2 §5.8.4.2), unless CMCV masks it. */
    cmp.eq p6,p7 = r29,r0
    ;;
(p6) br.cond.sptk .Lmc_resume_restore
    mov r16 = cr.cmcv
    mov r17 = cr.lid
    movl r18 = 0x80000000fee00000
    ;;
    tbit.nz p6,p7 = r16,16
    extr.u r19 = r17,24,8
    extr.u r20 = r17,16,8
    ;;
(p6) br.cond.dpnt .Lmc_resume_restore
    shl r19 = r19,12
    shl r20 = r20,4
    extr.u r16 = r16,0,8
    ;;
    or r18 = r18,r19
    ;;
    or r18 = r18,r20
    ;;
    mf
    ;;
    st8 [r18] = r16
    ;;
.Lmc_resume_restore:
    /* VMM-private return service, not an Intel-private rfi encoding.
     * The engine boundary restores Figure 11-2 and both interruption
     * levels together; bit 0 selects a new context (§11.3.3). */
    movl r16 = SVC_STATE_RESUME
    or r30 = r30,r31
    ;;
    st8 [r16] = r30
    ;;
.Lmc_resume_halt:
    hint @pause
    br.sptk .Lmc_resume_halt
    ;;
.Lbeyond:
    mov r8 = -8
    ;;
    br.sptk .Lreturn
    ;;
.Lno_info:
    mov r8 = -6
    ;;
    br.sptk .Lreturn
    ;;
.Lpositive:
    mov r8 = 1
    ;;
    br.sptk .Lreturn
    ;;
.Lunsupported:
    mov r8 = -1
    ;;
    br.sptk .Lreturn
    ;;
.Lia32_unsupported:
    /* PAL_ENTER_IA_32_ENV uses GR4 for status, including unavailable.
     * Its inputs do not follow the ordinary static convention. */
    mov r4 = -1
    mov r9 = 0
    mov r10 = 0
    mov r11 = 0
    ;;
    br.sptk .Lunsupported
    ;;
.Lerror:
    mov r8 = -3
    ;;
    br.sptk .Lreturn
    ;;
.Linvalid:
    mov r8 = -2
    ;;
.Lreturn:
    tbit.nz p6,p7 = r28,8
    ;;
(p6) mov ar.pfs = r14
    ;;
(p6) br.ret.sptk.many b0
(p7) br.sptk.many b0
    ;;
.endp pal_proc_entry
/* PALE_PMI (SDM Vol.2 §11.5): entered with bank 0, PSR.mc/mfl/mfh/pk kept
 * and every other bit 0, IIP/IPSR collected and IFS.v = 0; the vector
 * comes from the processor's PMI_VECTOR register. PAL vectors 4-15 have
 * no PAL function here, so they only return (Table 11-15). SAL vectors
 * 0-3 go to the registered SAL PMI entrypoint with GR24-GR29 and BR0 as
 * §11.5.2 defines, or return when none is registered (§11.5.1). Bank 0
 * GR24-GR31 are PALE_PMI's; everything else is restored or untouched. */
.align 16
.global pale_pmi
.proc pale_pmi
pale_pmi:
    mov r26 = ar.rsc
    mov r27 = b0
    mov r28 = b1
    mov r29 = pr
    movl r30 = SVC_PMI_VECTOR
    ;;
    ld8 r24 = [r30]
    movl r31 = SVC_XR0
    ;;
    ld8 r25 = [r31]
    dep r31 = r0,r26,0,2
    ;;
    mov ar.rsc = r31
    cmp.ltu p6,p7 = 3,r24
    movl r30 = SVC_SALE_PMI
    ;;
(p6) br.cond.dpnt .Lpmi_return
    ld8 r31 = [r30]
    ;;
    cmp.eq p6,p7 = r31,r0
    ;;
(p6) br.cond.dpnt .Lpmi_return
    .align 16
.Lpmi_ip:
    mov r30 = ip
    ;;
    adds r30 = .Lpmi_return-.Lpmi_ip,r30
    mov b1 = r31
    ;;
    mov b0 = r30
    br.sptk.many b1
    ;;
.Lpmi_return:
    /* SAL PMI returns through BR0 with GR26-GR29 preserved (§11.5.3). */
    mov ar.rsc = r26
    mov b0 = r27
    mov b1 = r28
    mov pr = r29,-1
    ;;
    rfi
    ;;
.endp pale_pmi
.align 8
.global pal_cpu_count
pal_cpu_count:
    .quad 1
.Lbrand_text:
    .asciz "VMM Montecito-compatible IA-64 CPU 1.60GHz 12MB"
.Lbrand_end:
.Lbrand_madison:
    .asciz "VMM Madison-compatible IA-64 CPU 1.60GHz 3MB"
.Lbrand_madison_end:
    /* Size/alignment returned by COPY_INFO; no dependencies outside this block. */
    .org pal_proc_entry+32768
