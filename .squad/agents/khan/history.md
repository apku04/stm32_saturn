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
2026-07-14: Hardened MAC/node address uniqueness (Perturabo Concern 6). Fixed UID_BASE from 0x1FFF6E50 (wrong, STM32L0 address) to 0x1FFF7590 (correct for STM32U073 per RM0503 §45). Added UID fingerprint clone-detection to mac_layer_init: stores a 1-byte UID hash in flash slot UID_FINGERPRINT; on boot, compares stored fingerprint to current UID — mismatch means cloned flash, re-derives address from UID. Existing boards (nodes 30, 38) migrate transparently: first boot with new firmware sees fingerprint=0xFF → stamps fingerprint, keeps address. Added writeFlashByte() to flash_config for programming a single slot without page erase. Build clean.

### 2026-04-26 — Session Completion: MAC Address Uniqueness (Concern 6)

**Scope:** Hardened MAC address generation to prevent collisions when boards are cloned.

**Problem:** Two boards with identical flash image got same DEVICE_ID (node address), causing LoRa MAC collisions. Code only derived UID on blank flash — cloned boards kept donor's address forever.

**Solution: UID Fingerprint Clone Detection**
- Store 1-byte UID fingerprint (XOR of 12 UID bytes) in flash alongside DEVICE_ID
- On boot: compare stored fingerprint to current UID
  - Match → same board, keep address
  - Mismatch → clone detected, derive new address from UID
  - 0xFF fingerprint → first boot with new firmware, migrate (preserve existing address)

**Bonus fix:** Corrected UID_BASE from 0x1FFF6E50 (STM32L0, wrong!) to 0x1FFF7590 (STM32U073 per RM0503 §45).

**Files changed:**
- firmware/include/stm32u0.h (UID_BASE fix)
- firmware/include/globalInclude.h (UID_FINGERPRINT field)
- firmware/src/system/flash_config.c/h (writeFlashByte implementation)
- firmware/src/protocol/maclayer.c (clone detection logic)

**Migration:** Existing boards (nodes 30, 38) see 0xFF fingerprint on first boot → preserve address + stamp fingerprint. Safe for all existing nodes.

**Build verified:** 30,868 text, 108 data, 2456 bss (clean, no warnings).

**Documentation:** Decision written to decisions.md. Orchestration log created.

**Status:** ✅ COMPLETE — Cloned boards now auto-detect and re-derive unique addresses.

