# Squad Team

> Saturn LoRa Tracker — STM32U073CBT6 + SX1262 (E22-900M22S)

## Coordinator

| Name | Role | Notes |
|------|------|-------|
| Squad | Coordinator | Routes work, enforces handoffs and reviewer gates. |

## Members

| Name | Role | Charter | Status |
|------|------|---------|--------|
| Guilliman | Architect | .squad/agents/guilliman/charter.md | active |
| Ferrus | PCB Expert | .squad/agents/ferrus/charter.md | active |
| Corax | RPi GPIO Expert | .squad/agents/corax/charter.md | active |
| Dorn | STM32 Expert | .squad/agents/dorn/charter.md | active |
| Khan | LoRa Expert | .squad/agents/khan/charter.md | active |
| Lion | Reviewer | .squad/agents/lion/charter.md | active |
| Russ | Tester | .squad/agents/russ/charter.md | active |
| Scribe | Scribe | .squad/agents/scribe/charter.md | active |
| Ralph | Work Monitor | — | active |

## Project Context

- **Project:** Saturn LoRa Tracker
- **MCU:** STM32U073CBT6, LQFP-48, Cortex-M0+, 64KB Flash, 40KB RAM
- **PCB tool:** EasyEDA — project file `stm32_lora.eprj` (SQLite3 database)
- **LoRa radio:** SX1262 via E22-900M22S module, SPI1, 868 MHz default
- **Debug probe:** Raspberry Pi 5 GPIO bitbang SWD via OpenOCD
- **Firmware:** Bare-metal C, no STM HAL, custom register defs in `firmware/include/stm32u0.h`
- **GitHub repo:** apku04/stm32_saturn
- **Created:** 2026-04-23
- **Universe:** Warhammer 40,000

## Issue Source

_Not yet configured. Use "pull issues from owner/repo" to connect._
