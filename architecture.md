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
  AppJsonContext.cs               Source-generated System.Text.Json context (trim-safe serialization)

  Driver/
    EcDriver.cs                   P/Invoke: CreateFile, DeviceIoControl, CloseHandle
    EcDevice.cs                   Persistent handle, thread-safe ReadByte/WriteByte/ReadSmapcTable, reconnect
    WriteGuard.cs                 Allowlist, read-modify-write, write-then-verify
    HidDevice.cs                  ITE USB HID enumeration + feature report sending (4 zones)
    BleDevice.cs                  WinRT BLE GATT: scan, connect (retry + cancellation), write, notify, disconnect

  Hardware/Nvidia/
    NvidiaApi.cs                   Versioned NVAPI BIOS/power/current requests; NVML enforced-limit readback
    NvidiaRuntimeNvml.cs           Selected-adapter PCI identity and enforced power telemetry
    NvidiaBudget.cs                Laptop/shared-budget reads and writes (originals kept in memory)
    NvidiaBudgetTransport.cs       Validated native F7 transport discovery (no copied driver RVAs)
    NvidiaBios.cs                  Validated BIT power-policy parser and targeted software override records
    NvidiaOverrideStore.cs         Registry overrides of the identified adapter; saved original values (originals.txt)
    NvidiaDeviceRestart.cs         Restart only the identified NVIDIA PnP device

  Profile/
    ProfileSchema.cs              C# types mirroring the JSON schema (MachineProfile, ModeDefaultInfo, etc.)
    ProfileLoader.cs              Detect project ID, load matching JSON, validate
    UniversalDefaults.cs          Platform-wide defaults: bitfields, limits, allowlists, battery modes
    RegisterEncoding.cs           Encode/decode: direct, double, offset, inverted_offset, fan_switch_speed
    UserProfile.cs                JSON-serializable user profile (CPU, GPU, fan, battery, lighting, liquid cooler)
    UserProfileManager.cs         Save/load 5 profile slots + settings next to exe (AppContext.BaseDirectory)
    GpuFailsafeSettings.cs        Persisted per-profile failsafe configuration + threshold bounds
    ThermalProtectionState.cs     Thermal hold state, held actions, trip record (persisted in settings)

  Features/
    HardwareDetection.cs          Startup EC interrogation (see Section 5)
    StartupValidator.cs           Gate features by EC register validation (see Section 6)
    EcActivationControl.cs        EC init 7-step handshake (see Section 4 startup-only table)
    EcActivationWatchdog.cs       Re-runs EC init after verify failure, on resume, or on a failed periodic spot check
    UefiSetup.cs                  One-time UEFI NVRAM flag setup (self-healing)
    CpuPowerControl.cs            PL1/PL2/PL4 + CPU temp limit
    GpuPowerControl.cs            TGP + Dynamic Boost toggle/value
    NvidiaPowerControl.cs         BIOS ceilings, NVVDD/MSVDD currents, restart/readback coordination
    GpuTemperatureFailsafe.cs     Thermal trip/acknowledged holds, optional CPU/fan actions, scoped restoration
    FanControl.cs                 Fan boost, custom fan curve mode, fan table write
    FanSnapshot.cs                One-shot fan-table + curve-control register capture (see Section 5)
    FanCurveModel.cs              Fan curve points + hysteresis (`RangeCelsius = 4`)
    FanSwitchSpeedControl.cs      Fan ramp speed (0x787)
    LinkedFanController.cs        Linked fan mode (see Section 4 subsection)
    BatteryControl.cs             Battery health mode (3 modes) + telemetry + variant detection
    LightingControl.cs            Unified HID+EC controller for keyboard, light bar, logo
    LightingIdleTimer.cs          Idle timeout for KB + light bar
    LightingState.cs              Persist HID-only zone state + idle timer to JSON
    LightingResumeRestore.cs      After resume: reopen HID handles, reapply saved keyboard/light bar state
    LocalDimmingControl.cs        Mini-LED local dimming toggle via WMI/ACPI GPIO
    LiquidCoolerControl.cs        BLE command layer: fan, pump, RGB, reset, commit, FW version
    LiquidCoolerState.cs          JSON persistence for saved device, auto-connect, last settings

  Monitoring/
    EcMonitor.cs                  WinForms Timer (1s), shared EC sampler (telemetry, failsafe, linked fans),
                                  fires Updated event
    PerfCounterPublisher.cs       Publishes CPU/GPU fan RPM as Windows perf counters from Updated (no extra EC reads)

  UI/
    MainForm.cs                   TabControl + monitor panel + profile bar + system tray + EC enrichment orchestration
    PowerTab.cs                   CPU + GPU TGP/DB/current sliders and opt-in NVIDIA BIOS override
    FailsafeTab.cs                Thermal Failsafe configuration, peak/history/status, test and one-step reset
    FanTab.cs                     Boost/curve checkboxes + single Apply button + FanCurvePanel + fan switch speed + linked mode
    FanCurvePanel.cs              GDI+ chart, draggable points (point 0 locked to 0% duty), mouse interaction, ReadOnly mode
    FanCurveDebugSection.cs       Section with read-only CPU (+ GPU if present) FanCurvePanels; live view
                                  of curves written to EC. Owns FanControl.CurvesApplied subscription.
                                  Mounted when curve editing is supported; hidden unless Link Fans is on.
    LinkedFanStatusBar.cs         Custom-painted status bar for linked fan mode (fill bar + mixed-font text)
    BatteryTab.cs                 3 radio buttons + battery telemetry (live, 2s poll)
    LightingTab.cs                3 LightingZonePanels (keyboard, light bar, logo) + idle timer + mini-LED dimming
    LightingZonePanel.cs          Per-zone control: enable + brightness + RGB + Apply/Pick Color/Save.
                                  Extends Section. Rows added directly to Section (no inner wrapper TLP).
                                  Button row is a plain Panel with fixed Height + Width and manually
                                  positioned buttons — see "UI layout pattern" below.
    ProfileBar.cs                 5 profile slots + Save + Restore/Gaming/Office/Battery Saver Defaults, docked at top
                                  Exposes ActiveSlot, ProfileApplied, ProfileListChanged for tray menu sync
    MonitorPanel.cs               Read-only status: fan speeds, CPU temp limit, battery, boost/curve status
    SystemInfoTab.cs              System identity, live sensor/power telemetry
    LiquidCoolerTab.cs            BLE cooler: scan/connect (15s timeout), fan/pump/RGB, auto-reconnect with BT retry
    Section.cs                    Custom labeled panel (title intersecting thin border); replaces GroupBox
    UiHelpers.cs                  Shared fonts + builders: BuildSection/SliderTable/KeyValueTable/RgbFlow/
                                  ApplyButton/ApplyReadRow, TryUi/TryUiAsync error wrappers
    UiMetrics.cs                  Design tokens: MarginLeft, SectionGap, RowGap, GroupPad*, Button* widths
    UiColors.cs                   Semantic colors: StatusOk/Warn/Error/Neutral, Hint, Disabled

  Profiles/MECHREVO/
    IDY_7Y.json                   Hardware-tested on this machine ("verified": false; no defaultFanCurve)
    IDY_7Y_defaults.json          RE reference only — ec_snapshot diff (OEM vs firmware post-power-cycle).
                                  Not a valid runtime profile (no projectId). Never auto-detected, but
                                  still listed by the unfiltered manual picker (fails validation if chosen).
    PH*.json (most)               OEM-derived modeDefaults (gaming/office PL1/PL2 + fan curves, CTGP on
                                  some) and defaultFanCurve
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
(12pt bold), `FontTiny` (7pt, graph labels). `Section` paints its title
with its own 10pt bold font. Layout is driven by `AutoSize` + `Dock` +
margins, with shared metrics for common sizes; some tabs still use a few
local pixel constants (indents, margins, responsive widths).

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
  ReadSmapcTable(int index) -> uint     // ECSpec config via IOCTL_SMAPCTABLE (no reconnect)
  ReadByte/WriteByte: on failure TryReconnect(cause), retry once
