/* Slave SH-2: runs from uncached cart ROM (entered via 32X header 0x3E4).*/
MEMORY
{
    rom : ORIGIN = 0x02002000, LENGTH = 0x1000
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
