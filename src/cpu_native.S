@ Native backend, 32-bit ARM host: the two bridges between game code and host.
@ ARM state throughout; the linker interworks the calls into the (Thumb) C++.
    .syntax unified
    .arm
    .text
    .align 2

@ Entered from an import stub with r0-r3 = the guest's arguments, r12 = the
@ import index, sp = the guest's sp (stack arguments start there), lr = return
@ address into the game. Builds uint32_t regs[16] on the stack, calls
@ hle_native_dispatch(regs, idx) and returns regs[0]/regs[1] in r0/r1.
    .global hle_native_entry
    .type hle_native_entry, %function
hle_native_entry:
    push    {r4-r11, r12, lr}       @ 40 bytes keep sp 8-byte aligned
    sub     sp, sp, #64             @ regs[16]
    stm     sp, {r0-r3}             @ regs[0..3]
    add     r4, sp, #104            @ the guest's sp: our 64 + 40 above it
    str     r4, [sp, #52]           @ regs[13]
    str     lr, [sp, #56]           @ regs[14]
    mov     r0, sp
    mov     r1, r12
    bl      hle_native_dispatch
    ldm     sp, {r0, r1}            @ result (r1: high word of 64-bit values)
    add     sp, sp, #64
    pop     {r4-r11, r12, pc}
    .size hle_native_entry, .-hle_native_entry

@ uint64_t native_call(uint32_t fn, const uint32_t r[4], const uint32_t *extra, uint32_t nextra)
@ Calls guest function fn (bit 0 = Thumb) with r[0..3] in r0-r3 and `nextra`
@ further words on the stack, AAPCS style. Returns r0:r1.
    .global native_call
    .type native_call, %function
native_call:
    push    {r4-r11, lr}            @ 36 bytes ...
    sub     sp, sp, #4              @ ... plus 4 keeps the alignment
    mov     r4, r0                  @ fn
    mov     r5, r1                  @ r[4]
    mov     r6, r2                  @ extra
    mov     r7, r3                  @ nextra
    mov     r8, sp                  @ to restore after the call
    lsl     r9, r7, #2
    add     r9, r9, #7
    bic     r9, r9, #7              @ stack argument area, 8-byte aligned
    sub     sp, sp, r9
    mov     r10, #0
1:  cmp     r10, r7
    bge     2f
    ldr     r11, [r6, r10, lsl #2]
    str     r11, [sp, r10, lsl #2]
    add     r10, r10, #1
    b       1b
2:  ldm     r5, {r0-r3}
    blx     r4
    mov     sp, r8
    add     sp, sp, #4
    pop     {r4-r11, pc}
    .size native_call, .-native_call

    .section .note.GNU-stack,"",%progbits
