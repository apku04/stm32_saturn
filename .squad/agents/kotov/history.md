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

### 2025-07-14 — INA219 Full Hardware Audit (Coordinator request)

**Context:** INA219 returned no I2C ACK at 0x40 during Brostin's hardware testing.

**Findings from EasyEDA DB (all 5 questions answered):**

1. **INA219 present:** YES — U10 (INA219AIDR), LCSC C138706, SOIC-8 package.
2. **I2C pull-ups:** YES — R40 (4.7kΩ INA_SDA→VCC), R41 (4.7kΩ INA_SCL→VCC). Both 0402WGF4701TCE.
3. **Power rail:** VS (pin 5) → VCC net = 3.3V from TPS7A0233DBVR (U23). Same rail as MCU.
4. **Address pins:** A0 (pin 2) = GND, A1 (pin 1) = GND → address 0x40 confirmed in schematic wiring.
5. **Shunt resistor:** R58 = PE0402FRF470R05L (50mΩ ±1%, 250mW thin film). Placed between INA_VIN (VIN+) and INA_VOUT (VIN−). Current path: SOLAR_IN → D9 (SS14) → INA_VOUT → R58 → INA_VIN → solar_in connector. Measures solar panel charging current.

**Root cause assessment:** Schematic is correct. If no ACK, likely: (a) U10 not populated, (b) solder defect, or (c) firmware using wrong I2C peripheral (must be I2C2 via AF6 on PB8/PB9, not I2C1).

### 2026-04-23 — VBAT/BAT_SENSE PCB Design Bug Confirmed (Deep Dive Analysis)

**Problem authority escalated to Architect (Cawl) after Yarrick's ADC fix rejected.**

**Findings (all via EasyEDA SQLite3 DB verification):**

1. **MCU VBAT pin (U3 pin 1):** Tied to VCC (3.3V), not battery rail
   - Internal VBAT/3 channel (CH13) reads constant ~1.1V → useless for monitoring

2. **BAT_SENSE routing:** PB4 (MCU pin 40) ← voltage divider (R30/R31 = 100kΩ/100kΩ = 1:1)
   - PB4 has NO ADC on STM32U073CBT6 (digital-only: SPI1_MISO, I2C2_SDA, TIM3_CH1, USART1_CTS, LPUART3_RTS)
   - **PCB design error** — routed to non-ADC pin

3. **SENSE_LDO_EN (PA15):** Controls U24 enable (separate sensor LDO power rail)
   - Not an analog sense path, just GPIO enable signal

4. **All ADC pins occupied:**
   - PA0–PA7: LoRa SPI (NSS/SCK/MISO/MOSI) + RX/TX enables + GPS UART
   - PB0–PB1: LoRa DIO1/DIO2 interrupts
   - **No free ADC pin** for reroute

**Key insight:** Three independent PCB problems compound the issue. No software-only ADC path exists. Recommended: use INA219 bus voltage as battery proxy for current cycle.

**Shared with team:** PCB methodology (SQLite3 DB querying), hardware constraint analysis process, workaround justification for architectural decisions (Cawl).

### 2026-04-23 — PCB note: ADC bug confirmed
- BAT_SENSE on PB4 has no ADC channel on STM32U073CBT6 — firmware workaround in place
- INA219 pull-ups R40/R41 (4.7kΩ) on main VCC confirmed working
- INA219 SOIC-8 D-package: Pin1=A1, Pin2=A0, Pin3=SDA, Pin4=SCL, Pin5=VS, Pin6=GND, Pin7=VIN-, Pin8=VIN+
- A0=A1=GND → address 0x40 confirmed