```

### HidDevice.cs -- ITE USB HID enumeration + feature reports

```
HidDevice : IDisposable
  _zoneHandles[4]  -- SafeFileHandle per zone (0=keyboard, 1=normally unused, 2=light bar, 3=logo)

  Open():
    Enumerates HID devices via SetupAPI, filters by VID=0x048D (ITE Tech),
    probes capabilities (FeatureReportByteLength == 9), and maps zones by
    HID usage value (0x0001 -> keyboard, 0x0002 -> light bar + logo).
    If usage mapping fails, falls back to sorted device-path order (which
    can fill zone 1). Stale handles are reopened after a send failure.

  SendFeatureReport(int zone, byte[9] report)
  IsZoneAvailable(int zone) -> bool
```

### BleDevice.cs -- WinRT BLE GATT (Nordic UART Service)

Third communication channel. Uses WinRT `Windows.Devices.Bluetooth` APIs. Full details in `Architecture_BLE.md`.

```
BleDevice : IDisposable
  ScanAsync(durationMs)            BluetoothLEAdvertisementWatcher, filters LCT21001/LCT22002
  ConnectAsync(address, ct)        Resolve NUS service + TX char (up to 3 attempts, 1.5s apart);
                                   RX notifications (CCCD) are optional.
                                   CancellationToken checked between GATT operations so callers
                                   can enforce a hard timeout via Task.WhenAny.
  WriteAsync(byte[])               Write to TX characteristic
  WriteAndReceiveAsync(byte[])     Write + await RX notification (FW version read)
  ConnectionLost : event           Fires on unexpected disconnect
