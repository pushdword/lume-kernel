SHELL := sh
.DEFAULT_GOAL := all
ARCH ?= x32
CPU ?= $(if $(filter aarch64,$(ARCH)),cortex-a76,)
BOARD ?= $(if $(filter aarch64,$(ARCH)),raspberrypi5,tang25k)
SDK ?=
PROFILE ?= diagnostic
DEBUG ?= 0
DRIVER_OBJECTS ?=
SD_POLL ?= 0
ifeq ($(filter $(SD_POLL),0 1),)
$(error SD_POLL must be 0 or 1)
endif

PROFILE_FILE := config/profiles/$(PROFILE).mk
ifeq ($(wildcard $(PROFILE_FILE)),)
$(error PROFILE must be diagnostic, normal or recovery)
endif
include $(PROFILE_FILE)
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
include mk/kernel.mk
COMMON_FLAGS := -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
ifeq ($(ARCH),aarch64)
include platform/$(BOARD)/config.mk
include arch/aarch64/config.mk
CPPFLAGS := -DLUME_BOOT_PROFILE=$(PROFILE_ID) -DLUME_SD_POLL=$(SD_POLL) -Isrc/core -Isrc/terminal -Iinclude/freestanding -Iinclude/ddk -Iinclude/kernel -Iinclude/uapi -Iinclude/abi/native -Ivendor/fatfs -Ivendor/lwip/src/include -Ivendor/mbedtls/include -DMBEDTLS_CONFIG_FILE='"lume/crypto_config.h"'
KERNEL_C := $(KERNEL_C_aarch64)
CPPFLAGS += $(PLATFORM_CPPFLAGS) -Iarch/aarch64/include/kernel -Isrc/core -Isrc/terminal -Ivendor/libfdt
KERNEL_C := $(filter-out %/gem.c,$(KERNEL_C))
ifeq ($(DEBUG),1)
CFLAGS += -g -fno-omit-frame-pointer
endif
KERNEL_S := arch/aarch64/boot/start.S arch/aarch64/context/user.S arch/aarch64/boot/elf_blob.S arch/aarch64/context/worker.S
ifneq ($(strip $(SHELL_ELF)),)
KERNEL_S += arch/aarch64/boot/shell_blob.S
$(OUT)/obj/arch/aarch64/boot/shell_blob.S.o: CPPFLAGS += -DLUME_SHELL_ELF_FILE='"$(SHELL_ELF)"'
empty :=
space := $(empty) $(empty)
SHELL_ELF_DEP = $(subst $(space),\$(space),$(SHELL_ELF))
$(OUT)/obj/arch/aarch64/boot/shell_blob.S.o: $(SHELL_ELF_DEP)
else
KERNEL_S += arch/aarch64/boot/empty_shell.S
endif
$(OUT)/obj/arch/aarch64/boot/elf_blob.S.o: CPPFLAGS += -DLUME_USER_ELF_FILE='"$(OUT)/diagnostic.elf"'
$(OUT)/obj/arch/aarch64/boot/elf_blob.S.o: $(OUT)/diagnostic.elf
SCRIPT := $(PLATFORM_LINKER)
LINK_FLAGS := -z max-page-size=4096 --build-id=sha1
PAYLOAD := $(OUT)/lume-pi5.img
NATIVE_CC ?= "$(TOOLBIN)clang$(HOSTEXE)"
NATIVE_CPPFLAGS ?= -nostdinc -isystem "$(SDK)/sdk/bare"
.PHONY: native-build native-directories native-responses
native-directories: $(OUT)/diagnostic.elf $(OUT)/configuration
	mkdir -p $(foreach directory,$(sort $(dir $(OBJECTS))),"$(directory)")
native-responses: native-directories
	$(foreach source,$(KERNEL_C),$(file >$(OUT)/obj/$(source).o.rsp,$(NATIVE_CPPFLAGS) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF "$(OUT)/obj/$(source).o.d" -MT "$(OUT)/obj/$(source).o" -c "$(source)" -o "$(OUT)/obj/$(source).o"))
	$(foreach source,$(KERNEL_S),$(file >$(OUT)/obj/$(source).o.rsp,$(NATIVE_CPPFLAGS) $(CPPFLAGS) $(filter-out -mgeneral-regs-only,$(CFLAGS)) $(if $(filter arch/aarch64/boot/elf_blob.S,$(source)),-DLUME_USER_ELF_FILE='"$(OUT)/diagnostic.elf"') $(if $(filter arch/aarch64/boot/shell_blob.S,$(source)),-DLUME_SHELL_ELF_FILE='"$(SHELL_ELF)"') -c "$(source)" -o "$(OUT)/obj/$(source).o"))
	$(file >$(OUT)/native-jobs)
	$(foreach object,$(OBJECTS),$(file >>$(OUT)/native-jobs,$(object).rsp))
