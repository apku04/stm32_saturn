# Yarrick — History

## Project Context

Project: Saturn LoRa Tracker — STM32U073CBT6 + SX1262 E22-900M22S
Repo: /home/pi/work/stm32/ (GitHub: apku04/stm32_saturn)
Firmware: bare-metal C, no HAL, custom registers in firmware/include/stm32u0.h
PCB: EasyEDA project stm32_lora.eprj (SQLite3 DB)
Flash path: USB DFU only (SWD disconnected)
First task: Fix INA219 on I2C2, add INA219 + battery voltage to beacon payload

## Learnings

- I2C1 (PB8/PB9 AF4) was wrong for INA219; the sensor is on I2C2 (PB13/PB14 AF6)
- I2C2 clock enable is bit 22 of RCC_APBENR1 (I2C1 is bit 21)
- TIMINGR value 0x30420F13 works for both I2C1 and I2C2 at 100kHz/16MHz HSI
- PB13/PB14 were previously LEDs; repurposing for I2C2 disables LED functionality in lora app
- INA219 shunt register 0x01 is signed 16-bit, LSB=10µV; bus register 0x02 bits[15:3] LSB=4mV
- Refactored i2c.c to use base-address macros (IC_CR1(base) etc.) to support both I2C1 and I2C2 without code duplication
- Beacon payload grew from 5 to 7 bytes: shunt_mv(2) + bus_mv(2) + bat_mv(2) + chg(1)
- The lora_monitor.py must try V2 beacon pattern (shunt/bus/bat) before legacy (bat/sol) to avoid misparse

### 2025-07-17 — Lock-out and Correction (Cross-agent Learning)

**Issue:** Commit 1bba9d5 assigned I2C2 to PB13/PB14 (LED pins, not wired to INA219). PCB ground truth is PB8/PB9.

**Root Cause:** Did not verify pin assignments against Kotov's PCB database before reassigning pins. The correct fix was AF4→AF6 (1-line register change), not pin migration (requires PCB verification).

**Eisenhorn's Verdict:** Code quality was sound (I2C2 driver, registers, beacon format all correct). The pin selection was wrong. **Locked out from I2C/pin changes.**

**Key Lesson:** 
1. Always consult Kotov's EasyEDA PCB database before changing pin assignments
2. Prefer AF changes (register-only, no PCB implications) over pin migrations
3. On STM32U073, PB8/PB9 support both AF4→I2C1 and AF6→I2C2 — the one-line AF fix was the correct approach

**Cross-sharing:** This decision shared with Ravenor (INA219 now on I2C2, beacon V2 format) and Eisenhorn's lock-out logic documented for future reference