```

---

## 3. Write Safety (WriteGuard.cs)

All EC writes go through WriteGuard. Only WriteGuard calls EcDevice.WriteByte directly.

1. **Allowlist** -- `HashSet<int>` of permitted addresses from the prepared profile. For unverified profiles, `UniversalDefaults` replaces the JSON allowlist with the universal init + feature list. Write to unlisted address throws.
2. **Read-modify-write** -- `WriteBitfield()` for bitfield registers (`0x7C5` bit 7, `0x7A6`, `0x743`): reads current value, applies mask, writes merged result. EC init does explicit read/merge/`Write` sequences for `0x726`, `0x727` and `0x7C5`; `LightingControl` does the same for `0x7C5` bit 5. `0x7AB` (logo) is written as a whole byte by `LightingControl` (one-hot brightness encoding); only Restore Defaults uses `WriteBitfield` on its bit 7.
3. **Write-then-verify** -- Every write is followed by readback.
   - Same-address verify: mismatch triggers one rewrite, then throws.
   - `verifyRedirect` (status registers that update asynchronously): up to four delayed readbacks of the read address (cumulative ~450/650/1000/1500 ms), no rewrite, then throws.
   - `skipVerify` (e.g., HID trigger registers): no readback.
   - A verify failure can invoke the recovery hook (EC activation watchdog), which re-runs EC init and retries the write-and-verify.

CPU/GPU feature controllers reject requests outside effective model limits or
supported encodings before any group writes; profile preflight also runs before
unrelated profile writes. Encoding caps remain defense in depth, not permission to
silently apply different settings. Doubled PL4 rounds down to an even wattage only
within model bounds. Firmware-default zero handoffs remain deliberately separate.

Special verification rules and the full allowlist are documented in `register_reference.md` section 8.

---

## 4. Feature Modules

Feature modules generally pair a read method (current hardware state) with an apply/set method (e.g. `CpuPowerControl.ReadCpuPower()` / `ApplyCpuPower(...)`). Direct EC writes go through WriteGuard; lighting also uses HID, local dimming uses WMI, and the liquid cooler uses BLE.

| Module | Controls | Key Registers |
|--------|----------|---------------|
| `CpuPowerControl` | PL1, PL2, PL4, CPU temp limit. Temp write sets bit 7 as the commit/enable flag on both encodings — `EncodeCpuTempLimit` (Intel inverted_offset) and `EncodeCpuTempLimitDirect` (AMD direct). Without bit 7 the EC stores the value but does not enforce the limit. WriteGuard verifies by masking the low 7 bits on both sides of the `0x786`→`0x463` redirect; bit 7 at the read address is firmware bookkeeping and may be set or clear depending on EC state. | `0x783`-`0x786` |
| `GpuPowerControl` | TGP, Dynamic Boost toggle/value | `0x743`-`0x746` |
| `FanControl` | Fan boost, custom fan curve mode, fan table write. `ApplyCurves(cpu, gpu, enabled)` is the primary entry point: sets/clears `0x7C5` bit 7 (master enable), writes `0x7C6 = 0x00` (deactivate latch), writes both tables, then `0x7C6 = 0x04` (the 0x00→0x04 transition forces EC re-evaluation), then fires `CurvesApplied` event for debug/UI consumers. `SetCustomFanCurveMode(bool)` writes only `0x7C5` bit 7 — standalone toggle for linked mode. `WriteCurve` writes `3 × entriesPerArray − 1` bytes per fan (47 for the usual 16 entries) in array-by-array order (all tempUp, all tempDown, all duty): skips `tempDown[0]`, forces `duty[0] = 0x00` (EC rejects non-zero), enforces 4°C hysteresis via `EnforceRange()`. Active duty entries after index 0 have a raw minimum of 2 (1%). Padding: `tempUp/tempDown = profile paddingTemp` (normally `0xFF`; first padding tempDown = lastActive.TempDown + 4), `duty = last active encoded duty`. GPU curves gated by `HasGpuFan`, all curves gated by format == RamFan1. `CurvesApplied` passes raw references (subscribers must Clone if persisting) and is gated at the subscriber via `Visible` cascade so it's a no-op when the form is hidden to tray. | `0x751`, `0x7C5`, `0x7C6`, `0xF00`-`0xF5F` |
| `FanSwitchSpeedControl` | Fan ramp speed | `0x787` |
| `LinkedFanController` | Linked fan mode: consumes shared 1s `EcMonitor` temperature samples, evaluates user curves, writes safe curves with per-sensor safety ramp | `cpuTemp`, `gpuTemp`, fan table |
| `BatteryControl` | Battery health mode (3 modes) + telemetry. Reads `0x7A6` at construction to detect variant: Intel bit 3 set (`0x08`/`0x18`/`0x28`) vs AMD bit 3 clear (`0x00`/`0x10`/`0x20`). Cached `bit3Flag` used for all encodes; profile `BatteryModes` updated in-place for RestoreDefaults. | `0x7A6`, `0x402`-`0x4AB` |
| `LightingControl` | Keyboard, light bar, logo LED | HID zones 0/2/3 + profile-resolved logo registers (typically `0x767`, `0x769`-`0x76B`, `0x78C`, `0x7AB`) + `0x7C5` bit 5 |
| `LightingIdleTimer` | Auto-off KB + light bar after idle | `GetLastInputInfo` polled 1s |
| `LocalDimmingControl` | Mini-LED local dimming toggle | WMI/ACPI + `0x72B` |
| `LiquidCoolerControl` | BLE cooler: fan, pump voltage, RGB, commit. Gated by EC `0x7A4` bit 7. See `Architecture_BLE.md`. | BLE (NUS) |

### GPU Temperature Failsafe

Behavior, setup, resource costs and validation are in
`notes/gpu_temperature_failsafe.md`; UI evidence is in
`notes/thermal_failsafe_validation.md`.

- `GpuFailsafeSettings` stores per-profile detection, threshold, optional CPU/fan
  actions and foreground preference. Missing CPU/fan fields default off.
- `ThermalProtectionState` stores `None` or `Tripped` (value 2; retired Stage 1
  loads as released), the held actions and one historical trip record.
  `UserProfileManager` atomically flushes/replaces this state in settings separately
  from user profiles. Interrupted records: a trip wins, held actions are unioned.
  Operational parsing is independent of history/unrelated settings. Unknown
  operational state retains known obligations plus the supported GPU fallback as a
  clearly identified configuration-recovery trip (cleared by Reset like any other).
  Legacy 30-49 C profile triggers migrate to 50 C without enabling detection.
- `GpuTemperatureFailsafe` accepts injected sensor/action/persistence dependencies.
  GPU, fan boost, then CPU actions execute independently through existing feature
  controllers and `WriteGuard`, before flushing the resulting trip state/errors.
  A crash before persistence can lose that new record; disk and hardware are not
  atomic. CPU writes preserve a once-per-operation freshly read, valid temperature
  limit; generic fallback CPU/TGP minima are not accepted as hardware authority.
- Trips use the rolling average of the last `AverageWindow` (5) valid samples,
  needing at least `MinimumSamples` (3), so single noisy readings cannot trip.
  Samples come from the 1s monitor plus fresh reads taken by `CheckBeforeApply()`
  and `Reset()`. Five consecutive invalid/unavailable recurring samples also trip,
  as does a pre-apply check with no usable sample.
- A trip survives restart and is enforced until `Reset()` or a deliberate profile
  load (`ApplyProfile(..., deliberate: true)`), either of which releases all held
  actions at once. With detection enabled, Reset refuses when a fresh reading is
  unavailable or the average (≥3 samples) is at/above the current trigger. After
  Reset, MainForm reapplies the active profile if an active saved slot exists.
  `EnsureCanSaveProfile()` blocks saving while tripped, so emergency values never
  become a normal profile.
  Startup/EC-recovery loads are not deliberate and keep the trip. While tripped,
  `ApplyExplicit` refuses deliberate writes; `Holds()` makes controllers substitute
  emergency values; fan boost on is never gated. EC reactivation invalidates
  verification immediately via an atomic epoch.
- A deliberate profile load applies the destination's failsafe settings directly.
- `EcMonitor` remains the sole recurring failsafe sampler. One GPU-temperature read
  is shared with linked fans and visible telemetry, or added to tray RPM reads.
  Disabled/unheld protection has no safety subscriber. Holds are maintained every
  five samples: previously verified CPU/GPU/fan requests in the same EC epoch use
  fresh complete-group readback, rewriting the affected group on drift/read failure.
  CPU temperature-limit validation remains mandatory. Maintenance may use those checks;
  initial/restart/recovery and forced rollback still write/verify. Skip/redirect
  exceptions for direct request registers retain the rewrite path. UI/notification
  subscriber faults cannot stop safety work.
- The Thermal Failsafe tab uses normal section/slider/key-value helpers, separate
  pending/applied/saved indicators, persistent configuration warnings and in-memory
  peak. Threshold range/default is 50-95/50 C; Reset needs a fresh reading and the
  average below the trigger.
  `MessageBeep`, tray warning
  and an independent nonmodal alert remain; the foreground option only activates
  the main form. No additional timer, GPU driver or BLE dependency is introduced.
  `TemperatureChanged` updates live readings separately from configuration/history
  `Changed` notifications; unchanged maintenance results do not trigger full UI
  refreshes or persistence. Hidden tabs and the bottom monitor panel skip label work.

The UI requires the failsafe controller even on unsupported machines; capabilities
represent unavailable hardware, not a second null-controller path. Feature controllers
remain optional and early watchdog construction guards remain. Configuration-warning
text changes notify MainForm's shared tab-title/tray presentation path. Successful
profile load/save clears resolved operation warnings, not the startup settings/recovery
warnings captured by the tab for the session (re-evaluated on next launch). Loader
diagnostics remain in the manager/log, current unsaved migrations remain visible,
and thermal holds take priority.

### Startup-only modules

| Module | Purpose |
|--------|---------|
| `HardwareDetection` | Reads 30 EC registers + 12 ECSpec config entries; provides `ExtractLimits()` and `ExtractModeDefaults()`. Reads `0x7A4` bit 7 for `SupportsLiquidCooling`. |
| `StartupValidator` | Validates bitfields + feature registers; gates which features are safe to enable. For unverified profiles, disables `gpuPower` if `ModuleIdGpu == 0` (no discrete GPU). |
| `UefiSetup` | One-time UEFI NVRAM setup: sets ApExistFlag, ApUseFlag, PowerMode, MemOC. Self-healing (checks on every launch, writes only if needed). Validates struct before writing. |
| `EcActivationControl` | EC init (7-step handshake): handshake, activation, feature enable, power management (`0x726`/`0x727`), `0x7C5 \|= 0x60` (bits 5+6 only, preserving bit 7), `0x7C7=0x0C`, `0x706=0x41`. Verifies `0x741` transformation post-init. **Does not** write the fan table, set `0x7C6`, or touch `0x7C5` bit 7 — those moved to the orchestrator. |
| `FanSnapshot` | One-pass capture of fan table + `0x7C5`/`0x7C6`. `IsValid()` checks CPU table only. `DetectGpuPresent()` returns tri-state. See Section 5 for detection logic. |

### Runtime recovery modules

| Module | Purpose |
|--------|---------|
| `EcActivationWatchdog` | Re-runs `EcActivationControl` init when a write fails verification (WriteGuard recovery hook), 3 s after resume, or when a 30 s spot check finds activation lost. Raises `Reactivated` so MainForm re-applies the user profile. Logs `EC activation lost` / `Re-applied user profile`. |
| `LightingResumeRestore` | 8 s after resume, reopens HID devices and reapplies saved keyboard/light bar state (enabled zones only; logo is EC-backed and skipped). |

`RegisterEncoding.cs` handles all encoding transforms (direct, double, offset, inverted_offset, fan_switch_speed, battery mode). Encoding type is specified per-register in the profile JSON, except PL4 (determined at runtime from `0x727` bit 7) and battery mode (variant detected at runtime by `BatteryControl`).

### Linked Fan Mode (LinkedFanController)

EC firmware evaluates CPU and GPU fan tables independently — no hardware "link fans" register exists. Linking is done in software:

1. **Sample** (shared 1s `EcMonitor`): Receive fresh CPU/GPU temperatures after GPU safety callbacks, before unrelated telemetry; evaluate each user curve via linear interpolation. No separate linked-fan timer or recurring sensor reads.
2. **Compute**: `linkedDuty = max(cpuDuty, gpuDuty)`
3. **Write**: Build a safe curve for each fan table at `linkedDuty`, write to both CPU and GPU fan table regions
4. **Skip**: Rewrite the EC when a rising sensor moves its safety-ramp start upward, or when duty changes by ≥2% and the old and new duties are not both in the ≤7% low zone. Otherwise skip. Failed curve writes invalidate the skip state so a return to old temperatures cannot suppress recovery.

The monitor reads both temperatures while linked mode is active, even tray-hidden
with thermal protection disabled, and does not auto-stop control sampling after
unrelated telemetry failures. Invalid/missing temperatures leave the existing EC
safety-ramp curves intact. Explicit enable takes fresh readings and synchronously
writes/verifies curves before subscribing; disable/disposal unsubscribe. Disposal
does not rewrite curves. Faster evaluation can increase writes during changing
temperatures; stable duty/headroom still produces no periodic curve writes.

**Safety ramp**: Each written curve is flat at `linkedDuty` up to the ramp start — the first fixed point strictly above the sensor temp + 4°C (`MinHeadroomCelsius`) — then ramps linearly (by point index) to 100% at the last point (97°C), which is always 100%. The ramp start is per-sensor — CPU curve ramps based on CPU temp, GPU curve based on GPU temp. If the app crashes, the EC keeps these curves and fans ramp up if temps rise.

Fixed temp points: 19, 29, 39, 49, 57, 65, 73, 81, 89, 97°C (10°C spacing below 49°C, 8°C above). Safe-curve TempDown uses `tempDown[i] = tempUp[i-1] − RangeCelsius` matching `FanCurveModel.EnforceRange`; point 0 duty is pinned to 0% (EC requires `duty[0] = 0x00`).

**Panels as source of truth**: The user's editable fan curves live in FanCurvePanel. LinkedFanController reads them via `Func<FanCurveModel?>` delegates each tick — never caches stale references. Profile save reads from panels, not EC (EC has flat/safe curves during linked mode). Profile load writes to panels; if the profile has linked mode on, it then calls `SetLinkedEnabled(true)`, which synchronously reads temperatures and writes the linked curves (even if linked mode was already on).

**Shutdown**: `LinkedFanController.Dispose()` does **not** write to the EC. The last-written safety curve stays in effect, letting the ramp-to-100 tail keep the fans honest even after the app is gone. Writing panel curves here used to cause a GPU fan cliff-drop while CPU was under load, which the EC interprets as inadequate cooling and latches CPU at 600 MHz. (Exit on `Disable()` — user toggling linked mode off in the running app — still writes panel curves, because the user is present to deal with any thermal consequences.)

**UI**: Compact painted status panel to the right of the Link Fans checkbox. Shows linked duty % as a filled bar, CPU/GPU temps and contributions, and safety ramp start temps. Only invalidates when visible (guards against hidden/minimized). A `FanCurveDebugSection` below the editable curves shows the actual CPU/GPU curves written to the EC (read-only, live); it is mounted always but `Visible = false` when Link Fans is off, and its `CurvesApplied` handler cascades the same `Visible` check so no Clone/Invalidate work happens when hidden.

**Profile persistence**: `linkedEnabled` is saved/loaded in user profiles. On auto-load, curves are loaded into panels first, then `SetLinkedEnabled(true)` starts the controller.

---

## 5. Hardware Detection & EC Enrichment

`HardwareDetection` reads the following from the EC at startup (all reads, no writes):

| Category | Registers | Purpose |
|----------|-----------|---------|
| Support bytes | `0x765`, `0x766`, `0x742`, `0x78E` | Feature capability flags |
| Liquid cooling | `0x7A4` bit 7 | `SupportsLiquidCooling` |
| Identity | `0x7D3`, `0x7D2`, `0x771`/`0x772`, `0x454`/`0x47D`, `0x456` | Module ID, ROM ID, EC version, System ID |
| CPU PL4 double flag | `0x727` | Bit 7 (`PL4D`) determines PL4 encoding: set = `0x785` stores watts/2 (raw × 2 = watts), clear = raw = watts directly. Firmware/DSDT-controlled, stable for the lifetime of the machine. |
| Gaming PL defaults | `0x730`-`0x733`, `0x7D8` | PL1/PL2/PL4/D/TCC for gaming mode |
| Office PL defaults | `0x734`-`0x737`, `0x7D9` | PL1/PL2/PL4/D/TCC for office mode |
| Battery saver defaults | `0x7A7`-`0x7AA` | PL1/PL2/PL4/D for battery saver mode |
| Turbo TCC | `0x7DA` | Turbo mode TCC offset |
| VRM | `0x753`, `0x754` | CPU VRM current limit / max |
| ECSpec (SMAPCTABLE) | IOCTL indices 0x00-0x0B | PL max, TGP base/max, DB max, TCC, GPU target temp |

### Profile enrichment (MainForm constructor)

EC-read data enriches the loaded profile. Hardware facts override the profile JSON; per-mode defaults from JSON take priority over EC values.

```
1. HardwareDetection reads all registers above
2. If unverified profile: ECSpec-derived limits replace the profile's limits
   (PL1/PL2/PL4 max, TGP range, DB max, CPU temp max) — including values
   populated in the JSON
