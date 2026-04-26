# Work Routing

## Routing Table

| Work Type | Route To | Examples |
|-----------|----------|---------|
| System design, architecture decisions | Guilliman | Layer boundaries, memory layout, protocol design |
| PCB layout, net connections, EasyEDA DB | Ferrus | Schematic queries, pin verification, PCB fixes |
| RPi GPIO, SWD, OpenOCD, DFU flash path | Corax | Wiring, probe script, flash procedure |
| STM32 firmware, registers, drivers | Dorn | Peripheral init, bare-metal code, bug fixes |
| LoRa driver, SX1262, MAC, beacon, protocol | Khan | Radio config, packet format, beacon payload |
| Code review, hallucination detection | Lion | All firmware before declaring done |
| Adversarial critique, design stress-test | Perturabo | Architecture proposals, protocol design, power/timing assumptions |
| Test plans, validation, edge cases | Russ | Test procedures, lora_monitor validation |
| Session logging, decisions | Scribe | Automatic — never needs routing |
| Backlog, issue queue | Ralph | Monitoring work queue |

## Issue Routing

| Label | Action | Who |
|-------|--------|-----|
| `squad` | Triage: analyze issue, assign `squad:{member}` label | Guilliman |
| `squad:guilliman` | Architecture work | Guilliman |
| `squad:ferrus` | PCB / schematic work | Ferrus |
| `squad:corax` | RPi / debug path work | Corax |
| `squad:dorn` | STM32 firmware work | Dorn |
| `squad:khan` | LoRa / radio / protocol work | Khan |
| `squad:lion` | Review pass | Lion |
| `squad:russ` | Testing / validation | Russ |
| `squad:perturabo` | Adversarial critique | Perturabo |

## Rules

1. **Eager by default** — spawn Dorn + Ferrus in parallel for hardware/firmware tasks.
2. **Lion reviews all firmware before done** — never skip the review gate.
3. **Russ writes test plan while implementation proceeds** — anticipatory, always parallel.
4. **Scribe always runs** after substantial work, background, never blocks.
5. **SWD is down** — Corax's flash guidance always uses DFU path, never flash.sh.
6. **Quick facts → coordinator answers directly.**
