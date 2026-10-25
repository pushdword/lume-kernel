PLATFORM_MODULE_NAMES := gic timer watchdog dma uart sd usb display entropy wifi gem
PLATFORM_MODULES := $(addprefix $(OUT)/modules/,$(addsuffix .lkm,$(PLATFORM_MODULE_NAMES)))
PLATFORM_gic := platform/raspberrypi5/gic.c
PLATFORM_timer := platform/raspberrypi5/timer.c
PLATFORM_watchdog := platform/raspberrypi5/watchdog.c src/aarch64/watchdog.c
PLATFORM_dma := platform/raspberrypi5/dma.c
PLATFORM_uart := platform/raspberrypi5/uart.c
PLATFORM_sd := platform/raspberrypi5/sd.c
PLATFORM_usb := platform/raspberrypi5/usb.c src/aarch64/usb_descriptors.c src/aarch64/hid_keyboard.c
PLATFORM_display := platform/raspberrypi5/display.c src/common/terminal.c src/common/font5x7.c
PLATFORM_entropy := platform/raspberrypi5/entropy.c
PLATFORM_wifi := platform/raspberrypi5/wifi.c platform/raspberrypi5/wifi_fw.c src/aarch64/sdio.c src/aarch64/wifi.c src/aarch64/wifi_net.c src/aarch64/wpa.c vendor/aarch64/mbedtls/library/aes.c vendor/aarch64/mbedtls/library/sha1.c
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
