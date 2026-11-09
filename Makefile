SHELL := sh
.DEFAULT_GOAL := all

ARCH ?= x32
CPU ?= $(if $(filter aarch64,$(ARCH)),cortex-a76,)
BOARD ?= $(if $(filter i686,$(ARCH)),pc,$(if $(filter amd64,$(ARCH)),q35,$(if $(filter aarch64,$(ARCH)),raspberrypi5,tang25k)))
SDK ?=
PROFILE ?= $(if $(filter amd64,$(ARCH)),normal,diagnostic)
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
ifeq ($(ARCH),i686)
include mk/i686.mk
else ifeq ($(ARCH),amd64)
ifneq ($(BOARD),q35)
$(error AMD64 requires BOARD=q35)
endif
ifeq ($(PROFILE),diagnostic)
$(error AMD64 supports normal and recovery boot profiles)
endif
include platform/q35/config.mk
include arch/amd64/config.mk
CPPFLAGS := -DLUME_ARCH_AMD64=1 -DLUME_BOOT_PROFILE=$(PROFILE_ID) -Iarch/amd64/include/kernel -Ivendor/libfdt $(PLATFORM_CPPFLAGS) -Isrc/core -Isrc/terminal -Iinclude/freestanding -Iinclude/ddk -Iinclude/kernel -Iinclude/uapi -Iinclude/abi/native -Ivendor/fatfs -Ivendor/lwip/src/include -Ivendor/mbedtls/include -DMBEDTLS_CONFIG_FILE='"lume/crypto_config.h"'
KERNEL_C := $(filter-out arch/aarch64/% platform/raspberrypi5/% src/watchdog/boot_watchdog.c src/core/worker_check.c drivers/input/usb_hid/hid_keyboard.c src/terminal/terminal.c src/terminal/font5x7.c,$(KERNEL_C_aarch64))
KERNEL_C += arch/amd64/boot/main.c arch/amd64/context/context.c arch/amd64/mm/mmu.c arch/amd64/mm/module_memory.c arch/amd64/module_relocate.c arch/amd64/dma.c arch/amd64/smp.c platform/q35/boot.c platform/q35/console.c platform/q35/clock.c platform/q35/hardware.c platform/q35/pci.c platform/q35/acpi.c platform/q35/firmware.c platform/q35/pci_bus.c src/device/usb_inventory.c
ifeq ($(DEBUG),1)
CFLAGS += -g -fno-omit-frame-pointer
endif
KERNEL_C += arch/amd64/apic.c
KERNEL_S := arch/amd64/boot/start.S arch/amd64/boot/secondary.S arch/amd64/boot/payload.S arch/amd64/context/interrupt.S arch/amd64/context/worker.S
ifneq ($(strip $(SHELL_ELF)),)
$(OUT)/obj/arch/amd64/boot/payload.S.o: CPPFLAGS += -DLUME_SHELL_ELF_FILE='"$(SHELL_ELF)"'
empty :=
space := $(empty) $(empty)
$(OUT)/obj/arch/amd64/boot/payload.S.o: $(subst $(space),\$(space),$(SHELL_ELF))
endif
SCRIPT := $(PLATFORM_LINKER)
LINK_FLAGS := -m elf_x86_64 -z max-page-size=4096 --build-id=sha1
PAYLOAD := $(OUT)/lume-amd64.bin
else ifeq ($(ARCH),aarch64)
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
	$(file >$@.rsp,$(LINK_FLAGS) --gc-sections -T "$(SCRIPT)" $(OBJECTS) $(DRIVER_OBJECTS) -o "$@")
	$(LD) @"$@.rsp"
$(OUT)/lume-amd64.bin: $(OUT)/lume.elf
	$(OBJCOPY) -O binary "$<" "$@"
$(OUT)/lume.bin: $(OUT)/lume.elf
	$(OBJCOPY) -O binary "$<" "$@"
$(OUT)/lume-pi5.img: $(OUT)/lume.elf
	$(OBJCOPY) -O binary "$<" "$@"
$(OUT)/image-tool$(HOSTEXE): tools/image.cpp
	mkdir -p "$(@D)"
	$(HOSTCXX) $(HOSTCXXFLAGS) "$<" -o "$@"
$(OUT)/lume.r32k: $(OUT)/lume.elf $(OUT)/image-tool$(HOSTEXE)
	"$(OUT)/image-tool$(HOSTEXE)" r32-kernel "$<" "$@"
UAPI_ABI := $(if $(filter aarch64 amd64 i686,$(ARCH)),include/abi/native,include/abi/$(ARCH))
TEST_CPPFLAGS := -Iplatform/raspberrypi5/include -Iarch/aarch64/include/kernel -Isrc/core -Isrc/terminal -Iinclude/kernel -Iinclude/ddk -Iinclude/kernel -Iinclude/uapi -Iinclude/abi/native -Ivendor/mbedtls/include -DMBEDTLS_CONFIG_FILE='"lume/crypto_config.h"'
TEST_NAMES := ddk devfs fdt-device fdt-initrd usb uart-input sdio wifi-net netif wpa volume watchdog stream icmp vfs poll heap user-stack mmu elf-load font memory
TEST_NAMES += io-queue io-calls vfs-storage-handles ip4-route
TEST_NAMES += net-service
.PHONY: check-console-calls
.PHONY: check-file-io
.PHONY: check-file-locks
FILE_IO_HOST_FIXTURE := $(if $(wildcard include/kernel/lume/sandbox_policy.h),tests/fixtures/vfs_unrestricted_services.c,)
FILE_IO_HOST_FLAGS := $(if $(FILE_IO_HOST_FIXTURE),-DLUME_ARCH_AMD64=1,)
$(OUT)/tests/vfs$(HOSTEXE) $(OUT)/tests/vfs-storage-handles$(HOSTEXE) $(OUT)/tests/vfs-file-io$(HOSTEXE) $(OUT)/tests/vfs-file-lock$(HOSTEXE): HOSTCFLAGS += $(FILE_IO_HOST_FLAGS)
$(OUT)/tests/vfs$(HOSTEXE) $(OUT)/tests/vfs-storage-handles$(HOSTEXE) $(OUT)/tests/vfs-file-io$(HOSTEXE) $(OUT)/tests/vfs-file-lock$(HOSTEXE): $(FILE_IO_HOST_FIXTURE)
check: check-file-locks
check-file-locks: $(OUT)/tests/vfs-file-lock$(HOSTEXE)
	env "$<"
