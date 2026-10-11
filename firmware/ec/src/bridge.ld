ENTRY(bridge_entry)
SECTIONS
{
    . = 0x00060000;
    .text : { *(.text*) *(.rodata*) }
    /DISCARD/ : { *(.comment*) *(.note*) *(.eh_frame*) *(.ARM.exidx*) *(.ARM.extab*) }
}
