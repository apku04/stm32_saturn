---
name: "stm32u073-spi1"
description: "STM32U073 SPI1 bare-metal driver — pins, AF, prescaler, CPOL/CPHA for SX1262, software NSS."
domain: "stm32-peripherals"
confidence: "high"
source: "earned"
---

## Context
SPI1 drives the SX1262 radio exclusively on this board. Driver: `firmware/src/driver/spi.c`. Used by `sx1262.c` via `spi_transfer()` and `spi_exchange_buffer()`.

## Patterns

**Pin map (hw_pins.h):**
| Signal | Pin | AF | Role |
|---|---|---|---|
| SCK  | PA5 | AF5 | clock |
| MISO | PA6 | AF5 | data in |
| MOSI | PA7 | AF5 | data out |
| NSS  | PA4 | **GPIO out (software)** | CS driven manually, NOT hardware NSS |

Software NSS is mandatory: SPI1_CR1 sets `SSM=1, SSI=1`. The SX1262 driver raises/lowers PA4 around each command via `CSN_LOW/HIGH` macros (BSRR writes).

**Clock & speed:**
- RCC: `RCC_APBENR2 |= (1<<12)` enables SPI1 (APB2 bit 12).
- SYSCLK = APB2 = 16 MHz (HSI16), prescaler `BR_DIV8` → **2 MHz SPI clock**. Well below SX1262's 16 MHz limit; chosen conservatively for signal integrity on short flex.
- Bump to `/4` (4 MHz) is safe if you need throughput; do not exceed `/2` without scope-verifying SCK.

**Mode for SX1262:** CPOL=0, CPHA=0 (mode 0), MSB first, 8-bit data. Both bits are zero, so `SPI1_CR1 = SSM|SSI|MSTR|BR_DIV8` is the full config (no CPOL/CPHA macros set).

**CR2:** `FRXTH | DS_8BIT` — FIFO threshold = 1 byte (RXNE fires on 1 byte), data size 8-bit. Critical: without `DS_8BIT` the U0 defaults to 4-bit, which reads garbage.

**GPIO init ordering (spi.c:17+):**
1. `RCC_IOPENR |= 1` (GPIOA clock) + nops
2. PA5/6/7 MODER = AF (0b10), AFRL = 5 each
3. OSPEEDR = 0b11 high-speed (required at 2+ MHz to meet rise time)
4. PA4 MODER = output (0b01), BSRR set HIGH (deselect)
5. `RCC_APBENR2 |= (1<<12)` + nops
6. Write CR1/CR2 **then** `CR1 |= SPE` (enable last)

**Blocking I/O (spi.c:60+):** `spi_transfer()` polls `SR.TXE` then writes a byte via 8-bit cast `*(volatile uint8_t *)&SPI_DR` (the u8 cast prevents the FIFO from swallowing 4 bytes at once on U0), then polls `SR.RXNE` and reads the same way. `spi_exchange_buffer(tx,len,rx)` is just a loop — `rx` may be NULL.

## Examples
- spi.c:17 `spi_init()` — full init sequence
- spi.c:60 `spi_transfer()` — 8-bit DR cast pattern
- sx1262.c `write_command()` — typical producer: `wait_busy → CSN_LOW → spi_exchange_buffer → CSN_HIGH`

## Anti-Patterns
- **Never** use hardware NSS on this board — PA4 isn't wired to the SPI NSS AF input and the radio needs CS held low across multi-byte commands anyway.
- Don't write `SPI1_DR` as 32-bit — on U0 that pushes 4 bytes into the FIFO. Always the `*(volatile uint8_t *)` cast.
- Don't enable SPI (`SPE=1`) before CR1/CR2 are configured — you'll get one phantom clock edge.
- Don't leave OSPEEDR at default (low) — at 2 MHz you'll see rounded edges and occasional bit slips on long wires.
- Don't forget to deselect (CSN HIGH) after every transaction — SX1262 latches commands on the rising edge.