$(OUT)/tests/vfs-file-lock$(HOSTEXE): tests/unit/vfs_file_lock_test.c src/fs/vfs/vfs.c src/fs/vfs/file_locks.h src/syscall/file_io.c src/syscall/file_lock.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c src/fs/path.c include/abi/native/lume/file_lock.h include/abi/native/lume/file_io.h include/kernel/lume/user_buffer.h src/security/sandbox.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) $(filter-out src/security/sandbox.c,$(filter %.c,$^)) -o "$@"
check: check-file-io
check-file-io: $(OUT)/tests/file-io-call$(HOSTEXE) $(OUT)/tests/vfs-file-io$(HOSTEXE)
	env "$(OUT)/tests/file-io-call$(HOSTEXE)"
	env "$(OUT)/tests/vfs-file-io$(HOSTEXE)"
$(OUT)/tests/file-io-call$(HOSTEXE): tests/unit/file_io_call_test.c src/syscall/file_io.c include/abi/native/lume/file_io.h include/kernel/lume/user_buffer.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) $(filter %.c,$^) -o "$@"
$(OUT)/tests/vfs-file-io$(HOSTEXE): tests/unit/vfs_file_io_test.c src/fs/vfs/vfs.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) $^ -o "$@"
check: check-console-calls
check-console-calls: $(OUT)/tests/console-calls$(HOSTEXE)
	env "$<"
$(OUT)/tests/console-calls$(HOSTEXE): tests/unit/console_calls_test.c src/core/console_calls.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) -Isrc/core "$<" -o "$@"
.PHONY: check-memory
.PHONY: check-poll-wait
check-poll-wait: $(OUT)/tests/poll-wait$(HOSTEXE)
	env "$<"
$(OUT)/tests/poll-wait$(HOSTEXE): tests/unit/poll_wait_test.c src/core/poll_calls.h src/core/poll_state.h src/core/signal_calls.h src/core/service_bus.h src/core/service_calls.h src/core/supervision_bus.h include/abi/native/lume/service.h include/abi/native/lume/supervision.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" -o "$@"
check: check-poll-wait

.PHONY: check-signal-event-wait
check-signal-event-wait: $(OUT)/tests/signal-event-wait$(HOSTEXE)
	env "$<"
$(OUT)/tests/signal-event-wait$(HOSTEXE): tests/unit/signal_event_wait_test.c tests/unit/poll_wait_test.c src/core/signal_calls.h src/core/signal_wait.h src/core/signal_mask.h src/core/task_wake.h src/core/poll_calls.h include/abi/native/lume/signal.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) -Isrc/core "$<" -o "$@"
check: check-signal-event-wait
check-memory: $(OUT)/tests/memory$(HOSTEXE)
	env "$<"
$(OUT)/tests/memory$(HOSTEXE): tests/unit/memory_test.c src/lib/memory.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -D_DEFAULT_SOURCE -fno-builtin -UNDEBUG "$<" -o "$@"
.PHONY: check-boot-storage
.PHONY: check-io-call
$(OUT)/tests/io-call$(HOSTEXE): tests/unit/io_call_test.c src/syscall/io.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"
check-io-call: $(OUT)/tests/io-call$(HOSTEXE)
	env "$<"

$(OUT)/tests/boot-storage$(HOSTEXE): tests/unit/boot_storage_test.c src/storage/storage.c src/storage/boot_storage.c src/storage/volume.c vendor/fatfs/ff.c vendor/fatfs/ffunicode.c tests/unit/sd_blocks_mock.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) -Ivendor/fatfs $(filter %.c,$^) -o "$@"
check-boot-storage: $(OUT)/tests/boot-storage$(HOSTEXE)
	env "$<" "$(MBR_IMAGE)" "$(DATA_IMAGE)" "$(BOOT_IMAGE)" "$(ROOT_IMAGE)"
.PHONY: check-boot-trial
$(OUT)/tests/boot-trial$(HOSTEXE): tests/unit/boot_trial_test.c tests/unit/boot_storage_test.c src/storage/storage.c src/storage/volume.c vendor/fatfs/ff.c vendor/fatfs/ffunicode.c tests/unit/sd_blocks_mock.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) -Ivendor/fatfs $(filter-out tests/unit/boot_storage_test.c,$(filter %.c,$^)) -o "$@"
check-boot-trial: $(OUT)/tests/boot-trial$(HOSTEXE)
	env "$<" "$(MBR_IMAGE)" "$(DATA_IMAGE)" "$(BOOT_IMAGE)" "$(ROOT_IMAGE)"
.PHONY: check-storage-handles
$(OUT)/tests/storage-handles$(HOSTEXE): tests/unit/storage_handles_test.c src/storage/storage.c src/storage/volume.c vendor/fatfs/ff.c vendor/fatfs/ffunicode.c tests/unit/sd_blocks_mock.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) -Ivendor/fatfs $(filter %.c,$^) -o "$@"
check-storage-handles: $(OUT)/tests/storage-handles$(HOSTEXE)
	env "$<"
check: check-storage-handles
.PHONY: check-storage-replace
$(OUT)/tests/storage-replace$(HOSTEXE): HOSTCFLAGS += $(FILE_IO_HOST_FLAGS) -DLUME_VFS_HOST_MAX_ALLOCATION=131072
$(OUT)/tests/storage-replace$(HOSTEXE): tests/unit/storage_replace_test.c tests/unit/storage_handles_test.c src/storage/storage.c src/storage/volume.c vendor/fatfs/ff.c vendor/fatfs/ffunicode.c tests/unit/sd_blocks_mock.h src/fs/vfs/vfs.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c src/fs/path.c $(FILE_IO_HOST_FIXTURE)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) -Ivendor/fatfs $(filter-out tests/unit/storage_handles_test.c,$(filter %.c,$^)) -o "$@"
check-storage-replace: $(OUT)/tests/storage-replace$(HOSTEXE)
	env "$<"
