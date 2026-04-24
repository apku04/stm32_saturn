---
name: "ina219-driver-patterns"
description: "INA219 usage on the Saturn board: 50 mΩ shunt, µV-resolution reads, charge-status GPIOs"
domain: "sensor-drivers"
confidence: "high"
source: "earned"
---

## Context
U10 on the Saturn LoRa Tracker is an INA219 current/voltage monitor at I²C address **0x40**. It measures battery-side shunt voltage and bus voltage, and is the sole source of both current AND battery-voltage telemetry (there is no battery ADC on this MCU — see `stm32u073-i2c-pin-mux` skill). The board shunt is non-standard, so generic breakout-board formulas give the wrong current by 2×.

## Patterns

### Board-specific shunt: R58 = 50 mΩ ±1%, 250 mW
- Confirmed from the BOM (薄膜电阻 50mΩ ±1% 250mW). **Not** the 100 mΩ shunt used on typical INA219 breakouts.
- Shunt-register LSB = **10 µV** (INA219 fixed).
- Current formula on this board:
  ```
  I[mA] = V_shunt[µV] / R_shunt[mΩ]   →   I[mA] = V_shunt[µV] / 50
  ```
- Parameterize the shunt value in any calculation; do **not** hard-code /50 without a named constant — future boards may respin the shunt.

### Register map used here
| Reg  | Name       | Notes                                              |
|------|------------|----------------------------------------------------|
| 0x00 | Config     | Write **0x399F** (default: 32V, ±320 mV, 12-bit, continuous) |
| 0x01 | Shunt V    | Signed 16-bit, LSB = 10 µV                         |
| 0x02 | Bus V      | Bits[15:3], LSB = 4 mV; bit1 = CNVR, bit0 = OVF   |

### Read helpers (`firmware/src/driver/ina219.h`)
- `int32_t ina219_read_shunt_uv(void)` — **preferred**. Returns signed µV, full sensor resolution.
- `int16_t ina219_read_shunt_mv(void)` — legacy, coarse. Truncates to 0 for any realistic charge current on a 50 mΩ shunt (1 mA → 50 µV → 0 mV). **Avoid for current math.**
- `uint16_t ina219_read_bus_mv(void)` — bus voltage in mV. Also used as **battery-voltage proxy** on this board (no PB4 ADC available).

### Power-up sequence
1. Drive **PA15 (SENSE_LDO_EN) HIGH**, wait ~1 ms (see `stm32u073-i2c-pin-mux`).
2. I²C write 0x00 ← 0x399F to configure.
3. Read 0x01 (shunt µV) and 0x02 (bus mV) on each sample tick.

### Charge-status GPIOs (CN3791 charger)
- **PA10 = CHRG**, **PA8 = STDBY** — configure as inputs with pull-ups during `ina219_init()`.
- `charge_get_status()` returns: `CHARGE_OFF` / `CHARGE_CHARGING` / `CHARGE_DONE` / `CHARGE_FAULT` (both low).

## Examples
- `firmware/src/driver/ina219.c` / `.h` — production implementation.
- Beacon V2 payload (7 bytes): `shunt_mv(2) + bus_mv(2) + bat_mv(2) + chg(1)`. `bat_mv` is sourced from `ina219_read_bus_mv()` on this board.
- Live verification: `i2cscan` finds 0x40; `get ina` returns `ret=0` for both registers; bus voltage matched a discharged LiPo (~924 mV) during bring-up.

## Anti-Patterns
- ❌ Returning shunt voltage as **mV** and then computing current — kills low-current resolution (anything < 20 mA reads as 0). Always read µV.
- ❌ Hard-coding `I = V_shunt_mV / 100` (breakout-board formula) — this board is 50 mΩ, not 100 mΩ; you'd be off by 2×.
- ❌ Talking to 0x40 before raising PA15 — see sibling skill; every transaction NACKs.
- ❌ Trusting PB4 as a battery ADC — it has no ADC channel on STM32U073 LQFP-48. Use INA219 bus voltage.
- ❌ Using a non-default config value without re-deriving LSBs — 0x399F assumes ±320 mV PGA; other PGA settings change the shunt range but **not** the 10 µV LSB.
