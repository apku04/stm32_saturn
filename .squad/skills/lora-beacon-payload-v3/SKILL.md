---
name: "lora-beacon-payload-v3"
description: "Saturn beacon telemetry payload v3 — format, source-side unit conversion, and RX decoding"
domain: "lora-protocol"
confidence: "high"
source: "earned"
---

## Context
Applies when touching Saturn beacon TX/RX code, adding new telemetry fields, or updating the
`lora_monitor.py` GUI. Beacons are emitted every 10 s by `beaconHandler()` in
`firmware/app/lora/main.c`. Payload sits after the 4-byte packet header and after any mesh
table entries (`mesh_tbl_entries * 3` bytes).

## Patterns

### v3 payload layout (7 bytes, little-endian)
| Offset | Size | Field    | Type   | Notes                                                  |
|-------:|-----:|----------|--------|--------------------------------------------------------|
| 0      | 2    | `i_ma`   | int16  | Signed current in mA, computed at source               |
| 2      | 2    | `bus_mv` | uint16 | INA219 bus voltage, mV                                 |
| 4      | 2    | `bat_mv` | uint16 | Battery mV (currently equal to `bus_mv` — see below)   |
| 6      | 1    | `chg`    | uint8  | Charge state: 0=Off, 1=Chrg, 2=Done, 3=Fault           |

Mesh table entries (if any) precede this block, so RX uses `app_off = mesh_tbl_entries * 3`.
Packet `length` for a zero-entry beacon is `4 (header) + 7 (telemetry) = 11`.

### Source-side unit conversion
Current is converted to mA **at the transmitter**, not the receiver. The shunt resistor value
(R58 = 50 mΩ) is a board constant — receivers should not need to know it. INA219 shunt is read
in µV and converted: `i_ma = sh_uv / 50`. This keeps the RX side board-agnostic and lets
heterogeneous receivers (STM32, PIC24, Python GUI) share one decode path.

### Battery proxy
`bat_mv` currently mirrors `bus_mv` because PB4 has no ADC channel on STM32U073. Field is
reserved for a future dedicated battery sense; do not repurpose.

### Charge state mapping (from PA10 CHRG, PA8 STDBY in `charge_get_status()`)
Both signals are **active-low** (0 = asserted). Table uses raw pin levels:
| CHRG | STDBY | Value | Meaning  |
|:----:|:-----:|:-----:|----------|
| 1    | 1     | 0     | Off      |
| 0    | 1     | 1     | Chrg     |
| 1    | 0     | 2     | Done     |
| 0    | 0     | 3     | Fault    |

### Python receiver
`tools/lora_monitor.py` uses `BEACON_PATTERN_V3` to parse the firmware `[BEACON] i_ma=… bus=…
bat=… chg=… entries=…` line. `BEACON_PATTERN_V2` is the legacy `shunt=` form; when matching v2,
derive `i_ma = shunt_mv * 20` (assumes 50 mΩ shunt, `1 mV / 50 mΩ = 20 mA`).

## Examples

TX build (`firmware/app/lora/main.c`, ~L82–110):
```c
int32_t  sh_uv  = ina219_read_shunt_uv();
int16_t  i_ma   = (int16_t)(sh_uv / 50);        /* 50 mΩ → mA */
uint16_t bus_mv = ina219_read_bus_mv();
uint16_t bat_mv = bus_mv;                        /* bus proxy */
uint8_t  chg    = (uint8_t)charge_get_status();
pkt.data[0] = i_ma & 0xFF;  pkt.data[1] = (uint16_t)i_ma >> 8;
pkt.data[2] = bus_mv & 0xFF; pkt.data[3] = bus_mv >> 8;
pkt.data[4] = bat_mv & 0xFF; pkt.data[5] = bat_mv >> 8;
pkt.data[6] = chg;
pkt.length  = 4 + 7;
```

RX decode (`firmware/app/lora/main.c`, ~L147):
```c
uint8_t app_off = pkt->mesh_tbl_entries * 3;
int16_t i_ma = (int16_t)(pkt->data[app_off] | ((uint16_t)pkt->data[app_off+1] << 8));
```

## Anti-Patterns
- Do **not** send raw shunt µV/mV over the air and convert on the RX side — receivers must stay
  board-agnostic.
- Do **not** reorder fields or insert new ones in the middle; append to the end and bump the
  version (v4) with a matching `BEACON_PATTERN_V4` in `lora_monitor.py`.
- Do **not** assume `data_len == 7`; always compute `app_off` from `mesh_tbl_entries` first.
- Do **not** treat `bat_mv` as an independent reading today — it's a `bus_mv` mirror.
