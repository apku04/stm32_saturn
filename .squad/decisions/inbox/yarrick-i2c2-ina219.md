# Decision: INA219 moved from I2C1 to I2C2

**Date:** 2025-07-17
**Author:** Yarrick (STM32 firmware)
**Status:** Implemented

## Context
INA219 current sensor was not responding. The driver was configured for I2C1 (PB8/PB9, AF4) but the hardware has the sensor on I2C2.

## Decision
1. **I2C2 pins**: PB13/PB14 (AF6) — only available I2C2 pins (PA10/PA11 taken by charge status and USB)
2. **LED sacrifice**: PB13/PB14 were LEDs (LED1/LED2). LED functions in lora app are now no-ops. HardFault handler still blinks PB13/PB14 as last-resort diagnostic.
3. **I2C driver refactored**: i2c.c now uses base-address parameterized macros (IC_CR1(base), etc.) with static internal functions. Public API: i2c_*() for I2C1, i2c2_*() for I2C2. Same TIMINGR (0x30420F13) for both.
4. **INA219 shunt voltage added**: New function `ina219_read_shunt_mv()` reads register 0x01 (signed 16-bit, LSB=10µV → mV).
5. **Beacon payload v2**: 7 bytes — shunt_mv(int16) + bus_mv(uint16) + bat_mv(uint16) + chg(uint8). Old format was 5 bytes (bat+sol+chg). Receiver handles both.
6. **Monitor updated**: lora_monitor.py parses new `[BEACON] shunt=N bus=N bat=N chg=N entries=N` format. Beacon tree has shunt_mv column.

## Consequences
- LEDs no longer functional in lora firmware (PB13/PB14 = I2C2)
- Beacon payload is backward-incompatible (new format). Old firmware (if running) uses legacy parse path.
- If LEDs are needed later, find alternative pins or use a single LED on a free pin.

## Files Changed
- `firmware/src/board/hw_pins.h` — PB13/PB14 redefined as I2C2_SCL/SDA
- `firmware/src/driver/i2c.h` — Added i2c2_* declarations
- `firmware/src/driver/i2c.c` — Refactored with base-address macros, added I2C2 init+API
- `firmware/src/driver/ina219.h` — Added ina219_read_shunt_mv()
- `firmware/src/driver/ina219.c` — Switched to I2C2, added shunt voltage read
- `firmware/app/lora/main.c` — Updated beacon payload (7 bytes), disabled LED functions, updated beacon RX parser
- `tools/lora_monitor.py` — Added V2 beacon parsing with shunt/bus/bat fields