3. If ECSpec reports absolute CPU temp encoding (AMD): the ECSpec cpuTempLimit
   and a direct 0x786→0x463 verify redirect are applied to ALL profiles,
   including verified ones
4. PL4 encoding stamped from 0x727 bit 7 (applies to all profiles — hardware
   fact, not a profile opinion). The same flag governs how ExtractLimits and
   ExtractModeDefaults decode PL4 max and per-mode PL4 defaults.
5. Only if the profile has no modeDefaults at all: EC per-mode PL registers
   populate them (gaming/office/batterySaver with PL1/PL2/PL4; TCC for
   gaming/office only — no GPU or fan curves). Invalid EC modes are omitted.
   A JSON modeDefaults dictionary is never merged with EC values.
```

CTGP/DynamicBoost are NOT populated from EC — the EC doesn't store per-mode GPU defaults in simple registers. Those only come from profile JSON (hand-tuned for specific models).

### Fan table detection & state orchestration (MainForm constructor, after EC init)

MainForm takes a single `FanSnapshot.Read` after init (one pass over the six profile-addressed fan arrays — normally `0xF00-0xF5F` — plus diagnostic `0xF60-0xF6F`, `0x7C5` and `0x7C6`) and uses the cached result for detection, diagnostics, panel seeding, and the custom-mode decision.

Detection overrides apply **only when `fanSnapshot.IsValid()`** (≥2 active CPU entries, strictly ascending tempUp, all ≤120°C). Invalid snapshots (all-zero on cold boot, all-0xFF after wipe) keep profile defaults.

When valid:
1. **activeEntries** — scans CPU tempUp, stops at padding (≥254), zero, or non-ascending. Overrides profile.
2. **GPU fan presence** — tri-state over GPU tempUp, ignoring `0x00`: `true` (any byte other than `0x00`/`0xFF`), `false` (only `0xFF` among non-zero bytes, OEM absent pattern), `null` (all `0x00`, ambiguous cold-boot state, keeps profile default). Gated by `IsValid()` to avoid misinterpreting garbage bytes.

**Panel seeding** — three-tier fallback:
1. **Snapshot** — `BuildCpuCurve()`/`BuildGpuCurve()`. Individual curves with 0 active entries fall back to generic default (`IsValid()` only checks CPU table).
2. **Profile default** — `profile.DefaultFanCurve` if snapshot invalid.
3. **Generic default** — `FanCurveModel.CreateGenericDefault()` (10-point, 30–90°C → 0–100%, point 0 at 0% duty).

**Custom-mode decision**:
- **User profile exists** → load and apply the slot through the normal profile path (curves via `FanTab.LoadCurves`/`ApplyCurves`, then linked mode restored), then re-read a snapshot for logging.
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
   - `dynamicBoostEn` (0x743 & 0x07): must be 0x00, 0x02, 0x05, or 0x07
   - `logoOnOff` (0x7AB & 0x80): must be 0x00 or 0x80

2. **Feature register validation** -- Read register, decode, check against sanity ranges:
   - PL1/PL2: 5-300W
   - PL4: 10-600W
   - CPU temp: 60-105C
   - TGP: 50-250W
   - Dynamic Boost: 0-50W
   - Logo RGB: raw max 60

   Missing registers are skipped. Feature-register read failures are non-definitive (don't disable the feature); mapped bitfield read failures do.

   **ECSpec-enriched bypass**: When ECSpec enriched any CPU limit (or either GPU limit), sanity and bitfield failures for that whole feature group are logged but don't disable it (e.g. PL1/PL2/PL4 = 0, the EC sentinel for "firmware controls power").

3. **GPU module presence** -- If `ModuleIdGpu` (0x7D2) == 0, disables `gpuPower`. On CPU-only machines, TGP/DB registers may contain random data that passes sanity checks; this catches them. GPU fan presence (`HasGpuFan`) is also logged as a secondary indicator.

Failed validation withholds the corresponding controller. Tabs are omitted when none of their features pass (CPU and GPU share the Power tab, which stays if either passes).

---

## 7. Monitoring (EcMonitor.cs)

WinForms Timer at 1000ms, always running (started tray-hidden at startup). It is the single shared EC sampler. Raises `Updated` event consumed by `MonitorPanel` (docked at bottom of MainForm), `FanTab`, `SystemInfoTab` and `PerfCounterPublisher`.

Each tick, before any telemetry:
1. If the failsafe or linked fans are subscribed, read GPU temp and dispatch `GpuTemperatureSample`.
2. If linked fans are subscribed, read CPU temp and dispatch `FanTemperatureSample`.

Subscriber exceptions are caught and logged, so a UI fault can't stop safety work.

Two telemetry modes:
- **Lightweight** (tray-hidden): CPU/GPU fan RPM (4 bytes).
- **Full** (window shown): additionally CPU/GPU fan %, CPU temp limit, battery SOC, battery mode, fan boost status, fan curve status, CPU/GPU temp, adapter current, system power, DB/TGP live mirrors (~18 EC bytes total).

`PerfCounterPublisher` passively republishes `\MechControl\CPU Fan RPM` and `\MechControl\GPU Fan RPM` from `Updated` (no extra EC reads), e.g. for an MSI Afterburner overlay.

After 5 consecutive telemetry read failures the monitor stops — but only when no failsafe or linked-fan subscriber is active. MainForm logs it, shows a tray balloon, and restarts the monitor after 60 s.

---

## 8. Profile Detection & JSON Schema

### Project ID Detection

```
1. Read EC register 0x740
2. Scan all profile JSONs, match projectIdValue against raw value
3. If no match, try raw >> 1 (newer models store ProjectID * 2)
4. WMI baseboard name (Win32_BaseBoard.Product) vs each profile's
   baseboardPatterns -- case-insensitive substring, longest pattern wins
