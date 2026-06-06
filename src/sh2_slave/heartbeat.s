! NanoCamelid 32XCD — Slave SH-2 heartbeat (Milestone 1)
!
! Entered from PicoDrive's HLE boot via the ROM pointer at 0x3E4.
! 'S_OK' is parked in COMM2/3 — leave COMM0-3 alone. We own COMM7 and
! SDRAM shared-state word 1.

    .text
    .global _start
_start:
    mov.l   .Lsp, r15
    mov.l   .Lgbr, r0
    ldc     r0, gbr
    mov.l   .Lcomm7, r1
    mov.l   .Lhb, r2
    mov     #0, r3
loop:
    add     #1, r3
    mov.l   r3, @r2         ! SDRAM heartbeat (u32, uncached)
    mov     r3, r0
    mov.w   r0, @r1         ! COMM7 heartbeat (u16, wraps)
    bra     loop
    nop

    .align 4
.Lsp:    .long 0x06040000   ! slave stack top (memory-map.md)
.Lgbr:   .long 0x20004000
.Lcomm7: .long 0x2000402E
.Lhb:    .long 0x26018004
