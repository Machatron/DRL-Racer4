ENTRY(font_applet)
MEMORY { RAM (rwx) : ORIGIN = 0x20020000, LENGTH = 0x2000 }
SECTIONS
{
    .text :
    {
        LONG(0x20024000)
        LONG(font_applet | 1)
        *(.text*)
        *(.rodata*)
    } > RAM
    .data : { *(.data*) } > RAM
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment*) }
}
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "Font helper must not need C runtime startup")
