# Multi-Model Support -- Current State

## Overview

MechControl now supports **unverified profiles** without crashing. The app detects the machine model, loads the matching profile, fills in universal defaults for missing fields, validates the EC register layout at startup, and enables features that pass validation. Conservative limits are applied for unverified profiles. ECSpec runtime extraction provides per-model power limits. A comprehensive diagnostic log captures everything needed to build a verified profile from a remote machine.

**1 fully verified profile** (IDY_7Y), **43 placeholder profiles** with universal defaults applied at runtime.

---

## 1. Model Detection

Detection runs at startup in `ProfileLoader.cs` with this fallback chain:

1. **EC register 0x740** -- read raw byte, match `projectIdValue` directly
2. **Shifted match** -- try `raw >> 1` (newer models store ProjectID * 2)
3. **WMI baseboard name** -- `Win32_BaseBoard.Product` substring match against profile filenames
4. **Manual selection** -- user picks from a ListBox of all available profiles

This works for all 44 models. The 44 `projectIdValue` values range from 1 (GI) to 6157 (PH6PG7x150W). One source profile (PH4AQxx) has `"unknown"` for its value and was excluded from placeholder generation.

### Known Issue: ProjectID Collisions

X6FR5xxY (AMD CPU) matched IDY_7Y via shifted ProjectID 13, but it's a completely different machine. Same EC platform, different CPU. The verified IDY_7Y profile was applied, causing wrong temp limit encoding and hardcoded Intel fan curves to be written. ProjectID alone is insufficient for unique model identification when Intel/AMD variants share the same EC.

### Known Baseboard Names

| Baseboard (WMI) | Profile | CPU | Notes |
|-----------------|---------|-----|-------|
| YAOSHI Series-X6AR55xY | IDY_7Y | Intel | Verified machine. Baseboard doesn't contain profile name -- WMI fallback won't match. |
| X6FR5xxY | IDY_7Y (wrong match) | AMD | Matched via shifted ProjectID 13. Needs own profile. |

Baseboard names are logged in `mechcontrol.log` for future mapping.

---

## 2. Universal Defaults (`Profile/UniversalDefaults.cs`)

A single source of truth for values shared across all Uniwill models, derived from `ACPIDriverDll.dll` disassembly and verified on IDY_7Y. Profile JSON values override these when populated.

### Bitfields (4 entries, universal across all models)

| Name | Register | Mask | Known Valid Values |
|------|----------|------|--------------------|
| fanCurveEnable | 0x7C5 | bit 7 (0x80) | 0x00, 0x80 |
| batteryMode | 0x7A6 | 0x38 | 0x08, 0x18, 0x28 |
| dynamicBoostEn | 0x743 | 0x07 | 0x00, 0x02, 0x05, 0x07 |
| logoOnOff | 0x7AB | bit 7 (0x80) | 0x00, 0x80 |

These masks come from DLL code, not per-model config. The same DLL serves all 44 models.

### Conservative Limits (for unverified profiles)

| Parameter | Conservative Range | Verified IDY_7Y Range |
|-----------|-------------------|----------------------|
| PL1 | 15-150W | 10-210W |
| PL2 | 15-150W | 10-210W |
| PL4 | 30-300W | 10-420W |
| CPU Temp Limit | 80-100C | 80-95C |
| TGP | 95-150W | 95-150W |
| Dynamic Boost | 0-25W | 5-25W |
| Fan Duty | 0-200 raw | 0-200 raw |
| Fan Temp | 0-105C | 0-105C |

### Universal Write Allowlist

Init registers (required for EC handshake on all models):
- 0x708, 0x709 (handshake), 0x726, 0x727 (power management activation)
- 0x741 (activation), 0x748 (feature enable)
- 0x7C5 (fan curve enable), 0x7C7 (fan curve param), 0x7C6 (fan curve commit)
- 0x706 (BIOS_CTRL), 0xF00-0xF5F (fan table)

Feature registers (enabled after validation):
- 0x783-0x786 (CPU PL1/PL2/PL4/temp), 0x743-0x746 (GPU TGP/DB)
- 0x751 (fan boost), 0x7A6 (battery), 0x787 (fan switch speed)
- 0x767, 0x769-0x76B, 0x7AB, 0x78C (logo lighting)

