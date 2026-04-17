# MechControl Code Review -- Fixes to Implement

This document describes code improvements identified during a thorough review of the entire MechControl codebase (~5,300 lines, 32 .cs files). Each fix includes the exact file, line numbers, what to change, and why. Fixes are ordered by impact.

Read `CLAUDE.md`, `architecture.md`, and `register_reference.md` before starting. Build and test after each fix with:
```bash
export PATH="$PATH:/c/Users/Jon/.dotnet"
cd /c/Users/Jon/clean/MechControl
dotnet build -c Release
```

---

## Fix 1: Delete Dead Code -- LogoLedControl.cs and LogoTab.cs

**Files:**
- `MechControl/Features/LogoLedControl.cs` (52 lines)
- `MechControl/UI/LogoTab.cs` (199 lines)

**Problem:** These two files are completely dead code. They are never instantiated anywhere in the application. `MainForm.cs` creates `LightingControl` + `LightingTab` instead, which handle the logo as one of three zones (keyboard, light bar, logo). The architecture doc labels these as "kept as reference" but they serve no purpose in the compiled app and create maintenance confusion.

`LogoLedControl` is a subset of `LightingControl` -- it only handles EC writes for the logo, without HID support or brightness levels. `LightingControl` fully supersedes it with proper HID+EC coordination, 5-level brightness encoding, and RGB scaling (0-60 EC range to 0-255 HID range).

`LogoTab` uses `LogoLedControl` and has a simpler UI without brightness control. `LightingTab` fully supersedes it.

**Fix:** Delete both files entirely. Verify no compilation errors. No other file references either class.

**Verify:** `grep -r "LogoLedControl\|LogoTab" MechControl/` should return zero results in .cs files (only this doc and architecture.md).

**Also:** Update `architecture.md` section 1 (Project Structure) to remove the two entries:
- Remove `LogoLedControl.cs` line: `LogoLedControl.cs             Legacy logo EC-only control (kept as reference)`
- Remove `LogoTab.cs` line: `LogoTab.cs                    Legacy logo-only tab (kept as reference)`

---

## Fix 2: Eliminate Per-Write Array Allocation in EcDevice

**File:** `MechControl/Driver/EcDevice.cs`, line 91

**Problem:** `WriteByteInternal` allocates a new `int[2]` array on every single EC write:
```csharp
private void WriteByteInternal(int addr, byte value)
{
    int[] inBuf = [addr, value];  // NEW ALLOCATION EVERY CALL
    ...
}
```

This is a hot path. During EC init, 96+ fan table entries are written. During profile apply, 50+ writes happen. The monitor doesn't write, but the fan curve panel triggers writes on every "Apply". Each allocation is small (8 bytes + object header) but unnecessary since the method is already called under `_lock`, making a shared buffer safe.

**Fix:** Add a private field and reuse it:

```csharp
// Add as a field (after _lock, around line 9):
private readonly int[] _writeBuf = new int[2];

// Change WriteByteInternal (line 89-105) to:
private void WriteByteInternal(int addr, byte value)
{
    _writeBuf[0] = addr;
    _writeBuf[1] = value;
    int outBuf = 0;
    bool ok = EcDriver.DeviceIoControl(
        _handle,
        EcDriver.IOCTL_ECWRITE,
        _writeBuf, 8,
        ref outBuf, 4,
        out _, IntPtr.Zero);

    if (!ok)
    {
        int err = Marshal.GetLastWin32Error();
        throw new Win32Exception(err, $"EC write failed at 0x{addr:X3} value 0x{value:X2} (error {err})");
    }
}
```

**Why safe:** `_writeBuf` is only accessed inside `WriteByteInternal`, which is only called from `WriteByte` (under `_lock`) and `TryReconnect` (also under `_lock`). No race condition possible.

---

## Fix 3: Cache GDI+ Objects in FanCurvePanel

**File:** `MechControl/UI/FanCurvePanel.cs`, lines 73-93 (inside `OnPaint`)

**Problem:** Every `OnPaint` call creates and disposes 6 GDI+ objects:
```csharp
using var titleFont = new System.Drawing.Font("Segoe UI", 9f, System.Drawing.FontStyle.Bold);
using var gridPen = new System.Drawing.Pen(System.Drawing.Color.FromArgb(190, 195, 200), 1);
using var axisPen = new System.Drawing.Pen(System.Drawing.Color.Black, 1);
using var labelFont = new System.Drawing.Font("Segoe UI", 7f);
using var linePen = new System.Drawing.Pen(System.Drawing.Color.RoyalBlue, 2);
using var fillBrush = new System.Drawing.SolidBrush(System.Drawing.Color.FromArgb(30, 65, 105, 225));
```

