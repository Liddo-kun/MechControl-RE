# EC Register & HID Protocol Reference

Single source of truth for all register addresses, encodings, write sequences, and USB HID commands used by MechControl. For reverse engineering methodology and evidence, see `report.md`. For app architecture, see `architecture.md`.

**How registers are used in code:** The app loads profiles from `MechControl/Profiles/MECHREVO/*.json` (compact ~35-line files with standardized names like `cpuFanDuty`). Do NOT confuse with `Profiles/MECHREVO/*.json` at the repo root — those are raw OEM extractions (~400 lines with names like `ADDR_EC_MAIN_FAN_L_DUTY_BYTE`) used only as RE reference. Code accesses registers by name via `profile.GetRegister("name")` — never by hardcoded address. The tables below show the canonical code name where applicable. See `architecture.md` section 6 for the full lookup flow.

---

## 1. EC Driver Interface

| Field | Value |
|-------|-------|
| Device path | `\\.\ACPIDriver` (UWACPIDriver.sys) |
| IOCTL read | `0x9C40A488` |
| IOCTL write | `0x9C40A48C` |

**Read:** Input = 4-byte int (address). Output = 4-byte int (low byte = value).

**Write:** Input = 8 bytes: int addr (offset 0, LE) + int value (offset 4, LE). Output always 0xFFFFFFFE (constant, not meaningful).

28 IOCTL codes exist in total; only the two above are used for EC access.

---

## 2. CPU Power Write Sequences

Registers: `0x783`-`0x786` (PL1/PL2/PL4/TempLimit). Mode defaults: `0x730`-`0x737`, `0x7A7`-`0x7AA`. See 13.4.

**Write sequence** (always write as a group of 4):
```
Write(0x783, pl1_watts)
Write(0x784, pl2_watts)
Write(0x785, pl4_watts / 2)
Write(0x786, 0x80 | offset)            -> Intel inverted_offset: offset = 105 - cpu_temp
                                        -> AMD direct:           offset = cpu_temp
                                        -> bit 7 is the commit/enable flag; without it the EC
                                           stores the value but does NOT enforce the limit
                                        -> verify via 0x463 redirect; mask low 7 bits on both
                                           sides (bit 7 at read address is EC firmware
                                           bookkeeping, may be set or clear). 0x463 updates on
                                           the EC's polling cycle (~300-900ms observed) — retry
                                           read with backoff until masked value matches.
```

**Defaults by mode (read-only reference):**
- Gaming: `0x730/731/732/733` (PL1/PL2/PL4/D)
- Office: `0x734/735/736/737`
- Battery saver: `0x7A7/7A8/7A9/7AA`
- TCC offset defaults: `0x7D8` (gaming), `0x7D9` (office), `0x7DA` (turbo)

---

## 3. GPU Power Write Sequences

Registers: `0x743`-`0x746` (TGP/DB control). See 13.5.

**Write sequence -- Dynamic Boost enabled:**
```
WriteBitfield(0x743, 0x07, 0x07)  -- sets TGP bits 0,2 + DB bit 1
Write(0x744, tgp_watts - 95)
Write(0x745, 255)
Write(0x746, db_value)
```

**Write sequence -- Dynamic Boost disabled:**
```
WriteBitfield(0x743, 0x07, 0x05)  -- sets TGP bits 0,2, clears DB bit 1
Write(0x744, tgp_watts - 95)
Write(0x745, 0)
Write(0x746, 0)
```

---

## 4. Fan Control Write Sequences

Registers: `0x43E`-`0x46C` (sensors/RPM), `0x751` (boost), `0x75B`/`0x75C` (duty), `0x768` (status), `0x787` (switch speed), `0x7C5` (curve enable). See 13.2, 13.5.

**Fan boost:** `Write(0x751, boost ? 0x40 : 0x00)`

**Fan curve enable:** `WriteBitfield(0x7C5, 0x80, enable ? 0x80 : 0x00)`

**Fan switch speed:** `Write(0x787, enabled ? (0x80 | ms/100) : 0x00)`

### Fan Table (0xF00-0xF5F)

10 active points (indices 0-9) + 6 padding (indices 10-15).

**CPU fan table:**

| Base | Offset | Purpose |
|------|--------|---------|
| `0xF00` | +N | Temp UP threshold |
| `0xF10` | +N | Temp DOWN threshold |
| `0xF20` | +N | Duty cycle (raw/2 = percent) |

**GPU fan table:**

| Base | Offset | Purpose |
|------|--------|---------|
| `0xF30` | +N | Temp UP threshold |
| `0xF40` | +N | Temp DOWN threshold |
| `0xF50` | +N | Duty cycle |

**EC evaluation rule** (right-point):
- `duty[i]` applies when `tempUp[i-1] < temp ≤ tempUp[i]`.
- Going up, crossing `tempUp[i]` jumps duty to `duty[i+1]`.
- Going down, crossing `tempDown[i]` drops duty to `duty[i-1]`.
- We set `tempDown[i] = tempUp[i-1] - 4` (cross-point), giving every zone a clean 4°C hysteresis band around its entry threshold `tempUp[i-1]`.

**Write order** (interleaved triplets, matches OEM):
```
active = number of active points (typically 10)
for i in 0..15:
  if i < active:
    Write(tempUpBase   + i, tempUp[i])
    if i > 0: Write(tempDownBase + i, tempUp[i-1] - 4)        -- cross-point; index 0 never written
    Write(dutyBase     + i, i == 0 ? 0x00 : duty[i])          -- raw = percent * 2; duty[0] must be 0
  else:
    Write(tempUpBase   + i, 0xFF)
    Write(tempDownBase + i, i == active ? tempDown[active-1] + 4 : 0xFF)   -- OEM leftover byte
    Write(dutyBase     + i, duty[active-1])                   -- last active duty
```

Total per apply: 47 writes per fan table (16 tempUp + 15 tempDown + 16 duty), 94 across CPU and GPU.

**Key details:**
- `tempDown[0]` is **not written**. Zone 0 has no lower zone to drop into, and writing the byte caused table rejection on some EC firmware versions.
- `duty[0]` must be `0x00`. Raw 0 at other indices triggers EC auto-override (~25% duty); minimum safe raw is 2 (= 1%).
- Adjacent points must be at least 4°C apart on `tempUp` (`Validate()` rejects violations; UI drag clamps to this).
- Padding `tempUp = 0xFF`; padding duty = last active entry's raw duty. Avoids a discontinuity if the EC ever evaluates a padding row.
- First padding `tempDown` (i == active) = `tempDown[active-1] + 4` (OEM leftover byte); remaining padding tempDown = `0xFF`. The EC never consults these because `tempUp == 0xFF` guarantees the row can't fire; we mirror the OEM writes for verify-diff cleanliness.
- Writes take effect immediately (no trigger byte needed).
- CPU and GPU sections are independent — changing a GPU point only rewrites `0xF30-0xF5F`.

**RamFan2 format** (for models with 3 fans): Uses separate address ranges for Fan1 (`0xF00+`), Fan2 (`0xF50+`), Fan3 (`0xFA0+`).

---

## 5. Battery Write Sequences

Registers: `0x7A6` (mode), `0x4AB` (SOC), `0x7B9`/`0x7D0` (charge limits). See 13.1, 13.6.

**Mode values:**

| Mode | Value | Binary |
|------|-------|--------|
| High Capacity (Performance) | `0x08` | 00**00**1000 |
| Balanced | `0x18` | 00**01**1000 |
| Stationary (Healthy) | `0x28` | 00**10**1000 |

**Write:** `WriteBitfield(0x7A6, 0x38, mode_value)`

---

## 6. Logo LED (EC + HID -- both required)

The logo requires BOTH EC writes AND USB HID commands. EC stores state for firmware persistence; HID commands the ITE controller to physically change the LED. EC writes alone do NOT change the LED.

