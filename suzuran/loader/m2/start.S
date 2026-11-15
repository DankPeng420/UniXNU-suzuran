// arm64 Linux Image header so Sony's S1 (LK-based) bootloader accepts us as a kernel.
    .section .text.head, "ax"
    .globl _start
_start:
    b       entry               // code0
    .word   0                   // code1
    .quad   0x80000             // text_offset
    .quad   _image_end - _start // image_size (includes stack)
    .quad   0x2                 // flags: LE, 4K pages
    .quad   0, 0, 0
    .ascii  "ARM\x64"           // magic
    .word   0
entry:
    msr     daifset, #0xf
    mov     x19, x0             // dtb pointer from bootloader
    adrp    x1, _stack_top
    add     x1, x1, :lo12:_stack_top
    mov     sp, x1
    mov     x0, x19
    bl      cmain
1:  wfe
    b       1b
