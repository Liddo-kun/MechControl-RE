# MechControl -- Architecture

Standalone replacement for the MECHREVO OEM Control Center. C# .NET 8.0 WinForms app that talks directly to the EC kernel driver, ITE USB HID controller, and BLE liquid cooler (Nordic UART Service).

For register addresses, encodings, and write sequences, see `register_reference.md`. For BLE liquid cooler details, see `Architecture_BLE.md`. For reverse engineering background, see `report.md`.

---

## 1. Project Structure

```
MechControl/
  MechControl.csproj              .NET 8.0, WinForms, WinExe, x64-only, Admin manifest
                                  TFM: net8.0-windows10.0.19041.0 (WinRT BLE APIs)
  Program.cs                      Entry point, UEFI setup, OEM conflict warning
  DiagnosticLog.cs                Session log: hardware detection, validation, EC state

  Driver/
    EcDriver.cs                   P/Invoke: CreateFile, DeviceIoControl, CloseHandle
    EcDevice.cs                   Persistent handle, thread-safe ReadByte/WriteByte/ReadSmapcTable, reconnect
    WriteGuard.cs                 Allowlist, read-modify-write, write-then-verify
    HidDevice.cs                  ITE USB HID enumeration + feature report sending (4 zones)
    BleDevice.cs                  WinRT BLE GATT: scan, connect (retry + cancellation), write, notify, disconnect

  Profile/
    ProfileSchema.cs              C# types mirroring the JSON schema (MachineProfile, ModeDefaultInfo, etc.)
    ProfileLoader.cs              Detect project ID, load matching JSON, validate
    UniversalDefaults.cs          Platform-wide defaults: bitfields, limits, allowlists, battery modes
    RegisterEncoding.cs           Encode/decode: direct, double, offset, inverted_offset, fan_switch_speed
    UserProfile.cs                JSON-serializable user profile (CPU, GPU, fan, battery, lighting, liquid cooler)
    UserProfileManager.cs         Save/load 5 profile slots + settings next to exe (AppContext.BaseDirectory)

  Features/
    HardwareDetection.cs          Startup EC interrogation (see Section 5)
    StartupValidator.cs           Gate features by EC register validation (see Section 6)
    EcActivationControl.cs        EC init 7-step handshake (see Section 4 startup-only table)
    UefiSetup.cs                  One-time UEFI NVRAM flag setup (self-healing)
    CpuPowerControl.cs            PL1/PL2/PL4 + CPU temp limit
    GpuPowerControl.cs            TGP + Dynamic Boost toggle/value
    FanControl.cs                 Fan boost, custom fan curve mode, fan table write
    FanSnapshot.cs                One-shot fan-table + curve-control register capture (see Section 5)
    FanCurveModel.cs              Fan curve points + hysteresis (`RangeCelsius = 4`)
    FanSwitchSpeedControl.cs      Fan ramp speed (0x787)
    LinkedFanController.cs        Linked fan mode (see Section 4 subsection)
    BatteryControl.cs             Battery health mode (3 modes) + telemetry + variant detection
    LightingControl.cs            Unified HID+EC controller for keyboard, light bar, logo
    LightingIdleTimer.cs          Idle timeout for KB + light bar
    LightingState.cs              Persist HID-only zone state + idle timer to JSON
    LocalDimmingControl.cs        Mini-LED local dimming toggle via WMI/ACPI GPIO
    LiquidCoolerControl.cs        BLE command layer: fan, pump, RGB, reset, commit, FW version
    LiquidCoolerState.cs          JSON persistence for saved device, auto-connect, last settings

  Monitoring/
    EcMonitor.cs                  WinForms Timer (1s), polls ~10 registers, fires Updated event

  UI/
    MainForm.cs                   TabControl + monitor panel + profile bar + system tray + EC enrichment orchestration
    PowerTab.cs                   CPU (PL1/PL2/PL4/temp limit) + GPU (TGP/DB) sliders
    FanTab.cs                     Boost/curve checkboxes + single Apply button + FanCurvePanel + fan switch speed + linked mode
    FanCurvePanel.cs              GDI+ chart, draggable points (point 0 locked to 0% duty), mouse interaction, ReadOnly mode
    FanCurveDebugSection.cs       Section with 2 read-only FanCurvePanels; live view of curves
                                  written to EC. Owns FanControl.CurvesApplied subscription.
                                  Mounted always but hidden unless Link Fans is on.
    LinkedFanStatusBar.cs         Custom-painted status bar for linked fan mode (fill bar + mixed-font text)
    BatteryTab.cs                 3 radio buttons + battery telemetry (live, 2s poll)
    LightingTab.cs                3 LightingZonePanels (keyboard, light bar, logo) + idle timer + mini-LED dimming
    LightingZonePanel.cs          Per-zone control: enable + brightness + RGB + Apply/Pick Color/Save.
                                  Extends Section. Rows added directly to Section (no inner wrapper TLP).
                                  Button row is a plain Panel with fixed Height + Width and manually
                                  positioned buttons — see "UI layout pattern" below.
    ProfileBar.cs                 5 profile slots + Save + Restore/Gaming/Office Defaults, docked at top
                                  Exposes ActiveSlot, ProfileApplied, ProfileListChanged for tray menu sync
    MonitorPanel.cs               Read-only status: fan speeds, temps, battery SOC
    LiquidCoolerTab.cs            BLE cooler: scan/connect (15s timeout), fan/pump/RGB, auto-reconnect with BT retry
    Section.cs                    Custom labeled panel (title intersecting thin border); replaces GroupBox
    UiHelpers.cs                  Shared fonts + builders: BuildSection/SliderTable/KeyValueTable/RgbFlow/
                                  ApplyButton/ApplyReadRow, TryUi/TryUiAsync error wrappers
    UiMetrics.cs                  Design tokens: MarginLeft, SectionGap, RowGap, GroupPad*, Button* widths
    UiColors.cs                   Semantic colors: StatusOk/Warn/Error/Neutral, Hint, Disabled

  Profiles/MECHREVO/
    IDY_7Y.json                   Verified profile (this machine)
    IDY_7Y_defaults.json          RE reference only — ec_snapshot diff (OEM vs firmware post-power-cycle).
                                  NOT loaded by the app. Restore Defaults uses IDY_7Y.json's defaultFanCurve.
    PH4AQE3.json, PH6PG0x.json   Profiles with hand-tuned modeDefaults (PL + GPU + fan curves)
    *.json                        Other models (registers only, limits/modes populated from EC at runtime)

  Resources/
    app.manifest                  requireAdministrator
    TrayIcons/kekwait_*.png       40 animated frames (embedded resource) for profile 5 tray icon

  build-scripts/
    build-release.ps1             Release build: dev exe + framework-dependent zip + self-contained zip + self-contained .7z (if 7-Zip installed)
```