### Universal Skip-Verify and Verify-Redirect

Skip-verify: 0x767, 0x78C (logo triggers), 0x726, 0x727 (power management), 0x741, 0x7C6 (mode flags), 0x708, 0x709, 0x748, 0x706, 0x7C7 (init registers).

Verify-redirect: 0x786 (CPU temp write) reads back from 0x463. Transform depends on CPU type -- see section 6.

---

## 3. Startup Validation (`Features/StartupValidator.cs`)

Runs after profile load and EC init. For unverified profiles:

### Bitfield Validation

Reads each bitfield register, applies the mask, checks if the masked value is in the known-valid set. If invalid, the associated feature is disabled.

### Feature Register Validation

Reads feature registers, decodes values, checks against sanity ranges:

| Parameter | Sanity Range | Purpose |
|-----------|-------------|---------|
| PL1 | 5-300W | Physical plausibility |
| PL2 | 5-300W | Physical plausibility |
| PL4 | 10-600W | Physical plausibility |
| CPU Temp | 60-105C | Physical plausibility |
| TGP | 50-250W | Physical plausibility |
| Dynamic Boost | 0-50W | Physical plausibility |
| Logo RGB | 0-60 each | EC range validation |

If a register decodes to a value outside the sanity range, the associated feature is disabled with a logged reason. Sanity ranges are deliberately wide -- they catch layout mismatches, not out-of-spec values.

### ECSpec Enrichment Override

If ECSpec provides valid limits for any register in a feature group, the register layout is confirmed and sanity check failures do not disable the feature. This prevents false negatives from decode mismatches (e.g., absolute temp encoding decoded as offset).

---

## 4. EC Initialization for Unverified Profiles

For unverified profiles, the init sequence is modified:

1. **Read BIOS fan table first** -- reads 0xF00-0xF5F from EC before any writes
2. **Validate** -- checks temps ascending, duties in range, detects active entry count
3. **Save to profile** -- stores as `DefaultFanCurve` with correct `activeEntries`
4. **Write back during init** -- EC needs a valid fan table written to accept the commit, so we write the machine's own table back (not IDY_7Y's hardcoded one)

This avoids writing a mismatched fan table (wrong entry count, wrong format) to an unknown machine.

### Known Issue: Verified Profile Fan Curves

For verified profiles (IDY_7Y), `ReadAndSaveBiosFanTable()` is skipped -- the hardcoded fan curve from the profile JSON is always written during EC init. If the profile loads on the wrong machine (ProjectID collision), the wrong fan curves are applied. The saved user profile fan curve then overwrites it again, meaning the default fan curve is written twice on startup -- once from JSON (pointless), once from the user's saved profile.

---

## 5. Diagnostic Logging (`DiagnosticLog.cs`)

Writes to `mechcontrol.log` next to the exe. One section per session. Contains everything needed to build a verified profile remotely:

- **Profile detection**: method, project ID, verified flag
- **WMI baseboard**: `Win32_BaseBoard.Product` string
- **EC identity**: raw 0x740 value + shifted
- **Extended identity**: ModuleID (0x7D3), ModuleID_GPU (0x7D2), ROM IDs (0x771/0x772), EC version (0x454/0x47D), SystemID (0x456)
- **Support bytes**: 0x765, 0x766, 0x742, 0x78E (Support6)
- **VRM current limits**: 0x753, 0x754
- **Per-mode PL defaults**: Gaming (0x730-0x733, 0x7D8), Office (0x734-0x737, 0x7D9), Battery Saver (0x7A7-0x7AA), Turbo TCC (0x7DA)
- **ECSpec config**: all 12 entries (0x00-0x0B), raw hex + decoded limits
- **Feature registers**: raw + decoded for all ~17 registers
- **Fan table dump**: 96 bytes (0xF00-0xF5F) + RamFan2 probe (0xF60-0xF6F)
- **HID devices**: zone availability (keyboard, light bar, logo)
- **Bitfield validation**: pass/fail per bitfield with raw/masked values
- **Feature validation**: pass/fail per register with decoded values
- **EC init**: pass/fail per step
- **Effective limits**: final limits used by UI (after ECSpec enrichment)

