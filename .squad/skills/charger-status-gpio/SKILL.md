---
name: "charger-status-gpio"
description: "CN3791 solar charger CHRG/STDBY status GPIOs — 4-state truth table, active-low, pull-up init."
domain: "power-management"
confidence: "high"
source: "earned"
---

## Context
The Saturn tracker charges from a solar panel through a **CN3791** (MPPT-style) Li-ion charger. Two open-drain status pins report charger state to the MCU. Driver lives in `firmware/src/driver/ina219.c` (`charge_get_status()` / `charge_status_str()`) because it initialises alongside the INA219 that shares the sense rail.

## Patterns

**Pin map:**
| Signal | Pin | Logic | Meaning |
|---|---|---|---|
| CHRG  | PA10 | **active-LOW** (low = charging in progress) |
| STDBY | PA8  | **active-LOW** (low = charge complete / top-off) |
| SENSE_LDO_EN | PA15 | active-HIGH output — gates VCC_SENSE LDO that also powers the CHRG/STDBY pull-up rail AND U10 INA219 |

**GPIO init (ina219.c `ina219_init()`):**
- Enable GPIOA clock (`RCC_IOPENR |= 1`)
- PA15 → output push-pull, drive **HIGH first**, then delay ~1 ms for LDO soft-start (busy-loop 200000 nops). Without this, CHRG/STDBY external pull-ups fight each other (4k7 to VCC_SENSE vs 4k7 to VCC) and sit at ~VCC/2 — reads as intermediate / random.
- PA10 and PA8 → input, **internal pull-up enabled** (`PUPDR = 0b01`). The external pull-up is on VCC_SENSE; the internal PU is a belt-and-braces guarantee when VCC_SENSE has dipped.

**4-state truth table** (`charge_get_status()` at ina219.c:113):
| CHRG (PA10) | STDBY (PA8) | Status | Enum |
|---|---|---|---|
| 1 | 1 | No input power / no battery | `CHARGE_OFF` |
| 0 | 1 | Charging in progress         | `CHARGE_CHARGING` |
| 1 | 0 | Charge complete / top-off    | `CHARGE_DONE` |
| 0 | 0 | Fault (both asserted)        | `CHARGE_FAULT` |

**String mapping** (`charge_status_str()`): "Off" / "Charging" / "Done" / "Fault". The beacon payload encodes the enum as 1 byte (data[6]).

**Reading:** single IDR read, bit-shift both pins, branch on the 4 combos. No debounce needed — the charger IC is already filtered.

## Examples
- ina219.c `ina219_init()` — GPIO init with PA15 LDO gating
- ina219.c `charge_get_status()` — IDR read + truth table
- terminal.c `get charge` / `get solar` — exposes state to user
- lora/main.c `beaconHandler()` — ships enum as beacon `data[6]`

## Anti-Patterns
- **Never** configure PA10/PA8 as outputs — will drive against the charger's open-drain status pins.
- **Never** skip PA15 setup before reading CHRG/STDBY. VCC_SENSE off → pull-ups float → false readings that look like intermittent hardware.
- Don't invert the polarity in code — CN3791 is LOW=active. Reading `if (chrg)` as "charging" is wrong.
- Don't bodge BAT_SENSE onto PA10/PA8; they're charger-status inputs. If a future revision needs a real ADC battery sense, candidates are PB1 (ex-DIO2) or remap through the INA bus voltage (current workaround — see `stm32u073-adc` skill).
- Don't treat `CHARGE_FAULT` as fatal — a brief both-low glitch can occur during source swap (USB↔solar). Require 2+ consecutive reads before acting.
