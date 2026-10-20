OUTPUT_FORMAT(elf32-x32)
ENTRY(_start)
SECTIONS {
 . = 0x10400000;
 .text : { KEEP(*(.text.entry)) *(.text .text.*) }
 .rodata ALIGN(4) : { *(.rodata .rodata.*) }
 .data ALIGN(4) : { *(.data .data.*) }
 __file_end = .;
 .bss ALIGN(4096) (NOLOAD) : { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __bss_end = .; }
 ASSERT(__bss_end <= 0x10700000, "Kernel exceeds initial3MiB image/BSS reservation")
 /DISCARD/ : { *(.comment .eh_frame .note.*) }
}
