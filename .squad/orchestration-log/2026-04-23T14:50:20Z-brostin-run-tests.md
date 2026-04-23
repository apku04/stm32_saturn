# Orchestration Log: brostin-run-tests

**Timestamp:** 2026-04-23T14:50:20Z  
**Agent:** Brostin (Integration Specialist)  
**Task:** Execute I2C2 / INA219 beacon test plan

## Status

**In Progress** — Running all target tests (single + target-to-target)

## Test Plan

Brostin documented comprehensive test plan in:
- `firmware/app/lora/test/ina219_beacon_test_plan.md`

### Test Scope
1. **Single target tests:**
   - INA219 I2C reads on PB8/PB9 with AF6
   - Beacon payload v2 format (shunt/bus/bat/chg)
   - LED functionality restored (toggle, on, init)
   - Monitor parsing of V2 beacon

2. **Target-to-target tests:**
   - Beacon TX from lora app
   - Beacon RX on monitor station
   - Payload decoding and field extraction

## Baseline Knowledge

Brostin previously identified:
- I2C1 vs I2C2 mismatch in prior firmware (RCC bits differ: bit 21 vs 22)
- Lack of shunt register access in baseline INA219 driver
- No I2C bus recovery mechanism (stuck SDA/SCL requires power cycle)
- Beacon payload originally 5 bytes (bat/sol/chg), now 7 bytes (shunt/bus/bat/chg)

## Files Generated

- `firmware/app/lora/test/ina219_beacon_test_plan.md` — test procedures and acceptance criteria
