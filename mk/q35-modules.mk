PLATFORM_MODULE_NAMES := vblk vnet vrng vga ps2 xhci
ifeq ($(PROFILE),recovery)
PLATFORM_MODULE_NAMES := $(filter-out vnet,$(PLATFORM_MODULE_NAMES))
endif
PLATFORM_MODULES := $(addprefix $(OUT)/modules/,$(addsuffix .lkm,$(PLATFORM_MODULE_NAMES)))
Q35_vblk := drivers/storage/virtio_blk/block.c platform/q35/virtio.c
Q35_vnet := drivers/net/virtio_net/net.c platform/q35/virtio.c
Q35_vrng := drivers/entropy/virtio_rng/rng.c platform/q35/virtio.c
Q35_vga := drivers/display/bochs_vbe/display.c src/terminal/terminal.c src/terminal/font5x7.c
Q35_ps2 := drivers/input/ps2/keyboard.c drivers/input/usb_hid/hid_keyboard.c
Q35_xhci := platform/q35/modules/usb.c drivers/usb/xhci/usb.c drivers/usb/xhci/usb_descriptors.c drivers/input/usb_hid/hid_keyboard.c
Q35_MODULE_SOURCES := $(sort $(foreach name,$(PLATFORM_MODULE_NAMES),$(Q35_$(name))))
Q35_MODULE_OBJECTS := $(addprefix $(OUT)/lkm/obj/,$(addsuffix .o,$(Q35_MODULE_SOURCES)))
.PHONY: platform-modules
all: platform-modules
platform-modules: $(PLATFORM_MODULES)
$(OUT)/lkm/obj/%.c.o: %.c $(OUT)/configuration $(OUT)/ddk/include/lume/module_build.h Makefile mk/q35-modules.mk
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) -I$(OUT)/ddk/include $(filter-out -ffunction-sections -fdata-sections,$(CFLAGS)) -MMD -MP -MF "$@.d" -MT "$@" -c "$<" -o "$@"

define q35_module_rule
$(OUT)/modules/$(1).lkm: $(addprefix $(OUT)/lkm/obj/,$(addsuffix .o,$(Q35_$(1))))
	mkdir -p "$$(@D)"
	$(LD) -r --build-id=none $$^ -o "$$@"
endef
$(foreach name,$(PLATFORM_MODULE_NAMES),$(eval $(call q35_module_rule,$(name))))
-include $(addsuffix .d,$(Q35_MODULE_OBJECTS))
