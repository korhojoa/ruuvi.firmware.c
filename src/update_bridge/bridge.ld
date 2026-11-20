ENTRY(bridge_reset)
MEMORY
{
    FLASH (rx) : ORIGIN = 0x26000, LENGTH = 4096
}
SECTIONS
{
    .vectors : { KEEP(*(.vectors)) } > FLASH
    .text : { *(.text*) *(.rodata*) } > FLASH
    /DISCARD/ : { *(.comment*) *(.ARM*) }
}
ASSERT(SIZEOF(.vectors) + SIZEOF(.text) <= 4096, "Bridge exceeds one page")
