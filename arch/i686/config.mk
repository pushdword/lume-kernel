CC := "$(TOOLBIN)clang$(HOSTEXE)" --target=i686-unknown-none-elf
LD := "$(TOOLBIN)ld.lld$(HOSTEXE)"
OBJCOPY := "$(TOOLBIN)llvm-objcopy$(HOSTEXE)"
CFLAGS := -O2 -std=c11 -m32 -march=i686 -mno-sse -mno-sse2 -mno-mmx -msoft-float -fno-pic -fno-pie $(COMMON_FLAGS)
