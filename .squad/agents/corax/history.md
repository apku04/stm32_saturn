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