check: check-storage-replace
.PHONY: check-sdhci
$(OUT)/tests/sdhci$(HOSTEXE): tests/unit/sdhci_test.c drivers/storage/sdhci/sd.c include/kernel/lume/sd.h src/storage/block.c src/storage/volume.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) tests/unit/sdhci_test.c src/storage/block.c src/storage/volume.c -o "$@"
check-sdhci: $(OUT)/tests/sdhci$(HOSTEXE)
	env "$<"
check: check-sdhci
.PHONY: check-sd-provider
$(OUT)/tests/sd-provider$(HOSTEXE): tests/unit/sd_provider_test.c src/device/platform_driver.c include/kernel/lume/platform_driver.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) tests/unit/sd_provider_test.c src/device/platform_driver.c -o "$@"
check-sd-provider: $(OUT)/tests/sd-provider$(HOSTEXE)
	env "$<"
check: check-sd-provider
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
	env "$(OUT)/tests/net-service$(HOSTEXE)"
	env "$(OUT)/tests/wpa$(HOSTEXE)" tests/fixtures/wpa-handshake.bin
	env "$(OUT)/tests/volume$(HOSTEXE)"
	env "$(OUT)/tests/watchdog$(HOSTEXE)"
	env "$(OUT)/tests/stream$(HOSTEXE)"
	env "$(OUT)/tests/icmp$(HOSTEXE)"
	env "$(OUT)/tests/vfs$(HOSTEXE)"
	env "$(OUT)/tests/poll$(HOSTEXE)"
	env "$(OUT)/tests/heap$(HOSTEXE)"
	env "$(OUT)/tests/user-stack$(HOSTEXE)"
	env "$(OUT)/tests/mmu$(HOSTEXE)"
	env "$(OUT)/tests/elf-load$(HOSTEXE)"
	env "$(OUT)/tests/font$(HOSTEXE)"
	env "$(OUT)/tests/memory$(HOSTEXE)"
	env "$(OUT)/tests/io-queue$(HOSTEXE)"
	env "$(OUT)/tests/io-calls$(HOSTEXE)"
	env "$(OUT)/tests/vfs-storage-handles$(HOSTEXE)"
	env "$(OUT)/tests/ip4-route$(HOSTEXE)"

.PHONY: check-scheduler
.PHONY: check-gem
.PHONY: check-framebuffer-present
.PHONY: check-graphics
.PHONY: check-page-span
check: check-page-span
check-page-span: $(OUT)/tests/page-span$(HOSTEXE)
	env "$<"
$(OUT)/tests/page-span$(HOSTEXE): tests/unit/page_span_test.c src/mm/pages.c include/kernel/lume/worker.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -DLUME_ARCH_AMD64=1 -Iarch/amd64/include/kernel -Iinclude/kernel "$<" -o "$@"
check: check-graphics
check-graphics: $(OUT)/tests/graphics$(HOSTEXE)
	env "$<"
$(OUT)/tests/graphics$(HOSTEXE): tests/unit/graphics_test.c src/device/graphics.c include/kernel/lume/user_buffer.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" -o "$@"
check: check-framebuffer-present
check-framebuffer-present: $(OUT)/tests/framebuffer-present$(HOSTEXE)
	env "$<"
$(OUT)/tests/framebuffer-present$(HOSTEXE): tests/unit/framebuffer_present_test.c drivers/display/framebuffer.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" -o "$@"
check-gem: $(OUT)/tests/gem$(HOSTEXE)
	env "$<"
$(OUT)/tests/gem$(HOSTEXE): HOSTCFLAGS += -UNDEBUG
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
$(OUT)/tests/stream$(HOSTEXE): tests/unit/stream_test.c src/net/stream.c tests/fixtures/counter/lume/counter.h include/kernel/lume/net.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) $(filter %.c,$^) -o "$@"
HOST_LWIP_C := $(filter vendor/lwip/%,$(KERNEL_C_aarch64))
$(OUT)/tests/net-service$(HOSTEXE): tests/unit/net_service_test.c src/net/net.c src/net/netif.c src/net/route.c src/core/work.c $(HOST_LWIP_C)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) -Ivendor/lwip/src/include $^ -o "$@"
$(OUT)/tests/ip4-route$(HOSTEXE): tests/unit/ip4_route_test.c src/net/route.c include/kernel/lwipopts.h include/kernel/lume/lwip_hooks.h $(HOST_LWIP_C)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) -Ivendor/lwip/src/include tests/unit/ip4_route_test.c src/net/route.c $(HOST_LWIP_C) -o "$@"
.PHONY: check-ip4-route
check-ip4-route: $(OUT)/tests/ip4-route$(HOSTEXE)
	env "$<"
$(OUT)/tests/vfs$(HOSTEXE): tests/unit/vfs_test.c src/fs/vfs/vfs.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c
$(OUT)/tests/vfs-storage-handles$(HOSTEXE): tests/unit/vfs_storage_handles_test.c src/fs/vfs/vfs.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c
$(OUT)/tests/io-queue$(HOSTEXE): tests/unit/io_queue_test.c src/core/io_queue.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" -o "$@"
$(OUT)/tests/io-queue$(HOSTEXE): HOSTCFLAGS += -UNDEBUG
$(OUT)/tests/io-queue$(HOSTEXE): TEST_CPPFLAGS += -Isrc/core
.PHONY: check-io-calls
.PHONY: check-io-classify
check: check-io-classify
check-io-classify: $(OUT)/tests/io-classify$(HOSTEXE)
	env "$<"
$(OUT)/tests/io-classify$(HOSTEXE): tests/unit/io_classify_test.c src/core/io_classify.h include/abi/native/lume/realtime_abi.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -DLUME_ARCH_AARCH64=1 $(TEST_CPPFLAGS) "$<" -o "$@"
check-io-calls: $(OUT)/tests/io-calls$(HOSTEXE)
	env "$<"
