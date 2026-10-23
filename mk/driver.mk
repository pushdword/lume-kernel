ifeq ($(origin CC),default)
CC := clang
endif
DDK ?= /data/sdk/ddk
DRIVER ?= ram_block
DDK_FLAGS := --target=aarch64-unknown-none-elf -mcpu=cortex-a76 -mstrict-align -std=c11 -O2 -g -fno-omit-frame-pointer -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -ffunction-sections -fdata-sections -nostdinc -isystem "$(DDK)/bare" -I"$(DDK)/include"
.PHONY: all
all: $(DRIVER).o
$(DRIVER).o: $(DRIVER).c
	$(CC) $(DDK_FLAGS) $(CFLAGS) -MMD -MP -c "$<" -o "$@"
-include $(DRIVER).d
