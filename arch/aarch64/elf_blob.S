.section .rodata.elf,"a"
.balign 8
.global lume_user_elf_begin, lume_user_elf_end
lume_user_elf_begin:
    .incbin LUME_USER_ELF_FILE
lume_user_elf_end:
.section .note.GNU-stack,"",%progbits
