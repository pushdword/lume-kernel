CC := "$(TOOLBIN)clang$(HOSTEXE)" --target=aarch64-unknown-none-elf
LD := "$(TOOLBIN)ld.lld$(HOSTEXE)"
OBJCOPY := "$(TOOLBIN)llvm-objcopy$(HOSTEXE)"
CFLAGS := -O2 -std=c11 -mcpu=$(CPU) -mstrict-align -fno-pic -fno-pie $(COMMON_FLAGS)
