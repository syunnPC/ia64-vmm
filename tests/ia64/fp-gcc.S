/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GCC 13.4 -O2 sequences from fp-guest.c, using static GRs for arguments.
 * Inlining removes the call/RSE dependency; br.ret is replaced by nop.b. */
// EXPECT r24 = 0xd555555555555556
// EXPECT r25 = 0x3fd5555555555555
// EXPECT r26 = 0x3eaaaaab
// EXPECT r27 = 0x3ff6a09e667f3bcd
// EXPECT r28 = 0x5555555555555555
// EXPECT r29 = 0xfffffffffffffffe
// EXPECT r30 = 0x43f0000000000000
// EXPECT r31 = 0x100000003
.text
.global _start
_start:

/* sd */
movl r20=0x8000000000000000
mov r21=3
;;
	.mii
	setf.sig f6 = r20
	cmp.eq p6, p7 = 0, r21
	;;
	(p6) break 1
	;;
	.mfi
	nop 0
	fcvt.xf f8 = f6
	nop 0
	.mmi
	setf.sig f6 = r21
	nop 0
	nop 0
	;;
	.mmf
	nop 0
	nop 0
	fcvt.xf f10 = f6
	;;
	.mmf
	nop 0
	nop 0
	frcpa.s1 f6, p6 = f8, f10
	;;
	.mfi
	nop 0
	(p6) fnma.s1 f7 = f10, f6, f1
	nop 0
	.mfi
	nop 0
	(p6) fmpy.s1 f9 = f8, f6
	nop 0
	;;
	.mfi
	nop 0
	(p6) fmpy.s1 f11 = f7, f7
	nop 0
	.mfi
	nop 0
	(p6) fma.s1 f9 = f9, f7, f9
	nop 0
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f7 = f6, f7, f6
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f9 = f11, f9, f9
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f7 = f7, f11, f7
	;;
	.mmf
	nop 0
	nop 0
	(p6) fnma.s1 f8 = f10, f9, f8
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f6 = f8, f7, f9
	;;
	.mmf
	nop 0
	nop 0
	fcvt.fx.trunc.s1 f6 = f6
	;;
	.mib
	getf.sig r8 = f6
	nop 0
	nop.b 0
;;
mov r24=r8
;;

/* dd */
movl r20=0x3ff0000000000000
movl r21=0x4008000000000000
;;
setf.d f8=r20
setf.d f9=r21
;;
	.mfi
	nop 0
	mov f7 = f8
	nop 0
	.mfi
	nop 0
	frcpa.s0 f8, p6 = f8, f9
	nop 0
	;;
	.mmf
	nop 0
	nop 0
	(p6) fnma.s1 f6 = f9, f8, f1
	;;
	.mfi
	nop 0
	(p6) fmpy.s1 f10 = f6, f6
	nop 0
	.mfi
	nop 0
	(p6) fma.s1 f6 = f8, f6, f8
	nop 0
	;;
	.mfi
	nop 0
	(p6) fmpy.s1 f11 = f10, f10
	nop 0
	.mfi
	nop 0
	(p6) fma.s1 f6 = f6, f10, f6
	nop 0
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f6 = f6, f11, f6
	;;
	.mmf
	nop 0
	nop 0
	(p6) fmpy.d.s1 f10 = f7, f6
	;;
	.mmf
	nop 0
	nop 0
	(p6) fnma.s1 f7 = f9, f10, f7
	;;
	.mfb
	nop 0
	(p6) fma.d.s0 f8 = f7, f6, f10
	nop.b 0
;;
getf.d r25=f8
;;

/* fd */
movl r20=0x3f800000
movl r21=0x40400000
;;
setf.s f8=r20
setf.s f9=r21
;;
	.mfi
	nop 0
	mov f6 = f8
	nop 0
	.mfi
	nop 0
	frcpa.s0 f8, p6 = f8, f9
	nop 0
	;;
	.mmf
	nop 0
	nop 0
	(p6) fnma.s1 f10 = f9, f8, f1
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f7 = f8, f10, f8
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f7 = f7, f10, f8
	;;
	.mmf
	nop 0
	nop 0
	(p6) fmpy.s.s1 f10 = f6, f7
	;;
	.mmf
	nop 0
	nop 0
	(p6) fnma.s1 f6 = f10, f9, f6
	;;
	.mfb
	nop 0
	(p6) fma.s.s0 f8 = f6, f7, f10
	nop.b 0
;;
getf.s r26=f8
;;

