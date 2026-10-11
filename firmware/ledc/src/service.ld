ENTRY(entry)
SECTIONS
{
    . = 0x00050000;
    .text : { *(.text.entry) *(.text*) *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
