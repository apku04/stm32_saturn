# INA219 + Battery ADC Beacon Test Plan

**Board:** Saturn LoRa Tracker (STM32U073CBT6 + SX1262 E22-900M22S)
**Interface:** USB CDC terminal @ 115200 baud (VID 0483 / PID 5740)
**Flash:** `firmware/tools/dfu_flash.sh app/lora/lora.bin --enter`
**Monitor:** `python3 tools/lora_monitor.py`

---

## Prerequisites

- Multimeter available for reference voltage measurements
- Two Saturn boards (one TX, one RX) — or one board + `lora_monitor.py` on a second
- LiPo battery connected (3.0–4.2 V range)
- Solar panel or bench supply on INA219 bus input (optional — can test with 0 V)

---

## 1. I2C Bus Sanity

### 1.1 Bus Scan

**Command:** `get i2cscan`

**Expected output (success):**
```
Scanning I2C2 (0x08-0x77)...
  Found: 0x40
Done
```

INA219 default address is **0x40** (A0=A1=GND). Seeing `Found: 0x40` confirms:
- I2C peripheral clock is enabled (RCC_APBENR1 bit for I2C)
- GPIO alternate-function is correct (SCL/SDA pins, AF4, open-drain)
- INA219 is powered and ACKing

**Expected output (failure — no ACK):**
```
Scanning I2C2 (0x08-0x77)...
Done
```
No `Found:` lines means the INA219 is not responding.

> **CRITICAL CHECK:** The driver file `i2c.c` currently aliases `I2Cx_*` to `I2C1_*`
> registers (base `0x40005400`). If the hardware has been rerouted to I2C2 (base
> `0x40005800`), these aliases **must** be updated to `I2C2_*`. The RCC enable bit
> also differs: I2C1 = APBENR1 bit 21, I2C2 = APBENR1 bit 22. Verify the correct
> peripheral is enabled before testing.

### 1.2 Post-Reset Lockup Check

After flashing and first boot:
1. Open terminal (`screen /dev/ttyACM0 115200` or lora_monitor.py)
2. Type `version` — expect `STM32_LORA_V1`
3. If no response within 3 seconds → I2C init may be hanging (bus stuck)

### 1.3 SCL Stuck-Low Recovery

If I2C bus is locked (no ACK on scan, `version` command still works):

1. Power-cycle the board (unplug USB + battery, wait 5 s, reconnect)
2. Re-run `get i2cscan`
3. If still stuck: the I2C driver has no software bus-recovery (no bit-bang toggle
   of SCL). This is a **known gap** — file a bug to add 9-clock recovery in
   `i2c_init()` before enabling the peripheral.

**Manual recovery:** briefly short SCL to GND then release, power-cycle.

---

## 2. INA219 Register Reads

### 2.1 Solar Voltage (Bus Voltage Register 0x02)

**Command:** `get solar`

**Expected output:**
```
Solar: <value> mV
```

| Condition               | Expected `sol` (mV) | Notes                                |
|-------------------------|----------------------|--------------------------------------|
| No solar / no input     | 0                    | Bus voltage = 0                      |
| USB 5 V on VBUS         | ~5000                | If INA219 is on USB rail             |
| 3.7 V LiPo passthrough | 3000–4200            | Depends on circuit topology          |
| Solar panel (bright)    | 4500–6000            | Typical 5 V panel                    |

**Plausibility bounds:** 0–6500 mV. Values above 6500 mV or exactly 65535 suggest a read error or register misparse.

### 2.2 No-Load Shunt Check

The current firmware only reads the bus voltage register (0x02). There is **no terminal
command** to read the shunt register (0x01) or current register (0x04).

**Manual verification via code inspection:**
- `ina219_read_bus_mv()` reads register 0x02, shifts right by 3, multiplies by 4
- INA219 bus voltage register: bits [15:3] = voltage, bit 2 = CNVR, bit 1 = OVF, bit 0 = reserved
- The `>> 3` shift correctly discards CNVR/OVF/reserved bits

**Recommendation:** Add a `get ina219` debug command that dumps raw registers:
```
Config (0x00): 0x399F
Shunt  (0x01): 0x0000
Bus    (0x02): 0x<raw>
Power  (0x03): 0x0000
Current(0x04): 0x0000
Calib  (0x05): 0x0000
```
Until that command exists, validate via `get solar` only.

### 2.3 Failure Mode: i2c_read_reg Returns Error

If `i2c_read_reg()` returns non-zero (NACK or timeout), `ina219_read_bus_mv()` returns **0**.
This is the graceful fallback — no crash, just zero mV. Verify:

1. If INA219 is physically absent, `get solar` should print `Solar: 0 mV`
2. The device should **not** hang or HardFault

---

## 3. Battery ADC (PB4 / ADC1_IN22)

### 3.1 Reading the Battery

**Command:** `get battery`

**Expected output:**
```
Battery: <value> mV (raw: <adc_raw>)
```

### 3.2 Expected Values

