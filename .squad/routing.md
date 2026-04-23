# Work Routing

## Routing Table

| Work Type | Route To | Examples |
|-----------|----------|---------|
| System design, architecture decisions | Cawl | Layer boundaries, memory layout, protocol design |
| PCB layout, net connections, EasyEDA DB | Kotov | Schematic queries, pin verification, PCB fixes |
| RPi GPIO, SWD, OpenOCD, DFU flash path | Mkoll | Wiring, probe script, flash procedure |
| STM32 firmware, registers, drivers | Yarrick | Peripheral init, bare-metal code, bug fixes |
| LoRa driver, SX1262, MAC, beacon, protocol | Ravenor | Radio config, packet format, beacon payload |
| Code review, hallucination detection | Eisenhorn | All firmware before declaring done |
| Test plans, validation, edge cases | Brostin | Test procedures, lora_monitor validation |
| Session logging, decisions | Scribe | Automatic — never needs routing |
| Backlog, issue queue | Ralph | Monitoring work queue |

## Issue Routing

| Label | Action | Who |
|-------|--------|-----|
| `squad` | Triage: analyze issue, assign `squad:{member}` label | Cawl |
| `squad:cawl` | Architecture work | Cawl |
| `squad:kotov` | PCB / schematic work | Kotov |
| `squad:mkoll` | RPi / debug path work | Mkoll |
| `squad:yarrick` | STM32 firmware work | Yarrick |
| `squad:ravenor` | LoRa / radio / protocol work | Ravenor |
| `squad:eisenhorn` | Review pass | Eisenhorn |
| `squad:brostin` | Testing / validation | Brostin |

## Rules

1. **Eager by default** — spawn Yarrick + Kotov in parallel for hardware/firmware tasks.
2. **Eisenhorn reviews all firmware before done** — never skip the review gate.
3. **Brostin writes test plan while implementation proceeds** — anticipatory, always parallel.
4. **Scribe always runs** after substantial work, background, never blocks.
5. **SWD is down** — Mkoll's flash guidance always uses DFU path, never flash.sh.
6. **Quick facts → coordinator answers directly.**