### UI layout pattern

Tabs use layout-manager containers, not absolute positioning. A typical tab:

1. `UiHelpers.BuildSectionStack()` — outer 1-col TableLayoutPanel (`Dock.Fill`,
   `AutoScroll`, column `Percent 100`). One section per row.
2. `UiHelpers.BuildSection(title, fitContent)` — returns a `Section`
   (custom panel with a painted title intersecting its border).
   `fitContent: true` makes the section wrap its content horizontally;
   default stretches to the stack column width (sliders, wide rows).
3. Inner layout: `BuildSliderTable()` (3-col label/slider/value),
   `BuildKeyValueTable()` (2-col caption/value), or a `FlowLayoutPanel`
   for short inline rows. Populate via `AddSliderRow` / `AddKeyValueRow`.
4. Controls: `BuildSliderRow`, `BuildRgbFlow`, `BuildApplyButton`,
   `BuildApplyReadRow`. Event handlers wrap writes in
   `UiHelpers.TryUi` / `TryUiAsync` for uniform error surfaces.

Shared sizes/colors live in `UiMetrics` and `UiColors`; fonts are statics
on `UiHelpers`: `FontNormal` (9pt), `FontBold` (9pt bold), `FontHeader`
(12pt bold, section titles), `FontTiny` (7pt, graph labels). No hand-coded
pixel offsets in tab code — everything flows from `AutoSize` + `Dock` +
margins.

**Exception:** `LightingZonePanel`'s button row uses a plain `Panel` with
explicit `Height` + `Width` and manually positioned buttons
(`b.Location = new Point(x, 0); x += b.Width + ButtonGap`). A nested
`TableLayoutPanel` or `FlowLayoutPanel` here caused the parent `Section`'s
last row to be over-allocated vertically (~39px extra) when the button row
was narrower than its siblings. The panel's `Width` is pinned to a
constant (`BtnRowWidth = ButtonApplyWidth*2 + ButtonWideWidth +
ButtonGap*2`) — making it narrower re-triggers the bug.

---

## 2. EC Communication Layer

### EcDriver.cs -- P/Invoke only, no logic

