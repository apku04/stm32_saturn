---
name: "sx1262-driver"
description: "SX1262 LoRa radio driver on STM32U073 — SPI command protocol, init sequence, BUSY discipline, IRQ handling."
domain: "lora-radio"
confidence: "medium"
source: "earned"
---

## Context
Saturn tracker uses an E22-900M22S module (SX1262 + TCXO + PA/LNA + antenna switch). Driver is `firmware/src/driver/sx1262.c`, opcodes/constants in `sx1262_register.h`, HAL surface in `radio.h`. All comms via SPI1 (see `stm32u073-spi1` skill).

## Patterns

**Pin map (hw_pins.h + sx1262.c init):**
| Signal | Pin | Direction | Notes |
|---|---|---|---|
| NSS  | PA4  | GPIO out | software chip-select, driven in driver |
| SCK/MISO/MOSI | PA5/6/7 | AF5 SPI1 | — |
| BUSY | PB2  | input    | must be LOW before every command |
| DIO1 | PB0  | input    | main IRQ line (polled) |
| DIO2 | PB1  | input    | unused |
| NRST | PB3  | GPIO out | hardware reset |
| RXEN | PA0  | GPIO out | antenna switch — RX path |
| TXEN | PA1  | GPIO out | antenna switch — TX path |

Never set RXEN and TXEN simultaneously. `set_standby()` clears both; `set_rx/set_tx` pick one.

**BUSY discipline (non-negotiable):** Every command path calls `wait_busy()` BEFORE asserting CSN. `wait_busy()` polls PB2 with a nested-loop timeout (~25 × 10000); on timeout it latches `radio_failed=1` and subsequent calls no-op. Skipping this will lock the chip.

**Reset → init sequence** (see `radio_init()`, sx1262.c L260+):
1. GPIO config (PA0/1 out, PB0/1/2 in, PB3 out)
2. `reset_radio()`: NRST HIGH (10 ms) → LOW (20 ms) → HIGH (10 ms), then `wait_busy()`
3. `set_standby(STANDBY_RC)` — 0x00
4. `SET_DIO3_AS_TCXO_CTRL` with `TCXO_CTRL_1_7V` (0x01), timeout `0x000140` (≈5 ms units of 15.625 µs) — **TCXO must be powered BEFORE calibration**
5. `CALIBRATE 0x7F` (all blocks), wait 10 ms
6. `set_standby(STANDBY_XOSC)` — 0x01 (now runs from TCXO)
7. `SET_REGULATOR_MODE DC_DC` (0x01)
8. `SET_PKT_TYPE LORA` (0x01)
9. Load freq from flash, `CALIBRATE_IMAGE` band params (`0xD7,0xDB` >860 MHz else `0x6B,0x6F`), then `SET_RF_FREQUENCY`
10. `SET_PA_CONFIG 0x04,0x07,0x00,0x01` (+22 dBm high-power path)
11. `SET_TX_PARAMS power, RAMP_200_US`
12. `SET_MODULATION_PARAMS SF, BW_125, CR_4_5, ldro` (ldro=1 for SF≥11)
13. `SET_PKT_PARAMS preamble=8, header=explicit, maxlen=64, CRC on, IQ standard`
14. `SET_BUFFER_BASE_ADDRESS tx=0x00, rx=0x80`
15. Write sync word MSB=0x14 / LSB=0x24 to regs 0x0740/0x0741 (private network)
16. Boost RX gain: write 0x96 to 0x08AC

**Key register addresses (sx1262_register.h):**
- `REG_LORA_SYNC_WORD_MSB`  0x0740
- `REG_LORA_SYNC_WORD_LSB`  0x0741
- `REG_RX_GAIN`             0x08AC  (0x96 = RX boosted)
- `REG_OCP_CONFIGURATION`   0x08E7
- `REG_XTA_TRIM / XTB_TRIM` 0x0911 / 0x0912

**TX/RX dispatch:**
- `radio_start_rx()` → standby XOSC, mask RX_DONE+CRC_ERR on DIO1, `set_rx(0xFFFFFF)` (continuous)
- `radio_send(payload,len)` → `write_buffer(0,…)` → re-apply packet_params with actual length → `set_tx(0x09C400)` (10 s timeout) → poll `tx_done_irq` or `GET_IRQ_STATUS` with 10000 × 1 ms
- `radio_irq_handler()` is POLLED from main loop (no NVIC IRQ wired); it reads `GET_IRQ_STATUS`, dispatches `receive_packet()` on RX_DONE (ignoring CRC_ERR), clears `IRQ_ALL` at the end

**IRQ mask values (16-bit, sx1262_register.h L60+):** TX_DONE=0x0001, RX_DONE=0x0002, CRC_ERR=0x0040, RX_TX_TIMEOUT=0x0200, ALL=0x03FF.

**Freq register math:** `freq_reg = freq_hz * 2^25 / F_xtal` with F_xtal = 32 MHz (so `freq_reg = mhz * 2^20 + round(rem * 2^20 / 1e6)`), implemented as `mhz*1048576 + (rem*1048576 + 500000)/1000000` to avoid 64-bit divides. See `set_rf_frequency()` in sx1262.c.

## Examples
- `radio_init()` at sx1262.c:256 — canonical boot sequence
- `write_command()` at L94 — wait_busy → CSN_LOW → SPI → CSN_HIGH
- `receive_packet()` at L395 — unpacks 15-byte header into `Packet` struct

## Anti-Patterns
- **Never** issue SPI without `wait_busy()` first — will silently corrupt state and eventually latch `radio_failed`.
- Don't enable both RXEN and TXEN; the E22 antenna switch will short the PA.
- Don't skip the STANDBY_RC → TCXO_CTRL → STANDBY_XOSC order. TCXO must be configured while still on RC oscillator.
- Don't assume NVIC IRQ on DIO1 — this project polls. If you wire the EXTI, remember `radio_irq_handler()` already does the IRQ-status read/clear.
- Don't change sync word to 0x3444 (public LoRaWAN) — nodes will stop hearing each other.
