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

### 2026-04-26 — GPS Driver Review APPROVED (Dorn's 6 Fixes)

**Status:** ✅ APPROVED — all 6 fixes verified, zero defects

**Fixes reviewed:**
1. SENSE_LDO_EN (PA15) power sequencing — output config, BSRR set, 100ms delay before USART2
2. AF4→AF7 on PA2/PA3 — AFRL bits [11:8] and [15:12] correctly set to 7
3. Non-blocking gps_poll() — SPSC ring buffer drain, returns immediately when empty
4. USART2 RXNE ISR — correct handler name, IRQ 28 in vector table, ORE cleared, race-safe ring buffer
5. Beacon payload v5 — GPS at data[9..17] (9 bytes), pkt.length=22, within 50-byte DATA_LEN
6. USART2 register defs in stm32u0.h — base 0x40004400, all offsets and bits match RM0503

**Key verification points:**
- NVIC_ISER direct write is safe (write-1-to-set semantics, doesn't clear other IRQs)
- Ring buffer uses power-of-2 size (64) with bitmask — correct SPSC lock-free pattern
- delay_ms available via timer.h include; no fabricated function names
- DFU/USB path (PA11/PA12) completely unaffected by GPS pin configuration
- Spec deviation: 100ms LDO warmup vs 500ms in decision #15, but TPS7A0233DBVR starts in <1ms; acceptable

### 2026-04-26 — Perturabo 7-Concern Fix Final Review

**Status:** ✅ APPROVED — all agent changes verified against source files

**Dorn's changes (Concerns 1, 3, 4, 7):**

1. **Concern 1 — TX length (sx1262.c + hal.c):**
   - `transmitFrame()` in hal.c line 18: uses `pkt->length` for `tx_len`, passes to `radio_send(payload, tx_len)`
   - `radio_send()` in sx1262.c line 479: passes `length` param to `write_buffer(0x00, payload, length)` and `set_packet_params(..., length, ...)`
   - `write_buffer()` uses `SX1262_CMD_WRITE_BUFFER` = 0x0E — **correct per SX1262 datasheet §13.2.1**
   - Verified in sx1262_register.h line 28: `#define SX1262_CMD_WRITE_BUFFER 0x0E` ✅
   - No hardcoded sizeof or constant — uses actual packet length throughout ✅

2. **Concern 3 — GPS ring buffer + checksum (gps.c):**
   - Ring buffer: `#define RX_BUF_SIZE 256u` (line 23), `rx_buf[RX_BUF_SIZE]` ✅
   - `nmea_checksum_valid()` at line 204: XORs bytes between `$` (exclusive) and `*` (exclusive), parses two hex digits after `*`, compares — **correct NMEA checksum algorithm** ✅
   - Checksum called at line 329 BEFORE any parse_gga/parse_rmc — bad checksums discarded ✅

3. **Concern 4 — GPRMC/GNRMC parsing (gps.c):**
   - `parse_rmc()` at line 246: handles `$GPRMC` and `$GNRMC` (lines 335-338)
   - Uses same `parse_latlon()` function as `parse_gga()` ✅
   - Status 'V': sets `fix.valid=0, fix.lat_udeg=0, fix.lon_udeg=0` (lines 264-268) ✅
   - Status 'A': updates time/lat/lon/valid (lines 259-263) ✅

4. **Concern 7 — Payload size (main.c):**
   - Beacon payload: data[0..17] = 18 bytes of telemetry
   - `pkt.length = 4 + 18 = 22` (line 123)
   - DATA_LEN = 50 in globalInclude.h line 135
   - PACKET_HEADER_SIZE = 3+5+4 = 12
   - Total on-wire: 12 (header) + 18 (data) = 30 bytes, well within limits ✅
   - `gps_init()` called at line 372, `gps_poll()` at line 404 ✅

**Khan's changes (Concern 6 — MAC address uniqueness):**

5. **UID_BASE address (stm32u0.h):**
   - Line 288: `#define UID_BASE 0x1FFF7590UL` — **correct for STM32U073 per RM0503 §45** ✅
   - NOT 0x1FFF6E50 (STM32L0) — correct MCU family address used ✅

6. **UID fingerprint reads (maclayer.c):**
   - `uid_fingerprint()` line 80-82: reads `UID_BASE`, `UID_BASE+4`, `UID_BASE+8` = 0x1FFF7590, 0x1FFF7594, 0x1FFF7598 ✅
   - `uid_derive_address()` line 90-92: same three 32-bit reads ✅

7. **Existing nodes preserved (maclayer.c lines 113-119):**
   - If `stored_addr` is valid (1..254) AND `stored_fp` is 0xFF (erased = first boot with new firmware): keeps stored address, stamps fingerprint ✅
   - Existing boards with valid flash addresses are NOT disturbed ✅

8. **Derived address safety (maclayer.c lines 93-98):**
   - `uid_derive_address()`: if derived == 0 or >= 254, applies `(derived ^ 0x55) & 0xFE`
   - Then if still 0, sets to 1
   - Result is always 1..253 — never 0x00 (broadcast) or 0xFF (invalid) ✅
   - `uid_fingerprint()` returns 0xFE instead of 0xFF — prevents collision with erased sentinel ✅

**No fabricated function names.** All functions verified against actual headers.
**DFU flash path unaffected** — no SWD usage.


### 2026-04-26 — Session Completion: Final Gate Review (Perturabo 7-Concern Audit)

**Scope:** Comprehensive final review of all firmware changes from Dorn, Ferrus, Corax, Khan agents.

**Review coverage:**

1. **Dorn's firmware fixes (concerns 1, 3, 4, 7):**
   - TX variable-length packets via pkt->length ✅
   - Ring buffer 64→256 bytes ✅
   - GPRMC/GNRMC parsing ✅
   - NMEA checksum validation ✅
   - **VTOR critical fix** (interrupt routing post-DFU) ✅

2. **Ferrus's pin verification (concern 2):**
   - PA15 confirmed correct for SENSE_LDO_EN ✅
   - Schematic coordinate tracing validated ✅

3. **Corax's monitor fix (concern 5):**
   - Stale GPS display now shows [stale] label + NO FIX indicator ✅
   - LAST FIX timestamp column added ✅

4. **Khan's MAC uniqueness (concern 6):**
   - UID_BASE corrected to STM32U073 value ✅
   - UID fingerprint clone detection logic verified ✅
   - Existing nodes (30, 38) migrate transparently ✅

**Hardware verification:** Both boards flashed, GPS operational (sentences=141, fix=1, sats=9).

**Build integrity:** 30,660–30,868 bytes, clean build, zero warnings, all source addresses verified against RM0503.

**Documentation:** All changes documented in decisions.md, session log, orchestration logs.

**Verdict:** ✅ **ALL 7 CONCERNS RESOLVED AND APPROVED** — ready for deployment to constellation.

**Status:** ✅ COMPLETE — Final gate passed, all changes cleared for production.

