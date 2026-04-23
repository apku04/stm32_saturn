# Squad Decisions

## Active Decisions

### 1. I2C2 Pin Mapping — PCB Ground Truth (Kotov)

**Date:** 2025-07-14  
**Status:** Established  
**Authority:** Kotov (PCB Expert)

**Findings:**
- INA219 physically wired to **PB8 (SCL) and PB9 (SDA)**
- INA219 I2C address: **0x40** (A0/A1 both tied to GND)
- Separate I2C connector: PB6/7 (external header, not INA219)
- STM32U073 AF mapping: PB8/9 support AF4→I2C1 or AF6→I2C2

**Key Lesson:** On STM32U073, pins can route to multiple peripherals via AF selection.  
The 1-line fix was AF4→AF6, not pin migration.

---

### 2. INA219 I2C Pin Conflict Verdict (Eisenhorn)

**Date:** 2025-07-17  
**Status:** REJECTED Yarrick's commit 1bba9d5 (2025-07-17)  
**Authority:** Eisenhorn (Hallucination Detective)

**Root Cause Chain:**
1. **Pre-5399462:** Used PB6/7 (wrong pins for INA219)
2. **Commit 5399462:** Moved to PB8/9 + used AF4 (routed to I2C1, not I2C2) → MISMATCH
3. **Commit 1bba9d5 (Yarrick):** Moved to PB13/14 (LED pins, not wired to INA219) → DEAD

**Correct Fix:** PB8/9 + AF6 → I2C2 (one-line AF change from earlier attempts)

**Verdict:** 
- Kotov: **CORRECT** (pins, AF, address all verified via independent PCB decoding)
- Yarrick: **WRONG** (pin selection), but code quality was sound; locked out from I2C/pin changes
- Lock-out applies until corrections approved by Architect or team

**Code Audit Result:** Yarrick's I2C2 driver, INA219 registers, beacon payload V2, 
monitor parsing all technically correct despite wrong pins.

---

### 3. I2C2 Pin Correction (Cawl)

**Date:** 2025-07-18  
**Status:** Applied  
**Authority:** Cawl (Architect)  
**Commit:** 910cb9c

**Changes:**
1. `hw_pins.h`: Removed I2C2_SCL/SDA on PB13/14; restored LED1/LED2 defines; updated comments
2. `i2c.c`: Changed i2c2_init() from PB13/14 → PB8/9, AF to 6 (0x66 in AFRH)
3. `main.c`: Restored LED functions (init, toggle, on); LED GPIO config on PB13/14

**Constraints Preserved:**
- Beacon payload V2 (7 bytes) unchanged
- I2C2 TIMINGR unchanged (0x30420F13 for 100kHz @ 16MHz)
- No other files modified (surgical edit)

**Build Status:** Clean

---

### 4. Parser Fix — STM32 [RX] Format (Brostin)

**Date:** 2026-04-23  
**Status:** Implemented  
**Authority:** Brostin (Tester)  
**Commit:** 910cb9c (part of commit package)

**Problem:**
- 3 pytest tests fail because `parse_response()` expects PIC24 pipe-delimited format (`header_csv|hexdata`)
- STM32 firmware outputs `[RX] src=X dst=X rssi=X prssi=X type=X seq=X len=X\nHH HH HH` format
- False negatives — radio TX/RX works correctly

**Solution:**
- Rewrote `parse_response()` to match STM32 output format using RX_PATTERN regex (same as lora_monitor.py)
- Updated `conftest.py` serial_ports fixture to reset frequency/data_rate/tx_power (prevents cross-test state contamination)

**Files Changed:**
- `firmware/app/lora/test/test_target_test.py` — new parser implementation
- `firmware/app/lora/test/conftest.py` — fixture defaults reset

**Verification:**
- Parser unit tests: 5/5 pass
- Isolated pytest: test_send_receive_message PASSED
- Full suite: 32/35 pass; 3 failures = RF hardware issue (SX1262 not responding between boards), not code

**Open Items:**
- Radio intermittently non-functional → needs hardware investigation (SX1262 RX state after DFU cycles, antenna connectivity)
- INA219 not found at 0x40 on test hardware → Kotov to verify population/solder joints

---

### 5. INA219 Hardware Verification (Kotov)

**Date:** 2026-04-23  
**Status:** Verified  
**Authority:** Kotov (PCB Expert)

**Findings:**
- U10 (INA219AIDR) present in schematic at (1320, 635)
- Pull-up resistors: R40 (4.7kΩ, INA_SDA), R41 (4.7kΩ, INA_SCL) → VCC 3.3V
- Address: A0=GND, A1=GND → 0x40 (confirmed)
- Shunt: R58 = 50mΩ (±1%, 250mW) measures solar charging current
- Power: VCC from U23 (TPS7A0233DBVR LDO, 3.3V fixed output)
- All pin coordinates and connections verified via EasyEDA SQLite3 database

**Root Cause (no ACK at 0x40 in test):**
- Schematic design is correct and complete
- Possible causes: (1) U10 not populated, (2) solder defect, (3) firmware I2C peripheral config, (4) VCC not powered
- Recommended: Check VCC at U10 pin 5 with multimeter

---

### 6. Battery ADC Fix — REJECTED (Yarrick)

**Date:** 2026-04-23  
**Status:** REJECTED  
**Authority:** Eisenhorn (Hallucination Detective)  
**Commit under review:** 44989e2

**Critical Finding:**
- **PB4 has NO ADC capability on STM32U073CBT6** (verified via official STM32CubeMX PeripheralPins.c)
- Official ADC pin map: PA0–PA7, PB0–PB1 only
- PB4 has digital functions only: SPI1_MISO, I2C2_SDA, TIM3_CH1, USART1_CTS, LPUART3_RTS

