TARGET = usb_cdc
CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size

CFLAGS = -mcpu=cortex-m0plus -mthumb -Os -Wall -ffreestanding -nostdlib
LDFLAGS = -T ../stm32u073cb.ld -nostartfiles -Wl,--gc-sections

all: $(TARGET).bin

$(TARGET).elf: main.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lgcc

$(TARGET).bin: $(TARGET).elf
	$(SIZE) $<
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f $(TARGET).elf $(TARGET).bin

.PHONY: all clean
