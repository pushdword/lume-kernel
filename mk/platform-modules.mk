PLATFORM_MODULE_NAMES := gic timer watchdog dma uart sd usb display entropy wifi gem
PLATFORM_MODULES := $(addprefix $(OUT)/modules/,$(addsuffix .lkm,$(PLATFORM_MODULE_NAMES)))
PLATFORM_gic := drivers/irq/gicv2/gic.c
PLATFORM_timer := arch/aarch64/timer.c
PLATFORM_watchdog := drivers/watchdog/bcm2712/watchdog.c src/watchdog/watchdog.c
PLATFORM_dma := platform/raspberrypi5/dma.c
PLATFORM_uart := drivers/serial/pl011/uart.c drivers/serial/pl011/rp1_irq.c
PLATFORM_sd := drivers/storage/sdhci/sd.c
PLATFORM_usb := drivers/usb/xhci/usb.c drivers/usb/xhci/usb_descriptors.c drivers/input/usb_hid/hid_keyboard.c
PLATFORM_display := drivers/display/rp1_dsi/display.c src/terminal/terminal.c src/terminal/font5x7.c src/terminal/font8x16.c
PLATFORM_entropy := drivers/random/rng200/entropy.c
PLATFORM_wifi := drivers/storage/bcm2712_sdio/host.c drivers/net/cyw43455/firmware.c drivers/storage/sdio/sdio.c src/net/wifi/wifi.c src/net/wifi/wifi_net.c src/net/wifi/wpa.c vendor/mbedtls/library/aes.c vendor/mbedtls/library/sha1.c
PLATFORM_gem := examples/ddk/gem_board.c examples/ddk/gem.c examples/ddk/gem_netif.c
PLATFORM_MODULE_SOURCES := $(sort $(foreach name,$(PLATFORM_MODULE_NAMES),$(PLATFORM_$(name)) platform/raspberrypi5/modules/$(name).c))
HDMI_MODULE_SOURCE := drivers/display/bcm2712_hdmi/module.c drivers/display/bcm2712_hdmi/firmware.c drivers/display/bcm2712_hdmi/console.c src/terminal/terminal.c src/terminal/font5x7.c src/terminal/font8x16.c
PLATFORM_MODULE_SOURCES += $(HDMI_MODULE_SOURCE)
PLATFORM_MODULES += $(OUT)/modules/bcm2712-hdmi.lkm
L2_MODULE_SOURCE := drivers/irq/brcm_l2/module.c
PLATFORM_MODULE_SOURCES += $(L2_MODULE_SOURCE)
PLATFORM_MODULES += $(OUT)/modules/brcm-l2.lkm
PLATFORM_MODULE_OBJECTS := $(addprefix $(OUT)/lkm/obj/,$(addsuffix .o,$(PLATFORM_MODULE_SOURCES)))
.PHONY: platform-modules
all: platform-modules
platform-modules: $(PLATFORM_MODULES)
$(OUT)/lkm/obj/%.c.o: %.c $(OUT)/configuration $(OUT)/ddk/include/lume/module_build.h Makefile mk/platform-modules.mk
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) -Iexamples/ddk -I$(OUT)/ddk/include -DLUME_PLATFORM_LKM=1 $(filter-out -ffunction-sections -fdata-sections,$(CFLAGS)) -MMD -MP -MF "$@.d" -MT "$@" -c "$<" -o "$@"

define platform_module_rule
$(OUT)/modules/$(1).lkm: $(addprefix $(OUT)/lkm/obj/,$(addsuffix .o,$(PLATFORM_$(1)) platform/raspberrypi5/modules/$(1).c))
	mkdir -p "$$(@D)"
	$(LD) -r --build-id=none $$^ -o "$$@"
endef
$(foreach name,$(PLATFORM_MODULE_NAMES),$(eval $(call platform_module_rule,$(name))))
-include $(addsuffix .d,$(PLATFORM_MODULE_OBJECTS))
$(OUT)/modules/bcm2712-hdmi.lkm: $(addprefix $(OUT)/lkm/obj/,$(addsuffix .o,$(HDMI_MODULE_SOURCE)))
	mkdir -p "$(@D)"
	$(LD) -r --build-id=none $^ -o "$@"

$(OUT)/modules/brcm-l2.lkm: $(OUT)/lkm/obj/$(L2_MODULE_SOURCE).o
	mkdir -p "$(@D)"
	$(LD) -r --build-id=none $^ -o "$@"

.PHONY: check-brcm-l2-module
check-brcm-l2-module: $(OUT)/tests/brcm-l2-module$(HOSTEXE)
	env "$<"
$(OUT)/tests/brcm-l2-module$(HOSTEXE): tests/unit/brcm_l2_module_test.c $(L2_MODULE_SOURCE) $(OUT)/ddk/include/lume/module_build.h src/device/irq.c src/device/irq_domain.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) -idirafter $(OUT)/ddk/include "$<" src/device/irq.c src/device/irq_domain.c -o "$@"

.PHONY: check-bcm2712-hdmi-module
check-bcm2712-hdmi-module: $(OUT)/tests/bcm2712-hdmi-module$(HOSTEXE)
	env "$<"
$(OUT)/tests/bcm2712-hdmi-module$(HOSTEXE): tests/unit/bcm2712_hdmi_module_test.c $(HDMI_MODULE_SOURCE) $(OUT)/ddk/include/lume/module_build.h src/device/irq.c src/device/irq_domain.c src/device/resource.c src/core/work.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) -idirafter $(OUT)/ddk/include "$<" src/device/irq.c src/device/irq_domain.c src/device/resource.c src/core/work.c -o "$@"

.PHONY: check-pi5-firmware
check-pi5-firmware: $(OUT)/tests/pi5-firmware$(HOSTEXE)
	env "$<"
$(OUT)/tests/pi5-firmware$(HOSTEXE): tests/unit/pi5_firmware_test.c platform/raspberrypi5/firmware.c
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" -o "$@"

.PHONY: check-hdmi-framebuffer
check-hdmi-framebuffer: $(OUT)/tests/hdmi-framebuffer$(HOSTEXE)
	env "$<"
$(OUT)/tests/hdmi-framebuffer$(HOSTEXE): tests/unit/hdmi_framebuffer_test.c $(HDMI_MODULE_SOURCE)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" -o "$@"

.PHONY: check-hdmi-console
check-hdmi-console: $(OUT)/tests/hdmi-console$(HOSTEXE)
	env "$<"
$(OUT)/tests/hdmi-console$(HOSTEXE): tests/unit/hdmi_console_test.c drivers/display/bcm2712_hdmi/console.c drivers/display/bcm2712_hdmi/console.h src/terminal/terminal.c src/terminal/font5x7.c src/terminal/font8x16.c src/terminal/console_font.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) "$<" src/terminal/terminal.c src/terminal/font5x7.c src/terminal/font8x16.c -o "$@"

.PHONY: check-hdmi-platform
check-hdmi-platform: check-bcm2712-hdmi-module check-brcm-l2-module check-pi5-firmware check-hdmi-framebuffer check-hdmi-console check-irq-domain check-pi5-fdt-irq $(OUT)/tests/mmu$(HOSTEXE)
	env "$(OUT)/tests/mmu$(HOSTEXE)"
