# Fan Curve Read/Write Refactor Plan

## Goal

Consolidate the fan curve startup flow from **5 reads + 3 writes** down to **1 read + 0-1 writes (+ 1 verify read after write)**.

Combine `0x7C5` bit 7 and `0x7C6` into a single "custom fan curve mode" semantic so the UI checkbox reflects the real EC state instead of just one of two registers.

Keep multi-model compatibility — no IDY_7Y-specific code, no baked-in addresses.

## Current state (what we're replacing)

### Current reads on startup (5)

| # | Location | What | Bytes |
|---|---|---|---|
| 1 | `MainForm.cs:150` → `DiagnosticLog.LogFanTable(device)` | `0xF00-0xF5F` + `0xF60-0xF6F` | 112 |
| 2 | `EcActivationControl.cs:136` → `ReadAndSaveBiosFanTable` (unverified only) | `0xF00-0xF5F` | 96 |
| 3 | `EcActivationControl.cs:159-163` → `0x7C5 |= 0xE0` RMW | `0x7C5` | 1 |
| 4 | `MainForm.cs:173-213` → active-entries / padding / GPU-present detection | CPU tempUp + 1 + GPU tempUp | ~33 |
| 5 | `MainForm.cs:309` → `_fanTab?.ReadCurvesFromEc()` | CPU + GPU active curves | ~60-80 |

Plus `StartupValidator.ValidateBitfields()` re-reads `0x7C5` for the bitfield sanity check.

### Current writes on startup (3 when user profile exists)

| # | Location | What |
|---|---|---|
| 1 | `EcActivationControl.InitializeEc` step "Default fan table" | 96 B to `0xF00-0xF5F` (verified: JSON default; unverified: the just-read BIOS values — no-op round trip) |
| 2 | `EcActivationControl.InitializeEc` step "Fan curve mode 0x7C6=0x04" | 1 B, unconditionally force custom mode on |
| 3 | `MainForm.cs:317` → `_profileBar.LoadAndApplySlot` → `ApplyProfile` → `_fanTab.LoadCurves` → `FanControl.ApplyCpuCurve/ApplyGpuCurve` | 96 B, overwrites #1 |

Plus `0x7C5 |= 0xE0` in init unconditionally force-sets bit 7 (fan curve enable) regardless of user intent.

## Decisions made (from conversation)

### D1. Drop `ReadAndSaveBiosFanTable` entirely
Unverified profiles no longer get their `defaultFanCurve` populated from the BIOS read at startup. `profile.DefaultFanCurve` is strictly whatever the JSON says — may be `null` for unverified machines. `RestoreDefaults_Click` already no-ops when `DefaultFanCurve == null`, so it just silently skips the fan-table portion for machines without a JSON default. Later we'll add curves to the other profile JSONs so Restore Defaults works for them too.

### D2. No startup auto-write of profile `defaultFanCurve`
Profile JSON `defaultFanCurve` is **only** applied when the user clicks *Restore Firmware Defaults*. Startup never auto-writes profile defaults, even for verified profiles (IDY_7Y). This is a user-visible behavior change: on a cold boot where the EC happens to have garbage in `0xF00-0xF5F`, the new behavior is "custom mode disabled, fans run at EC internal default (~26%)" — user confirmed this is acceptable.

### D3. Startup writes are conditional on user profile only
```
If user profile slot exists with saved curves:
  → apply user curves (1 fan-table write + verify-read + SetCustomFanCurveMode per profile.curveEnabled)
Else:
  → no fan-table write. Panels seeded from snapshot.
  → If snapshot is invalid (garbage EC table): disable custom mode (write bit 7 = 0, 0x7C6 = 0)
  → If snapshot is valid: leave 0x7C5 bit 7 and 0x7C6 at their current EC values, checkbox reflects real state
```

### D4. Combine `0x7C5` bit 7 and `0x7C6` semantics
Single concept "custom fan curve mode":
- **ON**: `0x7C5` bit 7 = 1 **AND** `0x7C6` = `0x04`
- **OFF**: `0x7C5` bit 7 = 0 **AND** `0x7C6` = `0x00`