5. WMI baseboard name vs profile filenames -- bounded token match, longest
   name wins, alphabetical tie-break
6. Prompt user to pick manually (ListBox dialog)
```

Observed on hardware: 0x740=26, 26>>1=13=IDY_7Y (`projectIdValue: 13`). Real baseboard names (e.g. `YAOSHI Series-X6AR55xY`) rarely contain profile names, so `baseboardPatterns` is the reliable WMI mapping. Known collision: X6FR5xxY (AMD) also matches IDY_7Y via shifted ID 13 (see `notes/other_machines.md`).

### Profile JSON Structure

```jsonc
{
  "projectId": "IDY_7Y",
  "projectIdValue": 13,
  "verified": false,
  "driver": { "devicePath": "\\\\.\\ACPIDriver" },
  "baseboardPatterns": [ /* WMI baseboard substrings for detection */ ],
  "registers": { /* canonical name -> hex address mapping */ },
  "fanTable": { /* format, base addresses, entriesPerArray, paddingTemp */ },
  "limits": { /* per-register min, max, encoding, offset, tjMax */ },
  "bitfields": { /* register -> mask mapping for RMW registers */ },
  "batteryModes": [ /* { name, value } for each mode */ ],
  "modeDefaults": { /* per-mode power presets: gaming, office, batterySaver */ },
  "defaultFanCurve": { /* optional factory fan curve snapshot */ },
  "defaultLogo": { /* optional factory logo state */ },
  "skipVerify": ["0x767", "0x78C"],
  "verifyRedirect": { /* write addr -> read addr for async registers */ },
  "writeAllowlist": ["0x783", "0x784", ..., "0xF00-0xF5F"]
}
```

### modeDefaults (per-mode power presets)

```jsonc
"modeDefaults": {
  "gaming": {
    "PL1": 45, "PL2": 55,                    // watts (required)
    "PL4": 90,                               // optional
    "CTGP": 10, "dynamicBoost": 25,          // optional GPU (profile-only, not from EC)
    "tccOffset": 100,                        // optional CPU temp limit in degrees C
    "fanCurve": { "cpu": { ... }, "gpu": { ... } }  // optional
  },
  "office": { ... }
}
```

Source priority: Profile JSON > EC registers > absent, decided per dictionary (EC fills in only when the JSON has no `modeDefaults` at all). The 23 runtime profiles with JSON `modeDefaults` (mostly PH-series) define gaming/office only, with PL1/PL2 + fan curves; 9 add `CTGP` on gaming. None set `PL4` or `tccOffset`. For all other models, `modeDefaults` is auto-populated from EC registers at startup with CPU power limits only (gaming/office/batterySaver).

### UniversalDefaults

`UniversalDefaults.ApplyToProfile()` runs after JSON load, before EC enrichment. Applies platform-wide constants derived from DLL disassembly:

- Filled only when the whole collection is empty in the JSON (individual missing entries are not merged):
  - Bitfield definitions (fanCurveEnable, batteryMode, dynamicBoostEn, logoOnOff)
  - Conservative power limits (PL1: 15-150W, PL2: 15-150W, etc.)
  - VerifyRedirect entries
  - Battery mode names and values
- **Replaced** for unverified profiles (JSON values discarded):
  - Write allowlist (init + feature registers + fan table range)
  - SkipVerify entries

### Register Lookup Flow

Model-dependent feature registers come from profile metadata: mostly the `registers` dictionary, plus `fanTable` (fan array bases) and `bitfields`. Code looks up a canonical string name to get the address (universal exceptions are listed after the table):

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

**Canonical register names** used in code (required by the feature that uses them; `cpuTemp`/`gpuTemp` are optional in EcMonitor):

| Name | Typical Address | Used By |
|------|----------------|---------|
| `cpuTemp` | `0x43E` | EcMonitor (sampled for LinkedFanController) |
| `gpuTemp` | `0x44F` | EcMonitor (sampled for failsafe + LinkedFanController) |
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

Universal hardcoded exceptions (same on all known Uniwill models):
- HardwareDetection discovery registers (0x730-0x737, 0x765, 0x7D3, etc.)
- Fan curve latch `0x7C6` (FanControl)
- Local dimming state `0x72B` (LocalDimmingControl)
- Battery telemetry `0x402-0x4AB` (below)
- EcMonitor system telemetry (adapter current, system power, DB/TGP mirrors)
- EC init registers (EcActivationControl)

### Battery Telemetry

`BatteryControl.ReadBatteryTelemetry()` reads ACPI battery registers (0x402-0x4AB) — hardcoded, same on all Uniwill models. Encoding details (endianness, units, temperature format) in `register_reference.md` section 11.1. `BatteryTab` polls every 2s while active.

---

## 9. Lighting State Persistence

Keyboard and light bar are HID-only -- no EC registers to read state from on startup. State is persisted to `lighting_state.json` next to the exe. On startup the Lighting tab's UI state is seeded (hardware is written separately, e.g. by profile apply or resume restore):
- Logo: reads current state from EC registers
- Keyboard, light bar: from saved JSON
- Idle timer timeout (seconds): from saved JSON, also saved per user profile

After resume, `LightingResumeRestore` reapplies the saved keyboard/light bar state to hardware.

---

## 10. User Profiles

5 saveable profile slots stored next to the exe (`AppContext.BaseDirectory`) as `userprofile_1.json` through `userprofile_5.json`, plus `settings.json` (last-used slot, profile names).

Profiles store decoded human-readable values (watts, degrees C, percent). Fan curves stored as raw EC bytes, padded to the profile's `entriesPerArray` (currently 16). Encoding/decoding happens at save/load time. Fan section also stores `linkedEnabled`. Lighting settings include idle timer timeout and local dimming state. Profiles also store `gpuFailsafe` settings and optional `liquidCooler` settings.

Requested missing/corrupt slots fail explicitly, including disappearance after an
exists check. Startup warnings distinguish missing arming from a thermal event and
retain known runtime/durable protection. Explicit ProjectId mismatches are refused;
legacy missing IDs receive a warning and current-model validation before writes.
Preflight orchestration lives in `ProfileBar.LoadAndApplySlot`, not the user-profile
DTO; effective power-limit validation remains in the feature controllers.
Profiles/settings use one same-directory atomic writer with source-generated JSON.
Unfinished profile temporary files never authorize an apply. A committed profile
followed by a failed settings save is reported as partial, without false selection/
saved bookkeeping or a pretend multi-file transaction. Migrated profiles remain
marked unsaved until an explicit successful save.

App-level `settings.json` also holds the durable `thermalProtection` state plus legacy
`gpuFailsafeTripReason`/`gpuFailsafeTripThresholdC` fields (migration only).

### Startup Flow

```
Program.cs:
 1. UEFI setup, OEM conflict check, EC device open, profile load,
    WriteGuard creation