`OnPaint` fires on every mouse drag (user dragging fan curve points), window resize, and invalidation. Each GDI+ object allocates a native handle. Creating and destroying them every frame is wasteful.

**Fix:** Replace with static readonly fields at the class level. Add them near the top of the class (after line 13):

```csharp
private static readonly System.Drawing.Font s_titleFont = new("Segoe UI", 9f, System.Drawing.FontStyle.Bold);
private static readonly System.Drawing.Font s_labelFont = new("Segoe UI", 7f);
private static readonly System.Drawing.Pen s_gridPen = new(System.Drawing.Color.FromArgb(190, 195, 200), 1);
private static readonly System.Drawing.Pen s_axisPen = new(System.Drawing.Color.Black, 1);
private static readonly System.Drawing.Pen s_linePen = new(System.Drawing.Color.RoyalBlue, 2);
private static readonly System.Drawing.SolidBrush s_fillBrush = new(System.Drawing.Color.FromArgb(30, 65, 105, 225));
```

Then in `OnPaint`, remove the 6 `using var` lines and replace all references:
- `titleFont` -> `s_titleFont`
- `gridPen` -> `s_gridPen`
- `axisPen` -> `s_axisPen`
- `labelFont` -> `s_labelFont`
- `linePen` -> `s_linePen`
- `fillBrush` -> `s_fillBrush`

Also replace the `labelBrush` local (`var labelBrush = System.Drawing.Brushes.DimGray;` on line 93) -- this one is already using a cached brush from `Brushes.DimGray` so it's fine, just keep it as-is.

Also on line 149, there's a per-point brush allocation inside the draw loop:
```csharp
using var brush = new System.Drawing.SolidBrush(color);
```
This creates a new brush per fan curve point per paint. Cache both colors:
```csharp
// Add as static fields:
private static readonly System.Drawing.SolidBrush s_pointBrush = new(System.Drawing.Color.RoyalBlue);
private static readonly System.Drawing.SolidBrush s_selectedPointBrush = new(System.Drawing.Color.OrangeRed);
```

Then replace the loop body (lines 146-151):
```csharp
var brush = i == _selectedIndex ? s_selectedPointBrush : s_pointBrush;
g.FillEllipse(brush, px.X - PointRadius, px.Y - PointRadius, PointRadius * 2, PointRadius * 2);
g.DrawEllipse(s_axisPen, px.X - PointRadius, px.Y - PointRadius, PointRadius * 2, PointRadius * 2);
```

**Net result:** 0 GDI+ allocations per paint, down from ~7+N (where N = number of fan curve points).

---

## Fix 4: Simplify EcActivationControl.InitializeEc

**File:** `MechControl/Features/EcActivationControl.cs`, lines 105-228

**Problem:** `InitializeEc()` is 120 lines containing 8 nearly identical try/catch blocks. Each block:
1. Tries a write operation
2. Logs success with `Console.WriteLine`
3. Catches exceptions, adds to error list, logs failure
4. Sleeps 50ms

The repetition makes it hard to see the actual init sequence at a glance and easy to introduce inconsistencies when modifying.

**Fix:** Replace the method body with a local helper function. The full replacement for lines 105-228:

```csharp
public List<string> InitializeEc()
{
    var errors = new List<string>();

    void Step(string label, Action action)
    {
        try
        {
            action();
            Console.WriteLine($"[EC Init] {label}");
        }
        catch (Exception ex)
        {
            errors.Add($"{label}: {ex.Message}");
            Console.WriteLine($"[EC Init] FAILED {label}: {ex.Message}");
        }
        Thread.Sleep(50);
    }

    Step("Handshake 0x709=0x05, 0x708=0x01", () =>
    {
        _guard.Write(AddrHandshake1, 0x05);
        _guard.Write(AddrHandshake2, 0x01);
    });

    Step("Activation 0x741=0x05", () =>
        _guard.Write(AddrActivation, 0x05));

    Step("Feature enable 0x748=0x81", () =>
        _guard.Write(AddrFeatureEnable, 0x81));

    Step("Fan curve enable 0x7C5 |= 0xE0", () =>
    {
        byte current = _device.ReadByte(AddrFanCurveEnable);
        _guard.Write(AddrFanCurveEnable, (byte)(current | 0xE0));
    });

    Step("Default fan table", WriteDefaultFanTable);

    Step("Fan curve param 0x7C7=0x0C", () =>
        _guard.Write(AddrFanCurveParam, 0x0C));

    Step("Fan curve mode 0x7C6=0x04", () =>
        _guard.Write(AddrFanCurveMode, 0x04));

    Step("BIOS_CTRL 0x706=0x41", () =>
        _guard.Write(AddrBiosCtrl, 0x41));

    Console.WriteLine(errors.Count == 0
        ? "[EC Init] All steps completed successfully"
        : $"[EC Init] Completed with {errors.Count} error(s)");

    return errors;
}
```

