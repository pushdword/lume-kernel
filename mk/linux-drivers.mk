LINUX_COMPAT_CPPFLAGS := -Iinclude/compat/linux -Ivendor/linux/include
LINUX_COMPAT_COMMON := src/compat/linux/memory.c src/compat/linux/runtime.c src/compat/linux/workqueue.c src/compat/linux/tasklet.c src/compat/linux/kthread.c src/compat/linux/ddk.c
LINUX_COMPAT_NET := src/compat/linux/net.c
LINUX_COMPAT_USB := src/compat/linux/usb.c
LINUX_RNG200_SOURCE := vendor/linux/drivers/char/hw_random/iproc-rng200.c
LINUX_VIRTIO_RNG_SOURCE := vendor/linux/drivers/char/hw_random/virtio-rng.c
LINUX_NATIVE_SOURCES := $(LINUX_COMPAT_COMMON)
LINUX_DRIVERS ?= 0
ifeq ($(filter $(LINUX_DRIVERS),0 1),)
$(error LINUX_DRIVERS must be 0 or 1)
endif
ifeq ($(ARCH),aarch64)
LINUX_MODULE := $(OUT)/modules/linux-iproc-rng200.lkm
LINUX_NATIVE_SOURCES += drivers/linux/iproc_rng200.c $(LINUX_RNG200_SOURCE)
else ifeq ($(ARCH),amd64)
LINUX_MODULE := $(OUT)/modules/linux-virtio-rng.lkm
LINUX_NATIVE_SOURCES += drivers/linux/virtio_rng.c $(LINUX_VIRTIO_RNG_SOURCE) platform/q35/virtio.c src/device/virtio_pci.c
endif

ifeq ($(ARCH),i686)
ifeq ($(LINUX_DRIVERS),1)
KERNEL_C += $(LINUX_COMPAT_COMMON) $(LINUX_COMPAT_NET) drivers/linux/virtio_rng.c $(LINUX_VIRTIO_RNG_SOURCE)
ifeq ($(filter platform/q35/virtio.c,$(KERNEL_C)),)
KERNEL_C += platform/q35/virtio.c
endif
CPPFLAGS += -DLUME_LINUX_BUILTIN=1
$(addprefix $(OUT)/obj/,$(addsuffix .o,$(LINUX_COMPAT_COMMON) drivers/linux/virtio_rng.c $(LINUX_VIRTIO_RNG_SOURCE))): CPPFLAGS += $(LINUX_COMPAT_CPPFLAGS) -DKBUILD_MODNAME='"virtio_rng"'
$(OUT)/obj/drivers/storage/virtio_blk/block.c.o: CPPFLAGS += $(LINUX_COMPAT_CPPFLAGS) -DLUME_LINUX_VIRTIO_BLOCK=1
$(OUT)/obj/src/compat/linux/net.c.o: CPPFLAGS += $(LINUX_COMPAT_CPPFLAGS)
$(OUT)/obj/drivers/net/virtio_net/net.c.o: CPPFLAGS += $(LINUX_COMPAT_CPPFLAGS) -DLUME_LINUX_VIRTIO_NET=1
endif
endif

