#ifndef LUME_ARCH_CONTRACT_H
#define LUME_ARCH_CONTRACT_H
#ifdef LUME_ARCH_R32
#define ARCH_REG_CALL 17u
#define ARCH_REG_RESULT 10u
#define ARCH_REG_ARG0 10u
#define ARCH_REG_ARG1 11u
#define ARCH_REG_ARG2 12u
#define ARCH_REG_ARGC 10u
#define ARCH_REG_ARGV 11u
#define ARCH_REG_ENVP 12u
#define ARCH_REG_SP 2u
#define ARCH_PAGE_USER 0x10u
#define ARCH_PAGE_TABLE 0x1fu
#define ARCH_ELF_MACHINE 0xfe33u
#define ARCH_KERNEL_BANNER "LUME KERNEL / R32 ISA v1\n"
#define ARCH_APP_SUFFIX ".r32"
#define arch_kernel_switch r32_kernel_switch
#else
#define ARCH_REG_CALL 1u
#define ARCH_REG_RESULT 1u
#define ARCH_REG_ARG0 4u
#define ARCH_REG_ARG1 2u
#define ARCH_REG_ARG2 3u
#define ARCH_REG_ARGC 8u
#define ARCH_REG_ARGV 7u
#define ARCH_REG_ENVP 3u
#define ARCH_REG_SP 5u
#define ARCH_PAGE_USER 0x30u
#define ARCH_PAGE_TABLE 0x3fu
#define ARCH_ELF_MACHINE 0xfe32u
#define ARCH_KERNEL_BANNER "LUME KERNEL / X32 ISA v1\n"
#define ARCH_APP_SUFFIX ".x32"
#define arch_kernel_switch x32_kernel_switch
#endif
#define ARCH_PAGE_READ (ARCH_PAGE_USER|3u)
#define ARCH_PAGE_WRITE (ARCH_PAGE_USER|7u)
#define ARCH_PAGE_EXEC (ARCH_PAGE_USER|11u)
#define ARCH_PAGE_BASE (ARCH_PAGE_USER|1u)
#endif