native-build: native-responses
	$(NATIVE_CC) --batch "$(OUT)/native-jobs"
	$(LD) $(LINK_FLAGS) --gc-sections -T "$(SCRIPT)" $(OBJECTS) $(DRIVER_OBJECTS) -o "$(OUT)/lume.elf"
	$(OBJCOPY) -O binary "$(OUT)/lume.elf" "$(PAYLOAD)"
$(OUT)/diagnostic.elf: tests/arch/aarch64/user/start.S tests/arch/aarch64/user/main.c tests/arch/aarch64/user/user.ld
	mkdir -p "$(@D)"
	$(CC) $(CFLAGS) -nostdlib -static -Wl,--build-id=none,-z,max-page-size=4096,-T,tests/arch/aarch64/user/user.ld tests/arch/aarch64/user/start.S tests/arch/aarch64/user/main.c -o "$@"
.PHONY: watchdog-test-image
watchdog-test-image: $(OUT)/watchdog-test.img
$(OUT)/watchdog-test.elf: tests/arch/aarch64/watchdog_image.S tests/arch/aarch64/watchdog_image.ld
	mkdir -p "$(@D)"
	$(CC) $(CFLAGS) -nostdlib -static -Wl,--build-id=none,-z,max-page-size=4096,-T,tests/arch/aarch64/watchdog_image.ld "$<" -o "$@"
$(OUT)/watchdog-test.img: $(OUT)/watchdog-test.elf
	$(OBJCOPY) -O binary "$<" "$@"
else
ifeq ($(ARCH),r32)
include arch/r32/config.mk
CPPFLAGS := -DLUME_ARCH_R32=1 -Iarch/legacy32
LINK_FLAGS := -m r32elf
PAYLOAD := $(OUT)/lume.r32k
else ifeq ($(ARCH),x32)
include arch/x32/config.mk
LINK_FLAGS := -m x32elf
PAYLOAD := $(OUT)/lume.bin
else
$(error Unsupported ARCH $(ARCH))
endif
CPPFLAGS += -Iinclude/uapi -Iinclude/abi/$(ARCH) -Isrc/core -Isrc/terminal -Iarch/$(ARCH)/kernel -Iarch/$(ARCH)/kernel/freestanding -Isrc/compat/legacy32/$(ARCH) -Isrc/compat/legacy32/$(ARCH)/freestanding -Iarch/$(ARCH) -Ivendor/legacy32/$(ARCH)/fatfs -Ivendor/legacy32/$(ARCH)/fat_ro -DLUME_KERNEL_PERF=1
CFLAGS := -std=c11 -O2 $(COMMON_FLAGS)
KERNEL_C := $(KERNEL_C_$(ARCH))
KERNEL_S := arch/$(ARCH)/entry.s arch/$(ARCH)/memory.s
ifeq ($(ARCH),x32)
KERNEL_S += arch/x32/kernel/kernel_work.s
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
	cp -Rp include/uapi/. "$(OUT)/include/"
	cp -Rp $(UAPI_ABI)/. "$(OUT)/include/"
$(OUT)/obj/%.c.o: %.c $(OUT)/configuration Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF "$@.d" -MT "$@" -c "$<" -o "$@"
$(OUT)/obj/%.s.o: %.s $(OUT)/configuration Makefile
	mkdir -p "$(@D)"
	$(CC) -c "$<" -o "$@"
$(OUT)/obj/%.S.o: %.S $(OUT)/configuration Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(filter-out -mgeneral-regs-only,$(CFLAGS)) -c "$<" -o "$@"
$(OUT)/lume.elf: $(PROFILE_FILE) arch/$(ARCH)/config.mk mk/kernel.mk $(OBJECTS) $(DRIVER_OBJECTS) $(SCRIPT) Makefile $(OUT)/configuration
	$(LD) $(LINK_FLAGS) --gc-sections -T "$(SCRIPT)" $(OBJECTS) $(DRIVER_OBJECTS) -o "$@"
