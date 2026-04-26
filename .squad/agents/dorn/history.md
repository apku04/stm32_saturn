# Dorn — History

## Project Context

Project: Saturn LoRa Tracker — STM32U073CBT6 + SX1262 E22-900M22S
Repo: /home/pi/work/stm32/ (GitHub: apku04/stm32_saturn)
Firmware: bare-metal C, no HAL, custom registers in firmware/include/stm32u0.h
PCB: EasyEDA project stm32_lora.eprj (SQLite3 DB)
Flash path: USB DFU only (SWD disconnected)
First task: Fix INA219 on I2C2, add INA219 + battery voltage to beacon payload

## Learnings

- I2C1 (PB8/PB9 AF4) was wrong for INA219; the sensor is on I2C2 (PB13/PB14 AF6)
- I2C2 clock enable is bit 22 of RCC_APBENR1 (I2C1 is bit 21)
- TIMINGR value 0x30420F13 works for both I2C1 and I2C2 at 100kHz/16MHz HSI
- PB13/PB14 were previously LEDs; repurposing for I2C2 disables LED functionality in lora app
- INA219 shunt register 0x01 is signed 16-bit, LSB=10µV; bus register 0x02 bits[15:3] LSB=4mV
- Refactored i2c.c to use base-address macros (IC_CR1(base) etc.) to support both I2C1 and I2C2 without code duplication
- Beacon payload grew from 5 to 7 bytes: shunt_mv(2) + bus_mv(2) + bat_mv(2) + chg(1)
- The lora_monitor.py must try V2 beacon pattern (shunt/bus/bat) before legacy (bat/sol) to avoid misparse

### 2025-07-17 — Lock-out and Correction (Cross-agent Learning)

**Issue:** Commit 1bba9d5 assigned I2C2 to PB13/PB14 (LED pins, not wired to INA219). PCB ground truth is PB8/PB9.

**Root Cause:** Did not verify pin assignments against Kotov's PCB database before reassigning pins. The correct fix was AF4→AF6 (1-line register change), not pin migration (requires PCB verification).

**Eisenhorn's Verdict:** Code quality was sound (I2C2 driver, registers, beacon format all correct). The pin selection was wrong. **Locked out from I2C/pin changes.**

**Key Lesson:** 
1. Always consult Kotov's EasyEDA PCB database before changing pin assignments
2. Prefer AF changes (register-only, no PCB implications) over pin migrations
3. On STM32U073, PB8/PB9 support both AF4→I2C1 and AF6→I2C2 — the one-line AF fix was the correct approach

**Cross-sharing:** This decision shared with Ravenor (INA219 now on I2C2, beacon V2 format) and Eisenhorn's lock-out logic documented for future reference

### 2025-07-22 — Battery ADC Channel Fix

**Issue:** Beacon reported `bat=215 mV` instead of ~4000 mV for a 4V LiPo.

**Root Cause:** ADC was reading channel 22, which does not exist on STM32U073. PB4 maps to ADC1_IN13 (not IN22). Additionally, ADC CKMODE=00 (async) was used without configuring the RCC_CCIPR ADCSEL clock source.

**Fix:**
1. Changed ADC_CHANNEL_BAT from 22 to 13 (empirically verified via GPIO toggle test)
2. Set CFGR2 CKMODE=01 (PCLK/2 synchronous clock) — no RCC_CCIPR dependency
3. Added VBATEN guard (CH13 shared with internal VBAT/3)
4. Added `adc_read_channel_raw()` and `get adcscan` diagnostic command

**Key Lessons:**
1. STM32U073 ADC channel mapping: PA0-7=CH0-7, PB0=CH8, PB1=CH9, PB4=CH13
2. CH11/12/13 are shared with internal TEMP/VREFINT/VBAT — must guard CCR enable bits
3. CKMODE=00 (async) needs RCC_CCIPR ADCSEL; CKMODE=01 (PCLK/2) is self-contained
4. Web search gives contradictory channel numbers for STM32U073 — always verify empirically
5. On USB-only power, VCC_BAT_IN may not carry battery voltage (MPPT path issue)

**Open:** Verify bat_mv on user's battery-powered board. If reading is off by a fixed ratio, the divider may not be 1:1 (1M/1M) as assumed.