/* sq */
movl r20=0x4000000000000000
;;
setf.d f8=r20
;;
	.mlx
	nop 0
	movl r14 = 0x3fe0000000000000
	.mmf
	nop 0
	nop 0
	mov f9 = f8
	;;
	.mmf
	setf.d f10 = r14
	nop 0
	frsqrta.s0 f8, p6 = f8
	;;
	.mmf
	nop 0
	nop 0
	(p6) fmpy.s1 f6 = f9, f8
	;;
	.mmf
	nop 0
	nop 0
	(p6) fmpy.s1 f7 = f10, f8
	;;
	.mmf
	nop 0
	nop 0
	(p6) fnma.s1 f11 = f6, f7, f10
	;;
	.mfi
	nop 0
	(p6) fma.s1 f6 = f6, f11, f6
	nop 0
	.mfi
	nop 0
	(p6) fma.s1 f7 = f7, f11, f7
	nop 0
	;;
	.mmf
	nop 0
	nop 0
	(p6) fnma.s1 f10 = f6, f7, f10
	;;
	.mfi
	nop 0
	(p6) fma.s1 f6 = f6, f10, f6
	nop 0
	.mfi
	nop 0
	(p6) fma.s1 f7 = f7, f10, f7
	nop 0
	;;
	.mmf
	nop 0
	nop 0
	(p6) fnma.s1 f10 = f6, f6, f9
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f6 = f10, f7, f6
	;;
	.mmf
	nop 0
	nop 0
	(p6) fnma.s1 f9 = f6, f6, f9
	;;
	.mfb
	nop 0
	(p6) fma.d.s1 f8 = f9, f7, f6
	nop.b 0
;;
getf.d r27=f8
;;

/* ud */
movl r20=0xffffffffffffffff
mov r21=3
;;
	.mii
	setf.sig f6 = r20
	cmp.eq p6, p7 = 0, r21
	;;
	(p6) break 1
	;;
	.mfi
	nop 0
	fcvt.xuf f8 = f6
	nop 0
	.mmi
	setf.sig f6 = r21
	nop 0
	nop 0
	;;
	.mmf
	nop 0
	nop 0
	fcvt.xuf f10 = f6
	;;
	.mmf
	nop 0
	nop 0
	frcpa.s1 f6, p6 = f8, f10
	;;
	.mfi
	nop 0
	(p6) fnma.s1 f7 = f10, f6, f1
	nop 0
	.mfi
	nop 0
	(p6) fmpy.s1 f9 = f8, f6
	nop 0
	;;
	.mfi
	nop 0
	(p6) fmpy.s1 f11 = f7, f7
	nop 0
	.mfi
	nop 0
	(p6) fma.s1 f9 = f9, f7, f9
	nop 0
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f7 = f6, f7, f6
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f9 = f11, f9, f9
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f7 = f7, f11, f7
	;;
	.mmf
	nop 0
	nop 0
	(p6) fnma.s1 f8 = f10, f9, f8
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f6 = f8, f7, f9
	;;
	.mmf
	nop 0
	nop 0
	fcvt.fxu.trunc.s1 f6 = f6
	;;
	.mib
	getf.sig r8 = f6
	nop 0
	nop.b 0
;;
mov r28=r8
;;

/* sm */
movl r20=0x8000000000000000
mov r21=3
;;
	.mmi
	setf.sig f10 = r20
	setf.sig f6 = r21
	cmp.eq p6, p7 = 0, r21
	.mmi
	sub r14 = r0, r21
	;;
	(p6) break 1
	nop 0
	;;
	.mfi
	nop 0
	fcvt.xf f9 = f10
	nop 0
	.mfi
	nop 0
	fcvt.xf f11 = f6
	nop 0
	;;
	.mmf
	nop 0
	nop 0
	frcpa.s1 f6, p6 = f9, f11
	;;
	.mfi
	nop 0
	(p6) fnma.s1 f7 = f11, f6, f1
	nop 0
	.mfi
	nop 0
	(p6) fmpy.s1 f8 = f9, f6
	nop 0
	;;
	.mfi
	nop 0
	(p6) fmpy.s1 f12 = f7, f7
	nop 0
	.mfi
	nop 0
	(p6) fma.s1 f8 = f8, f7, f8
	nop 0
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f7 = f6, f7, f6
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f8 = f12, f8, f8
	;;
	.mmf
	nop 0
	nop 0
	(p6) fma.s1 f7 = f7, f12, f7
	;;
	.mmf
	nop 0
	nop 0
	(p6) fnma.s1 f9 = f11, f8, f9
	;;
	.mfi
	nop 0
	(p6) fma.s1 f6 = f9, f7, f8
	nop 0
	.mmi
	setf.sig f7 = r14
	nop 0
	nop 0
	;;
	.mmf
	nop 0
	nop 0
	fcvt.fx.trunc.s1 f6 = f6
	;;
	.mmf
	nop 0
	nop 0
	xma.l f7 = f6, f7, f10
	;;
	.mib
	getf.sig r8 = f7
	nop 0
	nop.b 0
;;
mov r29=r8
;;

/* ui */
movl r20=0xffffffffffffffff
;;
	.mmi
	nop 0
	setf.sig f6 = r20
	nop 0
	;;
	.mfb
	nop 0
	fcvt.xuf.d f8 = f6
	nop.b 0
;;
getf.d r30=f8
;;

/* iu */
movl r20=0x41f0000000300000
;;
setf.d f8=r20
;;
	.mmf
	nop 0
	nop 0
	fcvt.fxu.trunc f8 = f8
	;;
	.mib
	getf.sig r8 = f8
	nop 0
	nop.b 0
;;
mov r31=r8
;;
break.i 0x1ffffe