One setter flips both. One getter checks both.

### D5. Init changes (Option A)
`EcActivationControl.InitializeEc` becomes:
- Handshake (`0x709`, `0x708`) — unchanged
- Activation (`0x741`) — unchanged
- Feature enable (`0x748`) — unchanged
- Power management (`0x726`, `0x727`) — unchanged
- **`0x7C5 |= 0x60`** (bits 5 and 6 only, preserving bit 7) ← was `|= 0xE0`
- **[REMOVED]** default fan table write
- Fan curve param (`0x7C7 = 0x0C`) — unchanged
- **[REMOVED]** fan curve mode `0x7C6 = 0x04`
- BIOS_CTRL (`0x706 = 0x41`) — unchanged

Rationale: `0x726/0x727` are what prevent failsafe (per `ec_activation_issue.md`). `0x7C5` bits 5/6 are OEM-universal init bits (bit 5 = logo flag, bit 6 = unknown "service managed"), harmless to keep. Bit 7 and `0x7C6` move to the orchestrator. This is untested on cold boot — user accepts the risk and will test.

## Proposed new flow

```
Program.cs / MainForm ctor (unchanged parts skipped):

1. EcActivation.InitializeEc()  // now without fan-table write / 0x7C6 / bit 7 manipulation
2. snapshot = FanSnapshot.Read(device, profile.FanTable)  // THE single read
3. DiagnosticLog.LogFanTable(snapshot)  // no EC reads
4. profile.FanTable.ActiveEntries = snapshot.DetectCpuActive()
   profile.FanTable.PaddingDuty   = snapshot.DetectPaddingDuty()
   profile.FanTable.HasGpuFan     = snapshot.DetectGpuPresent()
5. StartupValidator.Validate()  // unchanged (reads 0x7C5 for its own bitfield check — 1 byte, leave it)
6. Create feature controllers (FanControl, CpuPowerControl, etc.)
7. _fanTab.SeedCurvesFromSnapshot(snapshot)
   _fanTab.SetCurveEnableCheckbox(snapshot.IsCustomCurveModeActive())

8. if (user profile slot exists && slot file exists) {
       _profileBar.LoadAndApplySlot(lastSlot)  // writes user curves + SetCustomFanCurveMode
       wroteFanTable = true
   } else if (!snapshot.IsValid()) {
       fanControl.SetCustomFanCurveMode(false)  // park custom mode off
       // panel still shows garbage with red validation text
   }
   // else: valid snapshot, no user profile → leave EC alone

9. if (wroteFanTable) {
       verify = FanSnapshot.Read(device, profile.FanTable)
       DiagnosticLog.LogFanTableVerify(verify, userCurves)
   }
```

## File-by-file changes

### New file: `MechControl/Features/FanSnapshot.cs`

```csharp
namespace MechControl.Features;

class FanSnapshot
{
    public byte[] CpuTempUp;   // 16
    public byte[] CpuTempDown; // 16
    public byte[] CpuDuty;     // 16
    public byte[] GpuTempUp;   // 16
    public byte[] GpuTempDown; // 16
    public byte[] GpuDuty;     // 16
    public byte[] RamFan2Probe;// 16 (0xF60-0xF6F, diagnostic only)
    public byte CurveEnableReg;// 0x7C5
    public byte CurveModeReg;  // 0x7C6

    public static FanSnapshot Read(EcDevice device, FanTableInfo ft);

    public bool IsCustomCurveModeActive()
        => (CurveEnableReg & 0x80) != 0 && CurveModeReg == 0x04;

    public int DetectCpuActive();   // scans CpuTempUp for first >= 254, returns 1 if none
    public int DetectGpuActive();
    public bool DetectGpuPresent(); // GPU tempUp not all 0xFF
    public int DetectPaddingDuty(int cpuActive);

    public bool IsValid();  // CPU active >= 2, ascending tempUp in active region, all <= 120C

    public FanCurveModel BuildCpuCurve(int fallbackActive);
    public FanCurveModel BuildGpuCurve(int fallbackActive);
}
```

