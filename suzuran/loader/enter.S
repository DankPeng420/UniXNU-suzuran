// darwin_enter(x0 = boot_args PA, x1 = entry PA, x2 = window PA, x3 = bytes used)
// Same contract as DarwinBoot's DarwinEnter: clean+invalidate D-cache over the window,
// invalidate I-cache, MMU/D-cache/I-cache off, TLB flush, branch with x0 = boot_args.
    .text
    .globl darwin_enter
darwin_enter:
    msr     daifset, #15
    add     x5, x2, x3
    mrs     x6, ctr_el0
    ubfx    x6, x6, #16, #4
    mov     x7, #4
    lsl     x6, x7, x6
    sub     x7, x6, #1
    bic     x4, x2, x7
    dsb     sy
1:  dc      civac, x4
    add     x4, x4, x6
    cmp     x4, x5
    b.lo    1b
    dsb     sy
    ic      iallu
    dsb     sy
    isb
    mrs     x4, sctlr_el1
    bic     x4, x4, #1
    bic     x4, x4, #4
    bic     x4, x4, #0x1000
    msr     sctlr_el1, x4
    isb
    tlbi    vmalle1
    dsb     sy
    isb
    br      x1
