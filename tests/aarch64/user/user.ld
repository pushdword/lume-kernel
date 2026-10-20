OUTPUT_ARCH(aarch64)
ENTRY(_user_start)
PHDRS { code PT_LOAD FLAGS(5); ro PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS
{
    . = 0x4000040000;
    .text : { KEEP(*(.text.start)) *(.text .text.*) } :code
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) } :ro
    . = ALIGN(4096);
    .data : { *(.data .data.*) } :data
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
