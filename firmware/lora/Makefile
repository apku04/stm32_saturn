TARGET = lora
CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size

CFLAGS  = -mcpu=cortex-m0plus -mthumb -Os -Wall -Wno-unused-function
CFLAGS += -ffreestanding -ffunction-sections -fdata-sections
CFLAGS += -I.. -I.

LDFLAGS = -T ../stm32u073cb.ld -nostartfiles -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs

SRCS = main.c usb_cdc.c spi.c sx1262.c timer.c \
       packetBuffer.c maclayer.c networklayer.c terminal.c \
       flash_config.c hal.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET).bin

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lgcc

$(TARGET).bin: $(TARGET).elf
	$(SIZE) $<
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f $(TARGET).elf $(TARGET).bin $(OBJS)

flash: $(TARGET).bin
	cd .. && sudo ./flash.sh lora/$(TARGET).bin

.PHONY: all clean flash