$(OUT)/tests/io-calls$(HOSTEXE): HOSTCFLAGS += -DLUME_ARCH_AARCH64=1
$(OUT)/tests/io-calls$(HOSTEXE): tests/unit/io_calls_test.c src/core/io_calls.h src/core/pipe_fast.h src/core/io_queue.h src/core/io_domains.h src/core/io_dispatch.h src/core/stream_calls.h src/core/netctl_calls.h src/core/dns_calls.h src/core/scheduler_config.h include/kernel/lume/task_limits.h include/kernel/lume/net.h include/kernel/lume/wireless.h include/kernel/lume/vm.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" -o "$@"
$(OUT)/tests/io-calls$(HOSTEXE): TEST_CPPFLAGS += -Isrc/core
.PHONY: check-amd64-query
check-amd64-query: $(OUT)/tests/amd64-query$(HOSTEXE)
	env "$<"
$(OUT)/tests/amd64-query$(HOSTEXE): tests/arch/amd64/query_test.c arch/amd64/context/query.h include/kernel/lume/amd64_query.h include/kernel/lume/query.h include/abi/native/lume/abi.h include/abi/native/lume/syscall_numbers.h include/uapi/lume/thread.h include/kernel/lume/vm.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -DLUME_ARCH_AMD64=1 -Iarch/amd64/include/kernel -Iplatform/q35/include -Iinclude/kernel -Iinclude/ddk -Iinclude/uapi -Iinclude/abi/native -Isrc/core "$<" -o "$@"
.PHONY: check-i686-query
check-i686-query: $(OUT)/tests/i686-query$(HOSTEXE) $(OUT)/tests/i686-context$(HOSTEXE) $(OUT)/tests/i686-fpu$(HOSTEXE)
	env "$(OUT)/tests/i686-query$(HOSTEXE)"
	env "$(OUT)/tests/i686-context$(HOSTEXE)"
	env "$(OUT)/tests/i686-fpu$(HOSTEXE)"
$(OUT)/tests/i686-fpu$(HOSTEXE): tests/arch/i686/fpu_test.c arch/i686/context/fpu.h arch/i686/include/kernel/lume/context.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -Iarch/i686/include/kernel -Iinclude/kernel "$<" -o "$@"
$(OUT)/tests/i686-query$(HOSTEXE): tests/arch/i686/query_test.c arch/i686/context/query.h include/kernel/lume/i686_query.h include/kernel/lume/query.h include/abi/native/lume/abi.h include/abi/native/lume/syscall_numbers.h include/uapi/lume/thread.h include/uapi/lume/tty.h arch/i686/include/kernel/lume/context.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -DLUME_ARCH_I686=1 -Iarch/i686/include/kernel -Iplatform/pc/include -Iinclude/kernel -Iinclude/ddk -Iinclude/uapi -Iinclude/abi/native -Isrc/core "$<" -o "$@"
$(OUT)/tests/i686-context$(HOSTEXE): tests/arch/i686/context_test.c arch/i686/include/kernel/lume/context.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -Iarch/i686/include/kernel "$<" -o "$@"
$(OUT)/tests/poll$(HOSTEXE): tests/unit/poll_test.c src/fs/vfs/vfs.c src/net/socket.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c
$(OUT)/tests/devfs$(HOSTEXE): tests/unit/devfs_test.c examples/ddk/ram_block.c src/device/resource.c src/device/device.c src/fs/vfs/vfs.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c
$(OUT)/tests/heap$(HOSTEXE): tests/unit/heap_test.c src/mm/heap.c
$(OUT)/tests/heap$(HOSTEXE): override HOSTCFLAGS += -UNDEBUG
$(OUT)/tests/stream$(HOSTEXE): TEST_CPPFLAGS := -Itests/fixtures/counter $(TEST_CPPFLAGS) -Ivendor/lwip/src/include
$(OUT)/tests/sdio$(HOSTEXE): tests/unit/sdio_test.c drivers/storage/sdio/sdio.c
.PHONY: check-sdio-host-wait
check: check-sdio-host-wait
check-sdio-host-wait: $(OUT)/tests/sdio-host-wait$(HOSTEXE)
	env "$<"
$(OUT)/tests/sdio-host-wait$(HOSTEXE): tests/unit/sdio_host_wait_test.c drivers/storage/bcm2712_sdio/host.c include/kernel/lume/worker.h include/kernel/lume/sdio.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" -o "$@"
.PHONY: check-wifi-scan
check: check-wifi-scan
check-wifi-scan: $(OUT)/tests/wifi-scan$(HOSTEXE)
	env "$<"
$(OUT)/tests/wifi-scan$(HOSTEXE): tests/unit/wifi_scan_test.c drivers/net/cyw43455/firmware.c src/net/wifi/wifi_net.c src/net/wifi/wifi.c include/kernel/lume/wifi_protocol.h include/kernel/lume/wireless.h include/kernel/lume/worker.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -flto -fwhole-program -ffunction-sections -fdata-sections $(TEST_CPPFLAGS) "$<" src/net/wifi/wifi_net.c src/net/wifi/wifi.c -Wl,--gc-sections -o "$@"
$(OUT)/tests/wifi-net$(HOSTEXE): tests/unit/wifi_net_test.c src/net/wifi/wifi_net.c
$(OUT)/tests/netif$(HOSTEXE): tests/unit/netif_test.c src/net/netif.c src/net/wireless.c src/net/net_control.c
$(OUT)/tests/wpa$(HOSTEXE): tests/unit/wpa_test.c src/net/wifi/wpa.c vendor/mbedtls/library/aes.c vendor/mbedtls/library/sha1.c
$(addprefix $(OUT)/tests/,$(addsuffix $(HOSTEXE),$(filter-out user-stack mmu elf-load font memory io-queue io-calls ip4-route net-service stream,$(TEST_NAMES)))):
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"
clean:
	rm -rf -- "$(OUT)"

.PHONY: check-fat-cursor
check: check-fat-cursor
check-fat-cursor: $(OUT)/tests/fat-cursor$(HOSTEXE)
	env "$<"
$(OUT)/tests/fat-cursor$(HOSTEXE): tests/unit/fat_cursor_test.c tests/unit/vfs_test.c src/fs/vfs/vfs.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c include/kernel/lume/fat.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) tests/unit/fat_cursor_test.c src/fs/vfs/vfs.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c -o "$@"
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
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -DLUME_ARCH_AARCH64=1 $(TEST_CPPFLAGS) $^ -o "$@"
$(OUT)/tests/elf-load$(HOSTEXE): tests/unit/elf_load_test.c src/exec/elf.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"

