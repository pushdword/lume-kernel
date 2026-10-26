.section .rodata.shell,"a"
.balign 8
.global lume_shell_elf_begin, lume_shell_elf_end
lume_shell_elf_begin:
    .incbin LUME_SHELL_ELF_FILE
lume_shell_elf_end:
.section .note.GNU-stack,"",%progbits