Consumes `FanTableInfo` for register addresses (multi-model-safe).

### Modified: `MechControl/Features/EcActivationControl.cs`

- **Delete** `ReadAndSaveBiosFanTable` (~60 lines, method + helpers `ReadArray` / `ValidateFanArray`)
- **Delete** `WriteDefaultFanTable` and `WriteFanArray` helpers
- **Delete** `IsFanCurveModeEnabled` / `EnableFanCurveMode` / `DisableFanCurveMode` — dead code that's now actively misleading
- **Modify** `InitializeEc`:
  - Remove the `if (!_profile.Verified) ReadAndSaveBiosFanTable()` call at the top
  - Change `"Fan curve enable 0x7C5 |= 0xE0"` step to `"Fan curve init 0x7C5 |= 0x60"`:
    ```csharp
    Step("Fan curve init 0x7C5 |= 0x60", () =>
    {
        byte current = _device.ReadByte(AddrFanCurveEnable);
        _guard.Write(AddrFanCurveEnable, (byte)(current | 0x60));
    });
    ```
  - **Remove** the `Step("Default fan table", WriteDefaultFanTable)` step
  - **Remove** the `Step("Fan curve mode 0x7C6=0x04", ...)` step
  - Keep everything else identical (handshake, activation, feature enable, power mgmt, 0x7C7, BIOS_CTRL, post-verify)

### Modified: `MechControl/Features/FanControl.cs`

- **Add** constants:
  ```csharp
  private const int AddrFanCurveMode = 0x7C6;  // fan curve mode register
  ```
- **Add** new methods:
  ```csharp
  public void SetCustomFanCurveMode(bool enabled)
  {
      // Write 0x7C5 bit 7 and 0x7C6 atomically (enabled → bit=1, mode=0x04)
      _guard.WriteBitfield(_addrFanCurveEnable, _fanCurveEnableMask,
          enabled ? _fanCurveEnableMask : (byte)0x00);
      _guard.Write(AddrFanCurveMode, enabled ? (byte)0x04 : (byte)0x00);
  }

  public bool ReadCustomFanCurveMode()
  {
      byte enable = _device.ReadByte(_addrFanCurveEnable);
      byte mode = _device.ReadByte(AddrFanCurveMode);
      return (enable & _fanCurveEnableMask) != 0 && mode == 0x04;
  }
  ```
- **Remove** old `SetFanCurveEnabled(bool)` and `ReadFanCurveEnabled()` (replaced by the two above)

Note: `0x7C6` needs to be in the profile's `writeAllowlist`. Already is in IDY_7Y (`"0x7C6"`). `UniversalDefaults.InitAllowlist` also has it. `UniversalDefaults.SkipVerify` has it (it's a mode flag — skip verify). Good, no profile changes needed.

### Modified: `MechControl/UI/FanTab.cs`

- **Add** method:
  ```csharp
  public void SeedCurvesFromSnapshot(FanSnapshot snapshot)
  {
      if (!_curveEditingEnabled) return;
      int active = snapshot.DetectCpuActive();
      _cpuCurvePanel.Curve = snapshot.BuildCpuCurve(active);
      if (_hasGpuFan)
          _gpuCurvePanel.Curve = snapshot.BuildGpuCurve(active);
      UpdateValidation();
  }

  public void SetCurveEnableCheckbox(bool checked_)
  {
      _chkFanCurve.Checked = checked_;
  }
  ```
- **Modify** `RefreshFromEc()` line 341:
  - `_chkFanCurve.Checked = _control.ReadFanCurveEnabled();`
  - → `_chkFanCurve.Checked = _control.ReadCustomFanCurveMode();`
- **Modify** `BtnApplyCurveEnable_Click` line 525:
  - `_control.SetFanCurveEnabled(_chkFanCurve.Checked);`
  - → `_control.SetCustomFanCurveMode(_chkFanCurve.Checked);`
- Keep `ReadCurvesFromEc()` (used by the manual "Read from EC" button)

### Modified: `MechControl/UI/ProfileBar.cs`

