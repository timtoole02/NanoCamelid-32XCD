/* Main 68000 C runtime: lives in cart ROM at 0x1800, below the 64K
 * post-ADEN bank limit. No .data/.bss allowed — state lives at fixed
 * work-RAM addresses (memory-map.md). */
MEMORY
{
    rom : ORIGIN = 0x1800, LENGTH = 0x6800
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
ASSERT(SIZEOF(.data) == 0, "no .data allowed: use fixed work-RAM addresses")
ASSERT(SIZEOF(.bss) == 0, "no .bss allowed: use fixed work-RAM addresses")
