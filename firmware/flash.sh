#!/bin/bash
# Flash a binary to STM32U073CBT6 via SWD
# Usage: sudo ./flash.sh <binary.bin>
# Example: sudo ./flash.sh test/test.bin
#          sudo ./flash.sh blink/blink.bin

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -z "$1" ]; then
    echo "Usage: sudo $0 <firmware.bin>"
    echo ""
    echo "Available binaries:"
    echo "  test/test.bin   - Simple connectivity test (alternating LED blink)"
    echo "  blink/blink.bin - Multi-pattern LED blink"
    echo ""
    echo "Build first with: cd test && make  (or cd blink && make)"
    exit 1
fi

BIN="$1"
if [ ! -f "$BIN" ]; then
    echo "Error: $BIN not found"
    exit 1
fi

echo "=== Flashing $BIN to STM32U073CBT6 ==="

# Set PCIe ASPM to performance
echo performance | sudo tee /sys/module/pcie_aspm/parameters/policy > /dev/null 2>&1 || true

sudo openocd -f "$SCRIPT_DIR/openocd.cfg" \
    -c "init" \
    -c "halt" \
    -c "flash write_image erase $BIN 0x08000000" \
    -c "verify_image $BIN 0x08000000" \
    -c "echo \">>> Flash complete and verified!\"" \
    -c "reset run" \
    -c "echo \">>> Target reset and running.\"" \
    -c "shutdown"
