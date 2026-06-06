| NanoCamelid 32XCD — Genesis Main 68000 boot shell (Milestone 1, cart mode)
|
| Boots in plain MD mode (ROM at 0), runs TMSS, then enables the 32X by
| setting ADEN at 0xA15100. PicoDrive's HLE then resets both SH-2s, which
| boot via the pointers the packer patches into the 32X header block
| (0x3C0-0x3FF). The master parks 'M_OK' in COMM0, the slave 'S_OK' in
| COMM2 — we wait for both, then run the heartbeat loop.
|
| IMPORTANT: after the ADEN write the 68K address map changes (cart ROM
| window moves to 0x880000; low 64KB becomes the 32X 68K-ROM bank which
| mirrors cart contents from 0x100 up). All code here stays below 0x10000
| so execution continues seamlessly.
|
| Real-hardware note (emulator-first disclosure): a hardware cart needs the
| full Sega 32X security startup sequence here; PicoDrive's HLE does not.
| That work is deferred until a real-hardware milestone.

    .org    0
vectors:
    .long   0x00FFFE00          | initial SSP
    .long   entry               | initial PC
    .rept   62
    .long   trap
    .endr

header:
    .ascii  "SEGA 32X        "                                  | console
    .ascii  "(C)NC32 2026.JUN"                                  | copyright
    .ascii  "NANOCAMELID 32XCD M1 BOOT PROOF                 "  | domestic name
    .ascii  "NANOCAMELID 32XCD M1 BOOT PROOF                 "  | overseas name
    .ascii  "GM NC320001-01"                                    | serial
    .word   0                                                   | checksum
    .ascii  "J               "                                  | io support
    .long   0x000000, 0x0007FFFF                                | rom range
    .long   0x00FF0000, 0x00FFFFFF                              | ram range
    .ascii  "            "                                      | no sram
    .ascii  "            "                                      | modem
    .ascii  "                                        "          | memo
    .ascii  "JUE             "                                  | region

| --- 0x200: entry ------------------------------------------------------
entry:
    move.w  #0x2700, %sr
    | TMSS
    move.b  0xA10001, %d0
    andi.b  #0x0F, %d0
    beq.s   1f
    move.l  #0x53454741, 0xA14000
1:
    | clear our status/heartbeat block at FF6000-FF60FF
    lea     0xFF6000, %a0
    moveq   #63, %d0
2:  clr.l   (%a0)+
    dbra    %d0, 2b

    | enable 32X: ADEN (bit 0) + nRES (bit 1). ADEN remaps the bus;
    | nRES releases the SH-2s from reset so the HLE boot code runs.
    move.w  #0x0003, 0xA15100

    | wait for master SH-2: COMM0/1 == 'M_OK'
    move.l  #0x4D5F4F4B, %d1
3:  addq.l  #1, 0xFF6010        | spin counter (proves we got here)
    cmp.l   0xA15120, %d1
    bne.s   3b
    | wait for slave SH-2: COMM2/3 == 'S_OK'
    move.l  #0x535F4F4B, %d1
4:  addq.l  #1, 0xFF6014
    cmp.l   0xA15124, %d1
    bne.s   4b

    | mark mailbox ok
    move.w  #0x0001, 0xFF6020

main_loop:
    addq.l  #1, 0xFF6000        | MAIN68K heartbeat (u32)
    | snapshot SH-2 heartbeats from COMM6/7 for the 68K-side view
    move.w  0xA1512C, 0xFF6008  | SH2M comm heartbeat
    move.w  0xA1512E, 0xFF600A  | SH2S comm heartbeat
    bra.s   main_loop

trap:
    addq.l  #1, 0xFF6030        | unexpected exception counter
    bra.s   trap
