TARGET = lora
CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size

# Project root (firmware/)
ROOT = ../..

CFLAGS  = -mcpu=cortex-m0plus -mthumb -Os -Wall -Wno-unused-function
CFLAGS += -ffreestanding -ffunction-sections -fdata-sections
CFLAGS += -I$(ROOT)/include -I$(ROOT)/src/driver -I$(ROOT)/src/protocol
CFLAGS += -I$(ROOT)/src/system -I$(ROOT)/src/board -I.

LDFLAGS = -T $(ROOT)/linker/stm32u073cb.ld -nostartfiles -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs

# Sources by module
DRV_SRC = $(ROOT)/src/driver/usb_cdc.c $(ROOT)/src/driver/spi.c \
          $(ROOT)/src/driver/sx1262.c $(ROOT)/src/driver/timer.c \
          $(ROOT)/src/driver/adc.c $(ROOT)/src/driver/i2c.c \
          $(ROOT)/src/driver/ina219.c
PRO_SRC = $(ROOT)/src/protocol/maclayer.c $(ROOT)/src/protocol/networklayer.c
SYS_SRC = $(ROOT)/src/system/packetBuffer.c $(ROOT)/src/system/terminal.c \
          $(ROOT)/src/system/flash_config.c $(ROOT)/src/system/flash_ota.c \
          $(ROOT)/src/system/hal.c
APP_SRC = main.c

SRCS = $(APP_SRC) $(DRV_SRC) $(PRO_SRC) $(SYS_SRC)
OBJS = $(notdir $(SRCS:.c=.o))

# Build directory
BUILDDIR = build
BUILDOBJS = $(addprefix $(BUILDDIR)/,$(OBJS))

all: $(TARGET).bin

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: %.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/%.o: $(ROOT)/src/driver/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/%.o: $(ROOT)/src/protocol/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/%.o: $(ROOT)/src/system/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET).elf: $(BUILDOBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lgcc

$(TARGET).bin: $(TARGET).elf
	$(SIZE) $<
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(TARGET).elf $(TARGET).bin $(BUILDDIR)

flash: $(TARGET).bin
	cd $(ROOT) && sudo tools/flash.sh app/lora/$(TARGET).bin

.PHONY: all clean flash

.PHONY: all clean flash