---

## 6. CPU Temp Limit Encoding (Intel vs AMD)

The EC uses two different encodings for CPU temp limits, depending on CPU type. Detected at runtime via ECSpec byte[3].

### Detection: ECSpec byte[3]

| byte[3] value | Interpretation | Encoding |
|---------------|---------------|----------|
| <= 40 | TCC offset (Intel-style) | `inverted_offset` |
| 80-120 | Absolute max temperature (AMD-style) | `direct` |
| 41-79 or > 120 | Unknown / invalid | Not extracted |

### Intel: inverted_offset (TjMax=105)

- **Write 0x786**: `0x80 | (105 - tempC)` (enable bit + offset)
- **Read 0x463**: returns offset only (bit 7 clear)
- **Verify redirect**: 0x786 -> 0x463, transform strips bit 7 from expected
- **Decode**: `105 - readback = tempC`
- **Example**: 95C -> write 0x8A, read back 0x0A (10), decode 105-10=95C

### AMD: direct (absolute temperature)

- **Write 0x786**: raw temperature byte (e.g., 0x5F = 95)
- **Read 0x463**: returns same absolute temperature
- **Verify redirect**: 0x786 -> 0x463, direct compare (no transform)
- **Decode**: readback IS the temperature
- **Example**: 95C -> write 0x5F (95), read back 0x5F (95)

### What goes wrong without detection

Writing Intel encoding (0x8A = 138) to an AMD EC:
- EC receives 138 as a temperature -- nonsensical
- Sometimes accepted -> CPU locks at 600MHz failsafe
- Sometimes rejected -> EC keeps old value, no harm but verify fails

Observed error: `Write verify failed at 0x786: wrote 0x8A, verify 0x463 = 138 (expected 10)`

### Implementation

- `HardwareDetection.ExtractLimits()` detects encoding from ECSpec byte[3]
- Applied to ALL profiles (including verified) since it's a hardware property
- Updates both `profile.Limits["cpuTempLimit"]` encoding and `profile.VerifyRedirect["0x786"]` transform
- `CpuPowerControl`, `EcMonitor`, `StartupValidator`, `PowerTab`, `MainForm` all branch on encoding

---

## 7. ECSpec Buffer Layout (Confirmed)

Extracted via SMAPCTABLE IOCTL (0x9C40A500). Sliding-window DWORD reads, byte[i] = `EcSpecConfig[i] & 0xFF`.

| Index | Content | Encoding | IDY_7Y (Intel) | X6FR5xxY (AMD) |
|-------|---------|----------|----------------|----------------|
| 0 | PL1 max | direct (watts) | 0xD2 = 210W | 0xA2 = 162W |
| 1 | PL2 max | direct (watts) | 0xD2 = 210W | 0xA2 = 162W |
| 2 | PL4 max raw | double (raw x2 = watts) | 0xD2 = 420W | 0xC3 = 390W |
| 3 | TCC/TempMax | offset if <=40, absolute if >40 | 0x0A = offset 10 (95C) | 0x62 = 98C absolute |
| 4 | Configurable TGP | raw (add DefaultTGP for max) | 0x37 = 55 | 0x37 = 55 |
| 5 | Dynamic Boost max | direct (watts) | 0x19 = 25W | 0x19 = 25W |
| 6 | GPU target temp | direct (degrees C) | 0x57 = 87C | 0x57 = 87C |
| 7 | Default TGP | direct (watts, TGP base) | 0x5F = 95W | 0x5F = 95W |
| 8 | DB CPU TDP | 0xFF = unused | 0xFF | 0xFF |
| 9 | Validation marker | LSB should be 0xBB | 0xBB | 0xBB |

Entries 9-11 echo the SMAPCTABLE command byte (0xBB) as out-of-range markers, confirming buffer boundaries.

---

## 8. Hardware Capability Detection

### Support Bytes (EC registers, read-only)