```csharp
static class EcDriver
{
    CreateFile, DeviceIoControl (two overloads), CloseHandle
    Constants: GENERIC_READ_WRITE, FILE_SHARE_RW, OPEN_EXISTING
    IOCTL_ECREAD     = 0x9C40A488
    IOCTL_ECWRITE    = 0x9C40A48C
    IOCTL_SMAPCTABLE = 0x9C40A500
}
```

### EcDevice.cs -- Handle + thread-safe read/write

```
EcDevice : IDisposable
  _handle = CreateFile(@"\\.\ACPIDriver") at construction
  _lock   = new object()
  ReadByte(int addr) -> byte
  WriteByte(int addr, byte value)
  ReadSmapcTable(int index) -> uint     // ECSpec config via IOCTL_SMAPCTABLE
  TryReconnect() on failure, retry once
```

### HidDevice.cs -- ITE USB HID enumeration + feature reports

```
HidDevice : IDisposable
  _zoneHandles[4]  -- SafeFileHandle per zone (0=keyboard, 1=unused, 2=light bar, 3=logo)

  Open():
    Enumerates HID devices via SetupAPI, filters by VID=0x048D (ITE Tech),
    probes capabilities (FeatureReportByteLength == 9), and maps zones by
    HID usage value (0x0001 -> keyboard, 0x0002 -> light bar + logo).

  SendFeatureReport(int zone, byte[9] report)
  IsZoneAvailable(int zone) -> bool
```

### BleDevice.cs -- WinRT BLE GATT (Nordic UART Service)

Third communication channel. Uses WinRT `Windows.Devices.Bluetooth` APIs. Full details in `Architecture_BLE.md`.

```
BleDevice : IDisposable
  ScanAsync(durationMs)            BluetoothLEAdvertisementWatcher, filters LCT21001/LCT22002
  ConnectAsync(address, ct)        Resolve NUS service + TX/RX chars, enable CCCD, retry 3x.
                                   CancellationToken checked between GATT operations so callers
                                   can enforce a hard timeout via Task.WhenAny.
  WriteAsync(byte[])               Write to TX characteristic
  WriteAndReceiveAsync(byte[])     Write + await RX notification (FW version read)
  ConnectionLost : event           Fires on unexpected disconnect
```

---

## 3. Write Safety (WriteGuard.cs)

All EC writes go through WriteGuard. Nothing calls EcDevice.WriteByte directly.

1. **Allowlist** -- `HashSet<int>` of permitted addresses from profile JSON. Write to unlisted address throws.
2. **Read-modify-write** -- `WriteBitfield()` for bitfield registers (`0x7C5`, `0x7A6`, `0x743`): reads current value, applies mask, writes merged result.
3. **Write-then-verify** -- Every write is followed by readback. Mismatch triggers retry once, then throws. Supports `skipVerify` (e.g., HID trigger registers) and `verifyRedirect` (status registers that update asynchronously).

Value clamping is handled by individual feature controllers before calling WriteGuard, not by WriteGuard itself.

Special verification rules and the full allowlist are documented in `register_reference.md` section 9.

---

## 4. Feature Modules

Each feature module follows the same pattern: `ReadState()` reads current EC values, `Apply()` writes new values through WriteGuard.