MainForm constructor:
 2. Hardware detection + EC enrichment (Section 5)
 3. Load settings (UserProfileManager)
 4. Create EcMonitor + perf counters and start in lightweight mode (the timer
    only ticks once the message loop runs, i.e. after this constructor)
 5. EC init handshake (Section 4 startup-only table)
 6. Create and start the EC activation watchdog (WriteGuard recovery hook)
 7. Fan snapshot + fan table detection (Section 5)
 8. Startup validation (Section 6)
 9. Create feature controllers with their UI tabs (gated by validation):
    Power/Fan/Battery only for enabled features (GPU curve panel hidden if
    no GPU fan); Thermal Failsafe, Lighting and System Info always; Liquid
    Cooler if supported
10. Create ProfileBar (Restore/Gaming/Office/Battery Saver Defaults + 5 profile slots)
11. Seed fan panels + custom-mode decision (see Section 5 panel seeding)
12. Auto-load last user profile if saved, else leave EC alone or park custom mode off
13. Create tray icon/menu; start hidden (no window, tray icon only)
```

### Apply Order (Profile Load)

`ProfileBar.LoadAndApplySlot` first preflights the slot (project ID, null groups, CPU/GPU power validation), then runs the apply inside `GpuTemperatureFailsafe.ApplyProfile(...)`, which applies the destination failsafe settings and gates/holds as described in Section 4:

`FanTab.LoadCurves` (panels updated; if linked mode is off, `ApplyCurves(cpu, gpu, curveEnabled)` — `0x7C5` bit 7, `0x7C6=0x00`, both tables, `0x7C6=0x04`; if linked mode is already on, only `SetCustomFanCurveMode(curveEnabled)`) -> `SetLinkedEnabled(saved)` -> CPU power -> GPU power -> fan boost -> fan switch speed -> battery -> lighting (incl. local dimming) -> liquid cooler (if tab exists and profile has settings; BLE writes fire-and-forget).

### System Tray Behavior

Minimize or close hides to tray. On hide: EcMonitor switches to lightweight mode (fan RPM, plus failsafe/linked-fan temperature samples — see Section 7) and battery telemetry stops. On show: full mode resumes, battery telemetry restarts if applicable, and the monitor is restarted if it auto-stopped. Linked fans and the failsafe keep running off the shared sampler regardless of UI visibility. Status panel `Invalidate()` is guarded by `Visible` check (skips when minimized or on another tab).

**Tray context menu**: Show, 5 profile slots (checked = active, grayed = empty), Exit. Profile items call `ProfileBar.LoadAndApplySlot()` — same code path as the main UI. Menu rebuilds on profile load, save, or rename.

**Per-profile tray icons**: Profiles 1–4 show generated colored numbered circles (placeholder). Profile 5 plays a 40-frame kekwait animation (embedded PNGs, `System.Threading.Timer` at 67ms/15fps) whenever the tray icon is refreshed with slot 5 active (selection, startup, save/rename, warning-state changes), then holds the last frame. No profile active = default exe icon. A thermal hold (error icon, "THERMAL PROTECTION HOLD") or failsafe configuration warning (warning icon) overrides the profile icon.

### Shutdown / Disposal Order

On real exit, `FormClosing` hides the tray icon, closes the failsafe alert, stops battery telemetry, disposes the EC activation watchdog, lighting resume restore, monitor and perf counters, then closes the log.

`MainForm.Dispose` then:
1. Disposes the failsafe alert.
2. Runs `LiquidCoolerTab.ShutdownAsync()` (bounded to 3 s) while its reconnect timer is still live.
3. Disposes the tray animation timer, tray icon/menu and profile icons.
4. Calls `base.Dispose()`, which disposes child controls including FanTab → LinkedFanController.
5. Disposes the BLE device and HID device, and finally the EC device.

LinkedFanController.Dispose intentionally does not touch the EC (see Linked Fan Mode shutdown note above), so EC-last is defensive — child controls may still write during disposal.

### Mode Defaults (Gaming/Office/BatterySaver buttons)

Two-tier data source (see Section 8 `modeDefaults`):
1. **Profile JSON** (mostly PH-series models): applies PL1/PL2 + fan curves; CTGP/DB only when `CTGP` is set (DB alone is ignored). Currently gaming/office only, so Battery Saver reports no defaults on these models.
2. **EC register fallback** (profiles without `modeDefaults`): applies PL1/PL2 (+ PL4, + TCC for gaming/office) only — no GPU, no fan curves.

Missing PL4/TCC values keep the current hardware values. The confirmation dialog summarizes the categories being changed (power limits, GPU TGP, fan curves).

### Save Flow

Fan curves are read from FanTab panels (source of truth), not from EC — EC may have flat/safe curves during linked mode. All other state reads from feature controllers + LightingState, plus the Thermal Failsafe tab settings and liquid cooler settings. `linkedEnabled` is saved so linked mode restores on profile load. Saving is refused while a thermal hold is active.

---

## 11. Diagnostic Logging (DiagnosticLog.cs)

Every session appends to `mechcontrol.log` next to the exe. Logs hardware identity (ModuleID, ROMID, EC version, VRM), ECSpec config, per-mode PL defaults, effective limits and their source, feature register values (raw + decoded), fan table hex dumps (snapshot + post-write verify), fan detection results (activeEntries, GPU presence), startup validation pass/fail per feature, HID enumeration, and EC init steps. UI errors caught by `TryUi`/`TryUiAsync` are logged before the popup is shown, so most failed profile applies leave a trail (exception: tray-menu profile loads show a popup without logging the exception). Designed so a remote user's log contains everything needed to build a verified profile.

Retention: `Start()` trims the log on each launch before opening the writer — keeps the very first session (baseline snapshot of the machine's original state) plus the 5 most recent previous sessions, drops everything between, then appends the current session (so up to 7 sessions after launch). Trimming is opportunistic; any IO failure silently skips rotation for that launch.

---
