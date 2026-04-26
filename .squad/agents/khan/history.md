# Khan — History

## Project Context

Project: Saturn LoRa Tracker — STM32U073CBT6 + SX1262 E22-900M22S
Repo: /home/pi/work/stm32/ (GitHub: apku04/stm32_saturn)
Firmware: bare-metal C, no HAL, custom registers in firmware/include/stm32u0.h
PCB: EasyEDA project stm32_lora.eprj (SQLite3 DB)
Flash path: USB DFU only (SWD disconnected)
First task: Fix INA219 on I2C2, add INA219 + battery voltage to beacon payload

## Learnings


2026-04-24: See decisions.md entry "Saturn board hardware & workflow facts" — covers I²C mux exclusivity, U10 INA219 init/scaling, DFU flash workflow, beacon v3 payload.
2026-04-24: Extracted skill `.squad/skills/lora-beacon-payload-v3/SKILL.md` — beacon v3 wire format, source-side mA conversion, charge-state mapping, and RX regex patterns in `tools/lora_monitor.py`.
2026-04-24: Designed v5 beacon payload adding GPS lat/lon/fix (9 new bytes, 18 total). GPS driver already stores lat_udeg/lon_udeg as int32 micro-degrees — zero conversion needed, just pack into pkt.data[9..17]. 50-byte limit leaves 32 bytes free. Backward-compatible: old receivers fall through to v4 decode branch harmlessly. Decision written to `.squad/decisions/inbox/khan-gps-payload-design.md`.