EC Registers: `0x767` (trigger), `0x769`-`0x76B` (RGB), `0x78C` (commit), `0x7AB` (brightness), `0x7C5` bit 5 (logo flag). See 13.7.

**EC 0x7AB brightness encoding:**

| Level | Value | HID Equivalent | Meaning |
|-------|-------|----------------|---------|
| 0 | `0x01` | 0% | Off (bit 0 = LED exists) |
| 1 | `0x21` | 25% | Low (bit 5) |
| 2 | `0x41` | 50% | Medium (bit 6) |
| 3 | `0x81` | 100% | High (bit 7) |

Note: 75% level (value `0x61`) was inferred from the bit pattern but never observed in Frida captures. The app implements 4 levels: off/low/medium/high.

**EC RGB scaling:** `EC_value = round(HID_value * 50 / 255)`. E.g. HID 0xFF -> EC 0x32 (50).

### Turn ON sequence (HID + EC)
```
HID: [00 1A 03 01 04 00 00 00 01]          -- Power ON zone 3
HID: [00 14 03 01 R  G  B  00 00]          -- Set color
HID: [00 08 23 01 01 BRIGHT 01 00 00]      -- Apply with brightness
EC:  Write(0x767, 0x00)                     -- Clear trigger (*)
EC:  WriteBitfield(0x7C5, 0x20, 0x20)      -- Set logo flag bit 5 (*)
EC:  Write(0x769, R)  Write(0x76A, G)  Write(0x76B, B)
EC:  Write(0x7AB, brightness_encoding)
EC:  Write(0x78C, 0x10)                     -- Commit
```
(*) 0x767 and 0x7C5 bit 5 are written by our app for robustness but were NOT observed in the OEM per-change Frida capture. The OEM may only set these during init. The remaining EC writes (0x769-76B, 0x7AB, 0x78C) match the OEM capture exactly.

### Turn OFF sequence
```
HID: [00 12 00 03 00 00 00 00 00]          -- Reset
HID: [00 08 05 00 00 00 00 00 00]          -- Stop effects
HID: [00 08 01 03 00 00 00 00 00]          -- Clear zone 3
HID: [00 1A 03 00 00 00 00 00 01]          -- Power OFF
EC:  Write(0x7AB, 0x01)                     -- Off
EC:  Write(0x78C, 0x10)                     -- Commit
```

---

## 7. ITE USB HID Protocol

All lighting (keyboard, logo, light bar) uses ITE USB HID feature reports. Reports are 9 bytes: `[ReportID=0x00, Command, ...params...]`.

### Device Identification

- VID: `0x048D` (ITE Tech)
- Filter: `FeatureReportByteLength == 9`
- Zone mapping by HID usage value
- `HIDP_STATUS_SUCCESS` = `0x00110000` (not 0)

**Device topology (IDY_7Y):**

| PID | Interface | Usage | UsagePage | Zone |
|-----|-----------|-------|-----------|------|
| `0x600B` | mi_01 | `0x0001` | `0xFF03` | 0 (Keyboard) |
| `0x7001` | mi_01 | `0x0002` | `0xFF03` | 2 (Light Bar) + 3 (Logo, separate handle) |

### 4 Zones

| Zone | Zone Number | Zone CMD ID | Physical Device | Transport |
|------|-------------|-------------|-----------------|-----------|
| Keyboard | 0 | `0x02` | Keyboard backlight | HID only |
| (unused) | 1 | `0x21` | Unknown (seen during mode switch only) | -- |
| Light Bar | 2 | `0x22` | Front/hinge strip | HID only |
| Logo | 3 | `0x23` | Rear cover LED | HID + EC |

Zone CMD ID formula: Keyboard = `0x02` (special case); others = `0x20 + Zone Number`.

### HID Commands

#### Command 0x1A -- Power Control
```
[00 1A ZONE_NUM POWER MODE 00 00 00 SAVE]
```
| Byte | Meaning | Values |
|------|---------|--------|
| 2 | Zone Number | 0x00 (KB), 0x02 (LB), 0x03 (Logo) |
| 3 | Power | 0x00=off, 0x01=on |
| 4 | Mode | 0x00 when off, 0x04 when on |
| 8 | Save | 0x01 (always persists to BIOS) |

#### Command 0x14 -- Set Color
```
[00 14 ZONE_NUM TARGET R G B 00 SAVE]
```
| Byte | Meaning | Values |
|------|---------|--------|
| 2 | Zone Number | 0x00 (KB), 0x02 (LB), 0x03 (Logo) |
| 3 | Target | 0x01=volatile, 0x09=startup/BIOS default |
| 4-6 | R, G, B | 0x00-0xFF |
| 8 | Save | 0x00=volatile, 0x01=persist |

#### Command 0x08 -- Apply Effect
```
[00 08 ZONE_CMD_ID POWER EFFECT BRIGHTNESS SPEED PARAM SAVE]
```
| Byte | Meaning | Values |
|------|---------|--------|
| 2 | Zone CMD ID | 0x02 (KB), 0x22 (LB), 0x23 (Logo) |
| 3 | Power state | 0x00=off, 0x01=on |
| 4 | Effect mode | See per-zone table |
| 5 | Brightness | See brightness scales below |
| 6 | Speed/param | Zone-specific |
| 8 | Save flag | 0x00=volatile, 0x01=persist |

**Per-zone effect parameters:**

| Zone | Zone CMD ID | Effect | Speed | Brightness |
|------|-------------|--------|-------|------------|
| Keyboard | `0x02` | `0x07` | `0x08` | Custom scale (see below) |
| Light Bar | `0x22` | `0x01` (static) | `0x01` | 0x00-0x64 (percent) |
| Logo | `0x23` | `0x01` (static) | `0x01` | 0x00-0x64 (percent) |

**Sub-commands (encoded in byte 2):**
- `0x05`: Stop all effects -- `[00 08 05 00 00 00 00 00 00]`
- `0x01`: Set zone mode off -- `[00 08 01 ZONE_NUM 00 00 00 00 00]`

#### Command 0x12 -- Reset/Prepare
```
[00 12 00 PARAM1 00 00 00 00 00]
```
Always `[00 12 00 03 00 00 00 00 00]` before OFF sequences.

