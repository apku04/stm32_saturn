#!/bin/bash
# dfu_flash.sh — Flash STM32 via USB DFU bootloader
#
# Usage:
#   ./tools/dfu_flash.sh app/lora/lora.bin        # MCU already in DFU mode
#   ./tools/dfu_flash.sh app/lora/lora.bin --enter # Send 'dfu' command first
#
# The --enter flag sends the 'dfu' command over USB CDC to make the MCU
# jump to the system bootloader, then flashes via DFU.

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 <firmware.bin> [--enter]"
    echo "  --enter  Send 'dfu' command to running app first"
    exit 1
fi

BIN_FILE="$1"
ENTER_DFU="${2:-}"

if [ ! -f "$BIN_FILE" ]; then
    echo "Error: $BIN_FILE not found"
    exit 1
fi

STM32_CDC=""

enter_dfu_mode() {
    # Find STM32 CDC port (VID 0483, PID 5740)
    for dev in /dev/ttyACM*; do
        [ -e "$dev" ] || continue
        if udevadm info -q property "$dev" 2>/dev/null | grep -q "ID_VENDOR_ID=0483"; then
            STM32_CDC="$dev"
            break
        fi
    done

    if [ -z "$STM32_CDC" ]; then
        echo "Error: No STM32 CDC device found. Is the app running?"
        exit 1
    fi

    echo "=== Sending 'dfu' command to $STM32_CDC ==="
    # Send dfu command — port will close as MCU jumps to bootloader
    python3 -c "
import serial, time
try:
    s = serial.Serial('$STM32_CDC', timeout=1)
    s.write(b'dfu\r\n')
    time.sleep(0.5)
    s.close()
except:
    pass  # Expected — port closes when MCU enters bootloader
"
    echo "Waiting for DFU device..."
    for i in $(seq 1 20); do
        if lsusb | grep -q "0483:df11"; then
            echo "DFU device found"
            return 0
        fi
        sleep 0.5
    done
    echo "Error: DFU device did not appear after 10s"
    exit 1
}

# If --enter, send dfu command first
if [ "$ENTER_DFU" = "--enter" ]; then
    enter_dfu_mode
fi

# Verify DFU device is present
if ! lsusb | grep -q "0483:df11"; then
    echo "Error: No STM32 DFU device found."
    echo "Either use --enter flag, or put device in DFU mode manually."
    exit 1
fi

echo "=== Flashing $BIN_FILE via USB DFU ==="
sudo dfu-util -a 0 -s 0x08000000:leave -D "$BIN_FILE" 2>&1 || true
# The :leave option causes a reset, dfu-util may report an error — that's OK

echo ""
echo "=== Waiting for app to start ==="
sleep 3

if lsusb | grep -q "0483:5740"; then
    echo "App is running (USB CDC detected)"
else
    echo "Warning: USB CDC not detected — app may not have started"
fi
