! Master SH-2 C runtime entry. Reached via the 32X header pointer (0x3E0).
! Sets stack + GBR, jumps to cmain. No .data/.bss: C uses fixed addresses.
    .section .text.entry, "ax"
    .global _start
_start:
    mov.l   .Lsp, r15
    mov.l   .Lgbr, r0
    ldc     r0, gbr
    mov.l   .Lmain, r0
    jmp     @r0
    nop
    .align 4
.Lsp:   .long 0x0603F000        ! master stack top (memory-map.md)
.Lgbr:  .long 0x20004000
.Lmain: .long _cmain
