OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(lume_boot32)
SECTIONS
{
    . = 0x200000;
    __image_start = .;
    .text : { KEEP(*(.multiboot)) KEEP(*(.text.boot)) *(.text .text.*) }
    . = ALIGN(4096);
    __text_end = .;
    .rodata : { *(.rodata .rodata.*) }
    .note.gnu.build-id : { __lume_kernel_build_id = .; *(.note.gnu.build-id) }
    . = ALIGN(4096);
    __readonly_end = .;
    .data : { *(.data .data.*) }
    .lume_drivers : {
        __lume_drivers_start = .;
        KEEP(*(.lume.drivers))
        __lume_drivers_end = .;
    }
    __load_end = .;
    .bss (NOLOAD) : ALIGN(4096) { __bss_start = .; *(.bss .bss.*) *(COMMON) __bss_end = .; }
    .stacks (NOLOAD) : ALIGN(4096) {
        __stack_guard = .; . += 4096;
        __stack_bottom = .; . += 0x20000; __stack_top = .;
        __worker_guard = .; . += 4096;
        __worker_stack = .; . += 0x40000; __worker_top = .;
        __trap_guard = .; . += 4096;
        __trap_stack = .; . += 0x20000; __trap_top = .;
        __fault_guard = .; . += 4096;
        __fault_stack = .; . += 0x8000; __fault_top = .;
        __secondary_stacks = .; . += 3 * 0x50000;
    }
    . = ALIGN(4096);
    __image_end = .;
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note.GNU-stack) }
}
ASSERT(__image_end < 0x2000000, "Kernel exceeds the early mapping window")
