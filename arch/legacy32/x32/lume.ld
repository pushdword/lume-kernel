/* The kernel image leaves space for U-Boot code and its 1 MiB heap in low SDRAM. The kernel
 * stack lies below the final MiB reserved for the BIOS and boot stacks. */
OUTPUT_FORMAT(elf32-x32)
ENTRY(_start)
SECTIONS {
 . = 0x10400000;
 .text : { KEEP(*(.text.entry)) *(.text .text.*) }
 .rodata ALIGN(4) : { *(.rodata .rodata.*) }
 .data ALIGN(4) : { *(.data .data.*) }
 __file_end = .;
 .bss ALIGN(4) (NOLOAD) : { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __bss_end = .; }
 __stack_bottom = 0x13efe000;
 __stack_top = 0x13f00000;
 ASSERT(__bss_end < __stack_bottom, "Kernel overlaps private stack")
 /DISCARD/ : { *(.comment .eh_frame .note.*) }
}
