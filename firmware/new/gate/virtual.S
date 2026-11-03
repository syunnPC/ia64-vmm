/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Ia64/Memory/InitialTranslationActivator.S
 */
/* Loader-side translation activation.
 * Frameless, zero-argument leaf: IFS.v=0 retains its zero CFM at rfi.
 * Caller already installed region-7 translations; region 0 stays identity. */
.text
.explicit
.align 16
.global gate_enable_translation
.type gate_enable_translation,@function
.proc gate_enable_translation
gate_enable_translation:
    mov r14=psr
    ;;
    /* mov-from-PSR does not expose BN; compiled C must resume in bank one. */
    movl r15=0x101008020000
    ;;
    or r14=r14,r15
    rsm psr.i|psr.ic
    ;;
    srlz.d
    ;;
    movl r15=.Ltranslated
    ;;
    mov cr.ifs=r0
    mov cr.ipsr=r14
    mov cr.iip=r15
    ;;
    rfi
    ;;
.Ltranslated:
    br.ret.sptk.many b0
    ;;
.endp gate_enable_translation
