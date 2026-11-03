/* SPDX-License-Identifier: GPL-2.0-or-later */
.explicit
.text
.align 16
.global fpswa_guest_execute
.proc fpswa_guest_execute
fpswa_guest_execute:
    alloc r37=ar.pfs,5,4,0,0
    ;;
    mov r38=b0
    mov r39=pr
    mov r40=ar.fpsr
    ;;
    mov ar.fpsr=r33
    ldf.fill f7=[r34]
    ldf.fill f8=[r35]
    ;;
    cmp.eq p6,p7=0,r32
    ;;
(p6) br.cond.sptk .Ldivide
    ;;
    cmp.eq p6,p7=1,r32
    ;;
(p6) br.cond.sptk .Lsqrt
    ;;
    cmp.eq p6,p7=2,r32
    ;;
(p6) br.cond.sptk .Lpdivide
    ;;
    cmp.eq p6,p7=3,r32
    ;;
(p6) br.cond.sptk .Lpsqrt
    ;;
    cmp.eq p6,p7=4,r32
    ;;
(p6) br.cond.sptk .Lhigh
    ;;
    ldf.fill f32=[r34]
    ;;
    frcpa f6,p6=f32,f8
    ;;
    br.sptk .Lstore
    ;;
.Ldivide:
    frcpa f6,p6=f7,f8
    ;;
    br.sptk .Lstore
    ;;
.Lsqrt:
    frsqrta f6,p6=f8
    ;;
    br.sptk .Lstore
    ;;
.Lpdivide:
    fprcpa f6,p6=f7,f8
    ;;
    br.sptk .Lstore
    ;;
.Lpsqrt:
    fprsqrta f6,p6=f8
    ;;
    br.sptk .Lstore
    ;;
.Lhigh:
    frcpa f32,p6=f7,f8
    ;;
    stf.spill [r36]=f32,16
    ;;
    br.sptk .Lstatus
    ;;
.Lstore:
    stf.spill [r36]=f6,16
    ;;
.Lstatus:
    mov r14=ar.fpsr
    mov r15=pr
    ;;
    st8 [r36]=r14,8
    ;;
    st8 [r36]=r15
    mov ar.fpsr=r40
    mov pr=r39,-1
    mov b0=r38
    ;;
    mov ar.pfs=r37
    ;;
    br.ret.sptk.many b0
    ;;
.endp fpswa_guest_execute
