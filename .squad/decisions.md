# Squad Decisions

## Active Decisions

### 1. I2C2 Pin Mapping — PCB Ground Truth (Kotov)

**Date:** 2025-07-14  
**Status:** Established  
**Authority:** Kotov (PCB Expert)

**Findings:**
- INA219 physically wired to **PB8 (SCL) and PB9 (SDA)**
- INA219 I2C address: **0x40** (A0/A1 both tied to GND)
- Separate I2C connector: PB6/7 (external header, not INA219)
- STM32U073 AF mapping: PB8/9 support AF4→I2C1 or AF6→I2C2

**Key Lesson:** On STM32U073, pins can route to multiple peripherals via AF selection.  
The 1-line fix was AF4→AF6, not pin migration.

---

### 2. INA219 I2C Pin Conflict Verdict (Eisenhorn)

**Date:** 2025-07-17  
**Status:** REJECTED Yarrick's commit 1bba9d5 (2025-07-17)  
**Authority:** Eisenhorn (Hallucination Detective)

**Root Cause Chain:**
1. **Pre-5399462:** Used PB6/7 (wrong pins for INA219)
2. **Commit 5399462:** Moved to PB8/9 + used AF4 (routed to I2C1, not I2C2) → MISMATCH
3. **Commit 1bba9d5 (Yarrick):** Moved to PB13/14 (LED pins, not wired to INA219) → DEAD

**Correct Fix:** PB8/9 + AF6 → I2C2 (one-line AF change from earlier attempts)

**Verdict:** 
- Kotov: **CORRECT** (pins, AF, address all verified via independent PCB decoding)
- Yarrick: **WRONG** (pin selection), but code quality was sound; locked out from I2C/pin changes
- Lock-out applies until corrections approved by Architect or team

**Code Audit Result:** Yarrick's I2C2 driver, INA219 registers, beacon payload V2, 
monitor parsing all technically correct despite wrong pins.

---

### 3. I2C2 Pin Correction (Cawl)

**Date:** 2025-07-18  
**Status:** Applied  
**Authority:** Cawl (Architect)  
**Commit:** 910cb9c

**Changes:**
1. `hw_pins.h`: Removed I2C2_SCL/SDA on PB13/14; restored LED1/LED2 defines; updated comments
2. `i2c.c`: Changed i2c2_init() from PB13/14 → PB8/9, AF to 6 (0x66 in AFRH)
3. `main.c`: Restored LED functions (init, toggle, on); LED GPIO config on PB13/14

**Constraints Preserved:**
- Beacon payload V2 (7 bytes) unchanged
- I2C2 TIMINGR unchanged (0x30420F13 for 100kHz @ 16MHz)
- No other files modified (surgical edit)

**Build Status:** Clean

---

## Governance

- All meaningful changes require team consensus (decisions recorded here)
- Architectural decisions (pin assignments, peripherals, payload formats) locked by Architect
- Agent lock-outs prevent repeat errors in high-risk domains (I2C, pins, peripherals)
- Cross-agent learnings propagated to history.md files (see .squad/agents/*/history.md)

## Version History

- **2025-07-14** — Kotov: PCB findings
- **2025-07-17** — Eisenhorn: Verdict + Yarrick lock-out
- **2025-07-18** — Cawl: Pin correction applied
- **2026-04-23** — Scribe: Consolidated decisions, added cross-agent learnings