| Module | Controls | Key Registers |
|--------|----------|---------------|
| `CpuPowerControl` | PL1, PL2, PL4, CPU temp limit. Temp write sets bit 7 as the commit/enable flag on both encodings — `EncodeCpuTempLimit` (Intel inverted_offset) and `EncodeCpuTempLimitDirect` (AMD direct). Without bit 7 the EC stores the value but does not enforce the limit. WriteGuard verifies by masking the low 7 bits on both sides of the `0x786`→`0x463` redirect; bit 7 at the read address is firmware bookkeeping and may be set or clear depending on EC state. | `0x783`-`0x786` |
| `GpuPowerControl` | TGP, Dynamic Boost toggle/value | `0x743`-`0x746` |
| `FanControl` | Fan boost, custom fan curve mode, fan table write. `ApplyCurves(cpu, gpu, enabled)` is the primary entry point: sets `0x7C5` bit 7 + `0x7C6 = 0x04` before writing both tables (probe-verified order), then fires `CurvesApplied` event for debug/UI consumers. `SetCustomFanCurveMode(bool)` writes only `0x7C5` bit 7 — standalone toggle for linked mode. `WriteCurve` writes 94 bytes per fan in array-by-array order (all tempUp, all tempDown, all duty): skips `tempDown[0]`, forces `duty[0] = 0x00` (EC rejects non-zero), enforces 4°C hysteresis via `EnforceRange()`. Padding: `tempUp/tempDown = 0xFF` (first padding tempDown = lastActive.TempDown + 4), `duty = last active duty`. GPU curves gated by `HasGpuFan`, all curves gated by format == RamFan1. `CurvesApplied` passes raw references (subscribers must Clone if persisting) and is gated at the subscriber via `Visible` cascade so it's a no-op when the form is hidden to tray. | `0x751`, `0x7C5`, `0x7C6`, `0xF00`-`0xF5F` |
| `FanSwitchSpeedControl` | Fan ramp speed | `0x787` |
| `LinkedFanController` | Linked fan mode: polls temps (1.5s), evaluates user curves, writes safe curves with per-sensor safety ramp | `cpuTemp`, `gpuTemp`, fan table |
| `BatteryControl` | Battery health mode (3 modes) + telemetry. Reads `0x7A6` at construction to detect variant: Intel bit 3 set (`0x08`/`0x18`/`0x28`) vs AMD bit 3 clear (`0x00`/`0x10`/`0x20`). Cached `bit3Flag` used for all encodes; profile `BatteryModes` updated in-place for RestoreDefaults. | `0x7A6`, `0x402`-`0x4AB` |
| `LightingControl` | Keyboard, light bar, logo LED | HID zones 0/2/3 + EC `0x767`-`0x78C` |
| `LightingIdleTimer` | Auto-off KB + light bar after idle | `GetLastInputInfo` polled 1s |
| `LocalDimmingControl` | Mini-LED local dimming toggle | WMI/ACPI + `0x72B` |
| `LiquidCoolerControl` | BLE cooler: fan, pump voltage, RGB, commit. Gated by EC `0x7A4` bit 7. See `Architecture_BLE.md`. | BLE (NUS) |

### Startup-only modules

| Module | Purpose |
|--------|---------|
| `HardwareDetection` | Reads ~40 EC registers + ECSpec config; provides `ExtractLimits()` and `ExtractModeDefaults()`. Reads `0x7A4` bit 7 for `SupportsLiquidCooling`. |
| `StartupValidator` | Validates bitfields + feature registers; gates which features are safe to enable. Disables `gpuPower` if `ModuleIdGpu == 0` (no discrete GPU). |
| `UefiSetup` | One-time UEFI NVRAM setup: sets ApExistFlag, ApUseFlag, PowerMode, MemOC. Self-healing (checks on every launch, writes only if needed). Validates struct before writing. |
| `EcActivationControl` | EC init (7-step handshake): handshake, activation, feature enable, power management (`0x726`/`0x727`), `0x7C5 \|= 0x60` (bits 5+6 only, preserving bit 7), `0x7C7=0x0C`, `0x706=0x41`. Verifies `0x741` transformation post-init. **Does not** write the fan table, set `0x7C6`, or touch `0x7C5` bit 7 — those moved to the orchestrator. |
| `FanSnapshot` | One-pass capture of fan table + `0x7C5`/`0x7C6`. `IsValid()` checks CPU table only. `DetectGpuPresent()` returns tri-state. See Section 5 for detection logic. |

`RegisterEncoding.cs` handles all encoding transforms (direct, double, offset, inverted_offset, fan_switch_speed, battery mode). Encoding type is specified per-register in the profile JSON, except PL4 (determined at runtime from `0x727` bit 7) and battery mode (variant detected at runtime by `BatteryControl`).

### Linked Fan Mode (LinkedFanController)

EC firmware evaluates CPU and GPU fan tables independently — no hardware "link fans" register exists. Linking is done in software:

1. **Poll** (1.5s timer): Read CPU/GPU temps from EC, evaluate each user curve via linear interpolation
2. **Compute**: `linkedDuty = max(cpuDuty, gpuDuty)`
3. **Write**: Build a safe curve for each fan table at `linkedDuty`, write to both CPU and GPU fan table regions
4. **Skip**: Only rewrite EC if duty change is significant (>=2% or crossing the low zone threshold)

**Safety ramp**: Each written curve is flat at `linkedDuty` up to the current sensor temp, then ramps linearly to 100% at the last point (96°C). The ramp start is per-sensor — CPU curve ramps based on CPU temp, GPU curve based on GPU temp. If the app crashes, the EC keeps these curves and fans ramp up if temps rise.

