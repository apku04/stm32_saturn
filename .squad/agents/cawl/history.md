# Cawl — History

## Project Context

Project: Saturn LoRa Tracker — STM32U073CBT6 + SX1262 E22-900M22S
Repo: /home/pi/work/stm32/ (GitHub: apku04/stm32_saturn)
Firmware: bare-metal C, no HAL, custom registers in firmware/include/stm32u0.h
PCB: EasyEDA project stm32_lora.eprj (SQLite3 DB)
Flash path: USB DFU only (SWD disconnected)
First task: Fix INA219 on I2C2, add INA219 + battery voltage to beacon payload

## Learnings

### 2025-07-18 — I2C2 pin correction (Cawl override)
- Yarrick mapped I2C2 to PB13/PB14 (LED pins). PCB truth: INA219 on PB8/PB9.
- STM32U073 PB8/PB9 support AF6→I2C2 (not AF4→I2C1 as originally commented).
- AFRH bits [7:0] = 0x66 encodes AF6 for both PB8 (bits [3:0]) and PB9 (bits [7:4]).
- Always cross-check pin assignments against Kotov's PCB database before committing.
- LED pins (PB13/PB14) must never be reassigned without Architect approval.

### 2026-04-23 — Session Outcome (Cross-agent Learning)

**Status:** I2C2 + INA219 + Beacon V2 + LED restoration **COMPLETE** (commit 910cb9c, clean build)

**Key technical decisions locked:**
- **INA219 pins:** PB8/9 (AF6) — routed to I2C2 peripheral (verified against PCB)
- **LED pins:** PB13/14 (GPIO output) — restored to full functionality
- **Beacon format:** V2 (7 bytes: shunt/bus/bat/chg) — backward compatible with legacy parser
- **I2C topology:** I2C1 on PB6/7 (external), I2C2 on PB8/9 (INA219) — fully separated

**Agent performance:**
- Kotov: Expert PCB verification (EasyEDA SQLite DB decoding)
- Yarrick: Sound I2C driver code, but critical PCB validation gap
- Eisenhorn: Prevented 3rd wrong migration via independent verification
- Cawl: Surgical architect-level fix (minimal scope, maximum correctness)
- Brostin: Comprehensive testing strategy (single + target-to-target)

**Shared with team:** Kotov's PCB topology findings, Yarrick's lock-out reasoning, Cawl's fix principles