- Line 258, 271: `_fanControl.SetFanCurveEnabled(...)` → `_fanControl.SetCustomFanCurveMode(...)`
- Line 359: `_fanControl?.ReadFanCurveEnabled() ?? false` → `_fanControl?.ReadCustomFanCurveMode() ?? false`

### Modified: `MechControl/Features/LinkedFanController.cs`

- Line 52: `_fanControl.SetFanCurveEnabled(true);` → `_fanControl.SetCustomFanCurveMode(true);`

### Modified: `MechControl/UI/MainForm.cs`

Big restructure of the ctor area between lines 148 and 325:

1. **Remove** line 150: `DiagnosticLog.LogFanTable(device);` — this gets replaced with the snapshot-consuming version lower down
2. Keep EC init (line 153-161)
3. **Remove** the entire "Hardware capability detection" block lines 167-219 — replaced by snapshot-based detection
4. Insert new block after EC init:
   ```csharp
   // Single fan snapshot (one read, feeds detection + diagnostic + panels)
   var fanSnapshot = FanSnapshot.Read(device, profile.FanTable);
   DiagnosticLog.LogFanTable(fanSnapshot);

   // Hardware capability detection from snapshot
   {
       var ft = profile.FanTable;
       DiagnosticLog.Section("Fan Table Detection");

       int detectedActive = fanSnapshot.DetectCpuActive();
       if (detectedActive != ft.ActiveEntries) {
           DiagnosticLog.Write($"Active entries: detected {detectedActive} (profile default was {ft.ActiveEntries})");
           ft.ActiveEntries = detectedActive;
       } else {
           DiagnosticLog.Write($"Active entries: {detectedActive} (matches profile)");
       }

       if (detectedActive < ft.EntriesPerArray) {
           int detectedPaddingDuty = fanSnapshot.DetectPaddingDuty(detectedActive);
           if (detectedPaddingDuty != ft.PaddingDuty) {
               DiagnosticLog.Write($"Padding duty: detected {detectedPaddingDuty} (profile default was {ft.PaddingDuty})");
               ft.PaddingDuty = detectedPaddingDuty;
           } else {
               DiagnosticLog.Write($"Padding duty: {detectedPaddingDuty} (matches profile)");
           }
       }

       bool hasGpuFan = fanSnapshot.DetectGpuPresent();
       ft.HasGpuFan = hasGpuFan;
       DiagnosticLog.Write($"GPU fan table: {(hasGpuFan ? "present" : "not detected (all entries 0xFF)")}");

       if (ft.Format != "RamFan1")
           DiagnosticLog.Warn($"Fan table format '{ft.Format}' is not RamFan1 — fan curve editing will be disabled");
   }
   ```
5. Keep StartupValidator (line 222) and feature-controller creation (line 235-259) unchanged
6. **Replace** line 309 `_fanTab?.ReadCurvesFromEc();` with:
   ```csharp
   // Seed panels from the snapshot (no extra EC reads)
   _fanTab?.SeedCurvesFromSnapshot(fanSnapshot);
   _fanTab?.SetCurveEnableCheckbox(fanSnapshot.IsCustomCurveModeActive());
   ```
7. **Modify** the auto-load block (lines 311-324) to capture whether a write happened, and add the snapshot-invalid fallback:
   ```csharp
   bool wroteFanTable = false;
   int lastSlot = _profileManager.LastUsedSlot;
   if (lastSlot > 0 && _profileManager.SlotExists(lastSlot)) {
       try {
           _profileBar.LoadAndApplySlot(lastSlot);
           DiagnosticLog.Write($"Auto-loaded user profile slot {lastSlot}");
           wroteFanTable = true;
       } catch (Exception ex) {
           DiagnosticLog.Warn($"Auto-load slot {lastSlot} failed: {ex.Message}");
       }
   } else if (_fanControl != null && !fanSnapshot.IsValid()) {
       // No user profile and EC fan table is garbage — park custom mode off
       try {
           _fanControl.SetCustomFanCurveMode(false);
           DiagnosticLog.Warn("EC fan table is invalid — custom fan curve mode disabled");
       } catch (Exception ex) {
           DiagnosticLog.Warn($"Failed to disable custom fan curve mode: {ex.Message}");
       }
   }

   // Verify read after any write
   if (wroteFanTable) {
       var verify = FanSnapshot.Read(device, profile.FanTable);
       // TODO: compare verify against what we intended to write, log diffs
       DiagnosticLog.Write("Fan table verify-read complete");
   }
   ```
