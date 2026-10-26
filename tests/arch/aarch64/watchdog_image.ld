OUTPUT_ARCH(aarch64)
ENTRY(_start)
SECTIONS
{
    . = 0x200000;
    .text : { KEEP(*(.text.boot)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    __image_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT((_start == 0x200000), "Watchdog test entry must be 0x200000")
ASSERT((__image_end <= 0x201000), "Watchdog test exceeds one page")