| Register | Name | IDY_7Y Value | X6FR5xxY Value | Purpose |
|----------|------|-------------|----------------|---------|
| 0x765 | Support1 | 0xE1 | 0xE1 | Feature support flags |
| 0x766 | Support2 | 0x90-0x93 | 0x93 | LED capability flags (may vary between reads) |
| 0x742 | Support5 | 0x22 | 0x22 | Additional support flags |
| 0x78E | Support6 | -- | 0xFC | Additional support flags |

### Extended Identity Registers

| Register | Name | IDY_7Y | X6FR5xxY | Purpose |
|----------|------|--------|----------|---------|
| 0x7D3 | ModuleID | 0x00 | 0x00 | Main module identifier |
| 0x7D2 | ModuleID_GPU | 0x1B | 0x1B | GPU module identifier |
| 0x771 | ROMID1 | -- | 0x20 | EC ROM ID |
| 0x772 | ROMID2 | -- | 0x02 | EC ROM ID 2 |
| 0x454 | EC Version | -- | 95.0 | EC firmware version |
| 0x456 | SystemID | -- | 0x88 | System ID |
| 0x753 | VRM Current | -- | 0x0A | CPU VRM current limit |
| 0x754 | VRM Max Current | -- | 0x0A | CPU VRM max current limit |

### Per-Mode PL Defaults (EC Registers)

Read at startup from EC registers. Used as mode defaults when profile doesn't have hand-tuned modes.

| Mode | Registers | IDY_7Y | X6FR5xxY | Notes |
|------|-----------|--------|----------|-------|
| Gaming | 0x730-0x733, 0x7D8 | -- | PL1=75 PL2=85 PL4=170 | |
| Office | 0x734-0x737, 0x7D9 | -- | PL1=45 PL2=45 PL4=90 | |
| Battery Saver | 0x7A7-0x7AA | -- | PL1=145 PL2=145 PL4=348 | Values look wrong -- may be different encoding on AMD |
| Turbo TCC | 0x7DA | -- | invalid(95) | |

Note: X6FR5xxY Battery Saver values (145W) are suspiciously high. May indicate different register meaning on AMD, or these registers aren't used for battery saver on this model.

---

## 9. Profile Schema -- Verified vs Unverified (Runtime State)

### Verified Profile (IDY_7Y.json)

```
registers:      22 entries (all feature register addresses)
limits:         8 entries (PL1/PL2/PL4/cpuTempLimit/TGP/dynamicBoost/fanDuty/fanSwitchSpeed)
bitfields:      4 entries (fanCurveEnable, batteryMode, logoOnOff, dynamicBoostEn)
batteryModes:   3 entries (High Capacity, Balanced, Stationary)
writeAllowlist: 18 entries + range 0xF00-0xF5F (full write access)
skipVerify:     9 entries
verifyRedirect: 1 entry (0x786 -> 0x463)
fanTable:       complete (RamFan1, 10 active entries, hysteresis 6)
verified:       true
```

### Unverified Profile (after UniversalDefaults.ApplyToProfile)

```
registers:      22 entries (same addresses -- universal)
limits:         8 entries (filled from conservative defaults, overridden by ECSpec if available)
bitfields:      4 entries (filled from universal defaults)
batteryModes:   3 entries (filled from universal defaults)
writeAllowlist: init + feature registers (merged from universal defaults)
skipVerify:     9 entries (filled from universal defaults)
verifyRedirect: 1 entry (filled from universal defaults, transform updated by ECSpec detection)
fanTable:       read from BIOS at startup (active entries auto-detected)
verified:       false
```

### Default Safety

`ProfileSchema.cs` defaults `Verified` to `false`. A missing field in JSON = unverified = conservative behavior.

---

## 10. What's Universal vs Per-Model

### Confirmed Universal (all 44 models)

