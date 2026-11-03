/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The stub segment F000h the guest sees after the VMM's POST. It runs inside the guest's own x86
 * emulators (the Windows HAL's int 10h emulator), so it is plain real-mode
 * code: no VMMCALL. Its port I/O becomes guest port accesses, which reach
 * the VMM's mediation like any other guest access. 386 instructions are
 * used only where configuration mechanism #1 needs 32-bit I/O.
 *
 * Source of truth for platform/vbios_guest_stub.inc
 * (tools/vbios/gen_guest_stub.py); the vbios QEMU targets execute the
 * guest services and trace wrapper against the x86 calling conventions.
 *
 * Entries (segment F000h): int 1Ah PCI BIOS at E100h (IBM's FE6Eh jumps
 * there), int 13h/15h/16h "unsupported" at E400h, iret at F065h (int 10h
 * before POST, kept for the vectors POST stored there) and FF53h (the
 * default of every other vector). The byte at E0F0h is the last PCI bus,
 * written by the VMM.
 */
        .code16
        .text
        .equ LAST_BUS, 0xe0f0           /* absolute: no relocation to apply */
        .org LAST_BUS
last_bus:
        .byte 0

/* PCI BIOS Specification 2.1, int 1Ah AH=B1h. FLAGS is at 6(%bp) once %bp
 * is pushed (IP, CS, FLAGS from the interrupt). Status in AH, CF in the
 * stacked FLAGS; every register that is not an output is preserved. */
        .org 0xe100
int1a:
        cmp $0xb1, %ah
        je 1f
        iret                            /* not PCI BIOS: registers unchanged */
1:      push %bp
        mov %sp, %bp
        cmp $0x01, %al
        je present
        cmp $0x02, %al
        je find_device
        cmp $0x03, %al
        je find_class
        cmp $0x08, %al
        jb unsupported
        cmp $0x0d, %al
        jbe config
unsupported:
        mov $0x81, %ah                  /* FUNC_NOT_SUPPORTED */
fail:
        orb $1, 6(%bp)
        pop %bp
        iret
ok:
        xor %ah, %ah                    /* SUCCESSFUL */
        andb $0xfe, 6(%bp)
        pop %bp
        iret

present:                                /* B101h */
        mov $0x0001, %ax                /* AH=0, AL bit 0: mechanism #1 */
        mov $0x0300, %bx                /* version 3.00 */
        mov %cs:LAST_BUS, %cl
        mov $0x20494350, %edx           /* "PCI " */
        xor %edi, %edi                  /* no protected-mode entry */
        andb $0xfe, 6(%bp)
        pop %bp
        iret

/* CONFIG_ADDRESS for BX (bus, device/function) and DI (register) to CF8h;
 * DX = the CONFIG_DATA byte lane CFCh + (DI & 3). Clobbers EAX, DX. */
cfg_addr:
        movzwl %bx, %eax
        shl $8, %eax
        mov %di, %dx
        and $0xfc, %dx
        or %dx, %ax
        or $0x80000000, %eax
        mov $0xcf8, %dx
        out %eax, %dx
        mov %di, %dx
        and $3, %dx
        add $0xcfc, %dx
        ret

config:                                 /* B108h-B10Dh, register in DI */
        cmp $0xff, %di
        ja bad_register
        push %edx
        push %eax                       /* -8(%bp): the function code */
        cmp $0x09, %al
        je 2f
        cmp $0x0c, %al
        je 2f
        cmp $0x0a, %al
        je 3f
        cmp $0x0d, %al
        je 3f
        jmp 4f
2:      test $1, %di
        jnz 5f
        jmp 4f
3:      test $3, %di
        jnz 5f
4:      call cfg_addr
        movb -8(%bp), %al
        cmp $0x08, %al
        je 6f
        cmp $0x09, %al
        je 7f
        cmp $0x0a, %al
        je 8f
        cmp $0x0b, %al
        je 9f
        cmp $0x0c, %al
        je 10f
        mov %ecx, %eax                  /* B10Dh write dword */
        out %eax, %dx
        jmp 11f
6:      in %dx, %al                     /* B108h read byte */
        mov %al, %cl
        jmp 11f
7:      in %dx, %ax                     /* B109h read word */
        mov %ax, %cx
        jmp 11f
8:      in %dx, %eax                    /* B10Ah read dword */
        mov %eax, %ecx
        jmp 11f
9:      mov %cl, %al                    /* B10Bh write byte */
        out %al, %dx
        jmp 11f
10:     mov %cx, %ax                    /* B10Ch write word */
        out %ax, %dx
11:     pop %eax
        pop %edx
        jmp ok
5:      pop %eax
        pop %edx
bad_register:
        mov $0x87, %ah                  /* BAD_REGISTER_NUMBER */
        jmp fail

/* B102h (CX device, DX vendor) and B103h (ECX[23:0] class), SI = index:
 * scan bus 0..last_bus, every device/function. Frame: -2 BX, -6 EAX,
 * -10 EDX, -14 EDI, -16 the remaining index (a copy of SI). */
        .macro find_frame
        push %bx
        push %eax
        push %edx
        push %edi
        push %si
        xor %bx, %bx
        .endm
find_device:
        cmp $0xffff, %dx
        jne 1f
        mov $0x83, %ah                  /* BAD_VENDOR_ID */
        jmp fail
1:      find_frame
2:      xor %di, %di
        call cfg_addr
        in %dx, %eax
        cmp $0xffff, %ax
        je 3f
        cmp -10(%bp), %ax
        jne 3f
        shr $16, %eax
        cmp %cx, %ax
        jne 3f
        cmpw $0, -16(%bp)
        je found
        decw -16(%bp)
3:      call find_next
        jnc 2b
        jmp not_found

find_class:
        find_frame
2:      mov $0x08, %di
        call cfg_addr
        in %dx, %eax
        shr $8, %eax
        mov %ecx, %edx
        and $0xffffff, %edx
        cmp %edx, %eax
        jne 3f
        cmpw $0, -16(%bp)
        je found
        decw -16(%bp)
3:      call find_next
        jnc 2b

not_found:
        add $2, %sp
        pop %edi
        pop %edx
        pop %eax
        pop %bx
        mov $0x86, %ah                  /* DEVICE_NOT_FOUND */
        jmp fail
found:
        add $2, %sp
        pop %edi
        pop %edx
        pop %eax
        add $2, %sp                     /* BX is the result */
        jmp ok

/* Next device/function; CF set past the last bus. */
find_next:
        inc %bl
        jnz 1f
        cmp %cs:LAST_BUS, %bh
        jae 2f
        inc %bh
1:      clc
        ret
2:      stc
        ret

/* int 13h, 15h, 16h: no disk, system or keyboard services here. CF=1 in
 * the stacked FLAGS, registers unchanged. */
        .org 0xe400
unsupported_service:
        push %bp
        mov %sp, %bp
        orb $1, 6(%bp)
        pop %bp
        iret

/* Opt-in INT 10h observer. The veneer in C000h jumps here, so the
 * HAL still sees a vector inside C0000-CFFFFh. Intel SDM Vol.2 PUSHAD,
 * PUSHF, CALL and IRET: save all 32-bit general registers and EFLAGS;
 * forward the caller's interrupt FLAGS to the original handler and copy
 * its returned FLAGS back to the caller's frame. No vendor code changes.
 * Port 80h packets are consumed by the VMM only with vbios_trace_guest=1.
 * Header, sequence, EAX/EBX/ECX/EDX/ESI/EDI/EBP/ES/DS/FLAGS, all dword outputs.
 * Outputs append the input AX tag and, for successful VBE 4F01h, the
 * ModeInfoBlock attributes, pitch, geometry, depth and PhysBasePtr. */
        .org 0xe7f0
trace_target:
        .word 0, 0
        .macro trace_packet magic, flags_offset, modeinfo=0
        pushfl
        pushal
        mov %sp, %bp
        mov $0x80, %dx
        mov $\magic, %eax
        out %eax, %dx
        in %dx, %eax                   /* VMM transport allocates sequence */
        out %eax, %dx
        .irp offset,28,16,24,20,4,0,8
        mov \offset(%bp), %eax
        out %eax, %dx
        .endr
        xor %eax, %eax
        mov %es, %ax
        out %eax, %dx
        mov %ds, %ax
        out %eax, %dx
        movzwl \flags_offset(%bp), %eax
        out %eax, %dx
        .if \modeinfo
        movzwl 36(%bp), %eax          /* input AX saved below the call frame */
        shl $16, %eax
        cmpw $0x4f01, 36(%bp)
        jne .Ltrace_no_info\@
        cmpw $0x004f, 28(%bp)          /* BIOS success, not an unfilled buffer */
        jne .Ltrace_no_info\@
        /* VBE 3.0 ModeInfoBlock, also SeaBIOS src/std/vbe.h. Read the
         * caller's emulator buffer; do not issue another BIOS call. */
        mov %es:(%di), %ax            /* function tag : mode attributes */
        out %eax, %dx
        mov %es:16(%di), %eax         /* X resolution : bytes per scanline */
        out %eax, %dx
        xor %eax, %eax
        mov %es:25(%di), %al
        shl $16, %eax
        mov %es:20(%di), %ax          /* bits per pixel : Y resolution */
        out %eax, %dx
        mov %es:40(%di), %eax         /* 32-bit PhysBasePtr */
        out %eax, %dx
        jmp .Ltrace_info_done\@
.Ltrace_no_info\@:
        out %eax, %dx
        xor %eax, %eax
        out %eax, %dx
        out %eax, %dx
        out %eax, %dx
.Ltrace_info_done\@:
        .endif
        popal
        popfl
        .endm
        .org 0xe800
int10_trace:
        trace_packet 0x56424930, 40
        push %ax                      /* identify the result after the BIOS */
        pushf
        push %bp
        push %ax
        mov %sp, %bp
        mov 12(%bp), %ax
        mov %ax, 4(%bp)
        pop %ax
        pop %bp
        lcall *%cs:0xe7f0
        trace_packet 0x56424931, 32, 1
        pushf
        push %bp
        push %ax
        mov %sp, %bp
        mov 4(%bp), %ax
        mov %ax, 12(%bp)
        pop %ax
        pop %bp
        popf
        add $2, %sp                    /* discard input AX; IRET restores FLAGS */
        iret
trace_end:

        .org 0xf065
        iret                            /* IBM int 10h entry */
        .org 0xfe6e
        jmp int1a                       /* IBM int 1Ah entry */
        .org 0xff53
        iret                            /* IBM dummy interrupt entry */
