CC := "$(TOOLBIN)clang$(HOSTEXE)" --target=x86_64-unknown-none-elf
LD := "$(TOOLBIN)ld.lld$(HOSTEXE)"
OBJCOPY := "$(TOOLBIN)llvm-objcopy$(HOSTEXE)"
CFLAGS := -O2 -std=c11 -m64 -march=x86-64 -mno-red-zone -mgeneral-regs-only -fno-pic -fno-pie $(COMMON_FLAGS)