8. **Modify** `RestoreDefaults_Click` (around line 470):
   - `_guard.WriteBitfield(_profile.GetRegister("fanCurveEnable"), 0x80, 0x00);`
   - → `_fanControl?.SetCustomFanCurveMode(false);` (or, since `_fanControl` may be null on really broken profiles, fall back to manual writes to both registers)
9. **Modify** `ApplyModeDefaults` lines 564, 577:
   - `_fanControl.SetFanCurveEnabled(false);` → `_fanControl.SetCustomFanCurveMode(false);`
   - `_fanControl.SetFanCurveEnabled(true);` → `_fanControl.SetCustomFanCurveMode(true);`

### Modified: `MechControl/DiagnosticLog.cs`

- **Replace** `LogFanTable(EcDevice device)` with `LogFanTable(FanSnapshot snapshot)`:
  ```csharp
  public static void LogFanTable(FanSnapshot snapshot)
  {
      Section("Fan Table Dump");
      // RamFan1 range: emit CPU arrays then GPU arrays as hex rows
      WriteHexRow("CPU tempUp  ", snapshot.CpuTempUp);
      WriteHexRow("CPU tempDown", snapshot.CpuTempDown);
      WriteHexRow("CPU duty    ", snapshot.CpuDuty);
      WriteHexRow("GPU tempUp  ", snapshot.GpuTempUp);
      WriteHexRow("GPU tempDown", snapshot.GpuTempDown);
      WriteHexRow("GPU duty    ", snapshot.GpuDuty);
      WriteHexRow("RamFan2 probe", snapshot.RamFan2Probe);
      Write($"0x7C5 = 0x{snapshot.CurveEnableReg:X2}  " +
            $"0x7C6 = 0x{snapshot.CurveModeReg:X2}  " +
            $"customMode = {snapshot.IsCustomCurveModeActive()}");
  }

  private static void WriteHexRow(string label, byte[] bytes)
  {
      var parts = new string[bytes.Length];
      for (int i = 0; i < bytes.Length; i++) parts[i] = $"{bytes[i]:X2}";
      Write($"  {label}: {string.Join(" ", parts)}");
  }
  ```
- Old `WriteHexDump` helper and its `EcDevice` read loop are still used for other purposes? Check — if not, delete.

### Not modified (but verify during implementation)

- `StartupValidator.cs` still reads `0x7C5` for its bitfield validation (1 byte). Leave it — it's fine, it runs once and is separate from the snapshot.
- `EcMonitor.cs` / `PerfCounterPublisher.cs` — they read fan duty/RPM registers, unrelated to fan table.
- `UniversalDefaults.cs` — `0x7C6` is already in `InitAllowlist` and `SkipVerify`. No change.
- Profile JSONs — no schema change.

## Edge cases / expected behavior

| Scenario | Startup reads | Startup writes | User-visible state |
|---|---|---|---|
| Cold boot, no user profile, valid EC table | 1 snapshot | 0 | Panels show EC values; checkbox reflects real `0x7C5 bit 7 && 0x7C6 == 0x04` state |
| Cold boot, no user profile, garbage EC table | 1 snapshot | 1 × 2 bytes (disable custom mode) | Panels show garbage with red validation; checkbox off |
| Cold boot, user profile exists | 1 snapshot + 1 verify | 1 × 96 B + 2 × 1 B (curves + custom mode per profile) | User curves shown; checkbox per profile |
| Warm restart after OEM service ran, no user profile | 1 snapshot | 0 | Panels show OEM values; checkbox on |
| Click "Read from EC" button | 1 read (existing button path) | 0 | Panels updated |
| Click "Restore Firmware Defaults" (IDY_7Y) | 0 | 96 B + control | Panels show JSON default; custom mode on |
| Click "Restore Firmware Defaults" (unverified) | 0 | 0 fan-table writes (profile.DefaultFanCurve == null); PLs/lighting still reset | Fan table untouched — flag in dialog? |