LINUX_NATIVE_OBJECTS := $(addprefix $(OUT)/linux/obj/,$(addsuffix .o,$(LINUX_NATIVE_SOURCES)))
LINUX_COMPAT_HEADERS := $(wildcard include/compat/linux/linux/*.h include/compat/linux/asm/*.h include/compat/linux/lume/*.h) vendor/linux/include/linux/hw_random.h
.PHONY: linux-drivers linux-driver-modules linux-module-sdk check-linux-compat check-linux-memory
check: check-linux-memory
check-linux-memory: $(OUT)/tests/linux-memory$(HOSTEXE)
	env "$<"
$(OUT)/tests/linux-memory$(HOSTEXE): tests/unit/linux_memory_test.c src/compat/linux/memory.c $(LINUX_COMPAT_HEADERS)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -std=c11 -Wall -Wextra -Werror -UNDEBUG $(LINUX_COMPAT_CPPFLAGS) $(filter %.c,$^) -o "$@"
ifeq ($(ARCH),i686)
ifeq ($(LINUX_DRIVERS),1)
linux-drivers: $(OUT)/lume.elf
else
linux-drivers:
	$(error i686 Linux drivers require LINUX_DRIVERS=1)
endif
else
linux-drivers: linux-driver-modules
endif
ifneq ($(LINUX_MODULE),)
LINUX_SDK_DIR := $(OUT)/ddk/linux
LINUX_SDK_OBJECTS := $(addprefix $(OUT)/linux/obj/,$(addsuffix .o,$(LINUX_COMPAT_COMMON) $(LINUX_COMPAT_NET) $(LINUX_COMPAT_USB)))
LINUX_SDK_HEADERS := $(patsubst include/compat/linux/%,$(LINUX_SDK_DIR)/include/%,$(filter include/compat/linux/%,$(LINUX_COMPAT_HEADERS)))
LINUX_SDK_HEADERS += $(LINUX_SDK_DIR)/include/linux/hw_random.h
linux-module-sdk: module-sdk $(LINUX_SDK_HEADERS) $(LINUX_SDK_DIR)/runtime.a

$(LINUX_SDK_DIR)/include/%: include/compat/linux/%
	mkdir -p "$(@D)"
	cp "$<" "$@"

$(LINUX_SDK_DIR)/include/linux/hw_random.h: vendor/linux/include/linux/hw_random.h
	mkdir -p "$(@D)"
	cp "$<" "$@"

$(LINUX_SDK_DIR)/runtime.a: $(LINUX_SDK_OBJECTS)
	mkdir -p "$(@D)"
	"$(TOOLBIN)llvm-ar$(HOSTEXE)" rcs "$@" $^

linux-driver-modules: $(LINUX_MODULE)
$(LINUX_MODULE): $(LINUX_NATIVE_OBJECTS)
	mkdir -p "$(@D)"
	$(LD) -r --build-id=none $^ -o "$@"

$(OUT)/linux/obj/%.c.o: %.c $(LINUX_COMPAT_HEADERS) $(OUT)/configuration $(OUT)/ddk/include/lume/module_build.h mk/linux-drivers.mk
	mkdir -p "$(@D)"
	$(CC) $(LINUX_COMPAT_CPPFLAGS) $(CPPFLAGS) -I$(OUT)/ddk/include -DKBUILD_MODNAME='"virtio_rng"' $(filter-out -std=c11 -ffunction-sections -fdata-sections,$(CFLAGS)) -std=gnu11 -MMD -MP -MF "$@.d" -MT "$@" -c "$<" -o "$@"
-include $(addsuffix .d,$(LINUX_NATIVE_OBJECTS))
-include $(OUT)/linux/obj/$(LINUX_COMPAT_NET).o.d
-include $(OUT)/linux/obj/$(LINUX_COMPAT_USB).o.d
else
linux-driver-modules linux-module-sdk:
	$(error Dynamic Linux driver modules require aarch64 or amd64)
endif

LINUX_HOST_SOURCES := tests/unit/linux_compat_test.c src/compat/linux/memory.c src/compat/linux/runtime.c src/compat/linux/workqueue.c src/compat/linux/tasklet.c src/compat/linux/kthread.c src/core/work.c $(LINUX_RNG200_SOURCE) $(LINUX_VIRTIO_RNG_SOURCE)
LINUX_HOST_OBJECTS := $(addprefix $(OUT)/linux/host/,$(addsuffix .o,$(LINUX_HOST_SOURCES)))
LINUX_HOST_FLAGS := -std=gnu11 -Wall -Wextra -Werror -UNDEBUG
ifeq ($(OS),Windows_NT)
$(OUT)/linux/host/$(LINUX_VIRTIO_RNG_SOURCE).o: LINUX_HOST_FLAGS += -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
LINUX_HOST_LINK_FLAGS := -Wl,--image-base,0x400000,--disable-dynamicbase
endif
$(OUT)/linux/host/%.c.o: %.c $(LINUX_COMPAT_HEADERS) mk/linux-drivers.mk
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(LINUX_HOST_FLAGS) $(LINUX_COMPAT_CPPFLAGS) -Iinclude/kernel -Iinclude/ddk -Iinclude/uapi -Iinclude/abi/native -Wno-unused-function -DKBUILD_MODNAME='"virtio_rng"' -c "$<" -o "$@"
$(OUT)/tests/linux-compat$(HOSTEXE): $(LINUX_HOST_OBJECTS)
	mkdir -p "$(@D)"
	$(HOSTCC) $^ $(LINUX_HOST_LINK_FLAGS) -o "$@"
check-linux-compat: $(OUT)/tests/linux-compat$(HOSTEXE)
	env "$<"

.PHONY: check-linux-ddk check-linux-kthread check-linux-net check-linux-tasklet check-linux-usb check-linux-usb-storage check-linux-workqueue
check: check-linux-kthread check-linux-net check-linux-tasklet check-linux-usb check-linux-usb-storage check-linux-workqueue
check-linux-tasklet: $(OUT)/tests/linux-tasklet$(HOSTEXE)
	env "$<"
$(OUT)/tests/linux-tasklet$(HOSTEXE): tests/unit/linux_tasklet_test.c src/compat/linux/tasklet.c src/core/work.c $(LINUX_COMPAT_HEADERS)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -std=gnu11 -Wall -Wextra -Werror -UNDEBUG $(LINUX_COMPAT_CPPFLAGS) -Iinclude/kernel -Iinclude/ddk -Iinclude/uapi -Iinclude/abi/native $(filter %.c,$^) -o "$@"
check-linux-kthread: $(OUT)/tests/linux-kthread$(HOSTEXE)
	env "$<"
$(OUT)/tests/linux-kthread$(HOSTEXE): tests/unit/linux_kthread_test.c src/compat/linux/kthread.c src/core/work.c $(LINUX_COMPAT_HEADERS)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -std=gnu11 -Wall -Wextra -Werror -UNDEBUG $(LINUX_COMPAT_CPPFLAGS) -Iinclude/kernel -Iinclude/ddk -Iinclude/uapi -Iinclude/abi/native $(filter %.c,$^) -o "$@"
check-linux-workqueue: $(OUT)/tests/linux-workqueue$(HOSTEXE)
	env "$<"
$(OUT)/tests/linux-workqueue$(HOSTEXE): tests/unit/linux_workqueue_test.c src/compat/linux/workqueue.c src/core/work.c $(LINUX_COMPAT_HEADERS)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -std=gnu11 -Wall -Wextra -Werror -UNDEBUG $(LINUX_COMPAT_CPPFLAGS) -Iinclude/kernel -Iinclude/ddk -Iinclude/uapi -Iinclude/abi/native $(filter %.c,$^) -o "$@"
check-linux-usb: $(OUT)/tests/linux-usb$(HOSTEXE)
	env "$<"
$(OUT)/tests/linux-usb$(HOSTEXE): tests/unit/linux_usb_test.c $(LINUX_COMPAT_USB) $(LINUX_COMPAT_HEADERS)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -std=gnu11 -Wall -Wextra -Werror -UNDEBUG $(LINUX_COMPAT_CPPFLAGS) -Iinclude/kernel -Iinclude/ddk -Iinclude/uapi -Iinclude/abi/native $(filter %.c,$^) -o "$@"
check-linux-usb-storage: $(OUT)/tests/linux-usb-storage$(HOSTEXE)
	env "$<"
$(OUT)/tests/linux-usb-storage$(HOSTEXE): tests/unit/linux_usb_storage_test.c drivers/linux/usb_storage.c $(LINUX_COMPAT_USB) $(LINUX_COMPAT_HEADERS)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -std=gnu11 -Wall -Wextra -Werror -UNDEBUG $(LINUX_COMPAT_CPPFLAGS) -Itests/fixtures/counter -Iarch/amd64/include/kernel -Iinclude/kernel -Iinclude/ddk -Iinclude/uapi -Iinclude/abi/native $(filter %.c,$^) -o "$@"
check-linux-net: $(OUT)/tests/linux-net$(HOSTEXE)
	env "$<"
$(OUT)/tests/linux-net$(HOSTEXE): tests/unit/linux_net_test.c $(LINUX_COMPAT_NET) src/net/netif.c $(LINUX_COMPAT_HEADERS)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -std=gnu11 -Wall -Wextra -Werror -UNDEBUG $(LINUX_COMPAT_CPPFLAGS) -Iinclude/kernel -Iinclude/ddk -Iinclude/uapi -Iinclude/abi/native $(filter %.c,$^) -o "$@"
check-linux-ddk: $(OUT)/tests/linux-ddk$(HOSTEXE)
	env "$<"
check: check-linux-compat check-linux-ddk
$(OUT)/tests/linux-ddk$(HOSTEXE): tests/unit/linux_ddk_test.c tests/fixtures/linux_ddk_worker_host.h src/core/worker.c src/core/work.c src/compat/linux/memory.c src/compat/linux/runtime.c src/compat/linux/workqueue.c src/compat/linux/tasklet.c src/compat/linux/kthread.c src/compat/linux/ddk.c src/device/irq.c include/ddk/lume/irq.h $(LINUX_COMPAT_HEADERS)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -std=gnu11 -Wall -Wextra -Werror -UNDEBUG $(LINUX_COMPAT_CPPFLAGS) -Iinclude/kernel -Iinclude/ddk -Iinclude/uapi -Iinclude/abi/native -Iarch/amd64/include/kernel tests/unit/linux_ddk_test.c src/core/work.c src/compat/linux/memory.c src/compat/linux/runtime.c src/compat/linux/workqueue.c src/compat/linux/tasklet.c src/compat/linux/kthread.c src/device/irq.c $(LINUX_HOST_LINK_FLAGS) -o "$@"
