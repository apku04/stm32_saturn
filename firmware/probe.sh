#!/bin/bash
# Test SWD connectivity to STM32U073CBT6
# This script just tries to connect via OpenOCD and read the chip ID.
# If it works, your wiring and PCB are good.
#
# Run with: sudo ./probe.sh

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Probing STM32U073CBT6 via SWD ==="
echo "  SWCLK = GPIO25 (pin 22)"
echo "  SWDIO = GPIO24 (pin 18)"
echo "  nRST  = GPIO17 (pin 12)"
echo ""

# Set PCIe ASPM to performance (recommended for RPi 5 GPIO bitbang)
echo performance | sudo tee /sys/module/pcie_aspm/parameters/policy > /dev/null 2>&1 || true

sudo openocd -f "$SCRIPT_DIR/openocd.cfg" \
    -c "init" \
    -c "echo \">>> SWD connected successfully!\"" \
    -c "echo \">>> Target: [target current]\"" \
    -c "targets" \
    -c "halt" \
    -c "echo \">>> Reading DBGMCU_IDCODE...\"" \
    -c "set idcode [mrw 0x40015800]" \
    -c "echo \">>> DBGMCU_IDCODE = [format 0x%08X \$idcode]\"" \
    -c "echo \">>> Reading flash size...\"" \
    -c "set flashsz [mrh 0x1FFF6360]" \
    -c "echo \">>> Flash size = \${flashsz} KB\"" \
    -c "echo \"\"" \
    -c "echo \">>> SUCCESS: STM32U073 is alive and reachable!\"" \
    -c "resume" \
    -c "shutdown"
