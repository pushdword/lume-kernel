DDK ?= /data/sdk/ddk
DRIVER := bcm2712-v3d
KERNEL := ../../..
SOURCES := module.c v3d.c
MODULE := 1
include $(DDK)/driver.mk
CFLAGS += -Wall -Wextra -Werror -I. -I$(KERNEL)/include/kernel