$(OUT)/tests/gpu$(HOSTEXE): tests/unit/gpu_test.c src/device/gpu.c

	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) $^ -o "$@"

$(OUT)/tests/font$(HOSTEXE): tests/unit/font_test.c src/terminal/font5x7.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -Isrc/core -Isrc/terminal $^ -o "$@"

ifeq ($(ARCH),aarch64)
.PHONY: display-test-image
display-test-image: $(OUT)/tests/display.elf
$(OUT)/tests/display.elf: tests/arch/aarch64/display_test.c drivers/display/rp1_dsi/display.c src/terminal/terminal.c src/terminal/font5x7.c src/terminal/font8x16.c src/lib/memory.c platform/raspberrypi5/kernel.ld src/terminal/terminal.h src/terminal/font5x7.h include/kernel/lume/display.h $(OUT)/configuration Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -nostdlib -static -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096,-T,platform/raspberrypi5/kernel.ld tests/arch/aarch64/display_test.c src/terminal/terminal.c src/terminal/font5x7.c src/terminal/font8x16.c src/lib/memory.c src/dma/dma.c arch/aarch64/dma.c platform/raspberrypi5/dma.c -o "$@"
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

ifneq ($(filter aarch64 amd64,$(ARCH)),)
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
	cp arch/$(ARCH)/include/kernel/lume/mmio.h "$(OUT)/ddk/include/lume/mmio.h"
	cp mk/driver.mk "$(OUT)/ddk/driver.mk"
	printf 'ARCH := $(ARCH)\n' > "$(OUT)/ddk/arch.mk"
	env "$(OUT)/module-sdk$(HOSTEXE)" "$(OUT)/lume.elf" "$@"
endif

.PHONY: check-module-services
check-module-services: $(OUT)/tests/module-services$(HOSTEXE)
	env "$<"
$(OUT)/tests/module-services$(HOSTEXE): tests/unit/module_services_test.c src/module/module_services.c src/device/device.c src/device/resource.c src/device/irq.c src/device/irq_domain.c src/core/work.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) $^ -o "$@"

.PHONY: check-irq-domain check-pi5-fdt-irq
check-irq-domain: $(OUT)/tests/irq-domain$(HOSTEXE)
	env "$<"
$(OUT)/tests/irq-domain$(HOSTEXE): tests/unit/irq_domain_test.c src/device/irq.c src/device/irq_domain.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) $(if $(filter Windows_NT,$(OS)),,-pthread) $^ -o "$@"
check-pi5-fdt-irq: $(OUT)/tests/pi5-fdt-irq$(HOSTEXE)
	env "$<"
$(OUT)/tests/pi5-fdt-irq$(HOSTEXE): tests/platform/raspberrypi5/fdt_irq_test.c platform/raspberrypi5/fdt_irq.c vendor/libfdt/fdt.c vendor/libfdt/fdt_ro.c vendor/libfdt/fdt_sw.c vendor/libfdt/fdt_check.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -D_POSIX_C_SOURCE=200809L $(TEST_CPPFLAGS) -Ivendor/libfdt $^ -o "$@"
check: check-irq-domain check-pi5-fdt-irq

.PHONY: check-fdt-pcie
check-fdt-pcie: $(OUT)/tests/fdt-pcie$(HOSTEXE)
	env "$<"
$(OUT)/tests/fdt-pcie$(HOSTEXE): tests/unit/fdt_pcie_test.c src/device/fdt_inventory.c src/device/hardware.c src/device/resource.c vendor/libfdt/fdt.c vendor/libfdt/fdt_ro.c vendor/libfdt/fdt_check.c vendor/libfdt/fdt_sw.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(TEST_CPPFLAGS) -Ivendor/libfdt $^ -o "$@"

ifeq ($(ARCH),aarch64)
include mk/platform-modules.mk
else ifeq ($(ARCH),amd64)
include mk/q35-modules.mk
endif

.PHONY: check-platform-drivers check-hardware-polling
check-platform-drivers: $(OUT)/tests/platform-driver$(HOSTEXE)
	env "$<"
$(OUT)/tests/platform-driver$(HOSTEXE): tests/unit/platform_driver_test.c src/device/platform_driver.c src/core/console_output.c tests/fixtures/uart_host/lume/cpu.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -Itests/fixtures/uart_host $(TEST_CPPFLAGS) $(filter %.c,$^) -o "$@"

.PHONY: check-virtio-block-groups
check-virtio-block-groups: $(OUT)/tests/virtio-block-groups$(HOSTEXE)
	env "$<"
$(OUT)/tests/virtio-block-groups$(HOSTEXE): tests/platform/q35/block_groups_test.c drivers/storage/virtio_blk/block.c include/kernel/lume/platform_driver.h include/kernel/lume/sd.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(Q35_TEST_CPPFLAGS) "$<" -o "$@"
check: check-virtio-block-groups

.PHONY: check-q35
check-q35: $(OUT)/tests/q35-pci$(HOSTEXE) $(OUT)/tests/q35-virtio$(HOSTEXE) $(OUT)/tests/q35-acpi$(HOSTEXE) $(OUT)/tests/q35-bus$(HOSTEXE) $(OUT)/tests/q35-firmware$(HOSTEXE)
	env "$(OUT)/tests/q35-pci$(HOSTEXE)"
	env "$(OUT)/tests/q35-virtio$(HOSTEXE)"
	env "$(OUT)/tests/q35-acpi$(HOSTEXE)"
	env "$(OUT)/tests/q35-bus$(HOSTEXE)"
	env "$(OUT)/tests/q35-firmware$(HOSTEXE)"
.PHONY: check-amd64-context
check-q35: check-amd64-context
check-amd64-context: $(OUT)/tests/amd64-context$(HOSTEXE)
	env "$<"
