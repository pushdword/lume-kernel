.DEFAULT_GOAL := all
ifeq ($(origin CC),default)
CC := clang
endif
DDK ?= /data/sdk/ddk
DRIVER ?= ram_block
SOURCES ?= $(DRIVER).c
ifeq ($(origin LD),default)
LD := ld.lld
endif
LD ?= ld.lld
MODULE ?= 0
OBJECTS := $(SOURCES:.c=.o)
DDK_FLAGS := --target=aarch64-unknown-none-elf -mcpu=cortex-a76 -mstrict-align -std=c11 -O2 -g -fno-omit-frame-pointer -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -nostdinc -isystem "$(DDK)/bare" -I"$(DDK)/include"
.PHONY: all module
module: $(DRIVER).lkm
ifeq ($(MODULE),1)
all: $(DRIVER).lkm
$(DRIVER).lkm: $(OBJECTS)
	$(LD) -r --build-id=none $(OBJECTS) -o "$@"
else
all: $(OBJECTS)
endif
%.o: %.c
	$(CC) $(DDK_FLAGS) $(CFLAGS) -MMD -MP -c "$<" -o "$@"
-include $(OBJECTS:.o=.d)
