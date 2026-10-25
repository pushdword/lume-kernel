SHELL := sh
.DEFAULT_GOAL := all
ARCH ?= x32
BOARD ?= $(if $(filter aarch64,$(ARCH)),raspberrypi5,tang25k)
SDK ?=
PROFILE ?= diagnostic
DEBUG ?= 0
DRIVER_OBJECTS ?=
SD_POLL ?= 0
ifeq ($(filter $(SD_POLL),0 1),)
$(error SD_POLL must be 0 or 1)
endif
PROFILE_ID := $(if $(filter diagnostic,$(PROFILE)),0,$(if $(filter normal,$(PROFILE)),1,$(if $(filter recovery,$(PROFILE)),2,invalid)))
ifeq ($(PROFILE_ID),invalid)
$(error PROFILE must be diagnostic, normal or recovery)
endif
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
CFLAGS := -O2 -std=c11 -mcpu=cortex-a76 -mstrict-align -fno-pic -fno-pie $(COMMON_FLAGS)
CPPFLAGS := -DLUME_BOOT_PROFILE=$(PROFILE_ID) -DLUME_SD_POLL=$(SD_POLL) -Isrc/common -Iinclude/freestanding -Iinclude/ddk -Iinclude/internal/aarch64 -Iinclude/uapi/common -Iinclude/uapi/aarch64 -Ivendor/aarch64/fatfs -Ivendor/aarch64/lwip/src/include -Ivendor/aarch64/mbedtls/include -DMBEDTLS_CONFIG_FILE='"lume/crypto_config.h"'
KERNEL_C := $(wildcard src/aarch64/*.c platform/raspberrypi5/*.c vendor/aarch64/lwip/src/core/*.c vendor/aarch64/lwip/src/core/ipv4/*.c) vendor/aarch64/lwip/src/netif/ethernet.c vendor/aarch64/fatfs/ff.c vendor/aarch64/fatfs/ffunicode.c vendor/aarch64/mbedtls/library/aes.c vendor/aarch64/mbedtls/library/sha1.c arch/aarch64/mmu.c arch/aarch64/mmu_tables.c src/common/terminal.c src/common/font5x7.c
KERNEL_C += arch/aarch64/dma.c
KERNEL_C := $(filter-out %/gem.c,$(KERNEL_C))
ifeq ($(DEBUG),1)
CFLAGS += -g -fno-omit-frame-pointer
endif
KERNEL_S := arch/aarch64/start.S arch/aarch64/user.S arch/aarch64/elf_blob.S arch/aarch64/worker.S
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
NATIVE_CC ?= "$(TOOLBIN)clang$(HOSTEXE)"
NATIVE_CPPFLAGS ?= -nostdinc -isystem "$(SDK)/sdk/bare"
.PHONY: native-build native-directories native-responses
native-directories: $(OUT)/diagnostic.elf $(OUT)/configuration
	mkdir -p $(foreach directory,$(sort $(dir $(OBJECTS))),"$(directory)")
native-responses: native-directories
	$(foreach source,$(KERNEL_C),$(file >$(OUT)/obj/$(source).o.rsp,$(NATIVE_CPPFLAGS) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF "$(OUT)/obj/$(source).o.d" -MT "$(OUT)/obj/$(source).o" -c "$(source)" -o "$(OUT)/obj/$(source).o"))
	$(foreach source,$(KERNEL_S),$(file >$(OUT)/obj/$(source).o.rsp,$(NATIVE_CPPFLAGS) $(CPPFLAGS) $(filter-out -mgeneral-regs-only,$(CFLAGS)) $(if $(filter arch/aarch64/elf_blob.S,$(source)),-DLUME_USER_ELF_FILE='"$(OUT)/diagnostic.elf"') $(if $(filter arch/aarch64/shell_blob.S,$(source)),-DLUME_SHELL_ELF_FILE='"$(SHELL_ELF)"') -c "$(source)" -o "$(OUT)/obj/$(source).o"))
	$(file >$(OUT)/native-jobs)
	$(foreach object,$(OBJECTS),$(file >>$(OUT)/native-jobs,$(object).rsp))
native-build: native-responses
	$(NATIVE_CC) --batch "$(OUT)/native-jobs"
	$(LD) $(LINK_FLAGS) --gc-sections --build-id=none -T "$(SCRIPT)" $(OBJECTS) $(DRIVER_OBJECTS) -o "$(OUT)/lume.elf"
	$(OBJCOPY) -O binary "$(OUT)/lume.elf" "$(PAYLOAD)"
$(OUT)/diagnostic.elf: tests/aarch64/user/start.S tests/aarch64/user/main.c tests/aarch64/user/user.ld
	mkdir -p "$(@D)"
	$(CC) $(CFLAGS) -nostdlib -static -Wl,--build-id=none,-z,max-page-size=4096,-T,tests/aarch64/user/user.ld tests/aarch64/user/start.S tests/aarch64/user/main.c -o "$@"
.PHONY: watchdog-test-image
watchdog-test-image: $(OUT)/watchdog-test.img
$(OUT)/watchdog-test.elf: tests/aarch64/watchdog_image.S tests/aarch64/watchdog_image.ld
	mkdir -p "$(@D)"
	$(CC) $(CFLAGS) -nostdlib -static -Wl,--build-id=none,-z,max-page-size=4096,-T,tests/aarch64/watchdog_image.ld "$<" -o "$@"
$(OUT)/watchdog-test.img: $(OUT)/watchdog-test.elf
	$(OBJCOPY) -O binary "$<" "$@"
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
CPPFLAGS += -Iinclude/uapi/common -Iinclude/uapi/$(ARCH) -Isrc/common -Isrc/$(ARCH) -Isrc/$(ARCH)/freestanding -Iarch/$(ARCH) -Ivendor/$(ARCH)/fatfs -Ivendor/$(ARCH)/fat_ro -DLUME_KERNEL_PERF=1
CFLAGS := -std=c11 -O2 $(COMMON_FLAGS)
KERNEL_C := src/common/terminal.c $(addprefix src/$(ARCH)/,kernel.c pages.c fat.c console.c keymap_pt.c sd.c strings.c network.c uart_queue.c console_wait.c posix_fd.c random.c crypto_device.c) vendor/$(ARCH)/fat_ro/fat_ro.c vendor/$(ARCH)/fatfs/ff.c
KERNEL_S := arch/$(ARCH)/entry.s arch/$(ARCH)/memory.s
ifeq ($(ARCH),x32)
KERNEL_S += src/x32/kernel_work.s
endif
SCRIPT := arch/$(ARCH)/kernel.ld
endif
ifneq ($(strip $(SCHED_QUANTUM_MS)),)
SCHED_CPPFLAGS += -DLUME_SCHED_QUANTUM=$(SCHED_QUANTUM_MS)
endif
ifneq ($(strip $(SCHED_INTERVAL_MS)),)
SCHED_CPPFLAGS += -DSCHED_TICK_MS=$(SCHED_INTERVAL_MS)
endif
CPPFLAGS += $(SCHED_CPPFLAGS)
OBJECTS := $(addprefix $(OUT)/obj/,$(addsuffix .o,$(KERNEL_S) $(KERNEL_C)))
.PHONY: all headers check check-image clean
all: $(PAYLOAD) headers
headers:
	mkdir -p "$(OUT)/include"
	cp -Rp include/uapi/common/. "$(OUT)/include/"
	cp -Rp include/uapi/$(ARCH)/. "$(OUT)/include/"
$(OUT)/obj/%.c.o: %.c $(OUT)/configuration Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF "$@.d" -MT "$@" -c "$<" -o "$@"
$(OUT)/obj/%.s.o: %.s $(OUT)/configuration Makefile
	mkdir -p "$(@D)"
	$(CC) -c "$<" -o "$@"
$(OUT)/obj/%.S.o: %.S $(OUT)/configuration Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(filter-out -mgeneral-regs-only,$(CFLAGS)) -c "$<" -o "$@"
$(OUT)/lume.elf: $(OBJECTS) $(DRIVER_OBJECTS) $(SCRIPT) Makefile $(OUT)/configuration
	$(LD) $(LINK_FLAGS) --gc-sections --build-id=none -T "$(SCRIPT)" $(OBJECTS) $(DRIVER_OBJECTS) -o "$@"
$(OUT)/lume.bin: $(OUT)/lume.elf
	$(OBJCOPY) -O binary "$<" "$@"
$(OUT)/lume-pi5.img: $(OUT)/lume.elf
	$(OBJCOPY) -O binary "$<" "$@"
$(OUT)/image-tool$(HOSTEXE): tools/image.cpp
	mkdir -p "$(@D)"
	$(HOSTCXX) $(HOSTCXXFLAGS) "$<" -o "$@"
$(OUT)/lume.r32k: $(OUT)/lume.elf $(OUT)/image-tool$(HOSTEXE)
	"$(OUT)/image-tool$(HOSTEXE)" r32-kernel "$<" "$@"
TEST_CPPFLAGS := -Iinclude/ddk -Iinclude/internal/aarch64 -Iinclude/uapi/common -Iinclude/uapi/aarch64 -Ivendor/aarch64/mbedtls/include -DMBEDTLS_CONFIG_FILE='"lume/crypto_config.h"'
TEST_NAMES := ddk devfs fdt-device fdt-initrd usb uart-input sdio wifi-net netif wpa volume watchdog stream icmp vfs heap user-stack mmu elf-load font
.PHONY: check-boot-storage
$(OUT)/tests/boot-storage$(HOSTEXE): tests/aarch64/boot_storage_test.c src/aarch64/storage.c src/aarch64/boot_storage.c src/aarch64/volume.c vendor/aarch64/fatfs/ff.c vendor/aarch64/fatfs/ffunicode.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) -Ivendor/aarch64/fatfs $^ -o "$@"
check-boot-storage: $(OUT)/tests/boot-storage$(HOSTEXE)
	env "$<" "$(MBR_IMAGE)" "$(DATA_IMAGE)" "$(BOOT_IMAGE)" "$(ROOT_IMAGE)"
check-image: $(OUT)/tests/root-image$(HOSTEXE)
	env "$(OUT)/tests/root-image$(HOSTEXE)" "$(ROOT_IMAGE)"
$(OUT)/tests/root-image$(HOSTEXE): tests/aarch64/root_image_test.c src/aarch64/vfs.c src/aarch64/fat.c src/aarch64/block.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"
check: check-scheduler $(addprefix $(OUT)/tests/,$(addsuffix $(HOSTEXE),$(TEST_NAMES)))
	env "$(OUT)/tests/ddk$(HOSTEXE)"
	env "$(OUT)/tests/devfs$(HOSTEXE)"
	env "$(OUT)/tests/fdt-device$(HOSTEXE)"
	env "$(OUT)/tests/fdt-initrd$(HOSTEXE)"
	env "$(OUT)/tests/usb$(HOSTEXE)"
	env "$(OUT)/tests/uart-input$(HOSTEXE)"
	env "$(OUT)/tests/sdio$(HOSTEXE)"
	env "$(OUT)/tests/wifi-net$(HOSTEXE)"
	env "$(OUT)/tests/netif$(HOSTEXE)"
	env "$(OUT)/tests/wpa$(HOSTEXE)" tests/aarch64/fixtures/wpa-handshake.bin
	env "$(OUT)/tests/volume$(HOSTEXE)"
	env "$(OUT)/tests/watchdog$(HOSTEXE)"
	env "$(OUT)/tests/stream$(HOSTEXE)"
	env "$(OUT)/tests/icmp$(HOSTEXE)"
	env "$(OUT)/tests/vfs$(HOSTEXE)"
	env "$(OUT)/tests/heap$(HOSTEXE)"
	env "$(OUT)/tests/user-stack$(HOSTEXE)"
	env "$(OUT)/tests/mmu$(HOSTEXE)"
	env "$(OUT)/tests/elf-load$(HOSTEXE)"
	env "$(OUT)/tests/font$(HOSTEXE)"

.PHONY: check-scheduler
.PHONY: check-gem
check-gem: $(OUT)/tests/gem$(HOSTEXE)
	env "$<"
$(OUT)/tests/gem$(HOSTEXE): examples/ddk/gem_test.c examples/ddk/gem.c examples/ddk/gem.h examples/ddk/gem_netif.c include/ddk/lume/netif.h src/aarch64/device.c src/aarch64/netif.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) examples/ddk/gem_test.c src/aarch64/device.c src/aarch64/netif.c -o "$@"

check-scheduler: $(foreach c,1 2 4,$(OUT)/tests/scheduler-$(c)$(HOSTEXE))
	$(foreach c,1 2 4,env "$(OUT)/tests/scheduler-$(c)$(HOSTEXE)" &&) true
$(OUT)/tests/scheduler-%$(HOSTEXE): tests/common/scheduler_test.c src/common/scheduler.h src/common/scheduler_config.h $(OUT)/configuration
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(SCHED_CPPFLAGS) -Isrc/common -DSCHED_CPUS=$* "$<" -o "$@"
$(OUT)/tests/fdt-initrd$(HOSTEXE): tests/aarch64/fdt_initrd_test.c src/aarch64/fdt.c
$(OUT)/tests/fdt-device$(HOSTEXE): tests/aarch64/fdt_device_test.c src/aarch64/fdt.c
$(OUT)/tests/ddk$(HOSTEXE): tests/aarch64/ddk_test.c src/aarch64/device.c src/aarch64/irq.c src/aarch64/dma.c src/aarch64/block.c src/aarch64/work.c
$(OUT)/tests/usb$(HOSTEXE): tests/aarch64/usb_test.c src/aarch64/usb_descriptors.c src/aarch64/hid_keyboard.c
$(OUT)/tests/uart-input$(HOSTEXE): tests/aarch64/uart_input_test.c
$(OUT)/tests/volume$(HOSTEXE): tests/aarch64/volume_test.c src/aarch64/volume.c
$(OUT)/tests/watchdog$(HOSTEXE): tests/aarch64/watchdog_test.c src/aarch64/watchdog.c
$(OUT)/tests/icmp$(HOSTEXE): tests/aarch64/icmp_test.c src/aarch64/icmp.c
$(OUT)/tests/icmp$(HOSTEXE): TEST_CPPFLAGS += -Ivendor/aarch64/lwip/src/include
$(OUT)/tests/stream$(HOSTEXE): tests/aarch64/stream_test.c src/aarch64/stream.c
$(OUT)/tests/vfs$(HOSTEXE): tests/aarch64/vfs_test.c src/aarch64/vfs.c src/aarch64/fat.c src/aarch64/block.c
$(OUT)/tests/devfs$(HOSTEXE): tests/aarch64/devfs_test.c examples/ddk/ram_block.c src/aarch64/device.c src/aarch64/vfs.c src/aarch64/fat.c src/aarch64/block.c
$(OUT)/tests/heap$(HOSTEXE): tests/aarch64/heap_test.c src/aarch64/heap.c
$(OUT)/tests/stream$(HOSTEXE): TEST_CPPFLAGS += -Ivendor/aarch64/lwip/src/include
$(OUT)/tests/sdio$(HOSTEXE): tests/aarch64/sdio_test.c src/aarch64/sdio.c
$(OUT)/tests/wifi-net$(HOSTEXE): tests/aarch64/wifi_net_test.c src/aarch64/wifi_net.c
$(OUT)/tests/netif$(HOSTEXE): tests/aarch64/netif_test.c src/aarch64/netif.c src/aarch64/wireless.c src/aarch64/net_control.c
$(OUT)/tests/wpa$(HOSTEXE): tests/aarch64/wpa_test.c src/aarch64/wpa.c vendor/aarch64/mbedtls/library/aes.c vendor/aarch64/mbedtls/library/sha1.c
$(addprefix $(OUT)/tests/,$(addsuffix $(HOSTEXE),$(filter-out user-stack mmu elf-load font,$(TEST_NAMES)))):
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
	$(file >$@.tmp,$(CC) | $(CPPFLAGS) | $(CFLAGS) | $(SDK) | $(SHELL_ELF) | $(DRIVER_OBJECTS))
	cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"

$(OUT)/tests/user-stack$(HOSTEXE): tests/aarch64/user_stack_test.c src/aarch64/user_stack.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"
$(OUT)/tests/mmu$(HOSTEXE): tests/aarch64/mmu_test.c arch/aarch64/mmu_tables.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"
$(OUT)/tests/elf-load$(HOSTEXE): tests/aarch64/elf_load_test.c src/aarch64/elf.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"

$(OUT)/tests/font$(HOSTEXE): tests/aarch64/font_test.c src/common/font5x7.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -Isrc/common $^ -o "$@"

ifeq ($(ARCH),aarch64)
.PHONY: display-test-image
display-test-image: $(OUT)/tests/display.elf
$(OUT)/tests/display.elf: tests/aarch64/display_test.c platform/raspberrypi5/display.c src/common/terminal.c src/common/font5x7.c src/aarch64/memory.c arch/aarch64/pi5.ld src/common/terminal.h src/common/font5x7.h include/internal/aarch64/lume/display.h $(OUT)/configuration Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -nostdlib -static -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096,-T,arch/aarch64/pi5.ld tests/aarch64/display_test.c src/common/terminal.c src/common/font5x7.c src/aarch64/memory.c src/aarch64/dma.c arch/aarch64/dma.c platform/raspberrypi5/dma.c -o "$@"
endif