**Keep unchanged:** All the private constants (`AddrHandshake1`, etc.), `WriteDefaultFanTable()`, `WriteFanArray()`, and the public methods `IsUnlocked()`, `Unlock()`, `Lock()`, `IsFanCurveModeEnabled()`, `EnableFanCurveMode()`, `DisableFanCurveMode()`. Only the `InitializeEc()` method body changes.

**Result:** ~120 lines -> ~45 lines. Same behavior, same error handling, same logging. Much easier to read the init sequence.

---

## Fix 5: Pre-allocate FanCurveData Lists

**File:** `MechControl/Profile/UserProfile.cs`, lines 63-68

**Problem:** `FanCurveData` creates empty lists that get filled with 16 entries each via `.Add()`. Default `List<int>` capacity is 0, so it resizes at 1, 2, 4, 8, 16 items -- 5 internal array copies per list, 3 lists = 15 unnecessary copies.

```csharp
class FanCurveData
{
    [JsonPropertyName("tempUp")] public List<int> TempUp { get; set; } = new();
    [JsonPropertyName("tempDown")] public List<int> TempDown { get; set; } = new();
    [JsonPropertyName("duty")] public List<int> Duty { get; set; } = new();
}
```

**Fix:** The capacity hint belongs at the call site that fills the list, not in the class definition, because `EntriesPerArray` is model-specific (defaults to 16 but could differ). Leave `FanCurveData` unchanged. Instead, update `ProfileBar.FanCurveToData()` (line 424) to pre-allocate:

```csharp
private FanCurveData FanCurveToData(FanCurveModel model)
{
    int entriesPerArray = _profile.FanTable.EntriesPerArray;
    int paddingTemp = _profile.FanTable.PaddingTemp;
    int paddingDuty = _profile.FanTable.PaddingDuty;

    var data = new FanCurveData
    {
        TempUp = new List<int>(entriesPerArray),
        TempDown = new List<int>(entriesPerArray),
        Duty = new List<int>(entriesPerArray)
    };
    for (int i = 0; i < entriesPerArray; i++)
    {
        // ... rest unchanged ...
    }
    return data;
}
```

This uses the profile's actual `EntriesPerArray` value (could be 8, 12, 16, 20, etc.) instead of hardcoding 16. JSON deserialization doesn't go through this method, so the default empty lists in `FanCurveData` are fine for deserialization.

---

## Fix 6: Remove String from MonitorData Struct

**File:** `MechControl/Monitoring/EcMonitor.cs`, lines 1-18

**Problem:** `MonitorData` is a `struct` but contains `string BatteryModeName`. Strings are reference types, so this struct can't be fully stack-allocated. The string is constructed every 1-second tick via interpolation or list lookup (line 96-98):

```csharp
data.BatteryModeName = data.BatteryModeIndex < _profile.BatteryModes.Count
    ? _profile.BatteryModes[data.BatteryModeIndex].Name
    : $"Unknown ({data.BatteryModeIndex})";
```

**Fix:** Remove `BatteryModeName` from the struct. Move the name lookup to the consumer (`MonitorPanel`).

Step 1: Change `MonitorData` (lines 6-18):
```csharp
struct MonitorData
{
    public int CpuFanPercent;
    public int GpuFanPercent;
    public int CpuFanRpm;
    public int GpuFanRpm;
    public int CpuTempLimit;
    public int BatterySoc;
    public int BatteryModeIndex;
    public bool FanBoostActive;
    public bool FanCurveEnabled;
}
```

