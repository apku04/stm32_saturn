#!/bin/bash
# Run target tests for STM32 LoRa firmware
# Usage: ./run_pytest.sh [pytest args]
# Example: ./run_pytest.sh -v
#          ./run_pytest.sh -k "test_help"
#          SERIAL_PORTS=/dev/ttyACM1 ./run_pytest.sh

cd "$(dirname "$0")"
python3 -m pytest test_target_test.py "$@"
