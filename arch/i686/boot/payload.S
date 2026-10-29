.section .rodata,"a"
.balign 4096
.global __user_code_start, __user_code_end
.global lume_user_kernel_probe, lume_user_ro_probe
__user_code_start:
lume_user_kernel_probe:
lume_user_ro_probe:
    ud2
.balign 4096
__user_code_end:
.global lume_user_elf_begin, lume_user_elf_end
lume_user_elf_begin:
lume_user_elf_end:
.global lume_shell_elf_begin, lume_shell_elf_end
lume_shell_elf_begin:
#ifdef LUME_SHELL_ELF_FILE
.incbin LUME_SHELL_ELF_FILE
#endif
lume_shell_elf_end:
.section .note.GNU-stack,"",@progbits