$(OUT)/tests/amd64-context$(HOSTEXE): tests/arch/amd64/context_test.c arch/amd64/context/context.c arch/amd64/context/query.h include/kernel/lume/amd64_query.h include/kernel/lume/query.h include/abi/native/lume/syscall_numbers.h arch/amd64/include/kernel/lume/context.h arch/amd64/include/kernel/lume/smp.h arch/amd64/include/kernel/lume/apic.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -DLUME_ARCH_AMD64=1 $(filter-out -Itests/platform/q35/include,$(Q35_TEST_CPPFLAGS)) "$<" -o "$@"
.PHONY: check-amd64-mmu-views
check-amd64-mmu-views: $(OUT)/tests/amd64-mmu-views$(HOSTEXE)
	env "$<"
$(OUT)/tests/amd64-mmu-views$(HOSTEXE): tests/arch/amd64/mmu_views_test.c arch/amd64/mm/mmu.c include/kernel/lume/early.h include/kernel/lume/elf.h include/kernel/lume/vm.h include/kernel/lume/heap.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -DLUME_ARCH_AMD64=1 -flto -fwhole-program -ffunction-sections -fdata-sections $(filter-out -Itests/platform/q35/include,$(Q35_TEST_CPPFLAGS)) "$<" -Wl,--gc-sections -o "$@"
Q35_TEST_CPPFLAGS := -Itests/platform/q35/include -Iplatform/q35/include -Iarch/amd64/include/kernel $(filter-out -Iplatform/raspberrypi5/include -Iarch/aarch64/include/kernel,$(TEST_CPPFLAGS))
.PHONY: check-virtio-net check-v3d
check: check-virtio-net check-v3d
check-virtio-net: $(OUT)/tests/virtio-net$(HOSTEXE)
	env "$<"
$(OUT)/tests/virtio-net$(HOSTEXE): tests/platform/q35/virtio_net_test.c drivers/net/virtio_net/net.c platform/q35/virtio.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(Q35_TEST_CPPFLAGS) "$<" -o "$@"
check-v3d: $(OUT)/tests/v3d$(HOSTEXE)
	env "$<"
$(OUT)/tests/v3d$(HOSTEXE): tests/unit/v3d_test.c drivers/gpu/v3d/v3d.c drivers/gpu/v3d/v3d.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) -Idrivers/gpu/v3d "$<" -o "$@"
$(OUT)/tests/q35-pci$(HOSTEXE): tests/platform/q35/pci_test.c platform/q35/pci.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(Q35_TEST_CPPFLAGS) "$<" -o "$@"
$(OUT)/tests/q35-virtio$(HOSTEXE): tests/platform/q35/virtio_test.c platform/q35/virtio.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(Q35_TEST_CPPFLAGS) "$<" -o "$@"
$(OUT)/tests/q35-acpi$(HOSTEXE): tests/platform/q35/acpi_test.c platform/q35/acpi.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(Q35_TEST_CPPFLAGS) $^ -o "$@"
$(OUT)/tests/q35-bus$(HOSTEXE): tests/platform/q35/pci_bus_test.c platform/q35/pci_bus.c platform/q35/pci.c src/device/hardware.c src/device/resource.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(Q35_TEST_CPPFLAGS) $^ -o "$@"
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

$(OUT)/tests/q35-firmware$(HOSTEXE): tests/platform/q35/firmware_test.c platform/q35/firmware.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(Q35_TEST_CPPFLAGS) $^ -o "$@"

.PHONY: check-work-queue
check-work-queue: $(OUT)/tests/work-queue$(HOSTEXE)
	env "$<"
$(OUT)/tests/work-queue$(HOSTEXE): tests/unit/work_queue_test.c src/core/work.c include/ddk/lume/work.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) tests/unit/work_queue_test.c src/core/work.c -o "$@"
check: check-work-queue

.PHONY: check-worker-slices
check-worker-slices: $(OUT)/tests/worker-slices$(HOSTEXE)
	env "$<"
$(OUT)/tests/worker-slices$(HOSTEXE): tests/unit/worker_slices_test.c src/core/worker.c src/core/worker_check.c src/core/work.c src/core/activity.c include/kernel/lume/worker.h include/kernel/lume/activity.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" src/core/work.c -o "$@"
check: check-worker-slices

.PHONY: check-io-domains
check-io-domains: $(OUT)/tests/io-classify$(HOSTEXE)
	env "$<"
$(OUT)/tests/io-classify$(HOSTEXE): tests/unit/io_classify_test.c src/core/io_classify.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) -Isrc/core "$<" -o "$@"
check: check-io-domains

.PHONY: check-usb-poll
check-usb-poll: $(OUT)/tests/usb-poll$(HOSTEXE)
	env "$<"
$(OUT)/tests/usb-poll$(HOSTEXE): tests/unit/usb_poll_test.c drivers/usb/xhci/usb.c drivers/usb/xhci/usb_descriptors.c drivers/input/usb_hid/hid_keyboard.c include/kernel/lume/worker.h include/ddk/lume/work.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" drivers/usb/xhci/usb_descriptors.c drivers/input/usb_hid/hid_keyboard.c -o "$@"
check: check-usb-poll

.PHONY: check-ticket-lock
check-ticket-lock: $(OUT)/tests/ticket-lock$(HOSTEXE)
	env "$<"
$(OUT)/tests/ticket-lock$(HOSTEXE): tests/unit/ticket_lock_test.c include/kernel/lume/ticket_lock.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -Iinclude/kernel "$<" $(if $(filter Windows_NT,$(OS)),,-pthread) -o "$@"
check: check-ticket-lock

.PHONY: check-smp-lock
check-smp-lock: $(OUT)/tests/smp-lock$(HOSTEXE)
	env "$<"
$(OUT)/tests/smp-lock$(HOSTEXE): tests/unit/smp_lock_test.c src/core/smp_lock.h include/kernel/lume/ticket_lock.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" $(if $(filter Windows_NT,$(OS)),,-pthread) -o "$@"
check: check-smp-lock

.PHONY: check-query-calls
check-query-calls: $(OUT)/tests/query-calls$(HOSTEXE)
	env "$<"
$(OUT)/tests/query-calls$(HOSTEXE): tests/unit/query_calls_test.c src/core/query_calls.h src/core/thread_state.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" -o "$@"
check: check-query-calls

