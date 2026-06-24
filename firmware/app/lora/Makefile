TARGET = lora
CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size

# Project root (firmware/)
ROOT = ../..

# ---- Build version identity ------------------------------------------------
# Source identity from git (tag-aware, marks a dirty tree); UTC build stamp
# changes on every build so a flash can be verified on-target via `version`.
FW_GIT_VERSION := $(shell git -C $(ROOT) describe --always --dirty --tags 2>/dev/null || echo nogit)
FW_BUILD_ID    := $(shell date -u +%Y%m%dT%H%M%SZ)
VERSION_FLAGS  := -DFW_GIT_VERSION='"$(FW_GIT_VERSION)"' -DFW_BUILD_ID='"$(FW_BUILD_ID)"'

CFLAGS  = -mcpu=cortex-m0plus -mthumb -Os -Wall -Wno-unused-function
CFLAGS += -ffreestanding -ffunction-sections -fdata-sections
CFLAGS += -I$(ROOT)/include -I$(ROOT)/src/driver -I$(ROOT)/src/protocol
CFLAGS += -I$(ROOT)/src/system -I$(ROOT)/src/board -I.

LDFLAGS = -T $(ROOT)/linker/stm32u073cb.ld -nostartfiles -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs

# Sources by module
DRV_SRC = $(ROOT)/src/driver/usb_cdc.c $(ROOT)/src/driver/spi.c \
          $(ROOT)/src/driver/sx1262.c $(ROOT)/src/driver/timer.c \
          $(ROOT)/src/driver/adc.c $(ROOT)/src/driver/i2c.c \
          $(ROOT)/src/driver/ina219.c $(ROOT)/src/driver/gps.c \
          $(ROOT)/src/driver/bb_i2c.c $(ROOT)/src/driver/bme280.c \
          $(ROOT)/src/driver/sht3x.c $(ROOT)/src/driver/ext_flash.c
PRO_SRC = $(ROOT)/src/protocol/maclayer.c $(ROOT)/src/protocol/networklayer.c
SYS_SRC = $(ROOT)/src/system/packetBuffer.c $(ROOT)/src/system/terminal.c \
          $(ROOT)/src/system/flash_config.c $(ROOT)/src/system/flash_ota.c \
          $(ROOT)/src/system/ota_apply.c $(ROOT)/src/system/crc32.c \
          $(ROOT)/src/system/hal.c $(ROOT)/src/system/event_log.c
APP_SRC = main.c version.c

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

# version.o is force-rebuilt every make (phony prereq) so the embedded build
# stamp + git state always match the binary actually produced.
.PHONY: force_version
$(BUILDDIR)/version.o: version.c force_version | $(BUILDDIR)
	$(CC) $(CFLAGS) $(VERSION_FLAGS) -c -o $@ version.c

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
