PLATFORM_MODULE_NAMES := gic timer watchdog dma uart sd usb display entropy wifi gem
PLATFORM_MODULES := $(addprefix $(OUT)/modules/,$(addsuffix .lkm,$(PLATFORM_MODULE_NAMES)))
PLATFORM_gic := drivers/irq/gicv2/gic.c
PLATFORM_timer := arch/aarch64/timer.c
PLATFORM_watchdog := drivers/watchdog/bcm2712/watchdog.c src/watchdog/watchdog.c
PLATFORM_dma := platform/raspberrypi5/dma.c
PLATFORM_uart := drivers/serial/pl011/uart.c
PLATFORM_sd := drivers/storage/sdhci/sd.c
PLATFORM_usb := drivers/usb/xhci/usb.c drivers/usb/xhci/usb_descriptors.c drivers/input/usb_hid/hid_keyboard.c
PLATFORM_display := drivers/display/rp1_dsi/display.c src/terminal/terminal.c src/terminal/font5x7.c
PLATFORM_entropy := drivers/random/rng200/entropy.c
PLATFORM_wifi := drivers/storage/bcm2712_sdio/host.c drivers/net/cyw43455/firmware.c drivers/storage/sdio/sdio.c src/net/wifi/wifi.c src/net/wifi/wifi_net.c src/net/wifi/wpa.c vendor/mbedtls/library/aes.c vendor/mbedtls/library/sha1.c
PLATFORM_gem := examples/ddk/gem_board.c examples/ddk/gem.c examples/ddk/gem_netif.c
PLATFORM_MODULE_SOURCES := $(sort $(foreach name,$(PLATFORM_MODULE_NAMES),$(PLATFORM_$(name)) platform/raspberrypi5/modules/$(name).c))
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
