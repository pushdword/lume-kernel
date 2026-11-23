.DEFAULT_GOAL := all
ifeq ($(origin CC),default)
CC := clang
endif
DDK ?= /data/sdk/ddk
DDK_BARE ?= $(DDK)/bare
-include $(DDK)/arch.mk
ARCH ?= aarch64
DRIVER ?= ram_block
SOURCES ?= $(DRIVER).c
ifeq ($(origin LD),default)
LD := ld.lld
endif
LD ?= ld.lld
MODULE ?= 0
LINUX_COMPAT ?= 0
OBJECTS := $(SOURCES:.c=.o)
ifeq ($(ARCH),amd64)
DDK_TARGET := --target=x86_64-unknown-none-elf -march=x86-64 -mno-red-zone -mgeneral-regs-only
else ifeq ($(ARCH),aarch64)
DDK_TARGET := --target=aarch64-unknown-none-elf -mcpu=cortex-a76 -mstrict-align
else
$(error Unsupported driver architecture $(ARCH))
endif
DDK_FLAGS := $(DDK_TARGET) -std=c11 -O2 -g -fno-omit-frame-pointer -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -nostdinc -isystem "$(DDK_BARE)" -I"$(DDK)/include"
ifeq ($(LINUX_COMPAT),1)
DDK_FLAGS := $(filter-out -std=c11 -ffunction-sections -fdata-sections,$(DDK_FLAGS)) -std=gnu11 -I"$(DDK)/linux/include" -DKBUILD_MODNAME='"$(DRIVER)"'
DDK_RUNTIME := $(DDK)/linux/runtime.a
endif
.PHONY: all module
module: $(DRIVER).lkm
ifeq ($(MODULE),1)
all: $(DRIVER).lkm
$(DRIVER).lkm: $(OBJECTS) $(DDK_RUNTIME)
	$(LD) -r --build-id=none $(OBJECTS) $(if $(DDK_RUNTIME),"$(DDK_RUNTIME)") -o "$@"
else
all: $(OBJECTS)
endif
%.o: %.c
	$(CC) $(DDK_FLAGS) $(CFLAGS) -MMD -MP -c "$<" -o "$@"
-include $(OBJECTS:.o=.d)