Step 2: In `EcMonitor.OnTick` (lines 94-98), remove the name lookup. Just keep:
```csharp
byte batteryRaw = _device.ReadByte(_addrBatteryMode);
data.BatteryModeIndex = RegisterEncoding.DecodeBatteryMode(batteryRaw);
```
Delete the two lines that set `data.BatteryModeName`.

Step 3: `EcMonitor` needs to expose battery mode names for the consumer. Add a public property:
```csharp
public IReadOnlyList<BatteryModeInfo> BatteryModes => _profile.BatteryModes;
```

Step 4: In `MonitorPanel.cs`, update `OnMonitorUpdated` (line 58). The panel needs access to the battery mode list. Two options:

Option A (simpler): Pass the mode names at construction time. Change `MonitorPanel` to accept battery mode names:

```csharp
class MonitorPanel : UserControl
{
    // ... existing fields ...
    private readonly string[] _batteryModeNames;

    public MonitorPanel(IReadOnlyList<BatteryModeInfo> batteryModes)
    {
        _batteryModeNames = batteryModes.Select(m => m.Name).ToArray();
        // ... rest of constructor unchanged ...
    }

    public void OnMonitorUpdated(MonitorData data)
    {
        // ... other labels unchanged ...
        string modeName = data.BatteryModeIndex < _batteryModeNames.Length
            ? _batteryModeNames[data.BatteryModeIndex]
            : $"Unknown ({data.BatteryModeIndex})";
        _lblBattery.Text = $"Battery: {data.BatterySoc}% ({modeName})";
        // ... rest unchanged ...
    }
}
```

Then in `MainForm.cs` (line 55), update the construction:
```csharp
var monitorPanel = new MonitorPanel(profile.BatteryModes) { Dock = DockStyle.Bottom };
```

---

## Fix 7: Extract Z-Order Fix to Static Helper

**Files affected:**
- `MechControl/UI/LightingTab.cs` (lines 394-396)
- `MechControl/UI/FanTab.cs` (lines 221-222)
- `MechControl/UI/PowerTab.cs` (lines 129-130)
- `MechControl/UI/BatteryTab.cs` (lines 117-119)

**Problem:** The same z-order workaround is copy-pasted in 4 files:
```csharp
foreach (var c in Controls.Cast<Control>().OrderBy(c => c.Top).ToList())
    c.BringToFront();
```

In `LightingTab`, there's also a variant that operates on GroupBox children:
```csharp
foreach (Control grp in new Control[] { grpLogo, grpKeyboard, grpBar })
    foreach (var c in grp.Controls.Cast<Control>().OrderBy(c => c.Top).ToList())
        c.BringToFront();
```

**Fix:** Add a static helper method. You can put it in any shared location. A simple approach is a static method in a new file or at the bottom of one of the UI files. Suggested: add to the bottom of `MainForm.cs` or create a small `UiHelpers.cs`:

```csharp
// MechControl/UI/UiHelpers.cs
namespace MechControl.UI;

static class UiHelpers
{
    /// <summary>
    /// Fix WinForms z-order so controls lower on screen draw above
    /// overflow from upper controls (e.g., TrackBar covering labels below it).
    /// </summary>
    public static void FixZOrder(Control.ControlCollection controls)
    {
        foreach (var c in controls.Cast<Control>().OrderBy(c => c.Top).ToList())
            c.BringToFront();
    }
}
```

Then replace all 4 occurrences:
- `PowerTab.cs` line 129: `UiHelpers.FixZOrder(Controls);`
- `FanTab.cs` line 221: `UiHelpers.FixZOrder(Controls);`
- `BatteryTab.cs` line 117: `UiHelpers.FixZOrder(Controls);`
- `LightingTab.cs` lines 394-396: replace with:
  ```csharp
  foreach (Control grp in new Control[] { grpLogo, grpKeyboard, grpBar })
      UiHelpers.FixZOrder(grp.Controls);
  ```

---

## Fix 8: Font Allocation Duplication Across UI Tabs

**Files:**
- `MechControl/UI/LightingTab.cs` lines 46-47
- `MechControl/UI/ProfileBar.cs` lines 23-24
- `MechControl/UI/FanTab.cs` lines 38-39, 58-59 (inline `new Font(...)` calls)
- `MechControl/UI/PowerTab.cs` lines 84-86, 134-141, etc. (inline `new Font(...)` calls)
- `MechControl/UI/BatteryTab.cs` lines 30-31, 38-39 (inline `new Font(...)` calls)
- `MechControl/UI/MonitorPanel.cs` lines 67-68, 75-76 (inline `new Font(...)` calls)