- **EC register addresses** -- all 22 register keys identical across all profiles
- **IOCTL codes** -- all 29 codes identical
- **Driver path** -- `\\.\ACPIDriver` (UWACPIDriver.sys, Uniwill)
- **Fan table base addresses** -- 0xF00-0xF5F (RamFan1 format)
- **Mode constants** -- fan modes, turbo modes, battery enums
- **EC activation** -- 0x741 = activation byte, 0x7C6 = OEM6_APPLY
- **Bitfield masks** -- 4 entries, from shared DLL code (validated on IDY_7Y)
- **Init sequence** -- 8-step handshake, same registers/values (validated on IDY_7Y)
- **Skip-verify set** -- 9 entries (validated on IDY_7Y)
- **Battery modes** -- 3 modes with values 0x08/0x18/0x28 (validated on IDY_7Y)

### Varies Per Model

| Parameter | IDY_7Y Value | X6FR5xxY Value | Range Across Models | Source |
|-----------|-------------|----------------|-------------------|--------|
| PL1 limits | 10-210W | 10-162W | 15-210W | ECSpec + OEM fan tables |
| PL2 limits | 10-210W | 10-162W | 15-210W | ECSpec + OEM fan tables |
| PL4 limits | 10-420W | 20-390W | Model-dependent | ECSpec |
| TGP limits | 95-150W | 95-150W | Model-dependent | ECSpec |
| Dynamic Boost | 5-25W | 0-25W | 0-25W or "NA" | ECSpec + OEM fan tables |
| CPU temp encoding | inverted_offset | direct | Depends on CPU | ECSpec byte[3] |
| CPU temp max | 95C | 98C | Model-dependent | ECSpec byte[3] |
| Active fan points | 10 | 10 | 6-11 | OEM fan tables |
| GPU fan curve | 10 points | 10 points | 0-11 (0 = iGPU-only) | OEM fan tables |
| Hysteresis | 6C fixed | -- | 1-15C, varies per point | OEM fan tables |
| Fan table format | RamFan1 | RamFan1 | RamFan1, RamFan1p5, RamFan2 | Source profiles |
| LED capabilities | 0x90 | 0x93 | Unknown | Support byte 2 |
| Local dimming | Available | Unknown | Panel-dependent | GPIO via WMI |
| Verify-redirect transform | inverted_offset | direct | Depends on CPU | ECSpec byte[3] |

### Fan Table Formats

Source profiles list three fan table register layouts. The correct format per model is unknown:

| Format | Base Addresses | Notes |
|--------|---------------|-------|
| RamFan1 | 0xF00-0xF5F | 1 fan pair (CPU+GPU), 16 entries each. Used by IDY_7Y and X6FR5xxY. |
| RamFan1p5 | 0xF00-0xF5F + 0xF5D-0xF5F ctrl | RamFan1 + switch speed + TCC offset registers |
| RamFan2 | 0xF00-0xFFF | 3 fan pairs (F1/F2/F3), 16 entries each. For 3-fan or 3-mode systems. |

RamFan2 probe on IDY_7Y (0xF60-0xF6F) shows unstructured data, confirming RamFan1 is the correct format.

---

## 11. OEM Fan Table Data (UserFanTables-OEM/)

Factory fan curves exist for **24 of the 44 models**, extracted from the OEM driver package. Each model has 5 preset files (M1T1-M1T3, M2T1-M2T2 = Gaming/Office/Turbo modes).

Each file contains:
```json
{
  "PL1": "40", "PL2": "40", "TCC": "-5",
  "CTGP": "15", "DB": "0",
  "CpuTemp_DefaultMaxLevel": 11,
  "GpuTemp_DefaultMaxLevel": 11,
  "CPU": [16 fan curve points],
  "GPU": [16 fan curve points]
}
```

### Models With OEM Fan Tables

PH4AQE3, PH4AQxx, PH4ARxx, PH4AUxf, PH4AUxx, PH4AXxx, PH4PGx1, PH4PGx2, PH4PRxx, PH4PUxx, PH4TQx1, PH4TRX1, PH4TUX1, PH6AQxx, PH6ARxx, PH6PG0x, PH6PG0x150W, PH6PG3x, PH6PG3x150W, PH6PG7x, PH6PG7x150W, PH6PGEx, PH6PRxx, PH6TRX1

### Models Without OEM Fan Tables