| Battery State       | Voltage (multimeter) | Expected `bat_mv` | ADC raw (approx) |
|---------------------|----------------------|--------------------|-------------------|
| Fully charged LiPo  | 4.20 V              | 4100–4300 mV       | ~2600             |
| Nominal LiPo        | 3.70 V              | 3600–3800 mV       | ~2300             |
| Low LiPo            | 3.30 V              | 3200–3400 mV       | ~2050             |
| Dead / disconnected  | 0 V                 | 0–50 mV            | 0–30              |

**Formula:** `bat_mv = raw * 6600 / 4096`
- Divider is 1:1 (1MΩ/1MΩ), so pin sees VBAT/2
- ADC reference = VDDA = 3.3 V, 12-bit → raw = (VBAT/2) / 3.3 * 4096
- Firmware compensates by multiplying by 2 (the 6600 = 3300 × 2)

### 3.3 Validation Procedure

1. Measure battery voltage with multimeter at the LiPo connector
2. Run `get battery`
3. Compare: `bat_mv` should be within **±100 mV** of multimeter reading
4. If error > 200 mV, check:
   - Sense LDO enable (PA15) — should be high after `adc_init()`
   - Divider resistor values (assumed 1M/1M — verify on PCB)
   - VDDA accuracy (measure 3V3 rail — if 3.25 V, readings will be ~1.5% low)

### 3.4 Battery Disconnected

With no battery and USB power only:
- The ADC reads whatever leaks through the divider — typically **0–50 mV**
- The device does **not** crash; it just reports a very low value
- `get battery` → `Battery: 0 mV (raw: 0)` or similar small value

---

## 4. Beacon Payload Validation

### 4.1 Beacon Structure

The beacon is sent every ~10 s (timer callback). Payload layout in `pkt.data[]`:

| Byte offset | Field            | Encoding          | Size   |
|-------------|------------------|--------------------|--------|
| 0–1         | `bat_mv`         | uint16 LE          | 2 bytes|
| 2–3         | `sol_mv`         | uint16 LE          | 2 bytes|
| 4           | `charge_status`  | uint8 (enum 0–3)   | 1 byte |

**Total telemetry:** 5 bytes. `pkt.length` = PACKET_HEADER_SIZE (12) + 5 = 17.

Charge status values:
- `0` = Off (no power / no battery)
- `1` = Charging
- `2` = Done (charge complete)
- `3` = Fault

### 4.2 Monitor Verification

1. Flash firmware: `firmware/tools/dfu_flash.sh app/lora/lora.bin --enter`
2. Open monitor on the **receiving** board: `python3 tools/lora_monitor.py`
3. Connect to the receiver's serial port in the GUI
4. Wait for a beacon from the transmitter (up to 10 s)

**Expected console output on receiver USB CDC:**
```
[RX] src=<N> dst=255 rssi=<dBm> prssi=<dBm> type=0 seq=<N> len=17
[BEACON] bat=<mV> sol=<mV> chg=<0-3> entries=<N>
```

**In lora_monitor.py GUI:**
- **Packets tab:** row with TYPE=BEACON, INFO showing `Bat=X.XXV Solar=X.XXV Charge=<status>`
- **Beacons tab:** dedicated row with BAT V, SOL V, CHARGE columns

The monitor regex `BEACON_PATTERN` matches:
```
\[BEACON\]\s+bat=(\d+)\s+sol=(\d+)\s+chg=(\d+)\s+entries=(\d+)
```

### 4.3 Cross-Validation Steps

1. On the transmitter, run `get battery` and `get solar` and `get charge`
2. Note the values
3. On the receiver, wait for the next beacon
4. Compare: `bat` in beacon should match `get battery`, `sol` should match `get solar`,
   `chg` should match `get charge` (as numeric enum)
5. Tolerance: values may differ slightly because the beacon and manual query sample at
   different times — accept **±50 mV** difference

### 4.4 Legacy Compatibility

If a PIC24 node sends a beacon (older format with only battery, no solar/charge):
- Receiver firmware falls through to the `data_len >= app_off + 2` branch
- Monitor uses `BEACON_PATTERN_LEGACY` → shows bat only, sol/chg as "—"
- **Verify:** a PIC24 beacon does not crash the STM32 parser

---

## 5. Edge Cases

### 5.1 INA219 Not Responding

**Scenario:** INA219 absent, wrong I2C address, or SDA/SCL pins misconfigured.

| Behavior                             | Expected? |
|--------------------------------------|-----------|
| `get solar` returns `Solar: 0 mV`   | ✅ Yes    |
| `get i2cscan` shows no device at 0x40| ✅ Yes    |
| Device continues running (no hang)   | ✅ Yes    |
| HardFault / both LEDs blink rapidly  | ❌ Bug    |
| Beacon still sent with `sol=0`       | ✅ Yes    |

**How it works:** `i2c_read_reg()` returns -2 (NACK) or -1 (timeout), causing
`ina219_read_bus_mv()` to return 0. The timeout is bounded by `I2C_TIMEOUT = 100000`
loop iterations (~6 ms at 16 MHz). The main loop is **not** blocked indefinitely.

