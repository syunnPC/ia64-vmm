/* SPDX-License-Identifier: GPL-2.0-or-later */
.explicit
.text
.align 16
.global probe_unaligned
.proc probe_unaligned
probe_unaligned:
    alloc r14 = ar.pfs,1,1,0,0
    mov r33 = psr
    mov r16 = r32
    ;;
    rsm psr.ac
    ;;
    srlz.d
    ;;
    /* The IVT must update both r8 and the saved bank-one r16. */
    ld8 r8 = [r16],8
    ;;
    st4 [r16] = r8,4
    ;;
    ld2 r9 = [r16]
    ;;
    { .mmf
        nop.m 0
        ld8 r10 = [r16]
        nop.f 0
    }
    ;;
    { .mmb
        nop.m 0
        ld8 r10 = [r16]
        br.sptk .Lunaligned_templates
    }
    ;;
.Lunaligned_templates:

    mov psr.l = r33
    ;;
    srlz.d
    ;;
    mov ar.pfs = r14
    ;;
    br.ret.sptk.many b0
    ;;
.endp probe_unaligned

/* Exercise the largest interrupted frame, register source/base and immediate
 * updates through real faults. r80 and r127 must be recovered through RSE. */
.align 16
.global probe_unaligned_stacked
.proc probe_unaligned_stacked
probe_unaligned_stacked:
    alloc r14 = ar.pfs,1,95,0,8
    mov r90 = psr
    mov r91 = ar.lc
    mov r127 = r32
    ;;
    rsm psr.ac
    ;;
    srlz.d
    ;;
    mov ar.lc = r0
    mov r16 = 2
    ;;
    mov ar.ec = r16
    ;;
    br.ctop.sptk .Lunaligned_rotated
    ;;
.Lunaligned_rotated:
    ld8 r34 = [r127],8
    ;;
    mov r80 = r34
    ;;
    st8 [r127] = r80,8
    ;;
    ld8.s r81 = [r127]
    ;;
    chk.s r81,.Lstacked_nat
    ;;
    mov r8 = 0
    br.sptk .Lstacked_done
    ;;
.Lstacked_nat:
    mov r8 = r80
    ;;
.Lstacked_done:
    mov psr.l = r90
    mov ar.lc = r91
    ;;
    srlz.d
    ;;
    mov ar.pfs = r14
    ;;
    br.ret.sptk.many b0
    ;;
.endp probe_unaligned_stacked

.align 16
.global probe_unaligned_fp
.proc probe_unaligned_fp
probe_unaligned_fp:
    alloc r14 = ar.pfs,1,1,0,0
    mov r33 = psr
    mov r16 = r32
    ;;
    rsm psr.ac
    ;;
    srlz.d
    ;;
    /* These transfers use saved f8 and high f80, including NaTVal. */
    ldfd f8 = [r16],8
    ;;
    stfd [r16] = f8,8
    ;;
    ldf8 f80 = [r16],8
    ;;
    stf8 [r16] = f80,8
    ;;
    ldfs f8 = [r16],4
    ;;
    stfs [r16] = f8,4
    ;;
    ldfe f80 = [r16],16
    ;;
    stfe [r16] = f80
    ;;
    mov psr.l = r33
    ;;
    srlz.d
    ;;
    mov ar.pfs = r14
    ;;
    br.ret.sptk.many b0
    ;;
.endp probe_unaligned_fp

/* SDM Vol.2 tables 11-59/61. Save/compare all static-convention preserved
 * GRs, predicates and branches, including a rotating 96-register frame.
 * The raw static transfer deliberately does not make an RSE call. */
.align 16
.global probe_raw_static
.proc probe_raw_static
probe_raw_static:
    alloc r14 = ar.pfs,6,90,0,8
    mov r127 = r37
    ;;
    mov r16 = r127
    mov r17 = b0
    ;;
    st8 [r16] = r14,8
    ;;
    st8 [r16] = r17,8
    ;;
    st8 [r16] = r1,8
    ;;
    st8 [r16] = r4,8
    ;;
    st8 [r16] = r5,8
    ;;
    st8 [r16] = r6,8
    ;;
    st8 [r16] = r7,8
    ;;
    st8 [r16] = r12,8
    ;;
    st8 [r16] = r13,8
    ;;
    mov r17 = pr
    ;;
    st8 [r16] = r17,8
    ;;
    mov r17 = b1
    ;;
    st8 [r16] = r17,8
    ;;
    mov r17 = b2
    ;;
    st8 [r16] = r17,8
    ;;
    mov r17 = b3
    ;;
    st8 [r16] = r17,8
    ;;
    mov r17 = b4
    ;;
    st8 [r16] = r17,8
    ;;
    mov r17 = b5
    ;;
    st8 [r16] = r17,8
    ;;
    mov r17 = ar.lc
    ;;
    st8 [r16] = r17,8
    ;;
    mov r17 = ar.ec
    ;;
    st8 [r16] = r17,8
    ;;
    mov b6 = r32
    mov r28 = r33
    mov r29 = r34
    mov r30 = r35
    mov r31 = r36
    ;;
    mov r1 = 1
    ;;
    mov r4 = 4
    ;;
    mov r5 = 5
    ;;
    mov r6 = 6
    ;;
    mov r7 = 7
    ;;
    mov r12 = 12
    ;;
    mov r13 = 13
    ;;
    .irp reg,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126
    mov r\reg = \reg
    ;;
    .endr
    mov ar.lc = r0
    mov r16 = 2
    ;;
    mov ar.ec = r16
    ;;
    br.ctop.sptk .Lraw_rotated
    ;;