Fixed temp points: 10, 16, 33, 42, 51, 60, 69, 78, 87, 96°C. Points 0–1 are low-zone sentinels (below any realistic operating temp); points 2–9 form the working curve with 9°C spacing. Safe-curve TempDown uses `tempDown[i] = tempUp[i-1] − RangeCelsius` matching `FanCurveModel.EnforceRange`; point 0 duty is pinned to 0% (EC requires `duty[0] = 0x00`).

**Panels as source of truth**: The user's editable fan curves live in FanCurvePanel. LinkedFanController reads them via `Func<FanCurveModel?>` delegates each tick — never caches stale references. Profile save reads from panels, not EC (EC has flat/safe curves during linked mode). Profile load writes to panels, then EC picks up the new curves on next tick.

**Shutdown**: `LinkedFanController.Dispose()` restores real panel curves to EC. MainForm disposes the EC device *after* child controls (`base.Dispose`) so this write succeeds.

**UI**: Compact painted status panel to the right of the Link Fans checkbox. Shows linked duty % as a filled bar, CPU/GPU temps and contributions, and safety ramp start temps. Only invalidates when visible (guards against hidden/minimized). A `FanCurveDebugSection` below the editable curves shows the actual CPU/GPU curves written to the EC (read-only, live); it is mounted always but `Visible = false` when Link Fans is off, and its `CurvesApplied` handler cascades the same `Visible` check so no Clone/Invalidate work happens when hidden.

**Profile persistence**: `linkedEnabled` is saved/loaded in user profiles. On auto-load, curves are loaded into panels first, then `SetLinkedEnabled(true)` starts the controller.

---

## 5. Hardware Detection & EC Enrichment

`HardwareDetection` reads the following from the EC at startup (all reads, no writes):

| Category | Registers | Purpose |
|----------|-----------|---------|
| Support bytes | `0x765`, `0x766`, `0x742`, `0x78E` | Feature capability flags |
| Identity | `0x7D3`, `0x7D2`, `0x771`/`0x772`, `0x454`/`0x47D`, `0x456` | Module ID, ROM ID, EC version, System ID |
| CPU PL4 double flag | `0x727` | Bit 7 (`PL4D`) determines PL4 encoding: set = `0x785` stores watts/2 (raw × 2 = watts), clear = raw = watts directly. Firmware/DSDT-controlled, stable for the lifetime of the machine. |
| Gaming PL defaults | `0x730`-`0x733`, `0x7D8` | PL1/PL2/PL4/D/TCC for gaming mode |
| Office PL defaults | `0x734`-`0x737`, `0x7D9` | PL1/PL2/PL4/D/TCC for office mode |
| Battery saver defaults | `0x7A7`-`0x7AA` | PL1/PL2/PL4/D for battery saver mode |
| Turbo TCC | `0x7DA` | Turbo mode TCC offset |
| VRM | `0x753`, `0x754` | CPU VRM current limit / max |
| ECSpec (SMAPCTABLE) | IOCTL indices 0x00-0x0B | PL max, TGP base/max, DB max, TCC, GPU target temp |

### Profile enrichment (MainForm constructor)

EC-read data enriches the loaded profile. Profile JSON values always take priority when populated.

```
1. HardwareDetection reads all registers above
2. If unverified profile: ECSpec-derived limits override conservative defaults
   (PL1/PL2/PL4 max, TGP range, DB max, CPU temp max)
3. PL4 encoding stamped from 0x727 bit 7 (applies to all profiles — hardware
   fact, not a profile opinion). The same flag governs how ExtractLimits and
   ExtractModeDefaults decode PL4 max and per-mode PL4 defaults.
4. If profile has no modeDefaults: EC per-mode PL registers populate them
   (gaming/office/batterySaver with PL1/PL2/PL4/TCC — no GPU or fan curves)
5. Profile modeDefaults with hand-tuned fan curves + GPU settings take priority
```

CTGP/DynamicBoost are NOT populated from EC — the EC doesn't store per-mode GPU defaults in simple registers. Those only come from profile JSON (hand-tuned for specific models).

### Fan table detection & state orchestration (MainForm constructor, after EC init)

MainForm takes a single `FanSnapshot.Read` after init (one pass over `0xF00-0xF6F` + `0x7C5` + `0x7C6`) and uses the cached result for detection, diagnostics, panel seeding, and the custom-mode decision.

Detection overrides apply **only when `fanSnapshot.IsValid()`** (≥2 active entries, strictly ascending tempUp in `[1, 253]°C`). Invalid snapshots (all-zero on cold boot, all-0xFF after wipe) keep profile defaults.