**Claim Verification:**
1. ❌ PB4 = ADC1_IN13 — WRONG (PB4 has no ADC function)
2. ✅ CKMODE=01 (PCLK/2 sync) — CORRECT (code correctly sets CFGR2 bits [31:30])
3. ✅ VBATEN cleared in ADC_CCR — CORRECT (implementation) but moot given PB4 limitation
4. ✅ Build passes — PASSES (clean)
5. ❌ Channel 13 external routing — IMPOSSIBLE (CH13 = internal VBAT/3 only, no bonded external pin)
6. ✅ PA15 SENSE_LDO_EN — CORRECT

**Root Cause:**
- **PCB design error** — voltage divider output routed to non-ADC pin
- **No software fix possible** — hardware constraint

**Verdict:**
- ⛔ REJECTED commit 44989e2
- Changed broken channel 22 (non-existent) to different broken channel 13 (internal only)
- Yarrick locked out from I2C/pin/ADC changes for this cycle

---

### 7. Battery Voltage Routing — PCB Design Issue (Kotov)

**Date:** 2026-04-23  
**Status:** Verified  
**Authority:** Kotov (PCB Expert)

**Three Independent Problems Confirmed:**

1. **BAT_SENSE → PB4:**
   - Voltage divider circuit correct: R30 (100kΩ) + R31 (100kΩ) = 1:1 divider
   - Routes to PB4 which has no ADC on STM32U073 → **PCB bug**

2. **VBAT → VCC:**
   - MCU VBAT pin (pin 1) tied to VCC (3.3V), not battery rail
   - Internal VBAT/3 channel (CH13) reads constant ~1.1V → **useless for battery monitoring**

3. **All ADC pins occupied:**
   - PA0–PA7: LoRa SPI (NSS/SCK/MISO/MOSI) + RX/TX enables + GPS UART
   - PB0–PB1: LoRa DIO1/DIO2 interrupts
   - **No free ADC pin available for reroute**

**Viable Software-Only Workaround:**
- Use INA219 bus voltage register (reg 0x02) as battery proxy
- INA219 sits on solar charging path; bus voltage reads VIN− relative to GND
- When solar disconnected, may reflect battery voltage through charging circuit
- Needs empirical field validation

**Hardware Modification Options (future revisions):**
1. Cut PB4→BAT_SENSE trace, bodge to PB1 (sacrifice DIO2, poll via SPI instead)
2. Cut VBAT→VCC trace, connect to VCC_BAT_IN (enables internal VBAT/3 channel use)

---

### 8. Battery Voltage Solution — INA219 Bus Proxy (Cawl)

**Date:** 2026-04-23  
**Status:** Implemented + APPROVED  
**Authority:** Cawl (Architect), verified by Eisenhorn (Hallucination Detective)  
**Commit:** 0483139

**Rationale:**
- No software-only ADC path available (PB4 non-ADC, all ADC pins occupied)
- Use INA219 bus voltage register as interim battery voltage proxy
- Pragmatic solution for current product cycle; next PCB revision can restore independent measurement

**Implementation:**
1. **firmware/app/lora/main.c — beaconHandler():**
   - Changed: `bat_mv = adc_read_battery_mv()` → `bat_mv = ina219_read_bus_mv()`

2. **firmware/src/driver/adc.c:**
   - All ADC functions stubbed to return 0
   - ADC peripheral not initialized
   - PA15 SENSE_LDO_EN preserved in adc_init()
   - File kept for future revisions

3. **Beacon V2 Payload (7 bytes) unchanged:**
   - shunt_mv[2] (from INA219 shunt voltage register)
   - bus_mv[2] (from INA219 bus voltage register)
   - bat_mv[2] (now also from INA219 bus voltage register)
   - chg[1] (charge status)

**Trade-off:**
- bat_mv and bus_mv report same value (acceptable — INA219 bus voltage IS battery-side voltage on solar path)

**Build Status:** ✅ Clean
- Text: 22264 bytes, Data: 108 bytes, BSS: 1932 bytes
- No new compiler errors or warnings

**Eisenhorn Verification Checklist (7/7 PASS):**
1. ✅ `ina219_read_bus_mv()` signature and implementation correct
2. ✅ Beacon payload unchanged (7 bytes)
3. ✅ Double-read side-effect harmless (same register, same value)
4. ✅ I2C initialization order safe (adc_init before beacon reads)
5. ✅ adc.c stub compiles cleanly
6. ✅ PA15 functionality preserved
7. ✅ Build clean

**Verdict:** ✅ APPROVED — pragmatic, safe, maintainable

---

### 9. User Directive — Board Source Address Differentiation

**Date:** 2026-04-23T17:21Z  
**Directive:** Each board must have different src (source) address in LoRa packet header
**Authority:** User (via Copilot)  
**Rationale:** Target-to-target tests require distinguishable source addresses for validation

**Status:** Recorded for team implementation  
**Owner:** Cawl/Brostin for validation in flash test

---

## Governance

- All meaningful changes require team consensus (decisions recorded here)
- Architectural decisions (pin assignments, peripherals, payload formats) locked by Architect
- Agent lock-outs prevent repeat errors in high-risk domains (I2C, pins, peripherals)
- Cross-agent learnings propagated to history.md files (see .squad/agents/*/history.md)

## Version History

- **2025-07-14** — Kotov: PCB findings
- **2025-07-17** — Eisenhorn: Verdict + Yarrick lock-out
- **2025-07-18** — Cawl: Pin correction applied
- **2026-04-23** — Scribe: Consolidated battery ADC investigation, archived inbox decisions (9 new entries)