.PHONY: check-console-output
check-console-output: $(OUT)/tests/console-output$(HOSTEXE)
	env "$<"
$(OUT)/tests/console-output$(HOSTEXE): tests/unit/console_output_test.c src/core/console_output.c drivers/serial/pl011/uart.c include/kernel/lume/console_output.h include/kernel/lume/ticket_lock.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" $(if $(filter Windows_NT,$(OS)),,-pthread) -o "$@"
check: check-console-output

.PHONY: check-activity
check-activity: $(OUT)/tests/activity$(HOSTEXE)
	env "$<"
$(OUT)/tests/activity$(HOSTEXE): tests/unit/activity_test.c src/core/activity.c include/uapi/lume/activity.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) -Iarch/$(ARCH)/include/kernel "$<" -o "$@"
check: check-activity

.PHONY: check-uart-irq
check-uart-irq: $(OUT)/tests/uart-irq$(HOSTEXE)
	env "$<"
$(OUT)/tests/uart-irq$(HOSTEXE): tests/unit/uart_irq_test.c drivers/serial/pl011/rp1_irq.c drivers/serial/pl011/uart_irq.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" -o "$@"
check: check-uart-irq

.PHONY: check-uart-rx
check-uart-rx: $(OUT)/tests/uart-rx$(HOSTEXE)
	env "$<"
$(OUT)/tests/uart-rx$(HOSTEXE): tests/unit/uart_rx_test.c platform/raspberrypi5/uart_rx.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" -o "$@"
check: check-uart-rx

.PHONY: check-uart-rx-tx
check-uart-rx-tx: $(OUT)/tests/uart-rx-tx$(HOSTEXE)
	env "$<"
$(OUT)/tests/uart-rx-tx$(HOSTEXE): tests/unit/uart_rx_tx_test.c platform/raspberrypi5/uart_rx.c drivers/serial/pl011/uart.c src/core/console_output.c tests/fixtures/uart_host/lume/cpu.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -Itests/fixtures/uart_host $(TEST_CPPFLAGS) tests/unit/uart_rx_tx_test.c src/core/console_output.c $(if $(filter Windows_NT,$(OS)),,-pthread) -o "$@"
check: check-uart-rx-tx

.PHONY: check-realtime
check-realtime: $(OUT)/tests/realtime$(HOSTEXE)
	env "$<"
$(OUT)/tests/realtime$(HOSTEXE): tests/unit/realtime_test.c src/core/realtime.c include/kernel/lume/realtime.h include/abi/native/lume/realtime_abi.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) $(filter %.c,$^) -o "$@"
check: check-realtime

.PHONY: check-net-compat-scan
check-net-compat-scan: $(OUT)/tests/net-compat-scan$(HOSTEXE)
	env "$<"
$(OUT)/tests/net-compat-scan$(HOSTEXE): tests/unit/net_compat_scan_test.c src/net/net_compat.c include/kernel/lume/net.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) -flto -fwhole-program -ffunction-sections -fdata-sections $(filter %.c,$^) -Wl,--gc-sections -o "$@"
check: check-net-compat-scan

check: check-signal-mask
.PHONY: check-signal-mask
check-signal-mask: $(OUT)/tests/signal-mask$(HOSTEXE)
	env "$<"
$(OUT)/tests/signal-mask$(HOSTEXE): tests/unit/signal_mask_test.c src/core/signal_mask.h include/abi/native/lume/signal.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) -Isrc/core "$<" -o "$@"

.PHONY: check-vm-memory
check-vm-memory: $(OUT)/tests/vm-memory$(HOSTEXE)
	env "$<"