$(OUT)/lume.bin: $(OUT)/lume.elf
	$(OBJCOPY) -O binary "$<" "$@"
$(OUT)/lume-pi5.img: $(OUT)/lume.elf
	$(OBJCOPY) -O binary "$<" "$@"
$(OUT)/image-tool$(HOSTEXE): tools/image.cpp
	mkdir -p "$(@D)"
	$(HOSTCXX) $(HOSTCXXFLAGS) "$<" -o "$@"
$(OUT)/lume.r32k: $(OUT)/lume.elf $(OUT)/image-tool$(HOSTEXE)
	"$(OUT)/image-tool$(HOSTEXE)" r32-kernel "$<" "$@"
UAPI_ABI := $(if $(filter aarch64,$(ARCH)),include/abi/native,include/abi/$(ARCH))
TEST_CPPFLAGS := -Iplatform/raspberrypi5/include -Iarch/aarch64/include/kernel -Isrc/core -Isrc/terminal -Iinclude/kernel -Iinclude/ddk -Iinclude/kernel -Iinclude/uapi -Iinclude/abi/native -Ivendor/mbedtls/include -DMBEDTLS_CONFIG_FILE='"lume/crypto_config.h"'
TEST_NAMES := ddk devfs fdt-device fdt-initrd usb uart-input sdio wifi-net netif wpa volume watchdog stream icmp vfs heap user-stack mmu elf-load font
.PHONY: check-boot-storage
$(OUT)/tests/boot-storage$(HOSTEXE): tests/unit/boot_storage_test.c src/storage/storage.c src/storage/boot_storage.c src/storage/volume.c vendor/fatfs/ff.c vendor/fatfs/ffunicode.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) -Ivendor/fatfs $^ -o "$@"
check-boot-storage: $(OUT)/tests/boot-storage$(HOSTEXE)
	env "$<" "$(MBR_IMAGE)" "$(DATA_IMAGE)" "$(BOOT_IMAGE)" "$(ROOT_IMAGE)"
check-image: $(OUT)/tests/root-image$(HOSTEXE)
	env "$(OUT)/tests/root-image$(HOSTEXE)" "$(ROOT_IMAGE)"
$(OUT)/tests/root-image$(HOSTEXE): tests/unit/root_image_test.c src/fs/vfs/vfs.c src/fs/fat/fat.c src/storage/block.c
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
	env "$(OUT)/tests/wpa$(HOSTEXE)" tests/fixtures/wpa-handshake.bin
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
$(OUT)/tests/gem$(HOSTEXE): examples/ddk/gem_test.c examples/ddk/gem.c examples/ddk/gem.h examples/ddk/gem_netif.c include/ddk/lume/netif.h src/device/resource.c src/device/device.c src/net/netif.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) examples/ddk/gem_test.c src/device/resource.c src/device/device.c src/net/netif.c -o "$@"

check-scheduler: $(foreach c,1 2 4,$(OUT)/tests/scheduler-$(c)$(HOSTEXE))
	$(foreach c,1 2 4,env "$(OUT)/tests/scheduler-$(c)$(HOSTEXE)" &&) true
