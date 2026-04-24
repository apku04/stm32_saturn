---
name: "hal-clock-init"
description: "STM32U073 clock tree setup for this project — HSI16 SYSCLK, HSI48+CRS for USB, flash wait states, per-app main.c."
domain: "stm32-peripherals"
confidence: "medium"
source: "earned"
---

## Context
There is no central "HAL init" — each app has its own `main.c` (`firmware/app/{lora,blink,test,usb_cdc}/main.c`) that owns clock and GPIO bring-up. The `system/hal.c` module is a thin **layer bridge** (radio ↔ protocol ↔ USB print), not a hardware init layer. Register definitions live in `firmware/include/stm32u0.h`; project constants in `globalInclude.h`.

## Patterns

**Clock choice — simplest that works:**
- **SYSCLK = HSI16 = 16 MHz.** No PLL, no HSE. Set once in `clock_init()` (lora/main.c:46):
  ```c
  RCC_APBENR1 |= (1<<28);      // PWREN
  RCC_CR |= (1<<8);             // HSION
  while (!(RCC_CR & (1<<10)));  // HSIRDY
  RCC_CFGR = (RCC_CFGR & ~7U) | 1U;       // SW = HSI16
  while ((RCC_CFGR & (7U<<3)) != (1U<<3)); // SWS verify
  ```
- HCLK = PCLK1 = PCLK2 = 16 MHz (default AHB/APB prescalers = 1).
- Flash wait states: **0 WS** for HSI16 at Vcore Range 1 (reset default of `FLASH_ACR.LATENCY = 0`; valid up to 24 MHz in Range 1). Driver does not touch `FLASH_ACR`. If ever clocking >24 MHz, `FLASH_ACR.LATENCY` must be raised *before* the clock switch.
- Vcore: reset default (Range 1 / 1.2 V nominal). Not touched.

**USB clock — HSI48 + CRS autotrim:**
- `RCC_CRRCR |= 1` → HSI48 on; wait `CRRCR.HSI48RDY` bit 1.
- `RCC_CCIPR = (RCC_CCIPR & ~(3<<26)) | (3<<26)` → USB clock source = HSI48.
- CRS autotrimmed from USB SOF (`CRS_CFGR` SYNCSRC=2, `CRS_CR |= (1<<5)|(1<<6)`).
- `PWR_CR2.USV (1<<10)` — mandatory on U0 to power USB transceiver.
- See `stm32u073-usb-cdc` skill for the full USB side.

**Peripheral clock enables used (by peripheral):**
| Bus/bit | Peripheral |
|---|---|
| `RCC_IOPENR (1<<0/1/5)`       | GPIOA / GPIOB / GPIOF |
| `RCC_APBENR1 (1<<13)`         | USB |
| `RCC_APBENR1 (1<<16)`         | CRS |
| `RCC_APBENR1 (1<<21)`         | I2C1 |
| `RCC_APBENR1 (1<<28)`         | PWR |
| `RCC_APBENR2 (1<<0)`          | SYSCFG |
| `RCC_APBENR2 (1<<12)`         | SPI1 |
| `RCC_APBENR2 (1<<20)`         | ADC1 |

After any `RCC_*ENR |= …`, the code uses a short `for(;;) __asm__("nop")` spin before touching the peripheral — the IP clock takes a few cycles to propagate on U0; otherwise the first write is silently dropped.

**Vector table:** default at `0x08000000` (flash base). Startup code in linker script `firmware/linker/stm32u073cb.ld` sets up .isr_vector section; no `SCB_VTOR` relocation. DFU jump temporarily remaps via `SYSCFG_CFGR1.MEM_MODE = 01` — see `stm32u073-usb-cdc` skill.

**Per-app shape (lora/main.c, blink/main.c, etc.):**
```c
int main(void) {
    clock_init();     // HSI16 SYSCLK
    led_init();       // GPIOB LEDs
    gpio_init();      // enable GPIOA/B clocks
    timer_init();     // SysTick 1ms
    spi_init();       // SPI1
    usb_cdc_init();   // USB + HSI48 + CRS
    adc_init();
    ina219_init();
    radio_init(...);
    while (1) { timer_poll(); usb_cdc_poll(); radio_irq_handler(); ... }
}
```
Each app reuses the same driver objects from `firmware/src/`; only `main.c` + the local `Makefile` live per-app.

## Examples
- lora/main.c `clock_init()` — canonical HSI16 switch
- usb_cdc.c `usb_cdc_init()` — HSI48+CRS block
- stm32u0.h — all register definitions (RCC, GPIO, SPI, USB, ADC, I2C, FLASH at bases 0x40021000, 0x50000000 family, 0x40013000, 0x40005C00, 0x40012400, 0x40005400, 0x40022000)

## Anti-Patterns
- **Never** switch SYSCLK above 24 MHz without first raising `FLASH_ACR.LATENCY` — the core will run off flash with too-few wait states and fetch garbage.
- Don't enable HSI48 without CRS — USB will enumerate intermittently (HSI48 untrimmed is ~±3%, spec is ±500 ppm).
- Don't write a peripheral register in the instruction immediately after its RCC clock enable — the nop spin is required. (Observed silently on SPI1 and ADC1.)
- Don't assume a central `SystemInit()` exists — each app's `clock_init()` is the source of truth. Changing SYSCLK requires auditing every driver (SPI prescaler, SysTick reload 16000, I2C TIMINGR, ADC SMPR).
- Don't forget `PWR_CR2.USV` — easy to miss; the one thing that distinguishes U0 USB init from older STM32 families.
