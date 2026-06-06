! NanoCamelid 32XCD — Master SH-2 heartbeat (Milestone 1)
!
! Entered from PicoDrive's HLE boot via the ROM pointer at 0x3E0
! (uncached ROM address, set by the packer). The HLE already parked
! 'M_OK' in COMM0/1 — we must NOT touch COMM0-3 (the slave and the 68K
! key off them). We own COMM6 and SDRAM shared-state word 0.
!
! Heartbeats:
!   COMM6 (0x2000402C, u16)        — live counter for 68K/verifier
!   SDRAM 0x18000 (0x26018000 u32) — uncached so the verifier sees it

    .text
    .global _start
_start:
    mov.l   .Lsp, r15
    mov.l   .Lgbr, r0
    ldc     r0, gbr
    mov.l   .Lcomm6, r1
    mov.l   .Lhb, r2
    mov     #0, r3
loop:
    add     #1, r3
    mov.l   r3, @r2         ! SDRAM heartbeat (u32, uncached)
    mov     r3, r0
    mov.w   r0, @r1         ! COMM6 heartbeat (u16, wraps)
    bra     loop
    nop

    .align 4
.Lsp:    .long 0x0603F000   ! master stack top (memory-map.md)
.Lgbr:   .long 0x20004000   ! 32X system regs
.Lcomm6: .long 0x2000402C
.Lhb:    .long 0x26018000
