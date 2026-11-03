/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Static/stacked transfer and PSR restoration follow EDK2 BaseLib/Ipf/
 * AsmPalCall.s, restricted to the tested procedures. The adapted portions:
 * Copyright (c) 2006 - 2008, Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in
 * the documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE. */
.explicit
.text
.global model_pal_call
.proc model_pal_call
model_pal_call:
    alloc loc0 = ar.pfs,5,3,4,0
    mov loc1 = b0
    mov loc2 = psr
    ;;
    rsm psr.i
    ;;
    srlz.d
    mov r28 = in1
    mov b6 = in0
    ;;
    tbit.nz p6,p7 = in1,8
    ;;
(p6) br.cond.sptk .Lstacked
    ;;
.Lhere:
    mov r16 = ip
    ;;
    add r16 = .Lreturn-.Lhere,r16
    mov r29 = in2
    mov r30 = in3
    mov r31 = in4
    ;;
    mov b0 = r16
    ;;
    br.sptk b6
    ;;
.Lstacked:
    mov out0 = in1
    mov out1 = in2
    mov out2 = in3
    mov out3 = in4
    ;;
    br.call.sptk b0 = b6
    ;;
.Lreturn:
    mov psr.l = loc2
    ;;
    srlz.d
    mov b0 = loc1
    mov ar.pfs = loc0
    ;;
    br.ret.sptk b0
    ;;
.endp model_pal_call
