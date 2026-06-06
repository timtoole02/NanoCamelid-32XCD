| Harness self-test ROM: minimal Genesis cart that increments a u32
| heartbeat at work RAM 0xFF6000 forever. If nc-headless sees this
| counter strictly increasing across frames, the verification stack
| (core + memory patch + frontend) works end to end.
|
| Build: m68k-elf-as -m68000 smoke.s -o smoke.o
|        m68k-elf-ld -Ttext=0 --oformat binary smoke.o -o smoke.bin

    .org    0
vectors:
    .long   0x00FFFE00          | initial SSP
    .long   entry               | initial PC
    .rept   62
    .long   trap                | everything else -> spin
    .endr

header:
    .ascii  "SEGA MEGA DRIVE "                                  | console
    .ascii  "(C)NC32 2026.JUN"                                  | copyright
    .ascii  "NANOCAMELID HARNESS SMOKE TEST                  "  | domestic name
    .ascii  "NANOCAMELID HARNESS SMOKE TEST                  "  | overseas name
    .ascii  "GM NC320001-00"                                    | serial
    .word   0                                                   | checksum (unused)
    .ascii  "J               "                                  | io support
    .long   0x000000, 0x0003FFFF                                | rom range
    .long   0x00FF0000, 0x00FFFFFF                              | ram range
    .ascii  "            "                                      | no sram
    .ascii  "            "                                      | modem
    .ascii  "                                        "          | memo
    .ascii  "JUE             "                                  | region

entry:
    move.w  #0x2700, %sr        | no interrupts
    | TMSS: write 'SEGA' to 0xA14000 if VDP version nonzero
    move.b  0xA10001, %d0
    andi.b  #0x0F, %d0
    beq.s   no_tmss
    move.l  #0x53454741, 0xA14000
no_tmss:
    moveq   #0, %d0
    lea     0xFF6000, %a0
    move.l  %d0, (%a0)
loop:
    addq.l  #1, (%a0)
    bra.s   loop

trap:
    bra.s   trap
