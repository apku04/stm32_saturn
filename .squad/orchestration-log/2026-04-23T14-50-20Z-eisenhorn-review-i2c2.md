# Orchestration Log: eisenhorn-review-i2c2

**Timestamp:** 2026-04-23T14:50:20Z  
**Agent:** Eisenhorn (Hallucination Detective)  
**Task:** Code review of Yarrick's I2C2 + INA219 implementation

## Summary

Eisenhorn performed independent PCB verification and code audit:

### PCB Verification
- Independently decoded EasyEDA schematic and traced net coordinates
- **Confirmed:** INA219 physically wired to PB8 (SCL) and PB9 (SDA)
- Verified STM32U073 AF mapping: PB8/PB9 support AF4→I2C1 or AF6→I2C2
- Separate I2C connector on PB6/PB7 (not connected to INA219)

### Root Cause Analysis
Traced the bug chain across three commits:
1. **9050552:** Used PB6/PB7 (wrong pins for INA219)
2. **5399462 (Achuthan):** Moved to PB8/PB9 but used AF4 (routes to I2C1, not I2C2)
3. **1bba9d5 (Yarrick):** Moved to PB13/PB14 (LED pins, not wired to INA219)

**Correct fix:** Keep PB8/PB9 + change AF4 → AF6 (one-line fix)

### Code Audit
Reviewed Yarrick's implementation against INA219 datasheet and STM32 registers:
- I2C2 base address: ✅ 0x40005800
- RCC clock bit: ✅ bit 22 (I2C2, not bit 21 for I2C1)
- TIMINGR: ✅ 0x30420F13 correct for 100kHz @ 16MHz
- I2C register offsets: ✅ All correct
- INA219 shunt/bus registers: ✅ Correct per datasheet
- Beacon payload: ✅ 7 bytes within 50-byte LoRa limit
- Monitor parsing: ✅ BEACON_PATTERN_V2 regex correct

### Verdict

**REJECTED** — Yarrick's commit 1bba9d5

**Lock-out:** Yarrick is locked out from further I2C/pin changes

**Required corrections (assign to new agent or Kotov):**
1. `hw_pins.h` — Remove I2C2_SCL/SDA on PB13/14; restore LED1/LED2 defines
2. `i2c.c:i2c2_init()` — Change from PB13/14 → PB8/9 (AF6)
3. `main.c` — Restore LED functions (init, toggle, on)
4. Comments — Update to reflect I2C2 on PB8/9 AF6

## Files Generated

- `.squad/decisions/inbox/eisenhorn-i2c2-verdict.md` — detailed analysis + root cause chain
