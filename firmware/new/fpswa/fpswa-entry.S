/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Intel 245415-001 §7.2. Capture physical FRs before C can use f6-f11;
 * br.call clears the rotating bases (SDM Vol.1 §3.4.1.3). The OS supplies
 * interrupted rotation through IFS, independently of this call's frame. */
.explicit
.section .text.fpswa_emulation_entry,"ax",@progbits
.align 16
.global fpswa_emulation_entry
.proc fpswa_emulation_entry
fpswa_emulation_entry:
    alloc r40=ar.pfs,8,8,1,0
    ;;
    mov r41=b0
    mov r42=sp
    mov r43=gp
    mov r44=ar.fpsr
    mov r45=pr
    mov r46=ar.unat
    mov r47=psr
    ;;
    /* FP_STATE can supply every high FR while the caller has lazy-FPU
     * disable bits set. Snapshotting must not cause a nested FP fault, and
     * the specification requires atomic register save/restore (§7.2). */
    rsm psr.i | psr.dfl | psr.dfh
    ;;
    srlz.d
    ;;
    adds sp=-2144,sp
    ;;
    adds r14=16,sp
    ;;
    .irp reg,32,33,34,35,36,37,38,39
    st8 [r14]=r\reg,8
    ;;
    .endr
    /* FP_STATE.live has two unused constant slots, then f2..f127. */
    adds r14=112,sp
    ;;
    .irp reg,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
    stf.spill [r14]=f\reg,16
    ;;
    .endr
    adds r48=16,sp
    ;;
    br.call.sptk.many b0=fpswa_emulate
    ;;
    mov gp=r43
    adds r14=112,sp
    ;;
    .irp reg,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
    ldf.fill f\reg=[r14],16
    ;;
    .endr
    mov ar.fpsr=r44
    mov ar.unat=r46
    mov pr=r45,-1
    mov b0=r41
    mov sp=r42
    ;;
    mov psr.l=r47
    ;;
    srlz.d
    ;;
    mov ar.pfs=r40
    ;;
    br.ret.sptk.many b0
    ;;
.endp fpswa_emulation_entry
