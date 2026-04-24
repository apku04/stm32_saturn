---
name: "stm32u073-i2c-pin-mux"
description: "I2C1 pin-mux, AF exclusivity, and sense-LDO gating on the Saturn STM32U073 board"
domain: "stm32-peripherals"
confidence: "high"
source: "earned"
---

## Context
On the Saturn LoRa Tracker, **only I2C1 is wired**. The driver file `firmware/src/driver/i2c.c` is legacy-named `i2c2_*` for historical reasons (an earlier bring-up mis-routed to I2C2) — the names lie, the hardware uses I2C1. Two different pin pairs are physically connected to I2C-capable signals, and they share the same controller through AF mux. Getting this wrong is the #1 cause of "dead sensor" symptoms on this board.

## Patterns

### Pin inventory (ground truth: `firmware/src/board/hw_pins.h`)
| Pins        | AF | Peripheral | Net               |
|-------------|----|------------|-------------------|
| PB6 / PB7   | 4  | I2C1       | H4 external header (SCL/SDA) |
| PB8 / PB9   | 4  | I2C1       | U10 onboard INA219 (SCL/SDA) |

Both pairs map to **I2C1** via AF4 on STM32U073. The AF mux is **exclusive** — you can only activate one pair at a time. Configuring both simultaneously ties two SCL and two SDA drivers to one controller and bricks the bus.

### Clock + AF setup
- I2C1 clock enable: **`RCC_APBENR1` bit 21**. (Bit 22 is I2C2 — do not confuse.)
- `TIMINGR = 0x30420F13` at 100 kHz with 16 MHz HSI SYSCLK (works for either pair).
- Set GPIO MODER = AF (0b10), OTYPER = open-drain, PUPDR = pull-up, AFR = 4.

### PA15 SENSE_LDO_EN gating (critical for U10)
U10 (INA219) is powered through a sense LDO gated by **PA15 = SENSE_LDO_EN**. Required sequence before any I²C traffic to U10:
1. Configure PA15 as push-pull output.
2. Drive PA15 HIGH.
3. Wait **~1 ms** for the LDO soft-start.
4. Then issue the first I²C transaction.

Skipping the delay makes the INA219 NACK every address — looks identical to a dead chip or a bad solder joint.

### No PB4 ADC on STM32U073
STM32U073 in LQFP-48 has **no ADC channel on PB4**. An earlier assumption that PB4 = ADC1_IN22 was wrong (that's an STM32U5 mapping). Battery voltage is currently read via **INA219 bus voltage** as a proxy — see `ina219-driver-patterns` skill. Valid ADC channels that are present: PA0–PA7 (CH0–7), PB0 (CH8), PB1 (CH9), with CH11/12/13 shared with internal TEMP/VREFINT/VBAT (guarded by CCR bits).

## Examples
- `firmware/src/driver/i2c.c` — I2C1 init using AF4 on PB8/PB9 (production path, commit ccd3a1c).
- `firmware/src/board/hw_pins.h` — authoritative pin map; always consult before touching any GPIO.
- Historical lesson (Kotov → Eisenhorn → Cawl chain, now Ferrus/Lion/Guilliman): commit 1bba9d5 wrongly migrated to PB13/PB14 AF6 I2C2. The correct fix was a **one-line AF change, no pin migration** — prefer AF edits over pin moves whenever possible because pin moves require PCB verification.

## Anti-Patterns
- ❌ Enabling both PB6/PB7 and PB8/PB9 as AF4 simultaneously — AF mux conflict, bus dies.
- ❌ Using AF6 on PB8/PB9 to reach "I2C2" — the INA219 net is on I2C1; AF6 misroutes it.
- ❌ Setting `RCC_APBENR1` bit 22 (I2C2) when the driver is actually I2C1. Bit 21 only.
- ❌ Talking to U10 before raising PA15 + 1 ms settle — every transaction NACKs.
- ❌ Reassigning pins without checking `hw_pins.h` and the EasyEDA schematic (`stm32_lora.eprj`).
- ❌ Renaming `i2c2_*` symbols mid-flight — legacy name is tolerated; semantic correctness (AF4 on I2C1) is what matters.