#### Command 0x09 -- Query/Sync
```
[00 09 ZONE_CMD_ID 00 00 00 00 00 00]
```
Sent before keyboard OFF (other zones don't use this).

### Brightness Scales

**Logo / Light Bar (standard percent):**

| Percent | HID Value |
|---------|-----------|
| 0% | `0x00` |
| 25% | `0x19` |
| 50% | `0x32` |
| 75% | `0x4B` |
| 100% | `0x64` |

**Keyboard (custom scale):**

| Level | HID Value | Formula |
|-------|-----------|---------|
| 1 (min) | `0x08` | `8 + (level-1) * 14` |
| 2 | `0x16` | |
| 3 | `0x24` | |
| 4 (max) | `0x32` | |

### Standard Sequences

#### Turn ON any zone
```
1. [00 1A ZONE_NUM 01 04 00 00 00 01]       -- Power ON + save
2. [00 14 ZONE_NUM 01 RR GG BB 00 00]       -- Set color
3. [00 08 ZONE_CMD_ID 01 EFF BR SPD 00 00]  -- Apply effect
4. (Logo only: EC writes -- see section 6)
```

#### Turn OFF any zone
```
1. (Keyboard only: [00 09 02 00 00 00 00 00 00])  -- Query/sync
2. [00 12 00 03 00 00 00 00 00]                     -- Prepare
3. [00 08 05 00 00 00 00 00 00]                     -- Stop all effects
4. [00 08 01 ZONE_NUM 00 00 00 00 00]               -- Zone mode off
5. [00 1A ZONE_NUM 00 00 00 00 00 01]               -- Power OFF + save
6. (Logo only: EC writes -- see section 6)
```

#### Change Color (zone already on)
```
1. [00 14 ZONE_NUM 01 RR GG BB 00 00]       -- Set new color
2. [00 08 ZONE_CMD_ID 01 EFF BR SPD 00 00]  -- Re-apply (same brightness)
```

#### Change Brightness (zone already on)
```
1. [00 14 ZONE_NUM 01 RR GG BB 00 00]       -- Re-send color (unchanged)
2. [00 08 ZONE_CMD_ID 01 EFF BR SPD 00 00]  -- Apply with new brightness
```

#### Save to BIOS ("Startup Mode")
```
1. (KB only: [00 14 ZONE_NUM 09 RR GG BB 00 00])   -- Save startup color (byte 3=0x09)
2. [00 1A ZONE_NUM 01 04 00 00 00 01]                -- Power on + save
3. [00 14 ZONE_NUM 01 RR GG BB 00 01]                -- Set color + save (byte 8=1)
4. [00 08 ZONE_CMD_ID 01 EFF BR SPD 00 01]           -- Apply + save (byte 8=1)
```

**Save flag locations:**

| Command | Normal | Save to BIOS |
|---------|--------|--------------|
| 0x14 (Set Color) | byte 3 = `0x01` | byte 3 = `0x09` (KB only) |
| 0x08 (Apply) | byte 8 = `0x00` | byte 8 = `0x01` (all zones) |

---

## 8. WriteGuard Rules

### Allowlist (IDY_7Y -- 115 addresses)

| Category | Addresses |
|----------|-----------|
| CPU power | `0x783`, `0x784`, `0x785`, `0x786` |
| GPU power | `0x743`, `0x744`, `0x745`, `0x746` |
| Fan control | `0x751`, `0x7C5`, `0x787` |
| Fan table CPU | `0xF00`-`0xF2F` (48) |
| Fan table GPU | `0xF30`-`0xF5F` (48) |
| Battery | `0x7A6` |
| Logo/Lighting | `0x767`, `0x769`, `0x76A`, `0x76B`, `0x7AB`, `0x78C` |
| Service init | `0x741` (activation), `0x7C6` (fan curve mode flag) |

### Read-Modify-Write Registers

| Register | Mask | Values |
|----------|------|--------|
| `0x7C5` | `0x80` | Fan curve: 0x80=on, 0x00=off |
| `0x7C5` | `0x20` | Logo flag: 0x20=on |
| `0x7A6` | `0x38` | Battery: 0x08=perf, 0x18=balanced, 0x28=stationary |
| `0x743` | `0x07` | Dynamic Boost: 0x07=on, 0x05=off |

### Verification Rules

| Register | Rule |
|----------|------|
| `0x786` (CPU temp write) | Redirect: read `0x463`, compare low 7 bits only (`readback & 0x7F == written & 0x7F`). Readback settles on EC polling cycle (~300-900ms) — use retry with backoff. |
| `0x767` (trigger clear) | Skip verification (fire-and-forget) |
| `0x78C` (logo commit) | Skip verification (write-only trigger) |
| `0x741` (activation) | Skip verification (EC transforms value: write 0x05, reads back 0x01) |
| `0x7C6` (fan curve mode) | Skip verification (mode flag, set to 0x04 on startup and leave) |
| Everything else | Read same address, compare directly |

---

## 9. OEM Write Behavioral Patterns

Observed from Frida capture of GCUService.exe:

1. **Read-before-write:** Always read bitmask registers before writing (`0x7C5`, `0x7A6`, `0x743`, `0x7AB`).
2. **Group writes:** PL1/PL2/PL4 always written as a group of 3+1 (plus temp). GPU TGP/DB as a group of 4.
3. **Double-write:** PL changes produce two identical batches. Fan table is also double-written with a `0x7C6` apply flag between. Purpose unclear -- possibly firmware handshake.
4. **LED capability check:** OEM reads `0x766` before every HID operation (always `0x93`).
5. **EC RGB scaling:** Logo color stored at ~20% scale: `EC_value = round(HID_value * 50 / 255)`.
6. **Polling registers:** OEM app continuously reads (~250ms): `0x7CC` (watchdog), `0x727` (e-shutter), `0x7CA`, `0x74C`, `0x766`, `0x7C5`, `0x7A6`.
7. **Mode switch:** Writes 100+ registers -- PL, TGP, full fan table, lighting, apply flags (`0x7C6`). We do NOT replicate mode switching; we write individual features.

---

## 10. Service Startup Init Sequence

Captured via Frida hook of GCUService.exe during GCUBridge service start (2026-03-31). Full capture in `service_init_capture.md`. Init takes ~20s, after which only 0x7CC polling remains.

### Required for standalone operation (without OEM service)

Three mechanisms enable standalone operation:

**1. UEFI NVRAM flags (one-time, auto-set by MechControl):**
The BIOS reads `OemMagicVariable` (180 bytes, GUID `{9f33f85c-13ca-4fd1-9c4a-96217722c593}`) at boot to initialize the EC. MechControl sets four fields in this struct on first launch:
- `[0x2F] PowerMode = 3` (Custom)
- `[0x33] MemoryOverClockSwitch = 1`
- `[0x5D] ApExistFlag = 1`
- `[0x61] ApUseFlag = 1`

Pre-write sanity checks: size must be exactly 180 bytes, PowerMode must be 0-3, boolean fields must be 0 or 1. Write is verified by readback. Not strictly required for operation (MechControl's EC init handles everything at runtime) but ensures the EC boots into a sane state before the app launches.

**2. EC power management activation (0x726, 0x727):**
Setting bit 7 of `0x726` (ADDR_AP_OEM_BYTE9) and bit 6 of `0x727` (DSDT field `CUME`) activates the EC's PECI power management loop. Without these bits, PL register values sit in EC RAM but are never applied to the CPU via PECI — the CPU stays at its default 75W regardless of PL1/PL2 settings. Written as read-modify-write (OR) every startup. These persist in EC microcontroller NVRAM across reboots, but clearing them immediately deactivates power management without a reboot.

**3. Activation byte (0x741):**
EC clears this to 0x00 when the OEM service stops. While 0x741=0, the EC ignores writes to PL/fan/GPU registers (simple direct-control registers like fan boost 0x751 still work). Write 0x05 on app startup (EC transforms to 0x01). Required for PL and GPU writes to take effect. Post-init verification: if readback is 0x01, the EC is actively processing.

**Fan curve mode flag (0x7C6):** Must be set to 0x04 and **left set** for the EC to use the custom fan table (0xF00-0xF5F). This is a persistent mode flag, not a trigger. Without it, the EC ignores fan table writes and runs fans at ~26% default. Not needed for PL/GPU writes — only for fan curves.

### MechControl init sequence (9 steps)

```
1. Write(0x709, 0x05), Write(0x708, 0x01)  -- EC handshake
2. Write(0x741, 0x05)                       -- EC activation
3. Write(0x748, 0x81)                       -- feature enable
4. Write(0x726, Read(0x726) | 0x80)         -- power management activation
   Write(0x727, Read(0x727) | 0x40)         -- CUME bit (PECI loop enable)
5. Write(0x7C5, Read(0x7C5) | 0xE0)        -- fan curve enable
6. Write fan table (0xF00-0xF5F)            -- CPU+GPU curves
7. Write(0x7C7, 0x0C)                       -- fan curve parameter
8. Write(0x7C6, 0x04)                       -- fan curve commit
9. Write(0x706, 0x41)                       -- BIOS_CTRL init complete
-- Verify: Read(0x741) == 0x01              -- EC active confirmation
```

### Full OEM init sequence (for reference)

```
-- Phase 1: Handshake (+7s)
Write(0x709, 0x05)              -- unlock step 1
Write(0x708, 0x01)              -- unlock step 2, verify reads 0x01

-- Phase 2: Activation (+14s)
Read(0x741) -> Write(0x741, 0x05)   -- set activation byte
Write(0x748, 0x81)              -- feature enable (written 4x total)
Write(0x766, Read(0x766))       -- write-back Support2
Write(0x749, 0x00)              -- clear command registers
Write(0x74A, 0x00)
Write(0x74B, 0x00)
Write(0x767, 0x00)              -- clear trigger
Write(0x74E, 0x00)

-- Phase 3: Apply settings (+17s)
Write(0x726, Read(0x726) | 0x80)    -- power management byte 9
Write(0x727, Read(0x727) | 0x40)    -- CUME (PECI loop enable)
Write(0x7C5, Read(0x7C5) | 0xE0)   -- fan curve + logo + bit 6 (all three)
Write PL1/PL2/PL4/TCC           -- power limits
Write fan table (0xF00-0xF5F)    -- full CPU+GPU curves
Write TGP/DB/DB_MAX              -- GPU power
Write(0x787, 0x81)               -- fan switch speed
Write(0x743, 0x07)               -- DB_CTRL (TGP+DB on)
Write(0x7C6, 0x04)               -- APPLY
Write(0x7C6, 0x00)               -- clear

-- Phase 4: Double-write (+19s)
[Repeat entire fan table + PL + GPU write]
Write(0x7C6, 0x04)               -- APPLY again
Write(0x7C6, 0x00)

-- Phase 5: Logo LED + finalize
Write logo RGB + brightness via EC + HID
Write(0x706, 0x41)               -- BIOS_CTRL (init complete)

-- Phase 6: Steady state
Read(0x7CC) every ~1.5s          -- watchdog keepalive
```

### 0x7C5 bit 6

OEM writes 0x7C5 = 0xE0 (bits 7+6+5 all set). We previously only set bit 7 (fan curve) and bit 5 (logo flag). **Bit 6 is set by the OEM during init** -- purpose unknown but may be a "service managed" or "custom profile active" flag.

### Additional IOCTL codes observed during init

| IOCTL | Input | Output | Purpose |
|-------|-------|--------|---------|
| `0x9C40A4A0` | `[00 00 00 00]` | `[86 80 1C 7D]` | PCI device ID read (0x8086:0x7D1C = Intel PCH) |
| `0x9C40A500` | `[BB 00..0B]` | 4 bytes each | ECSpec batch config read (12 indices) |

---

## 11. Complete EC Register Map

Complete reference of every known register, organized by functional group. Includes all entries from ecRegisters (131), systemRegisters (90), fanTableRegisters (3 formats), and modeConstants (24). Registers documented in earlier sections are repeated here for completeness.

Legend: R = read-only, W = write-only, RW = read-write, F&F = fire-and-forget (write-only trigger, no readback).

---

### 11.1 ACPI Battery (0x402-0x4AB)

Standard ACPI BIF/BST registers, SOC, cycle count, temperature, power source.

These registers live in the DSDT `OperationRegion ECXP` mapped as `SystemMemory` at `ECMA+0x400`. All 16-bit pairs are **x86 little-endian**: the lower address holds the **low** byte. Read as `byte[addr] | (byte[addr+1] << 8)`.

DSDT `_BIF` sets Power Unit = 1 (**mAh**), so capacity values are mAh and rate values are mA. Voltage is always mV. Temperature is SBS standard 0.1 Kelvin.

**Note:** The OEM register names (BYTE1/BYTE2) label BYTE1 as "high byte" — this is misleading for these ACPI registers. The actual byte order is little-endian. Fan RPM registers (section 11.2) use the opposite convention (big-endian); see the comment in `EcMonitor.cs`.

| Address | R/W | OEM Name | Encoding | Description |
|---------|-----|----------|----------|-------------|
| `0x402-0x403` | R | ADDR_EC_BIF_DC | 16-bit LE, mAh | Battery design capacity (DSDT XIF1) |
| `0x404-0x405` | R | (ADDR_EC_BIF_LFC) | 16-bit LE, mAh | Last full charge capacity (DSDT XIF2). SOH% = LFC / DC * 100. |
| `0x408-0x409` | R | ADDR_EC_BIF_DV | 16-bit LE, mV | Battery design voltage (DSDT XIF4) |
| `0x434-0x435` | R | ADDR_EC_BST_BPR | 16-bit LE, mA | Battery present rate — charge/discharge (DSDT XST1) |
| `0x436-0x437` | R | ADDR_EC_BST_BRC | 16-bit LE, mAh | Battery remaining capacity (DSDT XST2) |
| `0x438-0x439` | R | ADDR_EC_BST_BPV | 16-bit LE, mV | Battery present voltage (DSDT XST3) |
| `0x490` | R | ecPowSource | Bit 0 (DSDT ACIC) | Power source: bit 0 = AC connected |
| `0x494` | R | ADDR_BATTERY_ALERT_BYTE | Byte | Battery alert flags |
| `0x4A2-0x4A3` | R | ecBt1Temperature | 16-bit LE, 0.1K | Battery temperature. Celsius = (raw / 10) - 273.15 |
| `0x4A6-0x4A7` | R | ADDR_EC_BT1CycleCount | 16-bit LE | Battery cycle count (DSDT CYCN) |
| `0x4AB` | R | ecBt1RSOC | Direct % | Battery relative state of charge |

---

### 11.2 System Identity (0x43E-0x49F)

EC version, system ID, live sensor temps, BIOS info, fan RPM, silent mode.

| Address | R/W | OEM Name | Description |
|---------|-----|----------|-------------|
| `0x43E` | R | (cpuTemp) | CPU temperature (direct degrees C, EC sensor) |
| `0x44F` | R | (gpuTemp) | GPU temperature (direct degrees C, EC sensor) |
| `0x454` | R | ecOEM_EC_VER | EC firmware version |
| `0x456` | R | ADDR_SystemID | System identification byte |
| `0x45B` | R | ADDR_SILENTMODE_STATUS_BYTE | Silent mode status (0=off, nonzero=active) |
| `0x463` | R | (cpuTempLimit read) | CPU temp limit readback. Low 7 bits are the offset: `105 - (reg & 0x7F)` = degrees C. Bit 7 is EC firmware bookkeeping (the commit flag mirrored from 0x786) — may be set or clear. Write via 0x786. |
| `0x464` | R | ADDR_EC_MAIN_FAN_RPM_BYTE1 | CPU fan RPM high byte (pair with 0x465). RPM = `(0x464 << 8) \| 0x465` |
| `0x465` | R | ADDR_EC_MAIN_FAN_RPM_BYTE2 | CPU fan RPM low byte (pair with 0x464) |
| `0x466` | R | ADDR_EC_BIOS_INFO5 | BIOS info register 5 |
| `0x46B` | R | ADDR_EC_SECOND_FAN_RPM_BYTE2 | GPU fan RPM low byte (pair with 0x46C). RPM = `(0x46C << 8) \| 0x46B` |
| `0x46C` | R | ADDR_EC_SECOND_FAN_RPM_BYTE1 | GPU fan RPM high byte (pair with 0x46B) |
| `0x476` | R | BIOSFuncReg | BIOS function register |
| `0x47D` | R | ecOEM_SUB_VER2 | EC sub-version 2 |
| `0x49F` | R | ADDR_BIOS_INFO_3_BYTE | BIOS info byte 3 |

---

### 11.3 BIOS / Service Init (0x706-0x74E)

Handshake, activation, feature enable, command clears, BIOS control.

| Address | R/W | OEM Name | Description |
|---------|-----|----------|-------------|
| `0x706` | W | ADDR_AP_BIOS_CONTROL_BYTE | BIOS control. Write 0x41 at end of init (signals init complete). |
| `0x708` | RW | (handshake step 2) | Unlock step 2. Write 0x01 after 0x709. Verify reads 0x01. |
| `0x709` | W | (handshake step 1) | Unlock step 1. Write 0x05 before 0x708. |
| `0x726` | RW | ADDR_AP_OEM_BYTE9 | **Power management activation.** Bit 7 must be set (`\|= 0x80`) to enable EC PECI power management loop. Persists in EC NVRAM. Clearing deactivates PL enforcement immediately. |
| `0x727` | RW | ADDR_AP_OEM_BYTE10 / ADDR_CPU_DOUBLE_FLAG_SUPPORT / ADDR_ESHUTTER_STATUS | **DSDT fields:** bit 6 = `CUME` (CPU Management Enable), bit 7 = `PL4D`. Bit 6 must be set (`\|= 0x40`) alongside 0x726 bit 7 to activate PECI loop. Also polled by OEM for e-shutter status. |
| `0x728` | RW | ADDR_AP_OEM_BYTEB | OEM byte B |
| `0x740` | R | ADDR_PROJECT_ID_BYTE | Model/project ID. `raw >> 1` = enum value (26>>1 = 13 = IDY_7Y). |
| `0x741` | RW | ADDR_AP_OEM_BYTE / ADDR_FAN_ALERT_BYTE | Activation byte + fan alert. Write 0x05 on startup (EC transforms to 0x01). EC clears on service disconnect. |
| `0x742` | R | ADDR_SUPPORT_BYTE5 | Support flags byte 5 (reads 0x22) |
| `0x748` | W | ADDR_LIGHTBAR_CONTROL_BYTE | Feature enable / lightbar control. Write 0x81 during init (4x). Also see lighting group. |
| `0x749` | W | ADDR_REDBAR_CONTROL_BYTE | Red bar control / command clear 1. Write 0x00 during init. |
| `0x74A` | W | ADDR_GREENBAR_CONTROL_BYTE | Green bar control / command clear 2. Write 0x00 during init. |
| `0x74B` | W | ADDR_BLUEBAR_CONTROL_BYTE | Blue bar control / command clear 3. Write 0x00 during init. |
| `0x74C` | R | ADDR_OEMSERVICE_PROJECT_ID_BYTE | OEM service project ID (polled by OEM app) |
| `0x74E` | W | ADDR_BIOS_OEM_BYTE | BIOS OEM byte / command clear 4. Write 0x00 during init. |

---

### 11.4 CPU Power (0x730-0x786)

Mode defaults (gaming/office/battery saver), live PL1/PL2/PL4, VRM current, temp limit.

| Address | R/W | OEM Name | Description |
|---------|-----|----------|-------------|
| `0x730` | R | ADDR_GAMING_PL1_DEFAULT_VALUE | Gaming mode PL1 default (watts) |
| `0x731` | R | ADDR_GAMING_PL2_DEFAULT_VALUE | Gaming mode PL2 default (watts) |
| `0x732` | R | ADDR_GAMING_PL4_DEFAULT_VALUE | Gaming mode PL4 default (byte x2 = watts) |
| `0x733` | R | ADDR_GAMING_D_DEFAULT_VALUE | Gaming mode "D" default value |
| `0x734` | R | ADDR_OFFICE_PL1_DEFAULT_VALUE | Office mode PL1 default (watts) |
| `0x735` | R | ADDR_OFFICE_PL2_DEFAULT_VALUE | Office mode PL2 default (watts) |
| `0x736` | R | ADDR_OFFICE_PL4_DEFAULT_VALUE | Office mode PL4 default (byte x2 = watts) |
| `0x737` | R | ADDR_OFFICE_D_DEFAULT_VALUE | Office mode "D" default value |
| `0x738` | RW | ADDR_MYFAN3_CPU_TAU | MyFan3 CPU TAU (time constant for power) |
| `0x753` | RW | ADDR_CPU_VRM_CURRENT_LIMIT_BYTE | CPU VRM current limit |
| `0x754` | RW | ADDR_CPU_VRM_MAXI_CURRENT_LIMIT_BYTE | CPU VRM maximum current limit |
| `0x783` | RW | ADDR_PL1_SETTING_VALUE | Live PL1 setting (direct byte = watts, 10-210) |
| `0x784` | RW | ADDR_PL2_SETTING_VALUE | Live PL2 setting (direct byte = watts, 10-210) |
| `0x785` | RW | ADDR_PL4_SETTING_VALUE | Live PL4 setting (byte x2 = watts when double flag=1, 10-420) |
| `0x786` | W | ADDR_L1_PWM_DEFAULT_MYFAN3 / (cpuTempLimit write) | CPU temp limit write. Bit 7 = commit/enable (required — without it the EC stores the value but does not enforce the limit). Intel inverted_offset: `0x80 \| (105 - temp)`. AMD direct: `0x80 \| temp`. Readback via 0x463; compare low 7 bits only. Also MyFan3 L1 PWM default in fan context. |

---

### 11.5 Fan Control (0x747-0x7A0)

MyFan2/3 PWM levels, fan duty, RPM, control mode, status, fan switch speed, MyFanI min speed.

| Address | R/W | OEM Name | Description |
|---------|-----|----------|-------------|
| `0x743` | RW | ADDR_MYFAN2_L1_PWM / ADDR_ConfigurableTGP_DynamicBoost_CTRL_BYTE | MyFan2 level 1 PWM. Also GPU TGP/DB control bitfield (bit0+2=TGP, bit1=DB). |
| `0x744` | RW | ADDR_MYFAN2_L2_PWM / ADDR_ConfigurableTGP_VALUE | MyFan2 level 2 PWM. Also configurable TGP value (reg + 95 = watts). |
| `0x745` | RW | ADDR_MYFAN2_L3_PWM / ADDR_DynamicBoost_TotalProcessingPowerTarget_VALUE | MyFan2 level 3 PWM. Also DB total power target (255=on, 0=off). |
| `0x746` | RW | ADDR_MYFAN2_L4_PWM / ADDR_DynamicBoost_MaxinumTGP_VALUE | MyFan2 level 4 PWM. Also DB max TGP (direct watts, 5-25). |
| `0x747` | RW | ADDR_MYFAN2_L5_PWM | MyFan2 level 5 PWM |
| `0x751` | RW | ADDR_MAFAN_CONTROL_BYTE | Fan control mode. 0x00=normal, 0x40=full blast (bit 6). |
| `0x75B` | R | ADDR_EC_MAIN_FAN_L_DUTY_BYTE | CPU fan duty (raw / 2 = percent, 0-200) |
| `0x75C` | R | ADDR_EC_MAIN_FAN_R_DUTY_BYTE | GPU fan duty (raw / 2 = percent, 0-200) |
| `0x75D` | RW | ADDR_TRIGGER_BYTE2 | Secondary trigger byte |
| `0x765` | R | ADDR_SUPPORT_BYTE1 | Feature support flags (reads 0xE1) |
| `0x766` | RW | ADDR_SUPPORT_BYTE2 / Light_ChinaMode | LED capability flags (reads 0x93). OEM writes back during init. Also China-mode lighting. |
| `0x767` | F&F | ADDR_TRIGGER_BYTE | Logo LED trigger clear. Write 0x00 before apply. |
| `0x768` | R | ADDR_STAUTS_BYTE | Status byte. Bit 2 = fan boost active. |
| `0x787` | RW | ADDR_L2_PWM_DEFAULT_MYFAN3 / ADDR_TimAP_FanSwitchSpeedT100mSec (RamFan1p5) | MyFan3 L2 PWM default. Also fan switch speed (bit 7=enable, bits 0-6=time x 100ms). |
| `0x788` | RW | ADDR_L3_PWM_DEFAULT_MYFAN3 | MyFan3 level 3 PWM default |
| `0x789` | RW | ADDR_L1_PWM_DEFAULT_MYFAN2 / ADDR_L4_PWM_DEFAULT_MYFAN3 | MyFan2 L1 PWM default. Also MyFan3 L4 PWM default (dual-mapped). |
| `0x78A` | RW | ADDR_L2_PWM_DEFAULT_MYFAN2 / ADDR_L5_PWM_DEFAULT_MYFAN3 | MyFan2 L2 PWM default. Also MyFan3 L5 PWM default (dual-mapped). |
| `0x78B` | RW | ADDR_L3_PWM_DEFAULT_MYFAN2 / ADDR_MYFAN3_GPU_SETTING | MyFan2 L3 PWM default. Also MyFan3 GPU setting. |
| `0x78C` | F&F | ADDR_L4_PWM_DEFAULT_MYFAN2 / ADDR_AP_OEM_BYTE2 / ADDR_SINGLEKBL_ENABLE / ADDR_AP_EC_LOGO_CONFIRM | Multi-use: MyFan2 L4 PWM, OEM byte 2, single-zone KBL enable, logo commit (write 0x10 to apply). |
| `0x78D` | RW | ADDR_L5_PWM_DEFAULT_MYFAN2 | MyFan2 level 5 PWM default |
| `0x78E` | R | ADDR_SINGLEKBL_SUPPORTPOWER / ADDR_SUPPORT_BYTE6 | Single-zone keyboard backlight power support. Also support byte 6. |
| `0x79E` | RW | ADDR_MYFANI_MIN_SPEED | MyFanI minimum speed setting |
| `0x79F` | RW | ADDR_MYFANI_MIN_TEMP | MyFanI minimum temperature threshold |
| `0x7A0` | RW | ADDR_MYFANI_EXTRA_SPEED | MyFanI extra speed setting |

---

### 11.6 Battery & Charge (0x7A6-0x7D0)

Battery mode, charge limits, battery saver PL defaults.

| Address | R/W | OEM Name | Description |
|---------|-----|----------|-------------|
| `0x7A6` | RW | ADDR_AP_OEM_BYTE4 | Battery mode. Bits 4-5 = mode (0x08=perf, 0x18=balanced, 0x28=healthy). Read-modify-write with mask 0x38. |
| `0x7A7` | R | ADDR_BATTERYSAVER_PL1_DEFAULT_VALUE | Battery saver mode PL1 default (watts) |
| `0x7A8` | R | ADDR_BATTERYSAVER_PL2_DEFAULT_VALUE | Battery saver mode PL2 default (watts) |
| `0x7A9` | R | ADDR_BATTERYSAVER_PL4_DEFAULT_VALUE | Battery saver mode PL4 default (byte x2 = watts) |
| `0x7AA` | R | ADDR_BATTERYSAVER_D_DEFAULT_VALUE | Battery saver mode "D" default value |
| `0x7B9` | RW | ADDR_BATTERY_CHARGE_LIMIT_UP | Battery charge upper limit (reads 0 on IDY_7Y -- OEM app feature broken) |
| `0x7D0` | RW | ADDR_BATTERY_CHARGE_LIMIT_DOWN | Battery charge lower limit (reads 0 on IDY_7Y -- OEM app feature broken) |

---

### 11.7 Logo / Lighting (0x748-0x78C, 0xD00, 0xD4E-0xD4F)

RGB bar, logo LED, single-zone keyboard backlight, color calibration.

| Address | R/W | OEM Name | Description |
|---------|-----|----------|-------------|
| `0x748` | W | ADDR_LIGHTBAR_CONTROL_BYTE | Lightbar / feature enable. Write 0x81 during init (4x). |
| `0x749` | W | ADDR_REDBAR_CONTROL_BYTE | Lightbar red channel / command clear |
| `0x74A` | W | ADDR_GREENBAR_CONTROL_BYTE | Lightbar green channel / command clear |
| `0x74B` | W | ADDR_BLUEBAR_CONTROL_BYTE | Lightbar blue channel / command clear |
| `0x769` | RW | ADDR_RGBKB_LEVEL_R / ADDR_AP_EC_LOGO_R | Logo/keyboard red. EC scales: `round(HID_value * 50 / 255)`. |
| `0x76A` | RW | ADDR_RGBKB_LEVEL_G / ADDR_AP_EC_LOGO_G | Logo/keyboard green. Same scaling as R. |
| `0x76B` | RW | ADDR_RGBKB_LEVEL_B / ADDR_AP_EC_LOGO_B | Logo/keyboard blue. Same scaling as R. |
| `0x76C` | R | ADDR_RGBKB_LEVEL_DEFAULT_R | Default red value for RGB keyboard |
| `0x76D` | R | ADDR_RGBKB_LEVEL_DEFAULT_G | Default green value for RGB keyboard |
| `0x76E` | R | ADDR_RGBKB_LEVEL_DEFAULT_B | Default blue value for RGB keyboard |
| `0x76F` | RW | ADDR_RGBKB_MUSIC_NO | RGB keyboard music mode number (0x00=disable, 0xFE=enable) |
| `0x771` | R | ADDR_ROMID | ROM ID 1 (for hardware identification) |
| `0x772` | R | ADDR_ROMID2 | ROM ID 2 (for hardware identification) |
| `0x782` | RW | ADDR_BIOS_OEM_BYTE2 / Light_SetToChinaMode | BIOS OEM byte 2. Also set-to-China-mode lighting flag. |
| `0x78C` | F&F | ADDR_AP_EC_LOGO_CONFIRM / ADDR_SINGLEKBL_ENABLE | Logo commit trigger: write 0x10 to apply. Also single-zone KBL enable. |
| `0x7AB` | RW | ADDR_AP_EC_LOGO / ADDR_MyFanCCI_Mode_Index | Logo brightness: bits 5-7=level, bit 0=LED exists (0x01=off, 0x21=low, 0x41=med, 0x81=high). Also MyFanCCI mode index. |
| `0xD00` | RW | ADDR_AP_OEM_SingleZone | Single-zone keyboard backlight control |
| `0xD4E` | R | EC_Color_Calibration_Support | Color calibration support flag |
| `0xD4F` | RW | ADDR_AP_MISCSWITCH2 | Miscellaneous switch register 2 |

---

### 11.8 OEM Config (0x7A3-0x7CC)

OEM config bytes, BIOS OEM bytes, watchdog, complex power status.

| Address | R/W | OEM Name | Description |
|---------|-----|----------|-------------|
| `0x7A3` | RW | ADDR_BIOS_OEM_BYTE3 | BIOS OEM byte 3 |
| `0x7A4` | RW | ADDR_AP_BIOS_BYTE | AP BIOS byte |
| `0x7A5` | RW | ADDR_AP_OEM_BYTE3 | AP OEM byte 3 |
| `0x7C5` | RW | ADDR_AP_OEM_BYTE5 | Multi-bitfield: bit 7=fan curve enable, bit 6=service managed(?), bit 5=logo flag. Read-modify-write. |
| `0x7C6` | RW | ADDR_AP_OEM_BYTE6 | Fan curve apply/mode flag. Write 0x04 to activate fan table, EC ignores 0xF00+ while 0x00. |
| `0x7C7` | RW | ADDR_AP_OEM_BYTE7 | OEM byte 7. Written 0x0C during init. Near OEM5/OEM6. |
| `0x7C8` | RW | ADDR_AP_OEM_BYTE8 | OEM byte 8 |
| `0x7CA` | R | ADDR_BIOS_OEM_BYTE8 | BIOS OEM byte 8 (polled by OEM app) |
| `0x7CC` | R | ADDR_COMPLEX_POWER_STATUS | Complex power status / watchdog. OEM polls ~1.5s (reads 0x81). |

---

### 11.9 System Identity Extended (0x7D1-0x7FB)

SSD temp, module IDs, ROM IDs, TCC offset defaults, port ID, liquid cooling.

| Address | R/W | OEM Name | Description |
|---------|-----|----------|-------------|
| `0x7D1` | R | AP_SSDTempr | SSD temperature |
| `0x7D2` | R | ADDR_ModuleID_GPU | GPU module identification |
| `0x7D3` | R | ADDR_ModuleID | CPU module identification |
| `0x7D8` | R | ADDR_GAMING_TCC_OFFSET_DEFAULT_VALUE | Gaming mode TCC offset default |
| `0x7D9` | R | ADDR_OFFICE_TCC_OFFSET_DEFAULT_VALUE | Office mode TCC offset default |
| `0x7DA` | R | ADDR_TURBO_TCC_OFFSET_DEFAULT_VALUE | Turbo mode TCC offset default |
| `0x7E6` | RW | ADDR_LC_FAN_VALUE | Liquid cooling fan value |
| `0x7E7` | RW | ADDR_LC_PUMP_VALUE | Liquid cooling pump value |
| `0x7FB` | RW | ADDR_CHANGE_PORT_ID_WKD | Change port ID (WKD) |

---

### 11.10 Fan Table -- RamFan1 (0xF00-0xF5F)

Basic 2-fan format. 6 base addresses, each spanning 16 bytes (indices 0-15, 10 active + 6 padding).

| Address | R/W | OEM Name | Description |
|---------|-----|----------|-------------|
| `0xF00`+N | RW | ADDR_CPU_FAN_TABLE_TEMP_UP0 | CPU temp UP thresholds (N=0..15). Going up, crossing `tempUp[N]` jumps duty to `duty[N+1]`. Padding entries = `0xFF`. |
| `0xF10`+N | RW | ADDR_CPU_FAN_TABLE_TEMP_DOWN0 | CPU temp DOWN thresholds (N=0..15). Going down, crossing `tempDown[N]` drops duty to `duty[N-1]`. Active entries N=1..active-1 = `tempUp[N-1] - 4` (cross-point 4°C range). Index 0 is **not written** (writing it caused table rejection on some EC versions). First padding slot (N=active) = `tempDown[active-1] + 4` (OEM leftover; EC never consults it). Remaining padding = `0xFF`. |
| `0xF20`+N | RW | ADDR_CPU_FAN_TABLE_DUTY0 | CPU fan duty cycle (N=0..15). raw/2 = percent. Padding = last active entry's raw duty (matches OEM, avoids discontinuity). |
| `0xF30`+N | RW | ADDR_GPU_FAN_TABLE_TEMP_UP0 | GPU temp UP thresholds (N=0..15). Padding entries = `0xFF`. |
| `0xF40`+N | RW | ADDR_GPU_FAN_TABLE_TEMP_DOWN0 | GPU temp DOWN thresholds (N=0..15). Same layout rules as `0xF10` (point 0 not written, cross-point 4°C range, first padding = `tempDown[active-1] + 4`, rest = `0xFF`). |
| `0xF50`+N | RW | ADDR_GPU_FAN_TABLE_DUTY0 | GPU fan duty cycle (N=0..15). raw/2 = percent. Padding = last active entry's raw duty. |

**RamFan1p5 additions** (enhanced format adds timing/status at end of same address range):

| Address | R/W | OEM Name | Description |
|---------|-----|----------|-------------|
| `0xF5D` | R | ADDR_RAMFAN1P5_TABLE_STATUS1 | Fan table status register 1 |
| `0xF5E` | R | ADDR_RAMFAN1P5_TABLE_STATUS2 | Fan table status register 2 |
| `0xF5F` | RW | ADDR_RAMFAN1P5_TABLE_CTRL | Fan table control register (commit/apply) |

---

### 11.11 Fan Table -- RamFan2 (0xF00-0xFF0)

3-fan format with separate Fan1/Fan2/Fan3 curve sets. Each fan has CPU temp up/down + GPU temp up/down + duty.

| Address | R/W | OEM Name | Description |
|---------|-----|----------|-------------|
| `0xF00`+N | RW | ADDR_TABLE_F1_CPU_TEMP_UP0 | Fan1 CPU temp UP thresholds (N=0..15) |
| `0xF10`+N | RW | ADDR_TABLE_F1_CPU_TEMP_DOWN0 | Fan1 CPU temp DOWN thresholds (N=1..15) |
| `0xF20`+N | RW | ADDR_TABLE_F1_DUTY0 | Fan1 duty cycle (N=0..15) |
| `0xF30`+N | RW | ADDR_TABLE_F1_GPU_TEMP_UP0 | Fan1 GPU temp UP thresholds (N=0..15) |
| `0xF40`+N | RW | ADDR_TABLE_F1_GPU_TEMP_DOWN0 | Fan1 GPU temp DOWN thresholds (N=1..15) |
| `0xF50`+N | RW | ADDR_TABLE_F2_CPU_TEMP_UP0 | Fan2 CPU temp UP thresholds (N=0..15) |
| `0xF60`+N | RW | ADDR_TABLE_F2_CPU_TEMP_DOWN0 | Fan2 CPU temp DOWN thresholds (N=1..15) |
| `0xF70`+N | RW | ADDR_TABLE_F2_DUTY0 | Fan2 duty cycle (N=0..15) |
| `0xF80`+N | RW | ADDR_TABLE_F2_GPU_TEMP_UP0 | Fan2 GPU temp UP thresholds (N=0..15) |
| `0xF90`+N | RW | ADDR_TABLE_F2_GPU_TEMP_DOWN0 | Fan2 GPU temp DOWN thresholds (N=1..15) |
| `0xFA0`+N | RW | ADDR_TABLE_F3_CPU_TEMP_UP0 | Fan3 CPU temp UP thresholds (N=0..15) |
| `0xFB0`+N | RW | ADDR_TABLE_F3_CPU_TEMP_DOWN0 | Fan3 CPU temp DOWN thresholds (N=1..15) |
| `0xFC0`+N | RW | ADDR_TABLE_F3_DUTY0 | Fan3 duty cycle (N=0..15) |
| `0xFD0`+N | RW | ADDR_TABLE_F3_GPU_TEMP_UP0 | Fan3 GPU temp UP thresholds (N=0..15) |
| `0xFE0`+N | RW | ADDR_TABLE_F3_GPU_TEMP_DOWN0 | Fan3 GPU temp DOWN thresholds (N=1..15) |

---

### 11.12 Fan CCI Profiles (0x7AB-0x7B2)

Custom Curve Interface mode profiles. 0x7AB is shared with logo brightness.

| Address | R/W | OEM Name | Description |
|---------|-----|----------|-------------|
| `0x7AB` | RW | ADDR_MyFanCCI_Mode_Index | CCI mode index selector (also logo brightness -- context-dependent) |
| `0x7B0` | RW | ADDR_MyFanCCI_Mode_Profile1 | CCI mode profile 1 value |
| `0x7B1` | RW | ADDR_MyFanCCI_Mode_Profile2 | CCI mode profile 2 value |
| `0x7B2` | RW | ADDR_MyFanCCI_Mode_Profile3 | CCI mode profile 3 value |

---

### 11.13 System Events / OSD Codes

All 90 systemRegisters entries. These are event notification constants (scan codes and status addresses), not standard EC registers.

| Code | Name | Description |
|------|------|-------------|
| `0x00` | Disable_RGB_Music | Command: disable RGB music sync mode |
| `0x001` | OSD_CAPSLOCK | OSD event: Caps Lock toggled |
| `0x002` | OSD_NUMLOCK | OSD event: Num Lock toggled |
| `0x003` | OSD_SROLLOCK | OSD event: Scroll Lock toggled |
| `0x004` | OSD_TPON | OSD event: touchpad enabled |
| `0x005` | OSD_TPOFF | OSD event: touchpad disabled |
| `0x006` | OSD_SILENTON | OSD event: silent mode enabled |
| `0x007` | OSD_SILENTOFF | OSD event: silent mode disabled |
| `0x008` | OSD_WLANON | OSD event: WLAN enabled |
| `0x009` | OSD_WLANOFF | OSD event: WLAN disabled |
| `0x00A` | OSD_WINMAXON | OSD event: WinMax enabled |
| `0x00B` | OSD_WINMAXOFF | OSD event: WinMax disabled |
| `0x00C` | OSD_BTON | OSD event: Bluetooth enabled |
| `0x00D` | OSD_BTOFF | OSD event: Bluetooth disabled |
| `0x00E` | OSD_RFON | OSD event: RF on |
| `0x00F` | OSD_RFOFF | OSD event: RF off |
| `0x010` | OSD_3GON | OSD event: 3G/mobile data enabled |
| `0x011` | OSD_3GOFF | OSD event: 3G/mobile data disabled |
| `0x012` | OSD_WEBCAMON | OSD event: webcam enabled |
| `0x013` | OSD_WEBCAMOFF | OSD event: webcam disabled |
| `0x014` | OSD_BRIGHTNESSUP | OSD event: screen brightness up |
| `0x14` | OS_VK_CAPITAL | Virtual key code: Caps Lock |
| `0x015` | OSD_BRIGHTNESSDOWN | OSD event: screen brightness down |
| `0x01A` | OSD_RADIOON | OSD event: radio on |
| `0x01B` | OSD_RADIOOFF | OSD event: radio off |
| `0x031` | OSD_POWERSAVEON | OSD event: power save enabled |
| `0x032` | OSD_POWERSAVEOFF | OSD event: power save disabled |
| `0x034` | OSD_MENU | OSD event: menu key pressed |
| `0x035` | OSD_MUTE | OSD event: mute toggled |
| `0x036` | OSD_VOLUMEDOWN | OSD event: volume down |
| `0x037` | OSD_VOLUMEUP | OSD event: volume up |
| `0x038` | OSD_OSD_MENU_2 | OSD event: secondary menu |
| `0x039` | OSD_BREATH_LED_ON | OSD event: breathing LED effect enabled |
| `0x03A` | OSD_BREATH_LED_OFF | OSD event: breathing LED effect disabled |
| `0x03B` | OSD_KB_LED_LEVEL0 | OSD event: keyboard LED level 0 (off) |
| `0x03C` | OSD_KB_LED_LEVEL1 | OSD event: keyboard LED level 1 |
| `0x03D` | OSD_KB_LED_LEVEL2 | OSD event: keyboard LED level 2 |
| `0x03E` | OSD_KB_LED_LEVEL3 | OSD event: keyboard LED level 3 |
| `0x03F` | OSD_KB_LED_LEVEL4 | OSD event: keyboard LED level 4 (max) |
| `0x040` | OSD_WINKEY_LOCK | OSD event: Windows key locked |
| `0x041` | OSD_WINKEY_UNLOCK | OSD event: Windows key unlocked |
| `0x042` | OSD_MENU_JP | OSD event: menu key (Japanese layout) |
| `0x90` | OS_VK_NUMLOCK | Virtual key code: Num Lock |
| `0x091` | OSD_CAMERAOFF | OSD event: camera disabled |
| `0x090` | OSD_CAMERAON | OSD event: camera enabled |
| `0x91` | OS_VK_SCROLL | Virtual key code: Scroll Lock |
| `0x0A4` | OSD_AIRPLANEMODE | OSD event: airplane mode toggled |
| `0x0A5` | WinKey_Update | Event: Windows key state update |
| `0x0A7` | OSD_FANBOOST_UPDATE | OSD event: fan boost state changed |
| `0x0A9` | OSD_LCD_SW | OSD event: LCD switch (external display toggle) |
| `0x0AA` | TimAP_MyFanOT | Timer event: MyFan over-temperature warning |
| `0x0AB` | OSD_MyBat_ACUpdate | OSD event: battery AC adapter status update |
| `0x0AC` | TimAP_MyBat_HPOff | Timer event: battery high-performance off |
| `0x0AD` | TimAP_ReleaseUSM | Timer event: release ultra-silent mode |
| `0x0AE` | OSD_Battery_Alert | OSD event: battery alert triggered |
| `0x0AF` | TimAP_HaierLB_Sw | Timer event: Haier lightbar switch |
| `0x0B0` | OSD_FanModeSwitch | OSD event: fan mode switched |
| `0x0B3` | BacklightLevelChange | Event: keyboard backlight level changed |
| `0x0B4` | BacklightPowerChange | Event: keyboard backlight power state changed |
| `0x0B7` | TimAP_MicMute_Sw | Timer event: microphone mute switch |
| `0x0B8` | OSD_FnChange | OSD event: Fn key behavior changed |
| `0x0BA` | CallAP | Event: call application (hotkey) |
| `0x0BB` | TimAP_HighTemp | Timer event: high temperature warning |
| `0x0BC` | TimAP_WhisperUpdate | Timer event: whisper mode update |
| `0x0C0` | SC_SwitchOC | Scan code: switch overclock mode |
| `0x0C1` | SC_PumpFanAuto | Scan code: pump/fan auto mode |
| `0x0C2` | TimAP_HWOCScanCode | Timer event: hardware OC scan code |
| `0x0C3` | TimAP_LidClose | Timer event: lid closed |
| `0x0C4` | TimAP_LidOpen | Timer event: lid opened |
| `0x0C7` | PDWarning_Event / TimAP_AC_Chg | PD warning event / AC charger change event |
| `0x0C8` | SC_ThermalProtect | Scan code: thermal protection activated |
| `0x0C9` | SC_CustomMode | Scan code: custom mode toggled |
| `0x0CB` | PHxGPURule1 | Event: GPU power rule 1 (dGPU policy) |
| `0x0CD` | TimAP_ELU_Fn2 | Timer event: ELU function 2 |
| `0x0CE` | TimAP_ELU_Fn3 | Timer event: ELU function 3 |
| `0x0CF` | TimAP_NotifyEShutter | Timer event: e-shutter notification |
| `0x0D0` | TimAP_NotifyFlipScreen | Timer event: flip screen notification |
| `0x0D1` | TimAP_DISPLAY_In | Timer event: display input change |
| `0xFE` | Enable_RGB_Music | Command: enable RGB music sync mode |

---

### 11.14 Mode Constants

The 24 modeConstants values. These are enum/flag values used in software, not EC addresses.

| Code | Name | Description |
|------|------|-------------|
| 0 | FAN_GAMING_MODE | Fan profile: gaming mode |
| 1 | FAN_GAMING_MODE_SMART | Fan profile: gaming smart mode |
| 2 | FAN_GAMING_MODE_PERFENHANCED | Fan profile: gaming performance-enhanced mode |
| 1 | FAN_OFFICE_MODE | Fan profile: office mode |
| 1 | FAN_OFFICE_MODE_BASIC | Fan profile: office basic sub-mode |
| 2 | FAN_OFFICE_MODE_ADVANCED | Fan profile: office advanced sub-mode |
| 2 | FAN_TURBO_MODE | Fan profile: turbo mode |
| 3 | FAN_CUSTOM_MODE | Fan profile: custom mode |
| 0 | FAN_MODE_NORMAL | Fan mode switch: normal |
| 1 | FAN_MODE_BOOST | Fan mode switch: boost |
| 2 | FAN_MODE_CUSTOMIZE | Fan mode switch: customize (custom curve) |
| 0 | FAN_CUSTOMIZEMODE_NORMAL | Custom sub-mode: normal |
| 1 | FAN_CUSTOMIZEMODE_BASIC | Custom sub-mode: basic |
| 2 | FAN_CUSTOMIZEMODE_HIGH | Custom sub-mode: high |
| 1 | POWER_SETTING_MODE_ECO | Power setting: eco mode |
| 2 | POWER_SETTING_MODE_STD | Power setting: standard mode |
| 0 | TURBO_MODE_CPUBOOST | Turbo: CPU boost |
| 1 | TURBO_MODE_1 / TURBO_MODE_MIN | Turbo level 1 (minimum) |
| 2 | TURBO_MODE_2 / TURBO_MODE_DEFAULT | Turbo level 2 (default) |
| 3 | TURBO_MODE_3 | Turbo level 3 |
| 4 | TURBO_MODE_4 / TURBO_MODE_MAX | Turbo level 4 (maximum) |
