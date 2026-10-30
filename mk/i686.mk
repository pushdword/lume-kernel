ifneq ($(BOARD),pc)
$(error i686 requires BOARD=pc)
endif
include arch/i686/config.mk
CPPFLAGS := -DLUME_ARCH_I686=1 -DLUME_BOOT_PROFILE=$(PROFILE_ID) -Iarch/i686/include/kernel -Iplatform/q35/include -Ivendor/libfdt -Isrc/core -Isrc/terminal -Iinclude/freestanding -Iinclude/ddk -Iinclude/kernel -Iinclude/uapi -Iinclude/abi/native -Ivendor/fatfs -Ivendor/lwip/src/include -Ivendor/mbedtls/include -DMBEDTLS_CONFIG_FILE='"lume/crypto_config.h"'
KERNEL_C := $(filter-out arch/aarch64/% platform/raspberrypi5/% src/watchdog/boot_watchdog.c src/core/worker_check.c src/terminal/terminal.c src/terminal/font5x7.c,$(KERNEL_C_aarch64))
KERNEL_C += arch/i686/boot/main.c arch/i686/context/context.c arch/i686/mm/mmu.c arch/i686/dma.c arch/i686/module_relocate.c arch/i686/builtins.c arch/i686/smp.c arch/amd64/mm/module_memory.c platform/pc/display.c drivers/storage/ata_pio/ata.c drivers/input/ps2/keyboard.c platform/q35/boot.c platform/q35/console.c platform/q35/clock.c platform/q35/hardware.c platform/q35/pci.c platform/q35/acpi.c platform/q35/firmware.c platform/q35/pci_bus.c
KERNEL_S := arch/i686/boot/start.S arch/i686/boot/payload.S arch/i686/context/interrupt.S arch/i686/context/worker.S
KERNEL_C += drivers/serial/uart16550/input.c
KERNEL_C += drivers/display/s3_trio64/display.c src/terminal/terminal.c src/terminal/font8x16.c src/terminal/font5x7.c
KERNEL_C += drivers/random/seed_file/entropy.c vendor/mbedtls/library/ctr_drbg.c
ifneq ($(strip $(SHELL_ELF)),)
$(OUT)/obj/arch/i686/boot/payload.S.o: CPPFLAGS += -DLUME_SHELL_ELF_FILE='"$(SHELL_ELF)"'
$(OUT)/obj/arch/i686/boot/payload.S.o: $(SHELL_ELF)
endif
SCRIPT := platform/pc/kernel.ld
LINK_FLAGS := -m elf_i386 -z max-page-size=4096 --build-id=sha1
PAYLOAD := $(OUT)/lume-i686.bin
$(OUT)/lume-i686.bin: $(OUT)/lume.elf
	$(OBJCOPY) -O binary "$<" "$@"
