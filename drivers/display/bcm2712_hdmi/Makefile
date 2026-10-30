DDK ?= /data/sdk/ddk
DRIVER := bcm2712-hdmi
KERNEL := ../../..
SOURCES := module.c firmware.c console.c $(KERNEL)/src/terminal/terminal.c $(KERNEL)/src/terminal/font5x7.c $(KERNEL)/src/terminal/font8x16.c
MODULE := 1
include $(DDK)/driver.mk
CFLAGS += -Wall -Wextra -Werror -I$(KERNEL)/include/kernel -I$(KERNEL)/src/terminal -isystem "$(shell $(CC) -print-resource-dir)/include"
