// EL1 vector table with full register save: exc_handler2(esr, far, frame) may return,
// and the exception returns via ERET (handler can advance ELR to skip a faulting load).
    .section .text.vectors, "ax"
    .balign 2048
    .globl vectors2
vectors2:
    .irp i, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    .balign 128
    sub     sp, sp, #256
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    b       exc_common
    .endr
exc_common:
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x29, [sp, #144]
    str     x30, [sp, #160]
    mrs     x0, esr_el1
    mrs     x1, far_el1
    mov     x2, sp
    bl      exc_handler2
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x29, [sp, #144]
    ldr     x30, [sp, #160]
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #256
    eret
    .globl install_vectors2
install_vectors2:
    adrp    x0, vectors2
    add     x0, x0, :lo12:vectors2
    msr     vbar_el1, x0
    isb
    ret
