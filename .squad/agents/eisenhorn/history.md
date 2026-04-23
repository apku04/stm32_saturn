# Eisenhorn — History

## Project Context

Project: Saturn LoRa Tracker — STM32U073CBT6 + SX1262 E22-900M22S
Repo: /home/pi/work/stm32/ (GitHub: apku04/stm32_saturn)
Firmware: bare-metal C, no HAL, custom registers in firmware/include/stm32u0.h
PCB: EasyEDA project stm32_lora.eprj (SQLite3 DB)
Flash path: USB DFU only (SWD disconnected)
First task: Fix INA219 on I2C2, add INA219 + battery voltage to beacon payload

## Learnings

### 2025-07-17 — I2C2 Pin Conflict Arbitration

**Finding:** INA219 is physically routed to PB8/PB9 (verified independently via
EasyEDA schematic coordinate tracing). On STM32U073CBT6, PB8/PB9 support BOTH
I2C1 (AF4) and I2C2 (AF6). The root cause was AF4 being used instead of AF6 —
pins routed to I2C1 but software talked to I2C2 peripheral.

**Kotov was correct**: PB8/PB9, AF6, I2C2.
**Yarrick was wrong**: moved to PB13/PB14 (LEDs, not connected to INA219).

**Key lesson**: Always verify pin assignments against the PCB schematic database
BEFORE reassigning pins. The AF table allows multiple peripherals per pin —
changing AF is a 1-line fix, moving pins requires PCB changes.

**Verdict**: REJECTED Yarrick's commit 1bba9d5. Yarrick locked out from I2C/pin
changes. Correction: restore PB8/PB9 with AF6, restore LEDs on PB13/PB14.

### 2026-04-23 — Cross-Agent Learning Shared

**From Cawl's correction (commit 910cb9c):** 
- I2C2 pin assignments now locked: PB8/9 for INA219 (AF6)
- LED pins locked: PB13/14 (GPIO output)
- Beacon V2 format locked: 7 bytes (shunt/bus/bat/chg)
- AFRH encoding: 0x66 = AF6 for both PB8 (bits [3:0]) and PB9 (bits [7:4])

**Shared with Ravenor:** I2C bus topology (I2C1 on PB6/7, I2C2 on PB8/9), Beacon V2 format, Kotov's PCB findings

