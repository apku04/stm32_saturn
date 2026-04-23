# Decision: Fix parse_response() for STM32 [RX] format

**Author:** Brostin (Tester)
**Date:** 2026-04-23
**Status:** Implemented

## Problem
3 pytest tests fail in `test_target_test.py` because `parse_response()` expects PIC24 pipe-delimited format (`header_csv|hexdata`) but STM32 firmware outputs `[RX] src=X dst=X rssi=X prssi=X type=X seq=X len=X\nHH HH HH` format. These are false negatives — radio TX/RX works correctly.

## Decision
Rewrite `parse_response()` to match STM32 output format using the same regex as `lora_monitor.py` RX_PATTERN. Also add radio config reset to `serial_ports` fixture to prevent cross-test state contamination.

## Files Changed
- `firmware/app/lora/test/test_target_test.py` — new `parse_response()` with RX_PATTERN regex
- `firmware/app/lora/test/conftest.py` — `serial_ports` fixture resets frequency/data_rate/tx_power

## Verification
- Parser unit tests: 5/5 pass
- Isolated pytest: test_send_receive_message PASSED with new parser
- Full suite: 32/35 pass; 3 communication tests still fail due to radio hardware not responding (unrelated to parser)

## Open Item
Radio intermittently non-functional between boards. Both have identical config but receiver gets no RF data. Needs hardware investigation (SX1262 RX state after DFU cycles, antenna connectivity). Not a test code issue.