When valid:
1. **activeEntries** — scans CPU tempUp, stops at padding (≥254), zero, or non-ascending. Overrides profile.
2. **GPU fan presence** — tri-state: `true` (real data in `[1, 253]`), `false` (all-`0xFF`, OEM absent pattern), `null` (ambiguous, keeps profile default). Gated by `IsValid()` to avoid misinterpreting garbage bytes.

**Panel seeding** — three-tier fallback:
1. **Snapshot** — `BuildCpuCurve()`/`BuildGpuCurve()`. Individual curves with 0 active entries fall back to generic default (`IsValid()` only checks CPU table).
2. **Profile default** — `profile.DefaultFanCurve` if snapshot invalid.
3. **Generic default** — `FanCurveModel.CreateGenericDefault()` (10-point, 30–90°C → 0–100%, point 0 at 0% duty).

**Custom-mode decision**:
- **User profile exists** → load slot, apply `SetCustomFanCurveMode(saved state)`, verify-read snapshot for logging.
- **No profile, valid snapshot** → EC untouched, panels hold snapshot curves, checkbox reflects EC state.
- **No profile, invalid snapshot** → park custom mode off, warn user to review Fan tab and Apply manually.

---

## 6. Startup Validation (StartupValidator.cs)

After profile loading, EC enrichment, and EC init handshake, `StartupValidator` determines which features are safe to enable:

**Verified profiles**: All features enabled unconditionally. Bitfields/registers still logged for diagnostics.

**Unverified profiles**: Each feature is gated by two checks:

1. **Bitfield validation** -- Read register, apply mask, check against known-valid values:
   - `fanCurveEnable` (0x7C5 & 0x80): must be 0x00 or 0x80
   - `batteryMode` (0x7A6 & 0x38): must be in `{0x00, 0x08, 0x10, 0x18, 0x20, 0x28}` (Intel bit3=set: `0x08`/`0x18`/`0x28`; AMD bit3=clear: `0x00`/`0x10`/`0x20`)
   - `dynamicBoostEn` (0x743 & 0x07): must be 0x00, 0x05, or 0x07
   - `logoOnOff` (0x7AB & 0x80): must be 0x00 or 0x80

2. **Feature register validation** -- Read register, decode, check against sanity ranges:
   - PL1/PL2: 5-300W
   - PL4: 10-600W
   - CPU temp: 60-105C
   - TGP: 50-250W
   - Dynamic Boost: 0-50W

   **ECSpec-enriched bypass**: When ECSpec already confirmed a register class's layout via `ExtractLimits()`, out-of-range current values (e.g. PL1/PL2/PL4 = 0, the EC sentinel for "firmware controls power") are skipped rather than disabling the feature.

3. **GPU module presence** -- If `ModuleIdGpu` (0x7D2) == 0, disables `gpuPower`. On CPU-only machines, TGP/DB registers may contain random data that passes sanity checks; this catches them. GPU fan presence (`HasGpuFan`) is also logged as a secondary indicator.

Failed validation disables the corresponding feature tab in the UI.

---

## 7. Monitoring (EcMonitor.cs)

WinForms Timer at 1000ms. Reads ~10 registers per tick. Raises `Updated` event consumed by `MonitorPanel` (docked at bottom of MainForm).

Polled data: CPU/GPU fan %, CPU/GPU fan RPM, CPU temp limit, battery SOC, battery mode, fan boost status, fan curve status.

Stops after 5 consecutive read failures.

---

## 8. Profile Detection & JSON Schema

### Project ID Detection

```
1. Read EC register 0x740
2. Scan all profile JSONs, match projectIdValue against raw value
3. If no match, try raw >> 1 (newer models store ProjectID * 2)
4. Try WMI baseboard name (Win32_BaseBoard.Product) -- substring match
5. Prompt user to pick manually (ListBox dialog)
```

Verified: 0x740=26, 26>>1=13=IDY_7Y.

### Profile JSON Structure

```jsonc
{
  "projectId": "IDY_7Y",
  "projectIdValue": 13,
  "verified": false,
  "driver": { "devicePath": "\\\\.\\ACPIDriver" },
  "registers": { /* canonical name -> hex address mapping */ },
  "fanTable": { /* format, base addresses, entry count, padding */ },
  "limits": { /* per-register min, max, encoding, offset, tjMax */ },
  "bitfields": { /* register -> mask mapping for RMW registers */ },
  "batteryModes": [ /* { name, value } for each mode */ ],
  "modeDefaults": { /* per-mode power presets: gaming, office, batterySaver */ },
  "defaultFanCurve": { /* optional factory fan curve snapshot */ },
  "skipVerify": ["0x767", "0x78C"],
  "verifyRedirect": { /* write addr -> read addr for async registers */ },
  "writeAllowlist": ["0x783", "0x784", ..., "0xF00-0xF5F"]
}
```