### 2026-04-23 — I2C confirmed working on hardware
- i2cscan found INA219 at 0x40 ✅
- get ina: ret=0 for both shunt and bus registers ✅  
- bus voltage 924mV (LiPo discharged) — sensor reads correctly
- AF4 + I2C1_BASE is THE fix — never use AF6+I2C2 on PB8/PB9
- Commit ccd3a1c is the correct production state
- bat_mv = ina219_read_bus_mv() (ADC workaround for PCB bug on PB4)

### 2026-04-23 — STM32 UID-Derived Unique Address Implementation

**CRITICAL — UID Base Address on STM32U073:**
- **UID location: 0x1FFF6E50** (NOT 0x1FFF7590 which is STM32U5)
- This is the factory-programmed 96-bit unique ID
- Incorrect address causes read of wrong memory — must use 0x1FFF6E50

**Implementation:**
- XOR three 32-bit UID words → uint32_t
- Fold 4 bytes into uint8_t via XOR
- Range 1–253 (avoid 0, 254, 255)
- Each chip gets stable, unique node_addr without configuration

**Verification:**
- Board 2: node_addr=33 ✅
- Both boards now distinguish in radio traffic
- Stable across resets (factory-programmed)

## Learnings — 2026-04-24 (solar telemetry bring-up)

