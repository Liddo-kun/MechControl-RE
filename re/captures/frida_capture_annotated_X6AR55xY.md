# Frida Full Capture Analysis -- Raw Evidence

> **Note:** This is the raw annotated Frida capture log. For the consolidated reference of all register encodings, write sequences, and HID commands, see `register_reference.md`.

**Log:** `frida_full_capture.log` (1960 lines, captured 2026-03-31 04:08-04:25)
**Process:** GCUService.exe (PID 12652)
**Hooks active:** EC read/write (IOCTL), HidD_SetFeature, HidD_GetFeature, HidD_SetOutputReport, CreateFileW, CoCreateInstance (WMI), WriteFile/ReadFile HID filter. DDC/CI (dxva2.dll) NOT loaded.

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [HID Handle-to-Zone Mapping](#2-hid-handle-to-zone-mapping)
3. [HID Feature Report Protocol](#3-hid-feature-report-protocol)
4. [Feature: Battery Health Mode](#4-battery-health-mode)
5. [Feature: Local Dimming (Mini LED)](#5-local-dimming-mini-led)
6. [Feature: Fan Switch Speed](#6-fan-switch-speed)
7. [Feature: CPU Temperature Limit (TCC Offset)](#7-cpu-temperature-limit-tcc-offset)
8. [Feature: Logo LED (Rear Cover)](#8-logo-led-rear-cover)
9. [Feature: Light Bar](#9-light-bar)
10. [Feature: Keyboard Backlight](#10-keyboard-backlight)
11. [Feature: Super Performance Switch](#11-super-performance-switch)
12. [Feature: IGPU Power Saving Mode](#12-igpu-power-saving-mode)
13. [App Initialization Sequence](#13-app-initialization-sequence)
14. [Background Polling](#14-background-polling)
15. [EC Register Map](#15-ec-register-map)

---

## 1. Executive Summary

| Feature | Transport | Key Register / Handle | Implementable? |
|---|---|---|---|
| Battery Health Mode | EC only | 0x7A6 (bits 5:4 = mode) | YES -- already implemented |
| Local Dimming | WMI only | CoCreateInstance(WbemLocator) | Needs WMI investigation |
| Fan Switch Speed | EC only | 0x787 (bit 7 = enable, 6:0 = time in 100 ms) | YES |
| CPU Temp Limit | EC only | 0x786 (bit 7 = enable, 6:0 = offset from Tjmax) | YES |
| Logo LED | **HID + EC** | h=0x11fc + EC 0x769-76B/0x7AB/0x78C | YES |
| Light Bar | **HID only** | h=0x1154 | YES |
| Keyboard Backlight | **HID only** | h=0x1004 | YES |
| Super Performance | EC only | 0x728 bit 1, 0x746 DB_MAX 5W->25W | YES |
| IGPU Power Saving | EC + HID | 0x748 bits 2,3 + full re-init | Probably not needed |

**Critical finding:** Logo LED, Light Bar, and Keyboard Backlight all use ITE USB HID feature reports. The EC writes for Logo LED are metadata/persistence only -- the HID commands are what physically control the LED. This explains why our EC-only logo implementation didn't work.

---

## 2. HID Handle-to-Zone Mapping

Four HID handles were observed. Each corresponds to a separate ITE USB HID endpoint controlling one LED zone.

| Handle | Zone Number | Zone CMD ID | Physical Device |
|---|---|---|---|
| 0x1004 | 0 | 0x02 | Keyboard backlight |
| 0x11cc | 1 | 0x21 | 4th zone (unknown, possibly hinge/side) |
| 0x1154 | 2 | 0x22 | Light Bar (front/hinge strip) |
| 0x11fc | 3 | 0x23 | Logo LED (rear cover) |

### Zone Number vs Zone CMD ID

These are two different identifiers used in different commands:

| | Zone Number | Zone CMD ID |
|---|---|---|
| **Used in** | cmds 0x14 (Set Color), 0x1A (Power) | cmds 0x08 (Apply), 0x09 (Query/Off) |
| **Range** | 0, 1, 2, 3 | 0x02, 0x21, 0x22, 0x23 |
| **Formula** | Sequential 0-3 | Keyboard = 0x02 (special); others = 0x20 + Zone Number |

**Quick reference:**

| Zone | Zone Number | Zone CMD ID |
|---|---|---|
| Keyboard | 0 | 0x02 |
| Zone 1 | 1 | 0x21 |
| Light Bar | 2 | 0x22 |
| Logo | 3 | 0x23 |

Evidence for mapping:
- Logo actions only fire HID on 0x11fc, accompanied by EC writes to 0x769/76A/76B/7AB
- Light Bar actions only fire HID on 0x1154, NO EC writes
- Keyboard actions only fire HID on 0x1004, NO EC writes
- Init sequence powers off all 4 handles, then re-enables with per-zone parameters

---

## 3. HID Feature Report Protocol

All feature reports are **9 bytes:** `[ReportID=0x00, Command, Param1, Param2, Param3, Param4, Param5, Param6, Param7]`

### 3.1 Command Reference

| Cmd | Name | Byte Layout | Description |
|---|---|---|---|
| 0x08 | Apply Effect | `[00 08 ZoneCmdID Power Effect Brightness Speed Param Save]` | Main control; sub-cmds 0x05 (stop all) and 0x01 (zone mode off) |
| 0x09 | Query / Zone Off | `[00 09 ZoneCmdID 00 00 00 00 00 00]` | Sent before OFF sequences (keyboard) and during init |
| 0x12 | Reset / Prepare | `[00 12 00 Param1 Param2 00 00 00 00]` | Prepare for update; always precedes OFF sequences |
| 0x14 | Set Color | `[00 14 ZoneNum Target R G B 00 Save]` | Target: 0x01=volatile, 0x09=startup. Save: 0x00/0x01 |
| 0x1A | Power Control | `[00 1A ZoneNum Power Mode 00 00 00 Save]` | Power: 0/1. Mode: 0x00 off, 0x04 on. Save: 0x01 always |

### 3.2 Command 0x09 -- Query / Zone Off

```
[00 09 ZONE_CMD_ID 00 00 00 00 00 00]
```

| Byte | Meaning | Values |
|---|---|---|
| 1 | Command | 0x09 |
| 2 | Zone CMD ID | 0x02 (KB), 0x21 (zone 1), 0x22 (LB), 0x23 (Logo) |
| 3-8 | Padding | All 0x00 |

Raw examples (init sequence -- power off all zones):
```
h=0x1004: [00 09 02 00 00 00 00 00 00]   <- keyboard off
h=0x1154: [00 09 22 00 00 00 00 00 00]   <- light bar off
h=0x11fc: [00 09 23 00 00 00 00 00 00]   <- logo off
h=0x11cc: [00 09 21 00 00 00 00 00 00]   <- zone 1 off
```

### 3.3 Command 0x14 -- Set Color

```
[00 14 ZONE_NUM TARGET R G B 00 SAVE]
```

| Byte | Meaning | Values |
|---|---|---|
| 1 | Command | 0x14 |
| 2 | Zone Number | 0x00 (KB), 0x01 (zone 1), 0x02 (LB), 0x03 (Logo) |
| 3 | Target | 0x01 = current/volatile, 0x09 = startup/BIOS default |
| 4 | Red | 0x00-0xFF |
| 5 | Green | 0x00-0xFF |
| 6 | Blue | 0x00-0xFF |
| 7 | Padding | 0x00 |
| 8 | Save flag | 0x00 = volatile, 0x01 = persist to BIOS |

Raw examples:
```
Logo yellow:     [00 14 03 01 FF FF 00 00 00]
Logo red:        [00 14 03 01 FF 00 00 00 00]
LB red:          [00 14 02 01 E2 00 00 00 00]
LB green:        [00 14 02 01 00 FF 00 00 00]
LB green+save:   [00 14 02 01 00 FF 00 00 01]   <- byte 8 = 1 for BIOS persist
KB blue:         [00 14 00 01 00 9C D3 00 00]
KB red:          [00 14 00 01 FF 00 00 00 00]
KB save startup: [00 14 00 09 FF 00 00 00 00]   <- byte 3 = 0x09 saves startup color
Zone 1 red:      [00 14 01 01 FF 00 00 00 00]
```

### 3.4 Command 0x08 -- Apply Effect / Control

This command has **sub-commands** encoded in byte 2.

#### Sub-command: Apply Effect to Zone (byte 2 = Zone CMD ID)

```
[00 08 ZONE_CMD_ID POWER EFFECT BRIGHTNESS SPEED PARAM SAVE]
```

| Byte | Meaning | Values |
|---|---|---|
| 1 | Command | 0x08 |
| 2 | Zone CMD ID | 0x02 (KB), 0x21, 0x22 (LB), 0x23 (Logo) |
| 3 | Power state | 0x00 = off, 0x01 = on |
| 4 | Effect mode | See per-zone table below |
| 5 | Brightness | 0x00-0x64 (percent) for Logo/LB; non-standard scale for KB |
| 6 | Speed/param | Zone-specific |
| 7 | Extra param | Usually 0x00 |
| 8 | Save flag | 0x00 = volatile, 0x01 = persist to BIOS |

**Per-zone effect parameters:**

| Zone | Zone CMD ID | Effect (byte 4) | Speed (byte 6) | Brightness Scale |
|---|---|---|---|---|
| Logo | 0x23 | 0x01 (static) | 0x01 | 0x00-0x64 (0%-100%) |
| Light Bar | 0x22 | 0x01 (static) | 0x01 | 0x00-0x64 (0%-100%) |
| Keyboard | 0x02 | 0x07 | 0x08 | Non-standard: 0x08/0x16/0x24/0x32 for levels 1-4 |
| Zone 1 | 0x21 | 0x05 | 0x01 | 0x00-0x64 |

**Keyboard brightness scale** (non-percent, calibrated):

| UI Level | Byte 5 | Decimal | Formula |
|---|---|---|---|
| 1 (min) | 0x08 | 8 | 8 + (1-1) * 14 = 8 |
| 2 | 0x16 | 22 | 8 + (2-1) * 14 = 22 |
| 3 | 0x24 | 36 | 8 + (3-1) * 14 = 36 |
| 4 (max) | 0x32 | 50 | 8 + (4-1) * 14 = 50 |

Formula: `byte5 = 8 + (level - 1) * 14`

**Logo/LB brightness scale** (standard percent):

| Percent | Byte 5 |
|---|---|
| 0% (off) | 0x00 |
| 25% | 0x19 |
| 50% | 0x32 |
| 75% | 0x4B |
| 100% | 0x64 |

Raw examples:
```
Logo 50%:        [00 08 23 01 01 32 01 00 00]   <- effect=static, brightness=50%
Logo 100%:       [00 08 23 01 01 64 01 00 00]
Logo 0%:         [00 08 23 01 01 00 01 00 00]
Logo 75%:        [00 08 23 01 01 4B 01 00 00]
Logo 25%:        [00 08 23 01 01 19 01 00 00]
LB on:           [00 08 22 01 01 00 01 00 00]   <- brightness=0% (min)
LB 100%:         [00 08 22 01 01 64 01 00 00]
LB save:         [00 08 22 01 01 64 01 00 01]   <- byte 8 = 1 persist
KB level 4:      [00 08 02 01 07 32 08 00 00]   <- effect=0x07, bright=0x32, speed=0x08
KB level 2:      [00 08 02 01 07 16 08 00 00]
KB save:         [00 08 02 01 07 16 08 00 01]   <- byte 8 = 1 persist
Zone 1:          [00 08 21 05 05 4B 01 01 00]   <- effect=5, bright=75%, param=01
```

#### Sub-command 0x05 -- Stop All Effects

```
[00 08 05 00 00 00 00 00 00]
```

Used in OFF sequences to halt any running animation before powering down.

#### Sub-command 0x01 -- Set Zone Mode Off

```
[00 08 01 ZONE_NUM 00 00 00 00 00]
```

| Byte 3 | Meaning |
|---|---|
| 0x00 | Keyboard zone off |
| 0x02 | Light bar zone off |
| 0x03 | Logo zone off |

Used in OFF sequences after stopping effects.

### 3.5 Command 0x1A -- Power Control + Save to BIOS

```
[00 1A ZONE_NUM POWER MODE 00 00 00 SAVE]
```

| Byte | Meaning | Values |
|---|---|---|
| 1 | Command | 0x1A |
| 2 | Zone Number | 0x00 (KB), 0x02 (LB), 0x03 (Logo) |
| 3 | Power state | 0x00 = off, 0x01 = on |
| 4 | Mode | 0x00 when off, 0x04 when on |
| 5-7 | Padding | 0x00 |
| 8 | Save flag | 0x01 (always persists) |

Raw examples:
```
Logo OFF:  [00 1A 03 00 00 00 00 00 01]
Logo ON:   [00 1A 03 01 04 00 00 00 01]
LB OFF:    [00 1A 02 00 00 00 00 00 01]
LB ON:     [00 1A 02 01 04 00 00 00 01]
KB OFF:    [00 1A 00 00 00 00 00 00 01]
KB ON:     [00 1A 00 01 04 00 00 00 01]
```

Note: byte 2 = 0x00 is used for keyboard zone AND also appears on lightbar handle (h=0x1154) during save-to-BIOS sequences. This may function as a "global save" trigger when byte 2 = 0x00.

### 3.6 Command 0x12 -- Prepare / Reset

```
[00 12 00 PARAM1 PARAM2 00 00 00 00]
```

Used before OFF sequences and during tab navigation. Observed values:
```
[00 12 00 03 00 00 00 00 00]   <- "prepare for update" (before every OFF sequence)
[00 12 00 00 08 00 00 00 00]   <- possibly "end update" or "save state" (seen once)
```

### 3.7 Standard Sequences

#### Turn OFF any LED zone

```
1. Read EC 0x766 (capability register, always returns 0x93)
2. [00 12 00 03 00 00 00 00 00]         <- Prepare
3. [00 08 05 00 00 00 00 00 00]         <- Stop all effects
4. [00 08 01 ZONE_NUM 00 00 00 00 00]   <- Set zone mode off
5. [00 1A ZONE_NUM 00 00 00 00 00 01]   <- Save OFF to BIOS
6. (Logo only: EC writes to 0x7AB, 0x78C)
```

#### Turn ON any LED zone

```
1. Read EC 0x766 (capability check)
2. [00 1A ZONE_NUM 01 04 00 00 00 01]       <- Power ON + save
3. [00 14 ZONE_NUM 01 RR GG BB 00 00]       <- Set color
4. [00 08 ZONE_CMD_ID 01 EFF BR SPD 00 00]  <- Apply effect
5. (Logo only: EC writes to RGB, 0x7AB, 0x78C)
```

#### Change Color

```
1. [00 14 ZONE_NUM 01 RR GG BB 00 00]       <- Set new color
2. [00 08 ZONE_CMD_ID 01 EFF BR SPD 00 00]  <- Re-apply effect (same brightness)
```

#### Change Brightness

```
1. [00 14 ZONE_NUM 01 RR GG BB 00 00]       <- Re-send color (unchanged)
2. [00 08 ZONE_CMD_ID 01 EFF BR SPD 00 00]  <- Apply with new brightness
```

#### Save to BIOS ("Startup Mode")

```
1. (KB only: [00 14 ZONE_NUM 09 RR GG BB 00 00])  <- Save startup color
2. [00 1A ZONE_NUM 01 04 00 00 00 01]              <- Power on + save
3. [00 14 ZONE_NUM 01 RR GG BB 00 01]              <- Set color + save flag (byte 8 = 1)
4. [00 08 ZONE_CMD_ID 01 EFF BR SPD 00 01]         <- Apply + save flag (byte 8 = 1)
```

**Save flag summary:**

| Command | Normal | Save to BIOS | Flag Location |
|---|---|---|---|
| 0x14 (Set Color) | byte 3 = 0x01 | byte 3 = 0x09 | byte 3 bit 3 |
| 0x08 (Apply) | byte 8 = 0x00 | byte 8 = 0x01 | byte 8 bit 0 |

Keyboard uses BOTH flags. Light Bar uses only byte 8 = 0x01 on cmds 0x14 and 0x08. The common denominator is **byte 8 = 0x01 on cmd 0x08 (Apply)**.

---

## 4. Battery Health Mode

**Transport:** EC only
**Register:** 0x7A6 (BATT_MODE)
**Pattern:** Read-modify-write

### Captured Transitions

| Transition | Timestamp | Read 0x7A6 | Write 0x7A6 |
|---|---|---|---|
| Stationary -> High Capacity | 04:09:18 | 0x28 | 0x08 |
| High Capacity -> Balanced | 04:09:40 | 0x08 | 0x18 |
| Balanced -> Stationary | 04:09:58 | 0x18 | 0x28 |

### Encoding

| Mode | Value | Binary | Bits 5:4 |
|---|---|---|---|
| High Capacity | 0x08 | 0000 1000 | 00 |
| Balanced | 0x18 | 0001 1000 | 01 |
| Stationary | 0x28 | 0010 1000 | 10 |

**Format:** Bit 3 always set. Bits 5:4 = mode (0 = high cap, 1 = balanced, 2 = stationary). No HID or WMI involvement.

**Confirmed:** Our existing implementation uses the same protocol.

---

## 5. Local Dimming (Mini LED)

**Transport:** WMI only
**No EC writes. No HID calls. No DDC/CI.**

### Captured Actions

**Turn ON:**
```
[04:10:22.632] CoCreateInstance(CLSID_WbemLocator) detected - WMI session
```

**Turn OFF:**
```
[04:10:35.706] CoCreateInstance(CLSID_WbemLocator) detected - WMI session
```

### Analysis

Only a single WMI CoCreateInstance fires for each toggle. The actual WMI method call goes out-of-process through COM to wmiprvse.exe, which our in-process hooks cannot intercept. The method name and parameters are invisible.

The log header confirms: `[INFO] Local dimming likely goes through WMI->ACPI, not DDC/CI`

**Implementation path:** Need to hook wmiprvse.exe or use WMI tracing (ETW) to capture the actual ACPI method name and parameter. Alternatively, enumerate WMI classes under `root\WMI` looking for MECHREVO/Uniwill-specific ACPI methods.

---

## 6. Fan Switch Speed

**Transport:** EC only
**Register:** 0x787 (ADDR_TimAP_FanSwitchSpeedT100mSec)
**Pattern:** Direct write (no read-before-write needed)

### Captured Actions

**Disabled (app init, line 525):**
```
ECWRITE addr=0x787 val=0x00   <- feature disabled
```

**Enabled at 300 ms (line 1392):**
```
ECWRITE addr=0x787 val=0x83   <- 0b10000011 = enable + 3 = 300 ms
```

**Changed to 600 ms (line 1410-1414, slider dragged):**
```
ECWRITE addr=0x787 val=0x84   <- enable + 4 = 400 ms (intermediate)
ECWRITE addr=0x787 val=0x87   <- enable + 7 = 700 ms (overshoot)
ECWRITE addr=0x787 val=0x86   <- enable + 6 = 600 ms (final)
```

**Changed to 10 s (line 1464-1483, slider dragged further):**
```
ECWRITE addr=0x787 val=0x87   <- 700 ms
ECWRITE addr=0x787 val=0x93   <- 1.9 s
ECWRITE addr=0x787 val=0xC0   <- 6.4 s
ECWRITE addr=0x787 val=0xCB   <- 7.5 s
ECWRITE addr=0x787 val=0xD6   <- 8.6 s
ECWRITE addr=0x787 val=0xE1   <- 9.7 s
ECWRITE addr=0x787 val=0xE2   <- 9.8 s
ECWRITE addr=0x787 val=0xE6   <- 10.2 s
ECWRITE addr=0x787 val=0xE4   <- 10.0 s (final)
```

### Encoding

```
Bit 7:   Enable flag (0 = disabled, 1 = enabled)
Bits 6:0: Time value in 100 ms units (0-127)
```

| Value | Binary | Enabled | Time |
|---|---|---|---|
| 0x00 | 0000 0000 | No | N/A (disabled) |
| 0x83 | 1000 0011 | Yes | 3 * 100 ms = 300 ms |
| 0x86 | 1000 0110 | Yes | 6 * 100 ms = 600 ms |
| 0xE4 | 1110 0100 | Yes | 100 * 100 ms = 10.0 s |
| 0xFF | 1111 1111 | Yes | 127 * 100 ms = 12.7 s (max) |

**Note:** The register name "T100mSec" in the profile confirms the 100 ms unit. Each write is preceded by ECREAD 0x727 (ESHUTTER), which is a standard Uniwill guard/handshake register.

---

## 7. CPU Temperature Limit (TCC Offset)

**Transport:** EC only
**Register:** 0x786 (TCC_OFFSET)
**Pattern:** Direct write, preceded by ECREAD 0x727 (ESHUTTER)

### Captured Actions

**Enable at 95C (offset = 10):**
```
ECREAD  addr=0x727  out=[C0 00 00 00]
ECWRITE addr=0x786  val=0x8A   <- 0b10001010 = enable + offset 10
```

**Change to 85C (offset = 20, slider dragged):**
```
ECWRITE addr=0x786  val=0x8B   <- offset 11 (94C)
ECWRITE addr=0x786  val=0x90   <- offset 16 (89C)
ECWRITE addr=0x786  val=0x93   <- offset 19 (86C)
ECWRITE addr=0x786  val=0x94   <- offset 20 (85C, final)
```

### Encoding

```
Bit 7:   Enable flag (0 = disabled, 1 = enabled)
Bits 6:0: Offset from Tjmax in degrees C
```

Target temperature = Tjmax - offset. Assuming Tjmax = 105C:
- 0x8A: 105 - 10 = 95C
- 0x94: 105 - 20 = 85C

---

## 8. Logo LED (Rear Cover)

**Transport:** HID (h=0x11fc) + EC (metadata)
**HID Zone:** 3 (Zone CMD ID: 0x23)
**EC Registers:** 0x769/76A/76B (RGB), 0x7AB (on/off + brightness), 0x78C (trigger)

### Critical Finding

The Logo LED requires BOTH HID commands AND EC writes. EC alone does NOT physically change the LED. The HID commands on handle 0x11fc control the ITE USB LED controller directly. The EC writes store the color/brightness state for firmware persistence.

### Turn OFF (line 1528)

```
ECREAD  addr=0x766  out=[93 00 00 00]              <- capability check
HID h=0x11fc: [00 12 00 03 00 00 00 00 00]         <- prepare
HID h=0x11fc: [00 08 05 00 00 00 00 00 00]         <- stop effects
HID h=0x11fc: [00 08 01 03 00 00 00 00 00]         <- zone 3 mode off
HID h=0x11fc: [00 1A 03 00 00 00 00 00 01]         <- power OFF + save to BIOS
ECREAD  addr=0x7AB  out=[41 00 00 00]              <- current state
ECWRITE addr=0x7AB  val=0x01                        <- brightness=0 (off)
ECWRITE addr=0x78C  val=0x10                        <- notification trigger
```

### Turn ON -- yellow, 50% brightness (line 1549)

```
ECREAD  addr=0x766  out=[93 00 00 00]
HID h=0x11fc: [00 1A 03 01 04 00 00 00 01]         <- power ON + save to BIOS
HID h=0x11fc: [00 14 03 01 FF FF 00 00 00]         <- set color: yellow (FF,FF,00)
HID h=0x11fc: [00 08 23 01 01 32 01 00 00]         <- apply: effect=static, brightness=50%
ECWRITE addr=0x769  val=0x32                        <- LOGO_R = 50 (scaled from 0xFF)
ECWRITE addr=0x76A  val=0x32                        <- LOGO_G = 50
ECWRITE addr=0x76B  val=0x00                        <- LOGO_B = 0
ECREAD  addr=0x7AB  out=[01 00 00 00]
ECWRITE addr=0x7AB  val=0x41                        <- brightness level 2 (50%)
ECWRITE addr=0x78C  val=0x10                        <- notification trigger
```

### Change Color: yellow -> red (line 1590)

```
HID h=0x11fc: [00 14 03 01 FF 00 00 00 00]         <- set color: red (FF,00,00)
HID h=0x11fc: [00 08 23 01 01 32 01 00 00]         <- re-apply (same brightness)
ECWRITE addr=0x769  val=0x32                        <- LOGO_R = 50
ECWRITE addr=0x76A  val=0x00                        <- LOGO_G = 0
ECWRITE addr=0x76B  val=0x00                        <- LOGO_B = 0
ECWRITE addr=0x7AB  val=0x41                        <- unchanged
ECWRITE addr=0x78C  val=0x10
```

### Increase Brightness to max (line 1618)

```
HID h=0x11fc: [00 14 03 01 FF 00 00 00 00]         <- set color (same red)
HID h=0x11fc: [00 08 23 01 01 4B 01 00 00]         <- brightness=75% (intermediate)
HID h=0x11fc: [00 08 23 01 01 64 01 00 00]         <- brightness=100% (final)
ECWRITE addr=0x7AB  val=0x81                        <- brightness level 4 (max)
ECWRITE addr=0x78C  val=0x10
```

### Decrease Brightness to minimum (line 1658)

```
HID h=0x11fc: [00 08 23 01 01 4B 01 00 00]         <- 75% (intermediate)
HID h=0x11fc: [00 08 23 01 01 19 01 00 00]         <- 25%
HID h=0x11fc: [00 08 23 01 01 00 01 00 00]         <- 0% (min, final)
ECWRITE addr=0x7AB  val=0x21                        <- brightness level 1 (intermediate)
ECWRITE addr=0x7AB  val=0x01                        <- brightness level 0 (min)
ECWRITE addr=0x78C  val=0x10
```

### EC 0x7AB -- Logo Brightness Encoding

Bits 7:5 encode brightness level. Bit 0 is always set (LED registered/present). Observed values are one-hot in bits 7:5, but the inferred level 3 (0x61) would break that pattern.

| Level | HID Brightness | 0x7AB Value | Binary (bits 7:5) | Source |
|---|---|---|---|---|
| 0 (off/min) | 0x00 (0%) | 0x01 | 000 | Captured |
| 1 | 0x19 (25%) | 0x21 | 001 | Captured |
| 2 (default) | 0x32 (50%) | 0x41 | 010 | Captured |
| 3 | 0x4B (75%) | 0x61 | 011 | **Inferred** (never observed) |
| 4 (max) | 0x64 (100%) | 0x81 | 100 | Captured |

### EC Logo RGB Scaling

The EC stores logo color at ~20% scale: `EC_value = round(HID_value * 50 / 255)`
- HID 0xFF -> EC 0x32 (50)
- HID 0x00 -> EC 0x00

### EC 0x78C -- Notification Trigger

Every logo LED change writes 0x10 to 0x78C. Read always returns 0x00 first. This is a notification/trigger register telling the EC firmware that LED settings changed.

---

## 9. Light Bar

**Transport:** HID only (h=0x1154)
**HID Zone:** 2 (Zone CMD ID: 0x22)
**No EC involvement whatsoever.**

### Turn OFF (line 1745)

```
ECREAD  addr=0x766  out=[93 00 00 00]              <- capability check
HID h=0x1154: [00 12 00 03 00 00 00 00 00]         <- prepare
HID h=0x1154: [00 08 05 00 00 00 00 00 00]         <- stop effects
HID h=0x1154: [00 08 01 02 00 00 00 00 00]         <- zone 2 mode off
HID h=0x1154: [00 1A 02 00 00 00 00 00 01]         <- power OFF + save
```

### Turn ON -- red, min brightness (line 1763)

```
ECREAD  addr=0x766  out=[93 00 00 00]
HID h=0x1154: [00 1A 02 01 04 00 00 00 01]         <- power ON + save
HID h=0x1154: [00 14 02 01 E2 00 00 00 00]         <- set color: red (E2,00,00)
HID h=0x1154: [00 08 22 01 01 00 01 00 00]         <- apply, brightness=0% (min)
```

### Change Color: red -> green (line 1783)

```
HID h=0x1154: [00 14 02 01 00 FF 00 00 00]         <- set color: green (00,FF,00)
HID h=0x1154: [00 08 22 01 01 00 01 00 00]         <- re-apply
```

### Increase Brightness: min -> max (line 1803)

```
HID h=0x1154: [00 14 02 01 00 FF 00 00 00]         <- color (same)
HID h=0x1154: [00 08 22 01 01 19 01 00 00]         <- brightness=25%
HID h=0x1154: [00 14 02 01 00 FF 00 00 00]         <- color (same)
HID h=0x1154: [00 08 22 01 01 64 01 00 00]         <- brightness=100% (max)
```

### Save to BIOS / Startup Mode (line 1831)

```
HID h=0x1154: [00 1A 00 01 04 00 00 00 01]         <- global power save
HID h=0x1154: [00 14 02 01 00 FF 00 00 01]         <- set color + SAVE (byte 8 = 1)
HID h=0x1154: [00 08 22 01 01 64 01 00 01]         <- apply + SAVE (byte 8 = 1)
```

### Brightness Scale (same as Logo)

| Percent | HID Value |
|---|---|
| 0% (min) | 0x00 |
| 25% | 0x19 |
| 50% | 0x32 |
| 75% | 0x4B |
| 100% (max) | 0x64 |

---

## 10. Keyboard Backlight

**Transport:** HID only (h=0x1004)
**HID Zone:** 0 (Zone CMD ID: 0x02)
**No EC involvement whatsoever.**

### Turn OFF (line 1866)

```
ECREAD  addr=0x766  out=[93 00 00 00]
HID h=0x1004: [00 09 02 00 00 00 00 00 00]         <- power off zone (cmd 0x09)
HID h=0x1004: [00 12 00 03 00 00 00 00 00]         <- prepare
HID h=0x1004: [00 08 05 00 00 00 00 00 00]         <- stop effects
HID h=0x1004: [00 08 01 00 00 00 00 00 00]         <- zone 0 mode off
HID h=0x1004: [00 1A 00 00 00 00 00 00 01]         <- save OFF
```

Note: Keyboard OFF sequence additionally sends cmd 0x09 first (other zones don't).

### Turn ON -- blue, level 4 (line 1883)

```
ECREAD  addr=0x766  out=[93 00 00 00]
HID h=0x1004: [00 1A 00 01 04 00 00 00 01]         <- power ON + save
HID h=0x1004: [00 14 00 01 00 9C D3 00 00]         <- set color: blue (00,9C,D3)
HID h=0x1004: [00 08 02 01 07 32 08 00 00]         <- apply: effect=0x07, bright=0x32, speed=0x08
```

### Change Color: blue -> red (line 1899)

```
HID h=0x1004: [00 14 00 01 FF 00 00 00 00]         <- set color: red
HID h=0x1004: [00 08 02 01 07 32 08 00 00]         <- re-apply (same params)
```

### Reduce Brightness: level 4 -> level 2 (line 1923)

```
HID h=0x1004: [00 14 00 01 FF 00 00 00 00]         <- color (same)
HID h=0x1004: [00 08 02 01 07 16 08 00 00]         <- brightness byte changed: 0x32 -> 0x16
```

### Save to BIOS / Startup Mode (line 1945)

```
HID h=0x1004: [00 14 00 09 FF 00 00 00 00]         <- save STARTUP color (byte 3 = 0x09)
HID h=0x1004: [00 1A 00 01 04 00 00 00 01]         <- power on + save
HID h=0x1004: [00 14 00 01 FF 00 00 00 00]         <- set current color
HID h=0x1004: [00 08 02 01 07 16 08 00 01]         <- apply + SAVE (byte 8 = 1)
```

Note: Keyboard uses byte 3 = 0x09 in cmd 0x14 to write startup color to ITE chip flash. This is unique to keyboard -- other zones use only byte 8 = 1.

### Keyboard-Specific Parameters

The keyboard apply command has different effect/speed bytes than Logo/LB:

```
[00 08 02 01 07 BB 08 00 SS]
                 ^^    ^^
                 |     |
                 |     Speed/mode = 0x08 (always)
                 Effect = 0x07 (always, likely "static all zones")
```

---

## 11. Super Performance Switch

**Transport:** EC only (fan table + power limit + dynamic boost rewrite)

### Turn ON (line 1090)

Key differences from normal mode (compared to the preceding apply-all):

```
ECWRITE addr=0x743(DB_CTRL) val=0x07     <- was 0x05 (enables dynamic boost high mode)
ECWRITE addr=0x746(DB_MAX)  val=0x19     <- was 0x05 (DB_MAX: 5W -> 25W)
ECREAD  addr=0x728          out=[04]
ECWRITE addr=0x728          val=0x06     <- was 0x04 (bit 1 set: super perf flag)
```

Plus full fan table rewrite (same curve values) and:
```
ECWRITE addr=0x783(PL1)  val=0x4B (75W)  <- same PL values
ECWRITE addr=0x784(PL2)  val=0x55 (85W)
ECWRITE addr=0x785(PL4)  val=0x91 (145W)
ECWRITE addr=0x744(TGP)  val=0x37 (55W)
```

### Turn OFF (line 1227)

```
ECWRITE addr=0x743(DB_CTRL) val=0x05     <- restore (clear high mode)
ECWRITE addr=0x746(DB_MAX)  val=0x05     <- DB_MAX back to 5W
ECREAD  addr=0x728          out=[06]
ECWRITE addr=0x728          val=0x04     <- clear bit 1 (super perf flag off)
```

### Analysis

Super Performance mode changes three things:
1. **DB_MAX** (0x746): 5W -> 25W (much more dynamic boost headroom)
2. **Register 0x728** bit 1: 0 -> 1 (super performance flag)
3. **DB_CTRL** (0x743): Temporarily set to 0x07 (from 0x05), though both on and off rewrite it to 0x05 at the end

The fan curve itself is identical -- only the power/dynamic boost limits differ.

---

## 12. IGPU Power Saving Mode

**Transport:** EC + HID (full re-apply of all settings)

### Captured Action: "Hybrid GPU mode, changing IGPU power saving OFF" (line 188)

This triggered a massive "apply all settings" sequence:

```
ECREAD  addr=0x748  out=[81]
ECWRITE addr=0x748  val=0x8D               <- set bits 2,3 (HID programming mode?)
HID: Power off ALL 4 zones (cmd 0x09 on each handle)
...
ECREAD  addr=0x748  out=[8D]
ECWRITE addr=0x748  val=0x81               <- clear bits 2,3 (normal mode)
HID: Re-init all 4 zones with colors and effects
EC: Full fan table write + PL/TGP + Logo RGB + LOGO_ONOFF + BATT_MODE
```

The user then noted "Ignore previous session - app didn't work" (line 303), indicating the IGPU mode change may have caused issues. All data before line 303 should be treated with caution.

### EC Register 0x748

New register. Values: 0x81 (normal) and 0x8D (programming mode).
- 0x81 = 0b10000001
- 0x8D = 0b10001101
- Difference: bits 2,3 (0x0C)

Setting bits 2,3 appears to put the ITE HID controller into a "programmable" state for the full re-initialization sequence. Cleared when done.

---

## 13. App Initialization Sequence

When the OEM app starts or switches major modes, it performs a full state apply (observed at lines 460-720 and lines 1090-1215):

1. Read 0x7C5 (OEM5_FANCURVE), 0x7CA, 0x74C, 0x743 (DB_CTRL), 0x7C6 (OEM6_APPLY)
2. Write 0x706 (BIOS_CTRL) = 0x41
3. Write 0x743 (DB_CTRL) = 0x05
4. Write 0x7C6 (OEM6_APPLY) = 0x00
5. Write full fan table (0xF00-0xF5F): CPU temps, CPU duty, GPU temps, GPU duty
6. Write 0x783/784/785 (PL1/PL2/PL4)
7. Write 0x744/745/746 (TGP/DB_TOTAL/DB_MAX)
8. Write 0x787 (FAN_SWITCH_SPEED) = 0x00 (disabled)
9. Verify fan table by reading back 0xF00, 0xF01
10. Write 0x7C6 (OEM6_APPLY) = 0x04 (apply), then 0x00 (clear)
11. Repeat entire fan table write and apply sequence (double-write pattern!)

The double-write of the entire fan table is notable -- the OEM app writes all 96 fan table registers, applies with 0x7C6, then writes all 96 again and applies again. This may be required for the EC to reliably accept the values.

---

## 14. Background Polling

The OEM app polls several registers continuously during normal operation:

| Register | Interval | Value | Purpose |
|---|---|---|---|
| 0x7CC | ~1.5 s | Always 0x81 | Watchdog/heartbeat (pure noise in log) |
| 0x7C5 (OEM5_FANCURVE) | ~1-2 s | Always 0xA0 | Detects external fan curve changes |
| 0x7CA + 0x74C | Periodic pair | 0x01, 0xC6 | Unknown, possibly battery/power state |

**WMI CoCreateInstance:** Fires periodically in bursts (especially during mode switches). Multiple WMI sessions created in quick succession. These are background WMI queries (possibly reading GPU/CPU telemetry or checking ACPI state).

---

## 15. EC Register Map

| Address | Name | Values Seen | Purpose |
|---|---|---|---|
| 0x706 | BIOS_CTRL | 0x41 (write) | Written during full mode apply |
| 0x727 | ESHUTTER | 0xC0 (read/write) | Guard register, read before many writes |
| 0x728 | (unnamed) | 0x04/0x06 | Bit 1 = super performance flag |
| 0x743 | DB_CTRL | 0x05/0x07 | Dynamic boost control mode |
| 0x744 | TGP | 0x37 (55W) | GPU total graphics power |
| 0x745 | DB_TOTAL | 0x00/0xFF | Dynamic boost total |
| 0x746 | DB_MAX | 0x05/0x06/0x19 | Dynamic boost max (W) |
| 0x748 | (unnamed) | 0x81/0x8D | IGPU power saving; bits 2,3 = HID programming mode |
| 0x74C | (unnamed) | 0xC6 | Unknown, polled with 0x7CA |
| 0x766 | LED_CAPS | 0x93 | LED capability register (read before HID ops) |
| 0x767 | TRIGGER | 0x00 (read/write) | Read-modify-write during logo apply |
| 0x769 | LOGO_R | 0x00/0x32 | Logo red (EC scale) |
| 0x76A | LOGO_G | 0x00/0x32 | Logo green (EC scale) |
| 0x76B | LOGO_B | 0x00 | Logo blue (EC scale) |
| 0x783 | PL1 | 0x4B (75W) | CPU power limit 1 |
| 0x784 | PL2 | 0x55 (85W) | CPU power limit 2 |
| 0x785 | PL4 | 0x91 (145W) | CPU power limit 4 |
| 0x786 | TCC_OFFSET | 0x8A-0x94 | Bit 7 = enable, bits 6:0 = offset from Tjmax |
| 0x787 | FAN_SWITCH_SPEED | 0x00-0xFF | Bit 7 = enable, bits 6:0 = time in 100 ms |
| 0x78C | OEM2_TURBO | 0x00/0x10 | LED change notification trigger |
| 0x78E | (unnamed) | 0xFC | Unknown, read during mode switches |
| 0x7A5 | (unnamed) | 0x00 | Unknown, read once during init |
| 0x7A6 | BATT_MODE | 0x08/0x18/0x28 | Bit 3 always set, bits 5:4 = mode |
| 0x7AB | LOGO_ONOFF | 0x01-0x81 | Bits 7:5 = brightness (one-hot, 0-4), bit 0 = registered |
| 0x7C5 | OEM5_FANCURVE | 0xA0 | Fan curve mode, polled for external changes |
| 0x7CA | (unnamed) | 0x01 | Unknown, polled with 0x74C |
| 0x7CC | WATCHDOG | 0x81 | Polled every 1.5 s (heartbeat) |

