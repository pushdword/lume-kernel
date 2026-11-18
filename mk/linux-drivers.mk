LINUX_COMPAT_CPPFLAGS := -Iinclude/compat/linux -Ivendor/linux/include
LINUX_COMPAT_COMMON := src/compat/linux/memory.c src/compat/linux/runtime.c src/compat/linux/ddk.c
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
LINUX_NATIVE_SOURCES += drivers/linux/virtio_rng.c $(LINUX_VIRTIO_RNG_SOURCE) platform/q35/virtio.c
endif

ifeq ($(ARCH),i686)
ifeq ($(LINUX_DRIVERS),1)
KERNEL_C += $(LINUX_COMPAT_COMMON) drivers/linux/virtio_rng.c $(LINUX_VIRTIO_RNG_SOURCE)
ifeq ($(filter platform/q35/virtio.c,$(KERNEL_C)),)
KERNEL_C += platform/q35/virtio.c
endif
CPPFLAGS += -DLUME_LINUX_BUILTIN=1
$(addprefix $(OUT)/obj/,$(addsuffix .o,$(LINUX_COMPAT_COMMON) drivers/linux/virtio_rng.c $(LINUX_VIRTIO_RNG_SOURCE))): CPPFLAGS += $(LINUX_COMPAT_CPPFLAGS) -DKBUILD_MODNAME='"virtio_rng"'
endif
endif

LINUX_NATIVE_OBJECTS := $(addprefix $(OUT)/linux/obj/,$(addsuffix .o,$(LINUX_NATIVE_SOURCES)))
LINUX_COMPAT_HEADERS := $(wildcard include/compat/linux/linux/*.h include/compat/linux/asm/*.h include/compat/linux/lume/*.h) vendor/linux/include/linux/hw_random.h
.PHONY: linux-drivers linux-driver-modules check-linux-compat check-linux-memory
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
linux-driver-modules: $(LINUX_MODULE)
$(LINUX_MODULE): $(LINUX_NATIVE_OBJECTS)
	mkdir -p "$(@D)"
	$(LD) -r --build-id=none $^ -o "$@"

$(OUT)/linux/obj/%.c.o: %.c $(LINUX_COMPAT_HEADERS) $(OUT)/configuration $(OUT)/ddk/include/lume/module_build.h mk/linux-drivers.mk
	mkdir -p "$(@D)"
	$(CC) $(LINUX_COMPAT_CPPFLAGS) $(CPPFLAGS) -I$(OUT)/ddk/include -DKBUILD_MODNAME='"virtio_rng"' $(filter-out -std=c11 -ffunction-sections -fdata-sections,$(CFLAGS)) -std=gnu11 -MMD -MP -MF "$@.d" -MT "$@" -c "$<" -o "$@"
-include $(addsuffix .d,$(LINUX_NATIVE_OBJECTS))
else
linux-driver-modules:
	$(error Dynamic Linux driver modules require aarch64 or amd64)
endif

LINUX_HOST_SOURCES := tests/unit/linux_compat_test.c src/compat/linux/memory.c src/compat/linux/runtime.c $(LINUX_RNG200_SOURCE) $(LINUX_VIRTIO_RNG_SOURCE)
LINUX_HOST_OBJECTS := $(addprefix $(OUT)/linux/host/,$(addsuffix .o,$(LINUX_HOST_SOURCES)))
LINUX_HOST_FLAGS := -std=gnu11 -Wall -Wextra -Werror -UNDEBUG
ifeq ($(OS),Windows_NT)
$(OUT)/linux/host/$(LINUX_VIRTIO_RNG_SOURCE).o: LINUX_HOST_FLAGS += -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
LINUX_HOST_LINK_FLAGS := -Wl,--image-base,0x400000,--disable-dynamicbase
endif
$(OUT)/linux/host/%.c.o: %.c $(LINUX_COMPAT_HEADERS) mk/linux-drivers.mk
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) $(LINUX_HOST_FLAGS) $(LINUX_COMPAT_CPPFLAGS) -Wno-unused-function -DKBUILD_MODNAME='"virtio_rng"' -c "$<" -o "$@"
$(OUT)/tests/linux-compat$(HOSTEXE): $(LINUX_HOST_OBJECTS)
	mkdir -p "$(@D)"
	$(HOSTCC) $^ $(LINUX_HOST_LINK_FLAGS) -o "$@"
check-linux-compat: $(OUT)/tests/linux-compat$(HOSTEXE)
	env "$<"

.PHONY: check-linux-ddk
check-linux-ddk: $(OUT)/tests/linux-ddk$(HOSTEXE)
	env "$<"
check: check-linux-compat check-linux-ddk
$(OUT)/tests/linux-ddk$(HOSTEXE): tests/unit/linux_ddk_test.c tests/fixtures/linux_ddk_worker_host.h src/core/worker.c src/compat/linux/memory.c src/compat/linux/runtime.c src/compat/linux/ddk.c $(LINUX_COMPAT_HEADERS)
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -std=gnu11 -Wall -Wextra -Werror -UNDEBUG $(LINUX_COMPAT_CPPFLAGS) -Iinclude/kernel -Iinclude/ddk -Iinclude/uapi -Iinclude/abi/native -Iarch/amd64/include/kernel tests/unit/linux_ddk_test.c src/compat/linux/memory.c src/compat/linux/runtime.c $(LINUX_HOST_LINK_FLAGS) -o "$@"
