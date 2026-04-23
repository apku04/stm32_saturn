# Brostin — History

## Project Context

Project: Saturn LoRa Tracker — STM32U073CBT6 + SX1262 E22-900M22S
Repo: /home/pi/work/stm32/ (GitHub: apku04/stm32_saturn)
Firmware: bare-metal C, no HAL, custom registers in firmware/include/stm32u0.h
PCB: EasyEDA project stm32_lora.eprj (SQLite3 DB)
Flash path: USB DFU only (SWD disconnected)
First task: Fix INA219 on I2C2, add INA219 + battery voltage to beacon payload

## Learnings

- **I2C1 vs I2C2 mismatch:** `i2c.c` aliases all registers to I2C1 (0x40005400) but the terminal `get i2cscan` message says "Scanning I2C2". The RCC enable bit differs (I2C1=bit21, I2C2=bit22 of APBENR1). This must be reconciled before INA219 will ACK on the correct bus.
- **No shunt/current register access:** Only `ina219_read_bus_mv()` (reg 0x02) is exposed. No terminal command to dump raw INA219 registers. Recommend adding `get ina219` for debug.
- **OVF bit silently discarded:** `ina219_read_bus_mv()` does `>> 3` which strips OVF (bit 0) and CNVR (bit 1). Safe for bus voltage reads but needs attention if current measurement is added.
- **No I2C bus recovery:** If SDA/SCL get stuck, there's no 9-clock bit-bang recovery in `i2c_init()`. Only fix is power cycle.
- **ADC formula verified:** `raw * 6600 / 4096` is correct for 1:1 divider with 3.3V VDDA. Max raw=4095 → 6598 mV, no overflow risk in uint32 math.
- **Beacon payload:** 5 bytes at `pkt.data[0..4]` — bat_mv(LE), sol_mv(LE), charge_status. `pkt.length = 17` (12 header + 5 data). Monitor regex `BEACON_PATTERN` matches the full format.
- **Graceful INA219 failure:** `i2c_read_reg` returns -2 (NACK) or -1 (timeout) bounded by 100k iterations (~6ms). `ina219_read_bus_mv()` returns 0 on error. No crash path.
- **Test app (`app/test/main.c`) is LED-blink only** — no USB, no I2C, no useful diagnostics. All testing must go through the lora app terminal.

### 2026-04-23 — I2C2 Testing & Integration (Cross-agent Learning)

**Baseline validation completed:**
- I2C2 hardware + firmware alignment verified (Kotov PCB + Cawl fix)
- Beacon V2 payload format validated (7 bytes, backward compatible)
- LED restoration confirmed (PB13/14 GPIO output)
- Monitor parsing ready (BEACON_PATTERN_V2 + legacy fallback)

**Recommended test sequence (for this session):**
1. **Single target:** I2C2 reads on PB8/9, LED toggle, beacon TX
2. **Target-to-target:** Beacon RX on monitor station, payload field extraction
3. **Regression:** Ensure V2 parser doesn't break legacy 5-byte format

**Known limitations to document:**
- No I2C bus recovery (stuck SDA/SCL → power cycle required)
- INA219 failure returns 0 (graceful, but no diagnostics)
- Test app limited (LED-blink only, use lora app terminal for validation)

**Cross-share with Ravenor:** I2C2/INA219 topology, Beacon V2 format, Kotov's PCB findings, Cawl's pin-locking principles