## Risks and testing

**Untested path:** removing fan-table write + `0x7C6 = 0x04` from cold-boot init. The root-cause fix for failsafe in `notes/ec_activation_issue.md` is `0x726/0x727`, which are still in init. Fan-table init writes are, to our best understanding, only needed for the EC to have valid RAM data *when* `0x7C6 = 0x04` is set — since we no longer set `0x7C6 = 0x04` during init, the fan table can be in any state until the orchestrator decides.

**Test plan (cold boot each time):**

1. Cold boot, no user profile file present → panels show current EC values, checkbox reflects real state, custom mode off if table invalid
2. Cold boot with a valid `userprofile_2.json` as last-used → user curves applied, custom mode on per profile.curveEnabled, verify-read passes
3. Cold boot with user profile that has `curveEnabled=false` → curves applied, custom mode off (bit 7=0, 0x7C6=0)
4. Manually zero out `0xF00-0xF5F` with `ec_write`, cold boot (no user profile) → panels show zeros, validation red, custom mode off
5. Toggle the checkbox in the UI → both registers flip together, verify via `ec_snapshot`
6. Click *Restore Firmware Defaults* on IDY_7Y → JSON default applied, custom mode on
7. CPU power (PL1/PL2) still applies correctly (this is the real failsafe risk)
8. Fan boost (`0x751`) still works
9. Linked fan mode still works
10. Save & reload a user profile via the profile bar

**Rollback plan:** git stash. Previous init sequence has been shipping; the new one introduces a 1-step skip (fan table + 0x7C6) that can be reverted by restoring two `Step(...)` calls in `EcActivationControl.InitializeEc`.

## Implementation order

1. Create `FanSnapshot.cs` with `Read`, `IsValid`, `IsCustomCurveModeActive`, detection helpers, `BuildCpuCurve`/`BuildGpuCurve`
2. Update `FanControl.cs`: add `SetCustomFanCurveMode` / `ReadCustomFanCurveMode`, remove old `SetFanCurveEnabled` / `ReadFanCurveEnabled`, add `AddrFanCurveMode` const
3. Update all callers of the old methods (`ProfileBar`, `LinkedFanController`, `FanTab`, `MainForm`) — compile error pass
4. Update `EcActivationControl.cs`: delete BIOS-read helpers, delete dead 0x7C6 methods, edit `InitializeEc` to the trimmed sequence
5. Update `DiagnosticLog.LogFanTable` to consume `FanSnapshot`
6. Update `FanTab.cs` to add `SeedCurvesFromSnapshot` / `SetCurveEnableCheckbox`
7. Update `MainForm.cs` ctor: insert snapshot read, replace detection block, replace panel seeding, add conditional autoload / invalid-fallback, add verify-read
8. Build, fix compile errors
9. Run test plan above

## Multi-model safety checklist

- [ ] No hardcoded addresses outside `profile.FanTable` / existing constants
- [ ] `FanSnapshot` takes `FanTableInfo` and uses its dynamic addresses
- [ ] `0x7C5` and `0x7C6` are already documented as universal Uniwill registers (`register_reference.md`)
- [ ] `0x7C6` is in `UniversalDefaults.InitAllowlist` and `SkipVerify` → works for all unverified profiles
- [ ] `fanCurveEnable` bitfield is already populated by `UniversalDefaults.Bitfields` for empty profiles
- [ ] No profile JSON schema change required

---

## Appendix — facts to remember (saves re-reading after /clear)

### A1. Type choice for `FanSnapshot` arrays