GI, GJ, GK, GICN, GJCN, GK5CN_X, GK5CP_4X_5X_6X, GK7CN_S, GK7CPCS_GK5CQ7Z, GK7NXXR, GM5MU1Y, IDP, IDY_6Y, IDY_7Y, PF, PF4MU_PF4MN_PF5MU, PH6AGxx, PH6TQxx, PHxAxxx, PHxPxxx

---

## 12. WriteGuard Behavior

### Verified Profiles

Full allowlist from profile JSON. All features writable within profile-defined limits.

### Unverified Profiles (after universal defaults merge)

Full init + feature allowlist from UniversalDefaults. All features writable within conservative limits (or ECSpec-derived limits), subject to startup validation passing. WriteGuard's write-then-verify catches any writes the EC rejects.

---

## 13. What Needs to Happen Next

### Immediate: X6FR5xxY Profile

The AMD tester needs their own profile:
- Create X6FR5xxY profile (unverified) with correct ProjectID to avoid IDY_7Y collision
- Verify fan curves are read from BIOS (unverified profile behavior)
- Confirm absolute temp encoding works correctly with ECSpec detection
- Test PL1/PL2/PL4 writes (these should work regardless of temp encoding)

### Phase 1: Decode Support Bytes

Map individual bits in 0x765/0x766/0x742/0x78E to specific features:
- Which bits control fan curve availability?
- Which bits indicate GPU presence (iGPU vs dGPU)?
- Which bits gate lighting zones?
- Requires testing on multiple machines or deeper DSDT analysis

### Phase 2: Build Baseboard Mapping

As log files come in from different machines:
- Map baseboard names to profile project IDs
- Add baseboard patterns to profiles for improved detection
- Build a community database of machine-to-profile mappings
- Address ProjectID collisions between Intel/AMD variants

### Phase 3: Populate Limits from OEM Fan Tables

For the 24 models with OEM fan table data:
- Extract PL1/PL2 defaults, CTGP (TGP), DB support, active point counts
- Populate profile limits with ranges derived from OEM presets
- Mark these as partially verified

---

## 14. Completed Work

### ECSpec Limit Extraction (Done)

Per-model power limits are extracted at runtime from ECSpec SMAPCTABLE buffer in `HardwareDetection.ExtractLimits()`. Confirmed on two machines (IDY_7Y Intel, X6FR5xxY AMD). Limits override conservative defaults for unverified profiles.

### CPU Temp Encoding Detection (Done)

Automatic detection of Intel (inverted_offset) vs AMD (direct/absolute) temp limit encoding via ECSpec byte[3] threshold. Applied to all profiles including verified. Implemented across all encode/decode/verify paths.

### JSON Escaping in Placeholder Profiles (Done)

All 43 placeholder profiles had malformed `devicePath` escaping:
- **Bad:** `"\\.\ACPIDriver"` (contains invalid JSON escape `\A`)
- **Fixed:** `"\\\\.\\ACPIDriver"` (properly escaped `\\.\ACPIDriver`)

### Verified Default in ProfileSchema (Done)

Changed `Verified` property default from `true` to `false`. A profile with a missing `verified` field is now treated as unverified (safe default).

---

## 15. Test Data: X6FR5xxY (AMD) Log Analysis

First external test machine. Key observations from `mechcontrol.log`:

- **Baseboard**: X6FR5xxY (AMD CPU)
- **Profile match**: IDY_7Y via shifted ProjectID 13 (wrong match)
- **ECSpec byte[3]**: 0x62 = 98 (absolute temp encoding, max 98C)
- **0x463 at startup**: 0x5F = 95 (absolute temperature, OEM had 95C set)
- **PL1/PL2/PL4**: all 0x00 at startup (uninitialized before EC activation)
- **Battery saver defaults**: PL1=145W -- suspiciously high, may be different encoding
- **Fan curves**: same temp breakpoints as IDY_7Y, different duty values
- **Error on apply**: `Write verify failed at 0x786: wrote 0x8A, verify 0x463 = 138 (expected 10)` -- Intel encoding written to AMD EC
- **Failsafe**: CPU locked at 600MHz when EC accepted the wrong value (138)
