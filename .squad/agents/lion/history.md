# Lion — History

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

### 2026-04-23 — ADC Channel Fix REJECTED — Yarrick Locked Out

**Critical finding from ADC investigation:**
- **PB4 has NO ADC on STM32U073CBT6** (verified via official STM32CubeMX PeripheralPins.c)
- Official ADC pin map: PA0–PA7, PB0–PB1 only
- Yarrick's fix changed broken channel 22 (non-existent) to broken channel 13 (internal VBAT/3 only)
- Channel 13 has no bonded external pin; cannot be driven by PB4 regardless of firmware

**Verdict:** ⛔ REJECTED commit 44989e2

**Eisenhorn cross-verification (7-point audit):**
1. ❌ PB4 = ADC1_IN13 — WRONG (no ADC function)
2. ✅ CKMODE=01 (PCLK/2) — CORRECT code, moot given hardware
3. ✅ VBATEN cleared — CORRECT implementation, moot given hardware
4. ✅ Build clean — PASSES
5. ❌ Channel 13 external routing — IMPOSSIBLE (internal only)
6. ✅ PA15 SENSE_LDO_EN — CORRECT
7. ❌ Root cause PCB mismatch — FUNDAMENTAL HARDWARE ISSUE

**Penalty:** Yarrick locked out from I2C/pin/ADC changes for this cycle

**Key lesson:** Always verify hardware constraints against authoritative sources (datasheets, official MCU DB files) BEFORE attempting peripheral fixes. Empirical channel scanning cannot overcome hardware limitations.

### 2026-04-23 — Battery Voltage Workaround Approved (Cawl's INA219 Solution)

**Status:** ✅ APPROVED — pragmatic interim solution (commit 0483139)

**Why Eisenhorn approved this despite PCB bug:**
- No software ADC path viable (PB4 non-ADC, all ADC pins occupied, no free pin for reroute)
- INA219 already on board, functional, measures charging path voltage
- Bus voltage register (0x02) reads battery-side when solar path active
- Acceptable interim solution for current product cycle
- Next PCB revision can restore independent ADC-based battery measurement

**Cross-agent learning:** When a PCB routing error blocks a feature, re-source the measurement from an existing working sensor rather than inventing impossible firmware workarounds. Architecture decision (Cawl override) based on PCB constraint (Kotov finding) + hardware verification (Eisenhorn audit).