- **I²C peripheral inventory:** Only I2C1 is wired. The driver is misleadingly named `i2c2_*`. Two AF4-mappable pin pairs share it as a hardware mutex: PB6/PB7 (H4 external) and PB8/PB9 (U10 onboard INA219). Never enable both — the AF mux is exclusive and a dual-config bricks the bus.
- **U10 power gate:** PA15 (SENSE_LDO_EN) controls the VCC_SENSE LDO that powers U10. Must be driven HIGH before any I²C activity, with ~1ms soft-start. Skipping this makes U10 silent on the bus and looks like a dead chip / bad solder.
- **INA219 R58 = 50 mΩ** (NOT the 100 mΩ that's typical for breakout boards). With the default config (LSB = 10 µV), 1 shunt LSB = 200 µA and the useful range needs µV resolution. `ina219_read_shunt_mv()` truncates to zero for typical charge currents — use `ina219_read_shunt_uv()` for any current calculation. I[mA] = V_shunt[µV] / 50.
- **Charge status GPIOs:** PA10 = CHRG (CN3791), PA8 = STDBY. Configure as inputs with pull-ups during INA init. `charge_get_status()` returns Off/Charging/Done/Fault.
- **No battery ADC on this MCU package:** PB4 has no ADC channel. Battery voltage is currently proxied through INA bus voltage. Future bodge wire candidates: PB1 (ex-DIO2) or PA8 (ex-BAT_STDBY).
- **Flash workflow:** App command `dfu` jumps to bootloader; `dfu-util -a 0 -s 0x08000000:leave -D <bin>` flashes and re-launches. The `Error during download get_status` message after `Submitting leave request...` is benign — it's the `:leave` triggering the app jump before dfu-util finishes its status poll.
- **Linux serial discipline:** Always `stty -F /dev/ttyACMn 115200 raw -echo` before reading/writing. Cooked-mode echo creates a feedback loop that drowns the terminal in `unknown cmd: T` errors.

### 2026-04-24 — Skills extracted

Authored three reusable skills under `.squad/skills/` for future agents:
- `stm32-dfu-flash/SKILL.md` — USB DFU flash workflow, benign `get_status` error, SWD disabled
- `stm32u073-i2c-pin-mux/SKILL.md` — I2C1-only, PB6/7 vs PB8/9 AF4 exclusivity, PA15 sense-LDO gating
- `ina219-driver-patterns/SKILL.md` — R58=50mΩ shunt, µV reads, bus-V as battery proxy

## Learnings — 2026-04-24 (driver & system skill extraction)

- `sx1262-driver/SKILL.md` — SPI command protocol, TCXO-then-calibrate init order, BUSY-before-every-command discipline, IRQ poll model, key reg addrs 0x0740/0x08AC
- `stm32u073-spi1/SKILL.md` — PA4-7 AF5, software NSS, /8 prescaler → 2MHz, mode 0 for SX1262, u8-cast DR to avoid 4-byte FIFO gulps
- `charger-status-gpio/SKILL.md` — CN3791 PA10 CHRG / PA8 STDBY active-low, 4-state truth table, PA15 LDO gating required before read
- `stm32u073-adc/SKILL.md` — PB4 has no ADC, CH13 VREFINT vs VBAT/3 conflict, CKMODE=01 sync (not async), battery proxied via INA219 bus-V
- `stm32u073-timer/SKILL.md` — SysTick polled (no ISR), 1ms RVR=15999, TICKINT=0, COUNTFLAG read-to-clear, 10s beacon cb single slot
- `stm32u073-usb-cdc/SKILL.md` — PA11/12 AF10, HSI48+CRS+`PWR_CR2.USV`, 4-EP CDC, DFU coexist via DPPU clear + FRES+PDWN + SYSCFG MEM_MODE=01
- `flash-config-storage/SKILL.md` — last page 0x0801F800, 2KB page size (U0 not L0), 64-bit doubleword mandatory, erase-before-write, 1 byte per 8-byte slot
- `hal-clock-init/SKILL.md` — HSI16 SYSCLK, no PLL, per-app main.c owns clock_init, nop-spin after RCC enables, `PWR_CR2.USV` for USB
- `packet-buffer/SKILL.md` — fixed 10 slots, polled single-thread no-locks, `search_pending_packet` copy-and-clear as layer-handoff pattern, drops on full
- `terminal-commands/SKILL.md` — argv parsing via in-place NUL, get/set/send/dfu dispatch, `get solar` production format, DO NOT change `[RX]`/`[BEACON]`/`Solar:` prefixes (test suite parses them)

None of the 10 skills were skipped — all modules warranted their own reusable notes. One `[verify]` flag in `flash-config-storage` on the 64 KB vs 128 KB flash size discrepancy between charter and flash_config.c.

### 2026-07-25 — GPS Driver Fix + Beacon Integration

**Six fixes applied in one pass, all verified by clean build:**

1. **SENSE_LDO_EN (PA15) power sequencing:** Added to `gps_init()` — configure PA15 as output push-pull, drive HIGH, then `delay_ms(100)` before USART2 config. GPS module was unpowered in firmware; this was the #1 reason for no NMEA data.

2. **AF4 → AF7 fix (CRITICAL):** PA2/PA3 USART2 alternate function was AF4 (wrong). Changed to AF7 per RM0503 Table 20. AF4 on these pins routes to LPUART2/USART3 — USART2 is electrically dead at AF4. Also switched from magic `2*2`/`3*2` to `GPS_RX_PIN`/`GPS_TX_PIN` hw_pins.h macros.

3. **Non-blocking gps_poll():** Was already non-blocking (drains available bytes and returns), but now reads from ring buffer instead of USART2_RDR directly. Never blocks the main loop. fix.valid field already existed as `fix.valid` (bool).

4. **USART2 RXNE interrupt + 64-byte ring buffer:** Added `USART2_IRQHandler` ISR that captures bytes into a power-of-2 ring buffer. Clears ORE on overrun. NVIC enabled for IRQ 28 (USART2). Vector table in main.c extended from 4 entries to 45 (16 system + 29 IRQs) to reach the USART2 slot. This prevents NMEA corruption during SX1262 blocking radio ops.

5. **GPS wired into beacon payload:** Beacon v5 now sends 18 bytes: existing 9 (i_ma, bus_mv, bat_mv, chg, tx_pwr, sf) + lat_udeg(4) + lon_udeg(4) + fix_valid(1) at pkt.data[9..17]. Total 18/50 bytes used. Sends zeros with fix_valid=0 until GPS acquires fix.

6. **USART2 register defs moved to stm32u0.h:** Removed local `#define` block from gps.c, added canonical definitions in stm32u0.h alongside other peripherals. Includes USART_CR1_RXNEIE for the interrupt enable bit.

**Key lessons:**
- STM32U073 USART2 on PA2/PA3 is AF7, not AF4 — datasheet table is the only trustworthy source
- PA15 (SENSE_LDO_EN) controls the TPS7A0233DBVR LDO that powers the GPS VCC_SENSE rail — must be HIGH before any GPS comms
- Ring buffer size 64 is adequate for 9600 baud (960 bytes/s) with 10ms worst-case main loop latency
- Vector table must be extended to cover any IRQ you enable — a short table leaves the handler pointing at 0x00000000 → HardFault
- Build size grew from ~22KB to ~29KB (added ISR, ring buffer, GPS payload encoding, extended vector table)
