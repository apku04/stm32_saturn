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

_No sessions yet._
