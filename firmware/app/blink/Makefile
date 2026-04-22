PREFIX  = arm-none-eabi-
CC      = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
SIZE    = $(PREFIX)size

ROOT = ../..

CFLAGS  = -mcpu=cortex-m0plus -mthumb -Os -Wall -ffreestanding -nostdlib
LDFLAGS = -T $(ROOT)/linker/stm32u073cb.ld -nostartfiles -Wl,--gc-sections

.PHONY: all clean

all: blink.bin blink.elf

blink.elf: main.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	$(SIZE) $@

blink.bin: blink.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f blink.elf blink.bin