**Test procedure:**
1. If possible, disconnect INA219 (desolder or cut trace) — or just test without
   solar input connected
2. Flash and boot
3. Run `get solar` — expect `0 mV`
4. Run `get i2cscan` — expect no hit at 0x40
5. Confirm `version` still responds (device is alive)

### 5.2 Bus Voltage Register Overflow (OVF Bit)

The INA219 bus voltage register (0x02) has an **OVF bit** at bit 0 (after the reserved
bit). This indicates a math overflow in the power/current calculations.

**Current handling:** The firmware does `raw >> 3` which shifts out bits [2:0] including
OVF. The OVF bit is **silently discarded** — no error is reported.

**Impact:** OVF only affects the power/current registers (which we don't read). The bus
voltage reading itself is still valid when OVF is set. No bug here, but if current
measurement is added later, OVF should be checked.

**CNVR bit (bit 1):** Conversion-ready flag. Also shifted out by `>> 3`. Not checking
CNVR means we may read a stale value if polled faster than the INA219 conversion time
(~532 µs in default config). At our 10 s beacon interval this is a non-issue.

### 5.3 I2C Bus Contention After Init Failure

If `ina219_init()` fails during the `i2c_write_reg()` config write (e.g., NACK), the
I2C peripheral is left in a potentially dirty state but still enabled. Subsequent reads
via `ina219_read_bus_mv()` will also NACK and return 0.

**Risk:** If the I2C bus gets stuck (SDA held low by a confused slave), all future I2C
operations will timeout. The main loop still runs (USB works, radio works), but all I2C
reads return -1.

**Missing feature:** No bus-recovery mechanism. Recommend adding SCL bit-bang recovery
(9 clock pulses with SDA high) at the start of `i2c_init()`.

### 5.4 ADC With No Battery (USB Power Only)

| Scenario                    | Expected `bat_mv` | Risk           |
|-----------------------------|---------------------|----------------|
| USB only, no LiPo           | 0–100 mV           | None           |
| USB only, LiPo connector open| 0–50 mV           | None           |
| USB + fully charged LiPo    | 4100–4300 mV       | None           |

The ADC averages 8 samples and returns a clean value. No division-by-zero or overflow
risk in `raw * 6600 / 4096` since raw is 12-bit max (4095) → max result = 6598.

### 5.5 Beacon With All Zeros

If both INA219 and ADC return 0 (no solar, no battery), the beacon payload is:
```
data[0..1] = 0x00 0x00   (bat_mv = 0)
data[2..3] = 0x00 0x00   (sol_mv = 0)
data[4]    = 0x00         (charge = CHARGE_OFF)
```
The beacon is still transmitted. The receiver decodes `bat=0 sol=0 chg=0`. This is
valid — verify it does not get filtered or misinterpreted.

---

## 6. I2C1 → I2C2 Migration Checklist

> The current `i2c.c` uses **I2C1** registers. If hardware routes INA219 to I2C2,
> the following changes are required. Verify each one during code review.

| Item                           | I2C1 value             | I2C2 value             | File          |
|--------------------------------|------------------------|------------------------|---------------|
| Base address                   | `0x40005400`           | `0x40005800`           | stm32u0.h     |
| RCC enable bit (APBENR1)      | bit 21                 | bit 22                 | i2c.c         |
| Register aliases (I2Cx_*)     | `I2C1_CR1`, etc.       | `I2C2_CR1`, etc.       | i2c.c         |
| GPIO pins                     | PB8/PB9 AF4 (I2C1)    | PB8/PB9 AF4? Check RM  | i2c.c         |
| Comment / header update       | "I2C1 master driver"   | "I2C2 master driver"   | i2c.c, i2c.h  |
| Terminal scan message          | (already says I2C2)    | Correct                | terminal.c    |

> **NOTE:** PB8/PB9 on STM32U073 can be either I2C1_SCL/SDA (AF4) or potentially
> I2C2 depending on the AF mapping. Consult the STM32U073 datasheet AF table to confirm
> which AF number maps PB8/PB9 to I2C2. If there is no I2C2 mapping on PB8/PB9, the
> pins must change too.

---

## 7. Quick Smoke Test Sequence

Run these commands in order after every new flash. All via USB CDC terminal.

```
version                → STM32_LORA_V1
get battery            → Battery: XXXX mV (raw: YYYY)   [compare to multimeter]
get solar              → Solar: XXXX mV                  [0 if no solar input]
get charge             → Charge: Off|Charging|Done|Fault
get i2cscan            → Found: 0x40                     [INA219 present]
ping                   → [wait for PONG from neighbor]
```

Then wait 10–20 s and check `lora_monitor.py` on the receiver for a `[BEACON]` line
with all three telemetry fields populated.

**PASS criteria:** All commands respond without hang, values match multimeter within
±100 mV, beacon appears in monitor with correct format.

---

*Test plan written by Brostin (Tester) — Saturn LoRa Tracker project*