$(OUT)/tests/scheduler-%$(HOSTEXE): tests/unit/scheduler_test.c src/core/scheduler.h src/core/scheduler_config.h $(OUT)/configuration
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(SCHED_CPPFLAGS) -Isrc/core -Isrc/terminal -DSCHED_CPUS=$* "$<" -o "$@"
$(OUT)/tests/fdt-initrd$(HOSTEXE): tests/platform/raspberrypi5/fdt_initrd_test.c platform/raspberrypi5/fdt.c
$(OUT)/tests/fdt-device$(HOSTEXE): tests/platform/raspberrypi5/fdt_device_test.c platform/raspberrypi5/fdt.c
$(OUT)/tests/ddk$(HOSTEXE): tests/unit/ddk_test.c src/device/resource.c src/device/device.c src/device/irq.c src/dma/dma.c src/storage/block.c src/core/work.c
$(OUT)/tests/usb$(HOSTEXE): tests/unit/usb_test.c drivers/usb/xhci/usb_descriptors.c drivers/input/usb_hid/hid_keyboard.c
$(OUT)/tests/uart-input$(HOSTEXE): tests/unit/uart_input_test.c
$(OUT)/tests/volume$(HOSTEXE): tests/unit/volume_test.c src/storage/volume.c
$(OUT)/tests/watchdog$(HOSTEXE): tests/unit/watchdog_test.c src/watchdog/watchdog.c
$(OUT)/tests/icmp$(HOSTEXE): tests/unit/icmp_test.c src/net/icmp.c
$(OUT)/tests/icmp$(HOSTEXE): TEST_CPPFLAGS += -Ivendor/lwip/src/include
$(OUT)/tests/stream$(HOSTEXE): tests/unit/stream_test.c src/net/stream.c
$(OUT)/tests/vfs$(HOSTEXE): tests/unit/vfs_test.c src/fs/vfs/vfs.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c
$(OUT)/tests/devfs$(HOSTEXE): tests/unit/devfs_test.c examples/ddk/ram_block.c src/device/resource.c src/device/device.c src/fs/vfs/vfs.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c
$(OUT)/tests/heap$(HOSTEXE): tests/unit/heap_test.c src/mm/heap.c
$(OUT)/tests/stream$(HOSTEXE): TEST_CPPFLAGS += -Ivendor/lwip/src/include
$(OUT)/tests/sdio$(HOSTEXE): tests/unit/sdio_test.c drivers/storage/sdio/sdio.c
$(OUT)/tests/wifi-net$(HOSTEXE): tests/unit/wifi_net_test.c src/net/wifi/wifi_net.c
$(OUT)/tests/netif$(HOSTEXE): tests/unit/netif_test.c src/net/netif.c src/net/wireless.c src/net/net_control.c
$(OUT)/tests/wpa$(HOSTEXE): tests/unit/wpa_test.c src/net/wifi/wpa.c vendor/mbedtls/library/aes.c vendor/mbedtls/library/sha1.c
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

$(OUT)/tests/user-stack$(HOSTEXE): tests/unit/user_stack_test.c src/mm/user_stack.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"
$(OUT)/tests/mmu$(HOSTEXE): tests/arch/aarch64/mmu_test.c arch/aarch64/mm/mmu_tables.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"
$(OUT)/tests/elf-load$(HOSTEXE): tests/unit/elf_load_test.c src/exec/elf.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"

$(OUT)/tests/font$(HOSTEXE): tests/unit/font_test.c src/terminal/font5x7.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -Isrc/core -Isrc/terminal $^ -o "$@"

ifeq ($(ARCH),aarch64)
.PHONY: display-test-image
display-test-image: $(OUT)/tests/display.elf
$(OUT)/tests/display.elf: tests/arch/aarch64/display_test.c drivers/display/rp1_dsi/display.c src/terminal/terminal.c src/terminal/font5x7.c src/lib/memory.c platform/raspberrypi5/kernel.ld src/terminal/terminal.h src/terminal/font5x7.h include/kernel/lume/display.h $(OUT)/configuration Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -nostdlib -static -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096,-T,platform/raspberrypi5/kernel.ld tests/arch/aarch64/display_test.c src/terminal/terminal.c src/terminal/font5x7.c src/lib/memory.c src/dma/dma.c arch/aarch64/dma.c platform/raspberrypi5/dma.c -o "$@"
endif

.PHONY: check-hardware
check-hardware: $(OUT)/tests/hardware$(HOSTEXE)
	env "$(OUT)/tests/hardware$(HOSTEXE)"
$(OUT)/tests/hardware$(HOSTEXE): tests/unit/hardware_test.c src/device/hardware.c src/device/hardware_control.c src/device/resource.c src/device/device.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"

.PHONY: check-fdt-inventory
check-fdt-inventory: $(OUT)/tests/fdt-inventory$(HOSTEXE)
	env "$<"
$(OUT)/tests/fdt-inventory$(HOSTEXE): tests/unit/fdt_inventory_test.c src/device/fdt_inventory.c src/device/hardware.c src/device/resource.c vendor/libfdt/fdt.c vendor/libfdt/fdt_ro.c vendor/libfdt/fdt_check.c vendor/libfdt/fdt_sw.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) -Iinclude/kernel -Ivendor/libfdt $^ -o "$@"

.PHONY: check-module-elf
check-module-elf: $(OUT)/tests/module-elf$(HOSTEXE) $(OUT)/tests/module-fixture.o
	env "$(OUT)/tests/module-elf$(HOSTEXE)" "$(OUT)/tests/module-fixture.o"
