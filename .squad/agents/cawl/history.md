# Cawl — History

## Project Context

Project: Saturn LoRa Tracker — STM32U073CBT6 + SX1262 E22-900M22S
Repo: /home/pi/work/stm32/ (GitHub: apku04/stm32_saturn)
Firmware: bare-metal C, no HAL, custom registers in firmware/include/stm32u0.h
PCB: EasyEDA project stm32_lora.eprj (SQLite3 DB)
Flash path: USB DFU only (SWD disconnected)
First task: Fix INA219 on I2C2, add INA219 + battery voltage to beacon payload

## Learnings

### 2025-07-18 — I2C2 pin correction (Cawl override)
- Yarrick mapped I2C2 to PB13/PB14 (LED pins). PCB truth: INA219 on PB8/PB9.
- STM32U073 PB8/PB9 support AF6→I2C2 (not AF4→I2C1 as originally commented).
- AFRH bits [7:0] = 0x66 encodes AF6 for both PB8 (bits [3:0]) and PB9 (bits [7:4]).
- Always cross-check pin assignments against Kotov's PCB database before committing.
- LED pins (PB13/PB14) must never be reassigned without Architect approval.

### 2026-04-23 — Session Outcome (Cross-agent Learning)

**Status:** I2C2 + INA219 + Beacon V2 + LED restoration **COMPLETE** (commit 910cb9c, clean build)

**Key technical decisions locked:**
- **INA219 pins:** PB8/9 (AF6) — routed to I2C2 peripheral (verified against PCB)
- **LED pins:** PB13/14 (GPIO output) — restored to full functionality
- **Beacon format:** V2 (7 bytes: shunt/bus/bat/chg) — backward compatible with legacy parser
- **I2C topology:** I2C1 on PB6/7 (external), I2C2 on PB8/9 (INA219) — fully separated

**Agent performance:**
- Kotov: Expert PCB verification (EasyEDA SQLite DB decoding)
- Yarrick: Sound I2C driver code, but critical PCB validation gap
- Eisenhorn: Prevented 3rd wrong migration via independent verification
- Cawl: Surgical architect-level fix (minimal scope, maximum correctness)
- Brostin: Comprehensive testing strategy (single + target-to-target)

**Shared with team:** Kotov's PCB topology findings, Yarrick's lock-out reasoning, Cawl's fix principles

### 2026-04-23 — Bat_mv Reroute to INA219 Bus Voltage (Accepted Solution)

**Problem escalation:** Yarrick's ADC fix (commit 44989e2) REJECTED by Eisenhorn
- PB4 has no ADC on STM32U073CBT6
- All ADC pins (PA0–PA7, PB0–PB1) occupied by LoRa SPI and GPS UART
- MCU VBAT pin tied to 3.3V, not battery rail
- **No software ADC path viable**

**Solution (Cawl architecture decision + Eisenhorn verification):**
- Use INA219 bus voltage register (0x02) as battery proxy
- beaconHandler() → `bat_mv = ina219_read_bus_mv()` (was `adc_read_battery_mv()`)
- adc.c stubbed: all functions return 0, ADC peripheral not initialized, PA15 SENSE_LDO_EN preserved
- Beacon V2 payload (7 bytes) unchanged — bat_mv and bus_mv both from INA219
- Trade-off acceptable: both values same (INA219 bus voltage IS battery-side on charging path)

**Verification checklist (Eisenhorn audit — 7/7 PASS):**
1. ✅ ina219_read_bus_mv() correct (no args, returns uint16_t mV, datasheet compliant)
2. ✅ Beacon payload unchanged (7 bytes: shunt/bus/bat/chg)
3. ✅ Double-read harmless (same register, same value)
4. ✅ I2C init order safe (adc_init before beacon reads)
5. ✅ adc.c compiles cleanly
6. ✅ PA15 functionality preserved
7. ✅ Clean build (22264 text, 108 data, 1932 bss)

**Build status:** CLEAN

**Key decision principle:** Re-source measurements from working sensors (INA219) rather than force impossible ADC channels. Pragmatic solution for current cycle; next PCB revision can add true ADC-based battery monitoring on correct pin.

**Next actions:**
- Brostin flash test: validate INA219 responses, beacon payload generation
- Hardware investigation: verify INA219 population/solder joints if no ACK at 0x40
- Future PCB: route BAT_SENSE to PB1 (sacrifice DIO2, poll via SPI instead) or similar ADC-capable pin


2026-04-24: See decisions.md entry "Saturn board hardware & workflow facts" — covers I²C mux exclusivity, U10 INA219 init/scaling, DFU flash workflow, beacon v3 payload.