Use **`List<int>`** (not `byte[]`, not `int[]`) for the six curve arrays. Reasons:
- `FanCurveModel.FromRawArrays(IReadOnlyList<int>, IReadOnlyList<int>, IReadOnlyList<int>)` already takes `IReadOnlyList<int>`. No conversion needed at call site.
- `FanCurveModel.DetectActiveEntriesFromList(IReadOnlyList<int>)` also takes `IReadOnlyList<int>`.
- Existing `FanCurveArrays` in `ProfileSchema.cs` uses `List<int>`. Consistent with codebase.
- 16 × 6 × 4 bytes = 384 bytes overhead per snapshot. Trivial.

The `RamFan2Probe` field (diagnostic only, not parsed) can be `byte[]` since it's just logged as hex.

### A2. Relevant existing method signatures (no re-reading needed)

```csharp
// Features/FanCurveModel.cs
public static int DetectActiveEntriesFromList(IReadOnlyList<int> tempUp);  // returns >= 1
public static FanCurveModel FromRawArrays(IReadOnlyList<int> tempUp,
                                          IReadOnlyList<int> tempDown,
                                          IReadOnlyList<int> duty);
public static int DetectActiveEntries(EcDevice, int tempUpBase, int entriesPerArray, int fallback);
  // ORPHANED after refactor — can delete. Only caller is MainForm detection block we're replacing.

public static FanCurveModel ReadFromEc(EcDevice, ...);
  // STILL USED by FanControl.ReadCpuCurve/ReadGpuCurve (manual "Read from EC" button). Keep.

// Driver/EcDevice.cs
public byte ReadByte(int addr);
// No bulk-read method — snapshot uses a loop of ReadByte calls. 114 byte reads total is fine.

// Driver/WriteGuard.cs
public void Write(int addr, byte value);                       // throws if not in allowlist
public void WriteBitfield(int addr, byte mask, byte value);    // RMW, throws if not in allowlist
```

### A3. Dead code to delete as part of this refactor

| Method | File | Why dead |
|---|---|---|
| `IsFanCurveModeEnabled` | `EcActivationControl.cs:85` | No callers (verified by grep) |
| `EnableFanCurveMode` | `EcActivationControl.cs:92` | No callers |
| `DisableFanCurveMode` | `EcActivationControl.cs:98` | No callers |
| `ReadAndSaveBiosFanTable` | `EcActivationControl.cs:195` | Removed by D1 |
| `WriteDefaultFanTable` | `EcActivationControl.cs:304` | Removed by D2 |
| `WriteFanArray` (private helper) | `EcActivationControl.cs:326` | Used only by `WriteDefaultFanTable` |
| `ReadArray` (private helper) | `EcActivationControl.cs:260` | Used only by `ReadAndSaveBiosFanTable` |
| `ValidateFanArray` (private helper) | `EcActivationControl.cs:268` | Used only by `ReadAndSaveBiosFanTable` |
| `FanCurveModel.DetectActiveEntries(EcDevice, ...)` | `FanCurveModel.cs:29` | Only caller (`MainForm.cs:173`) is being replaced by snapshot-based detection |
| `DiagnosticLog.LogFanCurveArrays` | `DiagnosticLog.cs:286` | Only callers are in `ReadAndSaveBiosFanTable`; can leave or delete |

### A4. Init timing note

`EcActivationControl.InitializeEc` does `Thread.Sleep(50)` between every `Step()`. Removing the two fan-related steps (default fan table + `0x7C6=0x04`) saves ~100ms of sleep. Not a problem.

### A5. Profile.ApplyProfile disable-write-enable pattern

`ProfileBar.ApplyProfile` lines 258-272 currently does:
```csharp
_fanControl.SetFanCurveEnabled(false);     // become: SetCustomFanCurveMode(false)
_fanTab.LoadCurves(cpuCurve, gpuCurve);    // writes fan table
_fanControl.SetFanCurveEnabled(profile.Fan.CurveEnabled);  // become: SetCustomFanCurveMode(profile.Fan.CurveEnabled)
```

This disable-write-enable pattern is a safety measure: clearing custom mode before writing the table prevents the EC from interpreting half-written curves. **Preserve this pattern** — the new `SetCustomFanCurveMode` is a drop-in replacement with the same ordering guarantee.

