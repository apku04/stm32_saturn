---
name: "stm32u073-adc"
description: "STM32U073 ADC1 bare-metal — usable channels, PB4-has-no-ADC, VREFINT/VDDA, CKMODE gotcha, INA219 battery proxy."
domain: "stm32-peripherals"
confidence: "medium"
source: "earned"
---

## Context
Driver: `firmware/src/driver/adc.c`. The board was designed with battery sense on PB4 — which **has no ADC channel on STM32U073CBT6**. Current architecture uses INA219 bus voltage as the battery proxy; ADC is kept alive mainly for VREFINT/VDDA health checks.

## Patterns

**Channel → pin mapping on STM32U073 (empirically verified — datasheet tables are contradictory):**
| CH | Source |
|---|---|
| 0–7 | PA0–PA7 |
| 8  | PB0 |
| 9  | PB1 |
| 12 | TEMP sensor (internal, needs `CCR.TSEN`) — `ADC_CH_TEMP` in adc.c |
| 13 | VREFINT (internal, needs `CCR.VREFEN`) — `ADC_CH_VREFINT` in adc.c |
| 14 | VBAT/3 (internal, needs `CCR.VBATEN`) — not used here |
| **PB4** | **no ADC** — dead for analog sense |

**Peripheral clocks / registers (adc.c):**
- `RCC_APBENR2 |= (1<<20)` — ADC1EN bit 20
- `ADC1_CCR`:  VREFEN (1<<22), TSEN (1<<23), VBATEN (1<<24 — enables VBAT on CH14; driver leaves it off)
- `ADC1_CFGR2` CKMODE bits [31:30] (per adc.c comment header):
  - `00` async — **requires** RCC_CCIPR ADCSEL config; easy to forget → ADC silently dead
  - `01` HCLK/2 sync
  - `10` HCLK/4 sync — used here, self-contained
  - `11` HCLK/1 sync
- Factory cal: `VREFINT_CAL @ 0x1FFF6E68` (u16, VDDA=3000 mV at T≈30 °C)

**Init sequence (`adc_init()` adc.c:50+):**
1. GPIOA clock + PA15 SENSE_LDO_EN output HIGH (shared with INA rail)
2. `RCC_APBENR2 |= ADC1EN` + nops
3. `CCR = VREFEN | TSEN` (enable internal channels; **no VBATEN** to avoid CH13 contention)
4. `CR = ADVREGEN`, wait ~20 µs (nop loop ×2000)
5. Calibrate: ADEN must be 0 → `CR |= ADCAL` → poll ADCAL
6. `CFGR1 = 0` (12-bit, single, software trigger), `CFGR2 = CKMODE_HCLK_DIV4`
7. `SMPR = 0x7` (≈160.5 cycles — required for high-Z internal channels)
8. Clear `ISR.ADRDY`, set `CR |= ADEN`, poll ADRDY

**Single read (`adc_read_channel_raw()` adc.c:100+):**
- `CHSELR = 1 << ch` (bit-mask, not register-index)
- For internal channels: short nop delay to let mux settle
- Clear `ISR.EOC`, `CR |= ADSTART`, poll EOC (timeout ~200000)
- Return `ADC1_DR & 0xFFFF`

**VDDA calc:** `VDDA = 3000 mV × VREFINT_CAL / VREFINT_raw`. Implemented as `adc_vdda_from_raw()`.

**Battery architecture (the workaround):**
`adc_read_battery_raw()` and `adc_read_battery_mv()` return 0 by design. Real battery voltage = `ina219_read_bus_mv()` (see `ina219-driver-patterns` skill). Beacon payload and `get battery` report via INA path; `get battery` still uses ADC call for diagnostic historical reasons.

## Examples
- adc.c `adc_init()` — full init, synchronous clock path
- adc.c `adc_read_vdda_mv()` — VREFINT self-check
- terminal.c `get vrefint` — 4-sample average + VDDA report
- terminal.c `get adcscan` — sweep CH0..19 for bring-up

## Anti-Patterns
- **Never** use CKMODE=00 (async) without configuring `RCC_CCIPR.ADCSEL`. The ADC will read 0 with no error flag.
- **Never** assume PB4 is an ADC pin — learned the hard way (bat_mv read 215 mV for a 4 V LiPo because channel 22 doesn't exist).
- Don't enable VBATEN unnecessarily — VBAT on CH14 pulls ~VDDA/3 across the internal divider; harmless for CH13 reads but wastes current.
- Don't skip `SMPR` long sampling — VREFINT/TEMP have high source impedance; with short sampling the conversion is noisy/wrong.
- Don't forget calibration must run with ADEN=0; otherwise it silently no-ops and conversions are ~5% off.
- Don't web-search "STM32U073 ADC channel map" — answers contradict each other. Trust the adc.c table (empirically verified via GPIO toggle + `get adcscan`).
