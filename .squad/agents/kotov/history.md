# Kotov — History

## Project Context

Project: Saturn LoRa Tracker — STM32U073CBT6 + SX1262 E22-900M22S
Repo: /home/pi/work/stm32/ (GitHub: apku04/stm32_saturn)
Firmware: bare-metal C, no HAL, custom registers in firmware/include/stm32u0.h
PCB: EasyEDA project stm32_lora.eprj (SQLite3 DB)
Flash path: USB DFU only (SWD disconnected)
First task: Fix INA219 on I2C2, add INA219 + battery voltage to beacon payload

## Learnings

### 2025-07-14 — I2C2 / INA219 PCB net tracing

- EasyEDA `.eprj` file is a SQLite DB. Schematic data is in `documents` table, `dataStr` column, base64+gzip encoded.
- Component symbols (pin definitions) are in the `components` table, same encoding.
- Schematic uses line-per-record JSON arrays: `["COMPONENT",...]`, `["WIRE",...]`, `["ATTR",...,"NET",<netname>,...]`.
- Pin absolute position = component origin + pin offset from symbol definition.
- INA219 (U10) placed at (1320,635). STM32 (U3) placed at (630,780).
- Two separate I2C bus nets exist on the schematic:
  - `I2C_SCL` / `I2C_SDA` → PB6 / PB7 (external I2C connector)
  - `INA_SCL` / `INA_SDA` → PB8 / PB9 (dedicated to INA219)
- INA219 A0 and A1 both tied to GND → 7-bit address **0x40**.
- STM32U073 AF mapping for PB8/PB9: AF4 = I2C1, **AF6 = I2C2**.
- Current firmware (`i2c.c`) uses AF4 + I2C1 registers — must change to AF6 + I2C2 to use the I2C2 peripheral.

### 2026-04-23 — PCB Findings Validated & Locked (Cross-agent Learning)

**Status:** All Kotov PCB findings validated and implemented (commit 910cb9c)

**Validation chain:**
1. Kotov: EasyEDA coordinate tracing + net topology
2. Eisenhorn: Independent PCB verification + AF mapping confirmation
3. Cawl: Applied PB8/9 AF6 fix (surgical precision)
4. Result: INA219 now functional (I2C2 on correct pins with correct AF)

**PCB truth locked into firmware:**
- INA219 pins: PB8 (SCL) / PB9 (SDA) — routed via AF6 to I2C2 peripheral
- I2C address: 0x40 (A0=GND, A1=GND)
- External I2C header: PB6/7 (AF4, I2C1) — separate bus for future expansion
- LED pins: PB13/14 (restored GPIO output)

**Shared with team:** EasyEDA decoding method, coordinate tracing technique, AF table interpretation for multi-peripheral pins. Establish Kotov as domain authority for PCB validation on future pin/peripheral changes