$(OUT)/tests/vm-memory$(HOSTEXE): tests/unit/vm_test.c src/mm/vm.c src/mm/vm_file_cache.h include/kernel/lume/vm.h include/abi/native/lume/memory.h $(wildcard tests/fixtures/vm_host/lume/*.h)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -DLUME_ARCH_AARCH64=1 -Itests/fixtures/vm_host -Iinclude/kernel -Iinclude/abi/native "$<" -o "$@"
check: check-vm-memory

.PHONY: check-user-io-lifetime
USER_IO_HOST_CPPFLAGS := -Iarch/amd64/include/kernel -Iplatform/q35/include $(filter-out -Iplatform/raspberrypi5/include -Iarch/aarch64/include/kernel,$(TEST_CPPFLAGS))
check-user-io-lifetime: $(OUT)/tests/user-io-lifetime$(HOSTEXE)
	env "$<"
$(OUT)/tests/user-io-lifetime$(HOSTEXE): HOSTCFLAGS += $(FILE_IO_HOST_FLAGS) -DLUME_VFS_HOST_EXTERNAL_ALLOCATOR=1
$(OUT)/tests/user-io-lifetime$(HOSTEXE): tests/unit/user_io_lifetime_test.c src/mm/user_buffer.c src/syscall/user_io.c src/syscall/file_io.c src/syscall/file_lock.c src/fs/vfs/vfs.c src/fs/path.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c src/core/work.c $(FILE_IO_HOST_FIXTURE) src/mm/vm.c src/core/worker.c src/core/activity.c vendor/fatfs/ffunicode.c include/kernel/lume/user_buffer.h include/kernel/lume/user_io.h include/kernel/lume/vm.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(USER_IO_HOST_CPPFLAGS) $(filter-out src/mm/vm.c src/core/worker.c src/core/activity.c,$(filter %.c,$^)) -o "$@"
check: check-user-io-lifetime

.PHONY: check-graphics-lifetime
.PHONY: check-small-io
check: check-small-io
check-small-io: $(OUT)/tests/small-io$(HOSTEXE)
	env "$<"
$(OUT)/tests/small-io$(HOSTEXE): tests/unit/small_io_test.c tests/unit/user_io_lifetime_test.c src/mm/user_buffer.c src/syscall/user_io.c src/syscall/file_io.c src/syscall/file_lock.c src/fs/vfs/vfs.c src/fs/path.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c src/core/work.c $(FILE_IO_HOST_FIXTURE) src/mm/vm.c src/core/worker.c src/core/activity.c vendor/fatfs/ffunicode.c include/kernel/lume/user_buffer.h include/kernel/lume/user_io.h include/kernel/lume/vm.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(FILE_IO_HOST_FLAGS) -DLUME_VFS_HOST_EXTERNAL_ALLOCATOR=1 -UNDEBUG $(USER_IO_HOST_CPPFLAGS) $(filter-out tests/unit/user_io_lifetime_test.c src/mm/vm.c src/core/worker.c src/core/activity.c,$(filter %.c,$^)) -o "$@"

.PHONY: check-acpi-high-memory
check: check-acpi-high-memory
check-acpi-high-memory: $(OUT)/tests/acpi-high-memory$(HOSTEXE)
	env "$<"
$(OUT)/tests/acpi-high-memory$(HOSTEXE): tests/platform/q35/acpi_high_memory_test.c platform/q35/firmware.c platform/q35/acpi.c platform/q35/include/lume/firmware.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -DLUME_ARCH_AMD64=1 $(Q35_TEST_CPPFLAGS) $(filter %.c,$^) -o "$@"
check: check-graphics-lifetime
check-graphics-lifetime: $(OUT)/tests/graphics-lifetime$(HOSTEXE)
	env "$<"
$(OUT)/tests/graphics-lifetime$(HOSTEXE): tests/unit/graphics_lifetime_test.c tests/unit/user_io_lifetime_test.c src/device/graphics.c src/mm/user_buffer.c src/syscall/user_io.c src/syscall/file_io.c src/syscall/file_lock.c src/fs/vfs/vfs.c src/fs/path.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c src/core/work.c $(FILE_IO_HOST_FIXTURE) src/mm/vm.c src/core/worker.c src/core/activity.c vendor/fatfs/ffunicode.c include/kernel/lume/user_buffer.h include/kernel/lume/graphics_device.h include/kernel/lume/vm.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(FILE_IO_HOST_FLAGS) -DLUME_VFS_HOST_EXTERNAL_ALLOCATOR=1 -UNDEBUG $(USER_IO_HOST_CPPFLAGS) $(filter-out tests/unit/user_io_lifetime_test.c src/device/graphics.c src/mm/vm.c src/core/worker.c src/core/activity.c,$(filter %.c,$^)) -o "$@"

SOCKET_LIFETIME_HOST_C := src/mm/user_buffer.c src/syscall/user_io.c src/syscall/file_io.c src/syscall/file_lock.c src/fs/vfs/vfs.c src/fs/path.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c src/core/work.c vendor/fatfs/ffunicode.c src/net/socket.c src/net/datagram.c
.PHONY: check-socket-network-lifetime
check-socket-network-lifetime: $(OUT)/tests/socket-network-lifetime$(HOSTEXE)
	env "$<"
$(OUT)/tests/socket-network-lifetime$(HOSTEXE): HOSTCFLAGS += -DLUME_ARCH_AMD64=1 -DLUME_VFS_HOST_EXTERNAL_ALLOCATOR=1
$(OUT)/tests/socket-network-lifetime$(HOSTEXE): tests/unit/socket_network_lifetime_test.c tests/unit/user_io_lifetime_test.c $(SOCKET_LIFETIME_HOST_C) include/kernel/lume/socket_buffer.h include/kernel/lume/user_buffer.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(USER_IO_HOST_CPPFLAGS) -Ivendor/lwip/src/include $(filter-out tests/unit/user_io_lifetime_test.c,$(filter %.c,$^)) -o "$@"
check: check-socket-network-lifetime

.PHONY: check-socket-scm-lifetime
check-socket-scm-lifetime: $(OUT)/tests/socket-scm-lifetime$(HOSTEXE)
	env "$<"
$(OUT)/tests/socket-scm-lifetime$(HOSTEXE): HOSTCFLAGS += -DLUME_ARCH_AMD64=1 -DLUME_VFS_HOST_EXTERNAL_ALLOCATOR=1
$(OUT)/tests/socket-scm-lifetime$(HOSTEXE): tests/unit/socket_scm_lifetime_test.c tests/unit/socket_network_lifetime_test.c tests/unit/user_io_lifetime_test.c $(SOCKET_LIFETIME_HOST_C) src/syscall/socket_io.c include/kernel/lume/socket_io.h include/kernel/lume/socket_buffer.h include/kernel/lume/user_buffer.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(USER_IO_HOST_CPPFLAGS) -Ivendor/lwip/src/include $(filter-out src/mm/user_buffer.c tests/unit/user_io_lifetime_test.c tests/unit/socket_network_lifetime_test.c,$(filter %.c,$^)) -o "$@"
check: check-socket-scm-lifetime

.PHONY: check-vfs-append
check-vfs-append: $(OUT)/tests/vfs-append$(HOSTEXE)
	env "$<"
$(OUT)/tests/vfs-append$(HOSTEXE): tests/unit/vfs_external_append_test.c src/fs/vfs/vfs.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c src/fs/vfs/file_locks.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -DLUME_ARCH_AARCH64=1 $(TEST_CPPFLAGS) $(filter %.c,$^) -o "$@"
check: check-vfs-append

.PHONY: check-signal-wait
check: check-signal-wait
check-signal-wait: $(OUT)/tests/signal-wait$(HOSTEXE)
	env "$<"
$(OUT)/tests/signal-wait$(HOSTEXE): tests/unit/signal_wait_test.c src/core/signal_wait.h src/core/signal_mask.h include/abi/native/lume/signal.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) $(filter %.c,$^) -o "$@"

.PHONY: check-vfs-mapping
check-vfs-mapping: $(OUT)/tests/vfs-mapping$(HOSTEXE)
	env "$<"
$(OUT)/tests/vfs-mapping$(HOSTEXE): tests/unit/vfs_mapping_test.c tests/fixtures/vfs_memory_host.c tests/fixtures/vfs_unrestricted_services.c src/fs/vfs/vfs.c src/fs/path.c src/security/file_modes.c src/fs/fat/fat.c src/storage/block.c vendor/fatfs/ffunicode.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -DLUME_ARCH_AMD64=1 -DLUME_VFS_HOST_EXTERNAL_ALLOCATOR=1 $(TEST_CPPFLAGS) $^ -o "$@"