.Lraw_rotated:
    add r16 = 256,r127
    ;;
    .irp reg,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
    st8 [r16] = r\reg,8
    ;;
    .endr
    movl r16 = 0xaaaaaaaaaaaaaaab
    ;;
    mov pr = r16,-1
    ;;
    mov r16 = 256
    ;;
    mov b1 = r16
    ;;
    mov r16 = 512
    ;;
    mov b2 = r16
    ;;
    mov r16 = 768
    ;;
    mov b3 = r16
    ;;
    mov r16 = 1024
    ;;
    mov b4 = r16
    ;;
    mov r16 = 1280
    ;;
    mov b5 = r16
    ;;
    /* Static PAL preserves bank-zero r16-r23 as well as bank one. */
    add r14 = 160,r127
    ;;
    bsw.0
    ;;
    st8 [r14] = r16,8
    ;;
    st8 [r14] = r17,8
    ;;
    st8 [r14] = r18,8
    ;;
    st8 [r14] = r19,8
    ;;
    st8 [r14] = r20,8
    ;;
    st8 [r14] = r21,8
    ;;
    st8 [r14] = r22,8
    ;;
    st8 [r14] = r23,8
    ;;
    mov r16 = 4016
    ;;
    mov r17 = 4017
    ;;
    mov r18 = 4018
    ;;
    mov r19 = 4019
    ;;
    mov r20 = 4020
    ;;
    mov r21 = 4021
    ;;
    mov r22 = 4022
    ;;
    mov r23 = 4023
    ;;
    bsw.1
    ;;
    .align 16
.Lraw_return_ip:
    mov r16 = ip
    ;;
    add r16 = .Lraw_return-.Lraw_return_ip,r16
    ;;
    mov b0 = r16
    ;;
    br.sptk.many b6
    ;;
.Lraw_return:
    add r16 = 1024,r127
    ;;
    .irp reg,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
    st8 [r16] = r\reg,8
    ;;
    .endr
    st8 [r16] = r1,8
    ;;
    st8 [r16] = r4,8
    ;;
    st8 [r16] = r5,8
    ;;
    st8 [r16] = r6,8
    ;;
    st8 [r16] = r7,8
    ;;
    st8 [r16] = r12,8
    ;;
    st8 [r16] = r13,8
    ;;
    mov r17 = pr
    ;;
    st8 [r16] = r17,8
    ;;
    mov r17 = b1
    ;;
    st8 [r16] = r17,8
    ;;
    mov r17 = b2
    ;;
    st8 [r16] = r17,8
    ;;
    mov r17 = b3
    ;;
    st8 [r16] = r17,8
    ;;
    mov r17 = b4
    ;;
    st8 [r16] = r17,8
    ;;
    mov r17 = b5
    ;;
    st8 [r16] = r17,8
    ;;
    add r14 = 1920,r127
    ;;
    bsw.0
    ;;
    st8 [r14] = r16,8
    ;;
    st8 [r14] = r17,8
    ;;
    st8 [r14] = r18,8
    ;;
    st8 [r14] = r19,8
    ;;
    st8 [r14] = r20,8
    ;;
    st8 [r14] = r21,8
    ;;
    st8 [r14] = r22,8
    ;;
    st8 [r14] = r23,8
    ;;
    add r14 = 160,r127
    ;;
    ld8 r16 = [r14],8
    ;;
    ld8 r17 = [r14],8
    ;;
    ld8 r18 = [r14],8
    ;;
    ld8 r19 = [r14],8
    ;;
    ld8 r20 = [r14],8
    ;;
    ld8 r21 = [r14],8
    ;;
    ld8 r22 = [r14],8
    ;;
    ld8 r23 = [r14],8
    ;;
    bsw.1
    ;;
    mov r16 = r127
    ;;
    ld8 r14 = [r16],8
    ;;
    ld8 r17 = [r16],8
    ;;
    mov b0 = r17
    ;;
    ld8 r1 = [r16],8
    ;;
    ld8 r4 = [r16],8
    ;;
    ld8 r5 = [r16],8
    ;;
    ld8 r6 = [r16],8
    ;;
    ld8 r7 = [r16],8
    ;;
    ld8 r12 = [r16],8
    ;;
    ld8 r13 = [r16],8
    ;;
    ld8 r17 = [r16],8
    ;;
    mov pr = r17,-1
    ;;
    ld8 r17 = [r16],8
    ;;
    mov b1 = r17
    ;;
    ld8 r17 = [r16],8
    ;;
    mov b2 = r17
    ;;
    ld8 r17 = [r16],8
    ;;
    mov b3 = r17
    ;;
    ld8 r17 = [r16],8
    ;;
    mov b4 = r17
    ;;
    ld8 r17 = [r16],8
    ;;
    mov b5 = r17
    ;;
    ld8 r17 = [r16],8
    ;;
    mov ar.lc = r17
    ;;
    ld8 r17 = [r16]
    ;;
    mov ar.ec = r17
    mov ar.pfs = r14
    ;;
    br.ret.sptk.many b0
    ;;
.endp probe_raw_static