Note: with the new combined semantic, `SetCustomFanCurveMode(false)` writes both `0x7C5` bit 7 = 0 and `0x7C6` = 0. Then after the table write, `SetCustomFanCurveMode(true)` writes both back on. This is 4 control-byte writes (2 × bitfield RMW + 2 × direct) per profile load instead of the current 2. Negligible.

### A6. Verify read after write — how to compare

In `MainForm` orchestration, after writing user curves and taking the verify snapshot, compare the **active portion** of the curves (not the padding, which may have legitimate differences due to `PaddingDuty` detection).

Minimum viable comparison (add to `FanSnapshot`):
```csharp
public List<string> CompareActiveRegion(FanSnapshot other, int activeEntries)
{
    var diffs = new List<string>();
    void Check(string label, List<int> a, List<int> b)
    {
        for (int i = 0; i < activeEntries; i++)
            if (a[i] != b[i])
                diffs.Add($"{label}[{i}]: 0x{a[i]:X2} != 0x{b[i]:X2}");
    }
    Check("CpuTempUp",   CpuTempUp,   other.CpuTempUp);
    Check("CpuTempDown", CpuTempDown, other.CpuTempDown);
    Check("CpuDuty",     CpuDuty,     other.CpuDuty);
    Check("GpuTempUp",   GpuTempUp,   other.GpuTempUp);
    Check("GpuTempDown", GpuTempDown, other.GpuTempDown);
    Check("GpuDuty",     GpuDuty,     other.GpuDuty);
    return diffs;
}
```

Log diffs as warnings if any. Empty list = verify OK.

### A7. Reference data

**User profile under test:** `C:\Users\Jon\clean\MechControl\bin\Release\net8.0-windows\userprofile_2.json`
- `projectId: "IDY_7Y"`
- `fan.curveEnabled: true`
- `fan.linkedEnabled: true`
- `fan.cpuCurve`: 10 active entries, tempUp starts at `[15, 24, 43, 58, 65, 71, 78, 86, 93, 101]`, duty `[2, 2, 2, 2, 22, 46, 72, 108, 150, 198]` (raw values, already ×2 percent)
- `fan.gpuCurve`: 10 active entries, similar shape
- Padding at indices 10-15 = `255` (tempUp/down) and `180` (duty)

**Verified profile under test:** `C:\Users\Jon\clean\MechControl\Profiles\MECHREVO\IDY_7Y.json`
- `verified: true`
- Has a `defaultFanCurve` with 10 active CPU + 10 active GPU entries
- `fanTable.paddingDuty: 200`, `paddingTemp: 255`, `entriesPerArray: 16`, `activeEntries: 10`
- Registers: `cpuTempUpBase: 0xF00`, `cpuTempDownBase: 0xF10`, `cpuDutyBase: 0xF20`, `gpuTempUpBase: 0xF30`, `gpuTempDownBase: 0xF40`, `gpuDutyBase: 0xF50`

### A8. Diagnostic log location

`mechcontrol.log` is written to `AppContext.BaseDirectory` (= `C:\Users\Jon\clean\MechControl\bin\Release\net8.0-windows\mechcontrol.log`). Open after a test run to verify the new "Fan Table Dump" and "Fan Table Detection" sections look right.

### A9. Build command (elevated shell not required for build, only for run)

```bash
export PATH="$PATH:/c/Users/Jon/.dotnet"
cd /c/Users/Jon/clean/MechControl
dotnet build -c Release
# Then run (elevated terminal required):
./bin/Release/net8.0-windows/MechControl.exe
```

### A10. What NOT to touch

- `Driver/EcDevice.cs` — no driver changes needed
- `Driver/WriteGuard.cs` — no guard changes needed
- Profile JSONs — no schema changes
- `StartupValidator.cs` — its `0x7C5` bitfield read is unrelated to the fan-curve state refactor; leave it
- `EcMonitor.cs`, `PerfCounterPublisher.cs` — read fan duty/RPM, not fan table
- `FanCurvePanel.cs` — UI rendering, no changes
- `LightingControl.cs` and logo code — `0x7C5` bit 5 is logo-adjacent but our init still sets it via `|= 0x60`. Lighting should be unaffected.