### modeDefaults (per-mode power presets)

```jsonc
"modeDefaults": {
  "gaming": {
    "PL1": 45, "PL2": 55, "PL4": 90,       // watts
    "CTGP": 10, "dynamicBoost": 25,          // GPU (profile-only, not from EC)
    "tccOffset": 100,                         // CPU temp limit in degrees C
    "fanCurve": { "cpu": { ... }, "gpu": { ... } }  // optional
  },
  "office": { ... }
}
```

Source priority: Profile JSON > EC registers > absent. Profiles with hand-tuned `modeDefaults` (e.g. PH4AQE3, PH6PG0x) include fan curves and GPU settings. For all other models, `modeDefaults` is auto-populated from EC registers at startup with CPU power limits only.

### UniversalDefaults

`UniversalDefaults.ApplyToProfile()` runs after JSON load, before EC enrichment. Fills empty profile fields with platform-wide constants derived from DLL disassembly:

- Bitfield definitions (fanCurveEnable, batteryMode, dynamicBoostEn, logoOnOff)
- Conservative power limits (PL1: 15-150W, PL2: 15-150W, etc.)
- Write allowlist (init + feature registers + fan table range)
- SkipVerify and VerifyRedirect entries
- Battery mode names and values

### Register Lookup Flow

All EC register access goes through the profile's `registers` dictionary. Code never hardcodes hex addresses for feature registers — it looks up a canonical string name to get the address:

```
Profile JSON                          Code
-----------------                     -------------------------------------
"registers": {                        int addr = profile.GetRegister("cpuTemp");
  "cpuTemp": "0x43E",        -->     byte val  = device.ReadByte(addr);
  "cpuFanDuty": "0x75B",             // val = current CPU temp in degrees C
  ...
}
```

`profile.GetRegister(name)` does a dictionary lookup and parses the hex string to an integer. If the register name is missing, it throws `KeyNotFoundException`.