**Problem:** Multiple files independently create identical `Font` objects. For example:
- `new Font("Segoe UI", 9f)` appears in at least 4 files
- `new Font("Segoe UI", 9f, FontStyle.Bold)` appears in at least 4 files
- `new Font("Segoe UI", 8.5f)` appears in at least 2 files
- `new Font(Font.FontFamily, 9f, FontStyle.Bold)` appears inline in FanTab, PowerTab, BatteryTab

Each `new Font(...)` allocates a native GDI+ font handle. These never get disposed.

**Fix:** If you created `UiHelpers.cs` in Fix 7, add shared font definitions there:

```csharp
static class UiHelpers
{
    public static readonly Font FontNormal = new("Segoe UI", 9f);
    public static readonly Font FontBold = new("Segoe UI", 9f, System.Drawing.FontStyle.Bold);
    public static readonly Font FontSmall = new("Segoe UI", 8.5f);
    public static readonly Font FontSmallBold = new("Segoe UI", 8.5f, System.Drawing.FontStyle.Bold);
    public static readonly Font FontHeader = new("Segoe UI", 10f, System.Drawing.FontStyle.Bold);

    // ... FixZOrder from Fix 7 ...
}
```

Then replace all inline `new Font(...)` calls throughout the UI tabs with references to these shared instances. Remove the duplicate `FontNormal`/`FontBold` static fields from `LightingTab` (lines 46-47) and `ProfileBar` (lines 23-24).

This is a broad search-and-replace. Go file by file:

- **ProfileBar.cs:** Remove lines 23-24 (`FontNormal`, `FontBold` declarations). Replace all `FontNormal` with `UiHelpers.FontSmall` (it uses 8.5f) and `FontBold` with `UiHelpers.FontSmallBold`. Actually check the font size -- ProfileBar uses 8.5f, so these map to `FontSmall`/`FontSmallBold`.
- **LightingTab.cs:** Remove lines 46-47. Same replacement. LightingTab uses 9f, so map to `UiHelpers.FontNormal` / `UiHelpers.FontBold`.
- **FanTab.cs:** Replace inline `new Font(Font.FontFamily, 9f, FontStyle.Bold)` with `UiHelpers.FontBold`. Replace `new Font(Font.FontFamily, 8.5f)` with `UiHelpers.FontSmall`. Replace `new Font(Font.FontFamily, 9.5f)` with... you may want to add a `FontMedium = new("Segoe UI", 9.5f)` to UiHelpers.
- **PowerTab.cs:** Same pattern. Replace all inline font creations.
- **BatteryTab.cs:** Same pattern.
- **MonitorPanel.cs:** Uses `new Font("Segoe UI", 8.5f)` -- replace with `UiHelpers.FontSmall`.

**Important:** `FanTab` and `PowerTab` use `Font.FontFamily` (inheriting from parent) while others hardcode "Segoe UI". Standardize to "Segoe UI" since that's what's used everywhere else. This should be visually identical since WinForms defaults to Segoe UI anyway.

---

## Summary Checklist

| # | Fix | Files Changed | Reduction |
|---|-----|---------------|-----------|
| 1 | Delete dead code (LogoLedControl + LogoTab) | Delete 2 files, edit architecture.md | -251 lines |
| 2 | Cache EC write buffer | EcDevice.cs | 0 net (perf improvement) |
| 3 | Cache GDI+ objects in FanCurvePanel | FanCurvePanel.cs | -5 lines, eliminates allocs |
| 4 | Simplify EC init | EcActivationControl.cs | -75 lines |
| 5 | Pre-allocate FanCurveData lists | UserProfile.cs | 0 net (perf improvement) |
| 6 | Remove string from MonitorData | EcMonitor.cs, MonitorPanel.cs, MainForm.cs | -3 lines |
| 7 | Extract z-order helper | New UiHelpers.cs + 4 UI tabs | -6 lines, DRY |
| 8 | Consolidate font allocations | UiHelpers.cs + 6 UI files | -12 lines, fewer GDI handles |

After all fixes: build with `dotnet build -c Release`, run from elevated terminal, verify:
- All 4 tabs load correctly
- Fan curve drag works smoothly
- Profile save/load works
- EC init sequence works (check console output)
- Lighting zones apply correctly
- Monitor panel updates every second
