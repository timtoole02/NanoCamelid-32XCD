/* Master SH-2: runs from uncached cart ROM (entered via 32X header 0x3E0).
 * No .data/.bss — state lives at fixed SDRAM addresses (mailbox.h). */
MEMORY
{
    rom : ORIGIN = 0x22000800, LENGTH = 0x800
}
SECTIONS
{
    .text : {
        *(.text.entry)
        *(.text*)
        *(.rodata*)
    } > rom
    /DISCARD/ : { *(.comment) *(.note*) }
}
ASSERT(SIZEOF(.data) == 0, "no .data allowed on SH-2: use SHARED() addresses")
ASSERT(SIZEOF(.bss) == 0, "no .bss allowed on SH-2: use SHARED() addresses")