**Canonical register names** used in code (must exist in each profile's `registers` dict):

| Name | Typical Address | Used By |
|------|----------------|---------|
| `cpuTemp` | `0x43E` | LinkedFanController |
| `gpuTemp` | `0x44F` | LinkedFanController |
| `cpuFanDuty` | `0x75B` | EcMonitor |
| `gpuFanDuty` | `0x75C` | EcMonitor |
| `cpuFanRpmLo/Hi` | `0x464/0x465` | EcMonitor |
| `gpuFanRpmLo/Hi` | `0x46B/0x46C` | EcMonitor |
| `fanControlMode` | `0x751` | FanControl |
| `fanCurveEnable` | `0x7C5` | FanControl, EcMonitor |
| `fanSwitchSpeed` | `0x787` | FanSwitchSpeedControl |
| `PL1/PL2/PL4` | `0x783-0x785` | CpuPowerControl |
| `cpuTempLimitRead/Write` | `0x463/0x786` | CpuPowerControl |
| `tgpDbCtrl/tgp/dbTotalPower/dbMaxTgp` | `0x743-0x746` | GpuPowerControl |
| `batteryMode/batterySoc` | `0x7A6/0x4AB` | BatteryControl, EcMonitor |
| `statusByte` | `0x768` | EcMonitor |
| `logoOnOff/logoR/logoG/logoB` | `0x7AB/0x769-0x76B` | LightingControl |
| `logoTrigger/logoCommit` | `0x767/0x78C` | LightingControl |
| `projectId` | `0x740` | ProfileLoader |

Note: HardwareDetection uses hardcoded addresses (0x730-0x737, 0x765, 0x7D3, etc.) because these are platform-wide discovery registers, not per-model feature registers.

### Battery Telemetry

`BatteryControl.ReadBatteryTelemetry()` reads ACPI battery registers (0x402-0x4AB) — hardcoded, same on all Uniwill models. Encoding details (endianness, units, temperature format) in `register_reference.md` section 11.1. `BatteryTab` polls every 2s while active.

---

## 9. Lighting State Persistence

Keyboard and light bar are HID-only -- no EC registers to read state from on startup. State is persisted to `lighting_state.json` next to the exe. On startup:
- Logo: reads current state from EC registers
- Keyboard, light bar: restores from saved JSON
- Idle timer timeout (seconds): restores from saved JSON, also saved per user profile

---

## 10. User Profiles

5 saveable profile slots stored next to the exe (`AppContext.BaseDirectory`) as `userprofile_1.json` through `userprofile_5.json`, plus `settings.json` (last-used slot, profile names).

Profiles store decoded human-readable values (watts, degrees C, percent). Fan curves stored as raw EC bytes (16 padded entries). Encoding/decoding happens at save/load time. Fan section also stores `linkedEnabled`. Lighting settings include idle timer timeout and local dimming state.

### Startup Flow

```
Steps 1-11 cover UEFI setup, OEM conflict check, EC device open, profile load,
WriteGuard creation, hardware detection, EC enrichment, EC init handshake,
fan snapshot, fan table detection, and startup validation.
See Sections 4 (startup modules), 5 (detection & enrichment), 6 (validation).

12. Create feature controllers (gated by validation results)
13. Create UI tabs (only for enabled features; GPU curve panel hidden if no GPU fan)
14. Create ProfileBar (Restore Defaults + Gaming/Office Defaults + 5 profile slots)
15. Seed fan panels + custom-mode decision (see Section 5 panel seeding)
16. Auto-load last user profile if saved, else leave EC alone or park custom mode off
17. Start monitor + show tray icon (starts hidden — no window, tray icon only)
```

### Apply Order (Profile Load)

`ApplyCurves(cpu, gpu, curveEnabled)` (sets `0x7C5` bit 7 + `0x7C6=0x04`, then writes both tables) -> linked fans enable -> CPU power -> GPU power -> fan boost -> fan switch speed -> battery -> lighting -> local dimming.

### System Tray Behavior

Minimize or close hides to tray. On hide: EC monitor and battery telemetry stop (display-only polling). On show: both restart if applicable. LinkedFanController timer keeps running — it must continue writing fan curves to the EC regardless of UI visibility. Status panel `Invalidate()` is guarded by `Visible` check (skips when minimized or on another tab).

**Tray context menu**: Show, 5 profile slots (checked = active, grayed = empty), Exit. Profile items call `ProfileBar.LoadAndApplySlot()` — same code path as the main UI. Menu rebuilds on profile load, save, or rename.

**Per-profile tray icons**: Profiles 1–4 show generated colored numbered circles (placeholder). Profile 5 plays a 40-frame kekwait animation (embedded PNGs, `System.Threading.Timer` at 67ms/15fps) once on selection or app startup, then holds the last frame. No profile active = default exe icon.

### Shutdown / Disposal Order

MainForm.Dispose disposes tray icon/menu first, then `base.Dispose()` (which disposes child controls including FanTab → LinkedFanController, and LiquidCoolerTab which unsubscribes `ConnectionLost` + stops reconnect timer), then EC device and HID device last. This ordering is critical: LinkedFanController.Dispose restores real panel curves to the EC, which requires the EC device to still be open.

### Mode Defaults (Gaming/Office/BatterySaver buttons)

Two-tier data source:
1. **Profile JSON** (PH-series models with hand-tuned data): applies PL1/PL2 + PL4 + TCC + CTGP/DB + fan curves
2. **EC register fallback** (all other models): applies PL1/PL2 + PL4 + TCC only (no GPU, no fan curves)

Confirmation dialog describes exactly what will be applied.

### Save Flow

Fan curves are read from FanTab panels (source of truth), not from EC — EC may have flat/safe curves during linked mode. All other state reads from feature controllers + LightingState. `linkedEnabled` is saved so linked mode restores on profile load.

---

## 11. Diagnostic Logging (DiagnosticLog.cs)

Every session appends to `mechcontrol.log` next to the exe. Logs hardware identity (ModuleID, ROMID, EC version, VRM), ECSpec config, per-mode PL defaults, effective limits and their source, feature register values (raw + decoded), fan table hex dumps (snapshot + post-write verify), fan detection results (activeEntries, GPU presence), startup validation pass/fail per feature, HID enumeration, and EC init steps. UI errors caught by `TryUi`/`TryUiAsync` are logged before the popup is shown, so failed profile applies etc. leave a trail. Designed so a remote user's log contains everything needed to build a verified profile.

Retention: `Start()` trims the log on each launch before opening the writer — keeps the very first session (baseline snapshot of the machine's original state) plus the 5 most recent sessions, drops everything between. Trimming is opportunistic; any IO failure silently skips rotation for that launch.

---

