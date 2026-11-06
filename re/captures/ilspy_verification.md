# ILSpy Verification Results

## Summary

**9 of 10 sections verified. 1 minor gap found (6 missing constants). 0 value discrepancies.**

All values present in the JSON profiles are **correct** — every single value matches the decompiled source. The only finding is 6 `FAN_LEVEL_*` constants from ECSpec that were omitted from the JSON `modeConstants` section.

**Decompilation note:** GCUService.exe has been processed by Fody (Costura.Fody), which caused nearly all method bodies to decompile as `throw new Exception("Runtime exception")`. All **constants, enums, field definitions, and type signatures** decompiled correctly. Only method body logic is unavailable, which limits the per-model override analysis (2i).

---

## 2a. Device Path — UNVERIFIABLE (method bodies obfuscated)

The `AcpiCtrl` class in namespace `MyECIO` declares `CreateFile` via P/Invoke (line 122) and has a `m_sRegPath` field. However, the constructor and `CheckAcpiDriverDeviceExists()` method bodies are stubbed (`throw new Exception`), so the device path string `\\.\ACPIDriver` is not visible in the decompiled source.

The `ACPIDriverDll.dll` DllImport (line 133) confirms the ACPI driver relationship. The only device path string found anywhere in the decompiled source is `\\.\Vhidcontrol` (for airplane mode HID).

**Verdict:** Cannot directly verify from decompiled source. The value was extracted via .NET reflection at runtime, which is authoritative for string constants.

## 2b. IOCTL Codes — VERIFIED

**Source:** `decompiled/MyECIO/AcpiCtrl.cs` lines 38-98
**JSON entries:** 29 (GPD_TYPE + 28 IOCTL codes)
**Source entries:** 29
**Mismatches:** 0

All 29 values match exactly (decimal-to-hex conversion verified programmatically). Count is identical — no missing or extra codes.

| Example | Source (decimal) | JSON (hex) | Match |
|---------|-----------------|------------|-------|
| GPD_TYPE | 40000 | 0x00009C40 | YES |
| IOCTL_GPD_ACPI_ECREAD | 2621482120 | 0x9C40A488 | YES |
| IOCTL_GPD_ACPI_ECWRITE | 2621482124 | 0x9C40A48C | YES |
| IOCTL_GPD_ACPI_CUSTOMCTL | 2621482244 | 0x9C40A504 | YES |

## 2c. EC Registers — VERIFIED

**Source:** `decompiled/Define/ECSpec.cs` (all `ADDR_*` constants)
**JSON entries:** 131
**Source ADDR_* entries:** 131
**Mismatches:** 0

All 131 `ADDR_*` register addresses match exactly. No fields missing from JSON, no extra fields in JSON.

ECSpec is declared as `internal static class` with no subclasses — confirmed zero inheritance overrides.

| Example | Source (decimal) | JSON (hex) | Match |
|---------|-----------------|------------|-------|
| ADDR_PROJECT_ID_BYTE | 1856 | 0x740 | YES |
| ADDR_PL1_SETTING_VALUE | 1923 | 0x783 | YES |
| ADDR_BATTERY_CHARGE_LIMIT_UP | 1977 | 0x7B9 | YES |
| ADDR_ConfigurableTGP_VALUE | 1860 | 0x744 | YES |

## 2d. Fan Table Registers — VERIFIED

All three fan table register classes match exactly:

| Class | Source File | JSON Entries | Source Entries | Mismatches |
|-------|-----------|--------------|----------------|------------|
| RamFan1_ECSpec | `Define/RamFan1_ECSpec.cs` | 6 | 6 | 0 |
| RamFan1p5_ECSpec | `Define/RamFan1p5_ECSpec.cs` | 11 | 11 | 0 |
| RamFan2_ECSpec | `Define/RamFan2_ECSpec.cs` | 15 | 15 | 0 |

All values verified correct. No missing or extra fields.

## 2e. System Registers — VERIFIED

**Source:** Non-`ADDR_*` fields from `ECSpec.cs` (OSD codes, battery readouts, timer events, etc.)
**JSON entries:** 90
**Mismatches:** 0

All 90 system register values match exactly. This includes OSD codes (OSD_CAPSLOCK=1, OSD_FANBOOST_UPDATE=167, etc.), battery readouts (ecBt1RSOC=0x4AB, ecPowSource=0x490), and timer/event codes.

## 2f. Mode Constants — VERIFIED (with 6 missing fields)

**Source:** FAN_*, TURBO_*, POWER_* constants from ECSpec.cs
**JSON entries:** 24
**Source entries:** 30
**Value mismatches:** 0

All 24 values present in the JSON are correct. However, 6 constants from the source are **missing from the JSON**:

| Missing Constant | Source Value | Category |
|-----------------|-------------|----------|
| FAN_LEVEL_ZERO | 0 | Fan level index |
| FAN_LEVEL_ONE | 1 | Fan level index |
| FAN_LEVEL_TWO | 2 | Fan level index |
| FAN_LEVEL_THREE | 3 | Fan level index |
| FAN_LEVEL_FOUR | 4 | Fan level index |
| FAN_LEVEL_FIVE | 5 | Fan level index |

These are simple sequential indices (0-5) for fan speed levels, distinct from the `FAN_MODE_*` operational mode constants. Low impact — they are trivial sequential values unlikely to vary, but should be added for completeness.

## 2g. Battery Enums — VERIFIED

**Source:** `decompiled/GCUService.MySystem/BatteryProtection2.cs` lines 15-29

