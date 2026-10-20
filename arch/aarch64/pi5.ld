OUTPUT_ARCH(aarch64)
ENTRY(_start)
PHDRS
{
    code PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS
{
    . = 0x200000;
    __image_start = .;
    .text : { KEEP(*(.text.boot)) *(.text .text.*) } :code
    . = ALIGN(4096);
    __text_end = .;
    .rodata : { *(.rodata .rodata.*) } :code
    . = ALIGN(4096);
    .usertext : {
        __user_code_start = .;
        KEEP(*(.user.text))
        __user_code_end = .;
    } :code
    . = ALIGN(4096);
    __readonly_end = .;
    .data : { *(.data .data.*) } :data
    . = ALIGN(16);
    __file_end = .;
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data
    .stack (NOLOAD) : {
        . = ALIGN(4096);
        __stack_guard = .;
        . += 4096;
        __stack_bottom = .;
        . += 0x10000;
        __stack_top = .;
    } :data
    __image_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT((_start == 0x200000), "Pi5 image entry must be 0x200000")
ASSERT((__image_end <= 0x400000), "Early image and stack exceed the reserved 2 MiB")
ASSERT(((__bss_end - __bss_start) % 16 == 0), "BSS alignment")
ASSERT((__user_code_end - __user_code_start <= 4096), "Early EL0 code must fit one page")