$(OUT)/tests/module-elf$(HOSTEXE): tests/unit/module_elf_test.c src/module/module_elf.c arch/aarch64/module_relocate.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"
$(OUT)/tests/module-fixture.o: tests/integration/module_fixture.c
	mkdir -p "$(@D)"
	$(CC) $(CFLAGS) -c "$<" -o "$@"

ifeq ($(ARCH),aarch64)
headers: module-sdk
.PHONY: module-sdk
module-sdk: $(OUT)/ddk/include/lume/module_build.h
$(OUT)/module-sdk$(HOSTEXE): tools/module_sdk.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) "$<" -o "$@"
$(OUT)/ddk/include/lume/module_build.h: $(OUT)/lume.elf $(OUT)/module-sdk$(HOSTEXE)
	mkdir -p "$(@D)"
	cp -Rp include/ddk/. "$(OUT)/ddk/include/"
	cp -Rp include/uapi/. "$(OUT)/ddk/include/"
	cp -Rp include/abi/native/. "$(OUT)/ddk/include/"
	cp -Rp include/freestanding/. "$(OUT)/ddk/include/"
	cp arch/aarch64/include/kernel/lume/mmio.h "$(OUT)/ddk/include/lume/mmio.h"
	cp mk/driver.mk "$(OUT)/ddk/driver.mk"
	env "$(OUT)/module-sdk$(HOSTEXE)" "$(OUT)/lume.elf" "$@"
endif

.PHONY: check-module-services
check-module-services: $(OUT)/tests/module-services$(HOSTEXE)
	env "$<"
$(OUT)/tests/module-services$(HOSTEXE): tests/unit/module_services_test.c src/module/module_services.c src/device/device.c src/device/resource.c src/device/irq.c src/core/work.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"

.PHONY: check-fdt-pcie
check-fdt-pcie: $(OUT)/tests/fdt-pcie$(HOSTEXE)
	env "$<"
$(OUT)/tests/fdt-pcie$(HOSTEXE): tests/unit/fdt_pcie_test.c src/device/fdt_inventory.c src/device/hardware.c src/device/resource.c vendor/libfdt/fdt.c vendor/libfdt/fdt_ro.c vendor/libfdt/fdt_check.c vendor/libfdt/fdt_sw.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) -Ivendor/libfdt $^ -o "$@"

ifeq ($(ARCH),aarch64)
include mk/platform-modules.mk
endif

.PHONY: check-platform-drivers check-hardware-polling
check-platform-drivers: $(OUT)/tests/platform-driver$(HOSTEXE)
	env "$<"
$(OUT)/tests/platform-driver$(HOSTEXE): tests/unit/platform_driver_test.c src/device/platform_driver.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"
check-hardware-polling: $(OUT)/tests/hardware-polling$(HOSTEXE)
	env "$<"
$(OUT)/tests/hardware-polling$(HOSTEXE): tests/unit/hardware_polling_test.c src/device/hardware.c src/device/resource.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"

.PHONY: check-module-boot
check-module-boot: $(OUT)/tests/module-boot$(HOSTEXE)
	env "$<"
$(OUT)/tests/module-boot$(HOSTEXE): tests/unit/module_boot_test.c src/module/module_boot.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) "$<" -o "$@"

.PHONY: check-boot-fdt
check-boot-fdt: $(OUT)/tests/boot-fdt$(HOSTEXE)
	env "$<"
$(OUT)/tests/boot-fdt$(HOSTEXE): tests/platform/raspberrypi5/boot_fdt_test.c platform/raspberrypi5/boot_fdt.c src/device/fdt_inventory.c src/device/hardware.c src/device/resource.c vendor/libfdt/fdt.c vendor/libfdt/fdt_ro.c vendor/libfdt/fdt_check.c vendor/libfdt/fdt_sw.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) -Ivendor/libfdt $^ -o "$@"
ifeq ($(ARCH),aarch64)
check: check-hardware check-fdt-inventory check-module-elf check-module-services check-fdt-pcie check-platform-drivers check-hardware-polling check-module-boot check-boot-fdt
endif

.PHONY: check-boot-fdt-file
check-boot-fdt-file: $(OUT)/tests/boot-fdt-file$(HOSTEXE)
	env "$<"
$(OUT)/tests/boot-fdt-file$(HOSTEXE): tests/platform/raspberrypi5/boot_fdt_file_test.c platform/raspberrypi5/boot_fdt_file.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"
ifeq ($(ARCH),aarch64)
check: check-boot-fdt-file
endif
