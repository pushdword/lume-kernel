SHELL := sh
.DEFAULT_GOAL := all
ARCH ?= x32
BOARD ?= $(if $(filter aarch64,$(ARCH)),raspberrypi5,tang25k)
SDK ?=
OUT ?= build/$(ARCH)
ifeq ($(filter build/%,$(OUT)),)
$(error OUT must be a directory below build/)
endif
ifneq ($(findstring ..,$(OUT)),)
$(error OUT cannot contain parent-directory components)
endif
HOSTCC ?= cc
HOSTCXX ?= c++
HOSTCFLAGS ?= -O2 -std=c11
HOSTCXXFLAGS ?= -O2 -std=c++17
HOSTEXE := $(if $(filter Windows_NT,$(OS)),.exe,)
TOOLBIN = $(if $(strip $(SDK)),$(SDK)/bin/,)
COMMON_FLAGS := -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
ifeq ($(ARCH),aarch64)
CC := "$(TOOLBIN)clang$(HOSTEXE)" --target=aarch64-unknown-none-elf
LD := "$(TOOLBIN)ld.lld$(HOSTEXE)"
OBJCOPY := "$(TOOLBIN)llvm-objcopy$(HOSTEXE)"
CFLAGS := -O2 -std=c11 -mcpu=cortex-a76 -mgeneral-regs-only -mstrict-align -fno-pic -fno-pie $(COMMON_FLAGS)
CPPFLAGS := -Iinclude/freestanding -Iinclude/internal/aarch64 -Iinclude/uapi/aarch64 -Ivendor/aarch64/fatfs -Ivendor/aarch64/lwip/src/include -Ivendor/aarch64/mbedtls/include -DMBEDTLS_CONFIG_FILE='"lume/crypto_config.h"'
KERNEL_C := $(wildcard src/aarch64/*.c platform/raspberrypi5/*.c vendor/aarch64/lwip/src/core/*.c vendor/aarch64/lwip/src/core/ipv4/*.c) vendor/aarch64/lwip/src/netif/ethernet.c vendor/aarch64/fatfs/ff.c vendor/aarch64/mbedtls/library/aes.c vendor/aarch64/mbedtls/library/sha1.c arch/aarch64/mmu.c
KERNEL_S := arch/aarch64/start.S arch/aarch64/user.S arch/aarch64/elf_blob.S
ifneq ($(strip $(SHELL_ELF)),)
KERNEL_S += arch/aarch64/shell_blob.S
$(OUT)/obj/arch/aarch64/shell_blob.S.o: CPPFLAGS += -DLUME_SHELL_ELF_FILE='"$(SHELL_ELF)"'
empty :=
space := $(empty) $(empty)
SHELL_ELF_DEP = $(subst $(space),\$(space),$(SHELL_ELF))
$(OUT)/obj/arch/aarch64/shell_blob.S.o: $(SHELL_ELF_DEP)
else
KERNEL_S += arch/aarch64/empty_shell.S
endif
$(OUT)/obj/arch/aarch64/elf_blob.S.o: CPPFLAGS += -DLUME_USER_ELF_FILE='"$(OUT)/diagnostic.elf"'
$(OUT)/obj/arch/aarch64/elf_blob.S.o: $(OUT)/diagnostic.elf
SCRIPT := arch/aarch64/pi5.ld
LINK_FLAGS := -z max-page-size=4096
PAYLOAD := $(OUT)/lume-pi5.img
$(OUT)/diagnostic.elf: tests/aarch64/user/start.S tests/aarch64/user/main.c tests/aarch64/user/user.ld
	mkdir -p "$(@D)"
	$(CC) $(CFLAGS) -nostdlib -static -Wl,--build-id=none,-z,max-page-size=4096,-T,tests/aarch64/user/user.ld tests/aarch64/user/start.S tests/aarch64/user/main.c -o "$@"
else
ifeq ($(ARCH),r32)
CC := "$(TOOLBIN)r32-clang$(HOSTEXE)"
LD := "$(TOOLBIN)r32-ld.lld$(HOSTEXE)"
OBJCOPY := "$(TOOLBIN)r32-objcopy$(HOSTEXE)"
CPPFLAGS := -DLUME_ARCH_R32=1 -Iarch/legacy32
LINK_FLAGS := -m r32elf
PAYLOAD := $(OUT)/lume.r32k
else ifeq ($(ARCH),x32)
CC := "$(TOOLBIN)clang$(HOSTEXE)" --target=x32-unknown-none-elf
LD := "$(TOOLBIN)ld.lld$(HOSTEXE)"
OBJCOPY := "$(TOOLBIN)llvm-objcopy$(HOSTEXE)"
LINK_FLAGS := -m x32elf
PAYLOAD := $(OUT)/lume.bin
else
$(error Unsupported ARCH $(ARCH))
endif
CPPFLAGS += -Iinclude/uapi/$(ARCH) -Isrc/$(ARCH) -Isrc/$(ARCH)/freestanding -Iarch/$(ARCH) -Ivendor/$(ARCH)/fatfs -Ivendor/$(ARCH)/fat_ro -DLUME_KERNEL_PERF=1
CFLAGS := -std=c11 -O2 $(COMMON_FLAGS)
KERNEL_C := $(addprefix src/$(ARCH)/,kernel.c pages.c fat.c console.c terminal.c keymap_pt.c sd.c strings.c network.c uart_queue.c console_wait.c posix_fd.c random.c crypto_device.c) vendor/$(ARCH)/fat_ro/fat_ro.c vendor/$(ARCH)/fatfs/ff.c
KERNEL_S := arch/$(ARCH)/entry.s arch/$(ARCH)/memory.s
ifeq ($(ARCH),x32)
KERNEL_S += src/x32/kernel_work.s
endif
SCRIPT := arch/$(ARCH)/kernel.ld
endif
OBJECTS := $(addprefix $(OUT)/obj/,$(addsuffix .o,$(KERNEL_S) $(KERNEL_C)))
.PHONY: all headers check clean
all: $(PAYLOAD) headers
headers:
	mkdir -p "$(OUT)/include"
	cp -Rp include/uapi/$(ARCH)/. "$(OUT)/include/"
$(OUT)/obj/%.c.o: %.c $(OUT)/configuration Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF "$@.d" -MT "$@" -c "$<" -o "$@"
$(OUT)/obj/%.s.o: %.s $(OUT)/configuration Makefile
	mkdir -p "$(@D)"
	$(CC) -c "$<" -o "$@"
$(OUT)/obj/%.S.o: %.S $(OUT)/configuration Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"
$(OUT)/lume.elf: $(OBJECTS) $(SCRIPT) Makefile $(OUT)/configuration
	$(LD) $(LINK_FLAGS) --gc-sections --build-id=none -T "$(SCRIPT)" $(OBJECTS) -o "$@"
$(OUT)/lume.bin: $(OUT)/lume.elf
	$(OBJCOPY) -O binary "$<" "$@"
$(OUT)/lume-pi5.img: $(OUT)/lume.elf
	$(OBJCOPY) -O binary "$<" "$@"
$(OUT)/image-tool$(HOSTEXE): tools/image.cpp
	mkdir -p "$(@D)"
	$(HOSTCXX) $(HOSTCXXFLAGS) "$<" -o "$@"
$(OUT)/lume.r32k: $(OUT)/lume.elf $(OUT)/image-tool$(HOSTEXE)
	"$(OUT)/image-tool$(HOSTEXE)" r32-kernel "$<" "$@"
TEST_CPPFLAGS := -Iinclude/internal/aarch64 -Iinclude/uapi/aarch64 -Ivendor/aarch64/mbedtls/include -DMBEDTLS_CONFIG_FILE='"lume/crypto_config.h"'
TEST_NAMES := fdt-initrd usb uart-input sdio wifi-net netif wpa
check: $(addprefix $(OUT)/tests/,$(addsuffix $(HOSTEXE),$(TEST_NAMES)))
	env "$(OUT)/tests/fdt-initrd$(HOSTEXE)"
	env "$(OUT)/tests/usb$(HOSTEXE)"
	env "$(OUT)/tests/uart-input$(HOSTEXE)"
	env "$(OUT)/tests/sdio$(HOSTEXE)"
	env "$(OUT)/tests/wifi-net$(HOSTEXE)"
	env "$(OUT)/tests/netif$(HOSTEXE)"
	env "$(OUT)/tests/wpa$(HOSTEXE)" tests/aarch64/fixtures/wpa-handshake.bin
$(OUT)/tests/fdt-initrd$(HOSTEXE): tests/aarch64/fdt_initrd_test.c src/aarch64/fdt.c
$(OUT)/tests/usb$(HOSTEXE): tests/aarch64/usb_test.c src/aarch64/usb_descriptors.c src/aarch64/hid_keyboard.c
$(OUT)/tests/uart-input$(HOSTEXE): tests/aarch64/uart_input_test.c
$(OUT)/tests/sdio$(HOSTEXE): tests/aarch64/sdio_test.c src/aarch64/sdio.c
$(OUT)/tests/wifi-net$(HOSTEXE): tests/aarch64/wifi_net_test.c src/aarch64/wifi_net.c
$(OUT)/tests/netif$(HOSTEXE): tests/aarch64/netif_test.c src/aarch64/netif.c src/aarch64/wireless.c src/aarch64/net_control.c
$(OUT)/tests/wpa$(HOSTEXE): tests/aarch64/wpa_test.c src/aarch64/wpa.c vendor/aarch64/mbedtls/library/aes.c vendor/aarch64/mbedtls/library/sha1.c
$(addprefix $(OUT)/tests/,$(addsuffix $(HOSTEXE),$(TEST_NAMES))):
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"
clean:
	rm -rf -- "$(OUT)"
-include $(OBJECTS:=.d)

.PHONY: FORCE
FORCE:
$(OUT):
	mkdir -p "$@"
$(OUT)/configuration: FORCE | $(OUT)
	$(file >$@.tmp,$(CC) | $(CPPFLAGS) | $(CFLAGS) | $(SDK) | $(SHELL_ELF))
	cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
