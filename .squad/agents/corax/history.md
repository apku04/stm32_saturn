# Corax — History

## Project Context

Project: Saturn LoRa Tracker — STM32U073CBT6 + SX1262 E22-900M22S
Repo: /home/pi/work/stm32/ (GitHub: apku04/stm32_saturn)
Firmware: bare-metal C, no HAL, custom registers in firmware/include/stm32u0.h
PCB: EasyEDA project stm32_lora.eprj (SQLite3 DB)
Flash path: USB DFU only (SWD disconnected)
First task: Fix INA219 on I2C2, add INA219 + battery voltage to beacon payload

## Learnings

- 2026-04-24: Wrote `linux-serial-hygiene` skill — `stty -F /dev/ttyACMn 115200 raw -echo` is mandatory before raw cat/echo on STM32 CDC ports; cooked-mode echo caused real `unknown cmd: T` spam on the receiver. pyserial (lora_monitor.py) is unaffected.
- 2026-04-26: Fixed stale GPS display in lora_monitor.py (Perturabo Concern 5). _update_gps_tab() was only called when fix=1, leaving GPS tab showing last good position indefinitely after fix loss. Now always called for v5 beacons; fix=0 shows greyed-out "[stale]" coords with "✗ NO FIX" indicator and last-fix timestamp. Replaced "alt" column with "LAST FIX" column for per-node fix-time tracking.
