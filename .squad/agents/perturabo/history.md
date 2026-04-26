# Perturabo — History

## Project Context

Project: Saturn LoRa Tracker — STM32U073CBT6 + SX1262 E22-900M22S
Repo: /home/pi/work/stm32/ (GitHub: apku04/stm32_saturn)
Firmware: bare-metal C, no HAL, custom registers in firmware/include/stm32u0.h
PCB: EasyEDA project stm32_lora.eprj (SQLite3 DB)
Flash path: USB DFU only (SWD disconnected on production board)
User: Copilot
Team universe: Warhammer 40,000

## Known Risk Areas (Day 1)

- Crystal startup / HSE ready polling — no timeout = cold boot brick risk
- SX1262 BUSY line handling — must be checked before every SPI transaction
- LoRa 868 MHz duty cycle — 1% legal limit, easy to violate with naive retries
- DFU lock-out — any firmware bug that prevents USB enumeration has no field recovery
- INA219 I2C2 — PB8/PB9 AF6 only; prior incident (Yarrick) confirmed AF4 mismatch
- Payload budget — 50-byte LoRaWAN limit, currently partially consumed
- IRQ stack depth — bare-metal, no OS, no guard pages

## Learnings

- **2026-04-26T11:48Z — GPS integration critique**
  - `firmware/src/driver/gps.c` currently configures **PA2/PA3 as AF4** for USART2 on STM32U0; the proposed AF7 mapping is a high-risk mismatch that can kill GPS bring-up outright.
  - The LoRa app beacons every **10 s**, but `transmitFrame()` sends **62 on-air bytes**, so duty-cycle margin at 868 MHz is already poor before any GPS fields are added.
  - The firmware is still heavily **blocking/polled** (`delay_ms()` in radio + MAC paths), so a polled 9600-baud GPS receiver will overrun unless RX is buffered outside the main loop.
  - `Reset_Handler()` initializes **gps before USB CDC**; any blocking GPS startup or fix wait threatens the only field DFU entry path (`dfu` over USB CDC).
  - PA15 / `SENSE_LDO_EN` is already a shared power-control signal, so using it for GPS power introduces rail-coupling and TTFF reset risks that must be measured, not assumed away.