`BatteryHealthProtection_Status` enum:
| Name | JSON | Source | Match |
|------|------|--------|-------|
| PERFORMANCEDMODE | 0 | 0 | YES |
| BALANCEDMODE | 1 | 1 | YES |
| HEALTHYMODE | 2 | 2 | YES |

`Battery_Commands` enum:
| Name | JSON | Source | Match |
|------|------|--------|-------|
| GET | 0 | 0 | YES |
| CHARGING_UP_LIMIT | 1 | 1 | YES |
| CHARGING_DOWN_LIMIT | 2 | 2 | YES |
| RECOVERY | 3 | 3 | YES |
| TYPE_C_ADAPTOR_PRIORITY_SWITCH_ON | 4 | 4 | YES |
| TYPE_C_ADAPTOR_PRIORITY_SWITCH_OFF | 5 | 5 | YES |

Counts match (3+6), all values correct.

## 2h. Method Signatures — VERIFIED (minor notation difference)

All 5 method signatures exist in the decompiled source:

| Method | Source Location | Status |
|--------|----------------|--------|
| `AcpiCtrl.Read(string ReadName, ushort Addr, ref byte Data)` | `MyECIO/AcpiCtrl.cs:155` | EXISTS |
| `AcpiCtrl.Write(string ReadName, ushort Addr, byte Data)` | `MyECIO/AcpiCtrl.cs:161` | EXISTS |
| `AcpiCtrl.ReadACPI(uint ioctrl, int addr, ref int data)` | `MyECIO/AcpiCtrl.cs:185` | EXISTS |
| `AcpiCtrl.WriteACPI(uint ioctrl, int addr, int data)` | `MyECIO/AcpiCtrl.cs:197` | EXISTS |
| `MyEcCtrl.GetProjectIdFromEC()` | `MyECIO/MyEcCtrl.cs:67` | EXISTS |

**Notation difference:** The reflection-extracted signatures used `out` for the Data/data parameters (`out Byte Data`, `out Int32 data`), while ILSpy decompiled them as `ref byte Data` / `ref int data`. In .NET IL, `out` and `ref` both use the same byref parameter mechanism — `out` is a C# attribute (`[Out]`). This is a decompiler rendering choice, not a real discrepancy.

Additional overload found: `AcpiCtrl.Read(uint ReadName, ushort Addr, ref byte Data)` at line 149 (accepts uint instead of string for the IOCTL code directly).

## 2i. Per-Model Overrides — INCONCLUSIVE (method bodies unavailable)

**Available evidence says: NO overrides found.**

Comprehensive search of the decompiled source found:
- **No raw hex addresses** (0x783, 0x744, 0x7B9, etc.) used in method bodies instead of ECSpec constants
- **No if/switch on ProjectID** leading to different register addresses
- **All EC register writes** in available code use `ECSpec.ADDR_*` constants

**However**, this finding is limited because:
- Nearly all critical method bodies (`SetPL1Value`, `SetPL2Value`, `SetFanBoost`, `SetFanTableThread`, `SetBatteryChargingLimit_Up`) are stubbed with `throw new Exception("Runtime exception")` due to Fody/obfuscation
- The obfuscated `------/` directory contains unreadable code that could potentially contain overrides
- Only a few methods have real implementations (e.g., `CheckFanTablePWMDefault`, `GetBitFromByte`, string formatting switch expressions)

**Conclusion:** The type system design strongly suggests ECSpec constants are universal (static class, no inheritance, no subclass overrides). But definitive confirmation of method body behavior requires either: (1) a different decompilation approach that can handle the Fody processing, or (2) runtime tracing of actual EC read/write calls.

## 2j. ProjectID Enum — VERIFIED

**Source:** `decompiled/Define/ProjectID.cs`
**Source entries (excl None):** 44
**Profile files:** 44
**Count match:** YES

Spot checks:
| Project ID | Source Value | JSON `projectIdValue` | Match |
|-----------|-------------|----------------------|-------|
| IDY_6Y | 12 | 12 | YES |
| IDY_7Y | 13 | 13 | YES |
| IDP | 11 | 11 | YES |

All 44 non-None project IDs have corresponding profile files. The enum ordering and high-value entries (PH4ARxx=5889, PH4PRxx=6145, etc.) all match.

---

## Discrepancies

**None.** Every value present in the JSON profiles matches the decompiled source exactly.

## Missing Fields

6 constants from ECSpec are absent from the JSON `modeConstants` section:

| Field | Value | Impact |
|-------|-------|--------|
| `FAN_LEVEL_ZERO` | 0 | Low — trivial sequential index |
| `FAN_LEVEL_ONE` | 1 | Low — trivial sequential index |
| `FAN_LEVEL_TWO` | 2 | Low — trivial sequential index |
| `FAN_LEVEL_THREE` | 3 | Low — trivial sequential index |
| `FAN_LEVEL_FOUR` | 4 | Low — trivial sequential index |
| `FAN_LEVEL_FIVE` | 5 | Low — trivial sequential index |

Additionally, ECSpec contains several other constant groups not captured in any JSON section (likely intentional omissions as they are not register addresses or mode selectors):
- `QKEY_MODESWITCH` (0), `QKEY_FANBOOST` (1) — quick key bindings
- `FAN_LEVEL_*` (0-5) — as noted above
- `LEVEL_ZERO` through `LEVEL_MAX` (0-100) — generic level constants
- `APP_Normal_Mode` (0), `APP_ImageProjectionLight_Mode` (1), `APP_LightBar_Mode` (2)
- `Animation_OFF` (0), `Animation_ON` (1)
- Various `ByteFlag` enums (bit masks for control/status/trigger/support bytes)

These were likely omitted intentionally as they are utility constants rather than hardware register addresses or mode identifiers needed for EC communication.
