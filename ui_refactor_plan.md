# MechControl UI Refactor Plan

## How to use this document

This plan is self-contained and authoritative. To execute after `/clear`:

1. Read `CLAUDE.md` (auto-loaded) for project conventions, build command, safety
   rules, and model-compatibility principle.
2. Read this file end-to-end.
3. The Reference Appendix (bottom of this file) contains the exact source for new
   files/helpers, the full slider inventory, a before/after example, and file:line
   pointers to every block being replaced. No other conversation context is
   required.
4. Use `TaskCreate` / `TaskUpdate` to track phase progress — one task per phase,
   mark in-progress on entry, completed only after the phase's Validation step
   passes.

## Goal

Unify the WinForms UI across all tabs behind shared design tokens, primitives, and a
`GroupBox` + `TableLayoutPanel` layout pattern. Remove magic positioning, kill the
`FixZOrder` hack, and cut ~750-900 LOC of hand-tuned slider/button/section boilerplate.

**Non-goal:** this is a layout/primitive refactor, not a behavior change. Every tab
must read/write the EC identically after each phase. Model compatibility (profile
loading, register detection, feature validation) is untouched.

## Current state (quick reference)

- 9 UI files in `MechControl/UI/`, ~3,200 LOC total.
- Only shared primitive today: `UiHelpers.AddLabeledRow` + 7 fonts + `FixZOrder`.
- 12 sliders across 4 tabs with 4 widths, 3 heights, 3 fonts, 3 value-label placements.
- Every section manually positioned with an `int y` cursor and magic spacing values.
- Only `ProfileBar` + `MonitorPanel` use `FlowLayoutPanel`; everything else is absolute.
- `FanTab` has a 31-line custom `Resize` handler to manually re-lay-out on resize.
- ~20 near-identical `try { ... } catch { MessageBox.Show(...) }` blocks.

The Reference Appendix below holds the full slider inventory, the source for every
new helper, and file:line pointers to each block being replaced. The source tree is
the authoritative ground truth for current state; the appendix tells you exactly
where to look.

## Target pattern

```
TabPage
└── FlowLayoutPanel (Dock=Fill, TopDown, WrapContents=false, AutoScroll=true)
    ├── GroupBox "Section A" (Dock=Top, AutoSize)
    │   └── TableLayoutPanel (rows: section content)
    │       ├── Row: SliderRow (Label | Slider Fill | Value)
    │       ├── Row: RgbRow    (R | G | B | Preview)
    │       └── Row: ApplyReadRow
    ├── GroupBox "Section B"
    └── (global Apply/Read row if tab-level commit semantics)
```

Sliders Dock.Fill inside their cell — width follows the window. Button sizes, fonts,
colors, margins all come from `UiMetrics` / `UiColors`.

## Decisions locked in

1. **Apply button placement:** per-section (inside GroupBox) when writes are
   independent; global (outside GroupBoxes, bottom of tab) when writes commit as a
   group. Keeps current semantics — Lighting/LC stay per-section, Power/Battery stay
   global.
2. **Slider height:** `30` everywhere. Drop 28 and 45 variants.
3. **Tick rule:** `Math.Max(1, (max-min)/10)` everywhere.
4. **Value-label placement:** right side, same row as slider.
5. **Value-label font:** `FontMedium`.
6. **Value-label format:** `$"{v} {unit}"` with `unit` param; override via `formatter`
   callback for `v*100 ms` and PL4-doubled display cases.
7. **Enable-gated sliders:** caller wires `slider.Enabled = checkbox.Checked`; not the
   helper's job.
8. **Section container:** `GroupBox` with `AutoSize + GrowAndShrink`, `Dock.Top`,
   `FontBold` title. Applied consistently across all tabs.
9. **Content width:** fill tab — no max-width cap or centering. Matches existing
   behavior.

## Open questions (resolve during Phase 1)

- **AutoScroll ownership:** the outer FlowLayoutPanel, or the TabPage? Only one should
  own it. Pick during BatteryTab pilot.
- **Min tab width:** does current 750×550 MinimumSize still look right with GroupBoxes
  added? Verify in Phase 4.
- **DPI:** test Phase 4 output at 100/125/150% scale before locking pattern.

## Phase order

Each phase leaves the app building and running. Don't start phase N+1 until N passes
its validation.

### Phase 0 — baseline
- Create branch `ui-refactor`.
- Build + run, screenshot each tab at current state for visual regression reference.
- Store screenshots in `notes/ui-baseline/` (local, not committed).

### Phase 1 — design tokens (additive, zero behavior change)
- **Add:** `MechControl/UI/UiMetrics.cs`
  - Margins: `MarginLeft=16`, `SectionGap=12`, `RowGap=6`, `GroupPadX=12`, `GroupPadTop=22`, `GroupPadBottom=10`.
  - Slider: `SliderHeight=30`, `SliderMinWidth=160`.
  - Button: `ButtonApplyWidth=110`, `ButtonReadWidth=140`, `ButtonHeight=32`.
  - Tab: target row height, inter-row gap.
- **Add:** `MechControl/UI/UiColors.cs`
  - `StatusOk`, `StatusWarn`, `StatusError`, `StatusNeutral`, `Hint`, `Disabled`.
- **Modify:** `UiHelpers.cs` — add `FontMediumBold` (9.5pt Bold) if needed to replace
  `MonitorPanel.StatusFont`.
- **Validation:** build passes, no visual change.

### Phase 2 — color + font consolidation (mechanical)
- Replace ad-hoc `Color.DarkRed` / `Red` / `OrangeRed` / `DarkOrange` / `DarkGreen` /
  `DarkBlue` / `Gray` usages with `UiColors.Status*` or `UiColors.Hint`.
- Replace `SystemColors.GrayText` literal with `UiColors.Hint` (same color, named).
- Delete `MonitorPanel.StatusFont`; use `UiHelpers.FontMediumBold`.
- **Validation:** build, run, check each tab still looks correct; status colors
  now uniform across tabs.

### Phase 3 — primitive helpers (additive)
Add to `UiHelpers.cs`:
- `BuildSectionStack()` → `FlowLayoutPanel` configured for top-down section stack.
- `BuildSection(title, autoSize=true)` → standardized `GroupBox`.
- `BuildSliderRow(label, min, max, unit, initial?, formatter?)` → record of `(Label,
  TrackBar, Label)`.
- `BuildRgbRow(initialColor)` → record of `(NumericUpDown R, NumericUpDown G,
  NumericUpDown B, Panel Preview)` with wiring for preview-on-change.
- `BuildApplyReadRow(onApply, onRead?)` → standardized button row (hide Read if
  `onRead==null`).
- `TryUi(action, operationName, title?)` → wraps try/catch + MessageBox with
  standardized error title.

**Validation:** build passes. No existing tab touched yet.

### Phase 4 — pilot: BatteryTab
Smallest tab, no sliders. Proves the GroupBox + TableLayoutPanel pattern without
introducing `BuildSliderRow` risk.

- Wrap "Charging Mode" and "Battery Telemetry" in `BuildSection()`.
- Inside each, use `TableLayoutPanel` with `AutoSize` rows.
- Replace manual `y +=` cursor with row-add calls.
- Use `UiHelpers.AddLabeledRow` inside the Telemetry section (already works).
- Remove `FixZOrder` call.

**Validation:**
- Build + run elevated.
- Open Battery tab, check all 3 radios + Apply/Read + 10 telemetry rows render.
- Click Apply, verify EC write (check diagnostic log).
- Resize window — section reflows, no overlap.
- Test at 125% DPI.

### Phase 5 — PowerTab
First real use of `BuildSliderRow`. 6 sliders.

- Replace local `AddSlider`, `AddLabel`, `AddSectionHeader`, `AddDisabledLabel` with
  helpers.
- `[CPU Power]` and `[GPU Power]` become GroupBoxes.
- Global Apply + Read stays outside GroupBoxes (atomic commit semantics).
- PL4 doubled-tooltip case uses the `formatter` param.
- Remove `FixZOrder` call.

**Validation:** all 6 sliders move, value labels update with correct units, Apply
writes PL1/PL2/PL4/Temp/TGP/DBMax correctly, tooltips still show register addresses.

### Phase 6 — FanTab
Tests gated slider + custom formatter + drops the bespoke Resize handler.

- `[Fan Boost]`, `[Custom Fan Curve]`, `[Fan Switch Speed]` become GroupBoxes.
- Switch-speed slider uses `formatter: v => $"{v * 100} ms"`.
- Switch-speed checkbox gates slider: caller wires `slider.Enabled = chk.Checked`.
- `FanCurvePanel` instances Dock.Fill inside their GroupBox (`[Custom Fan Curve]`
  section gets a fixed height since curve panels have their own min size).
- **Delete the 31-line `Resize` handler** — GroupBox + Dock.Fill handles it.
- Remove `FixZOrder` call.

**Validation:** fan boost toggles, curve editing still works (drag points), switch
speed slider gated correctly, window resize reflows curve panels smoothly, linked
fans status bar still paints correctly.

### Phase 7 — LiquidCoolerTab
Tests `BuildRgbRow` before Lighting needs it in Phase 8.

- `[Connection]`, `[Radiator Fan]`, `[Pump]`, `[LED]` become GroupBoxes.
- Fan + Pump sliders → `BuildSliderRow(..., "%")`.
- LED RGB section → `BuildRgbRow(...)`.
- Per-section Apply buttons stay inside their GroupBox.
- BT-unavailable overlay stays as `Dock.Fill` Panel on top of the section stack.
- Remove `FixZOrder` call.

**Validation:** BLE connect/disconnect/scan works, fan + pump + RGB Apply buttons
each write correctly, BT overlay appears when BT is off, reconnect timer still works.

### Phase 8 — extract LightingZonePanel + convert LightingTab
Biggest LOC win. Do this after `BuildRgbRow` + `BuildSliderRow` are proven.

- **Add:** `MechControl/UI/LightingZonePanel.cs`
  - `UserControl` exposing: `Title`, `HasSaveButton`, `Enabled`, `Color`, `Brightness`,
    `ApplyClicked`, `SaveClicked`, `PickColorClicked` events.
  - Internally composes: checkbox + `BuildSliderRow` (brightness) + `BuildRgbRow` +
    Apply/Save/PickColor buttons.
- **Modify:** `LightingTab.cs`
  - 3× `LightingZonePanel` instances replace the 3 hand-coded zone blocks.
  - `[Idle Timer]` and `[Local Dimming]` become GroupBoxes.
  - Drop fixed `420×170` dimensions — sections now AutoSize and fill tab width.
  - Brightness sliders go from 160px to full-width (Dock.Fill).
  - Remove `FixZOrder` call.

**Validation:** all 3 zones apply independently, save-to-BIOS works for keyboard + bar,
pick-color dialog works, idle timer + local dimming still work, `RefreshFromEc`
repopulates all 3 zones correctly.

### Phase 9 — SystemInfoTab (trivial)
- Wrap Identity / Temperatures / Power Delivery in `BuildSection()`.
- Keep `UiHelpers.AddLabeledRow` usage (it already works well here).
- Remove `FixZOrder` call.

**Validation:** monitor updates still populate labels correctly.

### Phase 10 — cross-cutting: TryUi sweep
Mechanical cleanup, run after all tabs are converted so we see the final set of
sites.

- Replace ~20 `try { ... } catch (Exception ex) { MessageBox.Show(...); }` blocks
  with `UiHelpers.TryUi(() => ..., "apply fan boost");`.
- Preserve existing "title" and "operation" wording where it's user-facing.
- Keep the catches that do something non-trivial (e.g., revert state) — those aren't
  just message boxes.

**Validation:** build + run + manually trigger 2-3 error paths (e.g., unplug or
simulate a write failure) to confirm the message still renders.

### Phase 11 — final sweep
- `Grep` for `new System.Drawing.Point(` in `MechControl/UI/` — should be near zero
  outside of `FanCurvePanel` internals and the tray-icon drawing code.
- `Grep` for `FixZOrder` — should be zero.
- `Grep` for ad-hoc `Color.Dark*`, `Color.Red`, `Color.OrangeRed` — should be zero
  outside `UiColors.cs`.
- Full app run: open each tab, click Apply/Read, resize to min + max, test at 100%
  and 125% DPI.
- Compare final screenshots to Phase 0 baseline — information density + content
  should be equivalent, layout should be cleaner.

### Phase 12 — deferred
- **ProfileSchema.cs record/policy refactor:** skipped. `PL1`/`PL2`/`PL4`/`CTGP`/`TGP`
  caps mean `JsonNamingPolicy.CamelCase` would require per-field overrides, cancelling
  the savings. Records fight mutable runtime state. Revisit only if profile schema
  needs independent work.
- **Control-construction DSL:** superseded by the TableLayoutPanel + helpers approach.
  `Ui.Label(parent, x, y, text)` was solving the wrong problem (verbose construction)
  — the real problem was absolute positioning, which GroupBox + TableLayoutPanel
  solves structurally.

## Artifacts expected

**New files (Phase 1, 3, 8):**
- `MechControl/UI/UiMetrics.cs` — ~40 LOC, constants only.
- `MechControl/UI/UiColors.cs` — ~20 LOC, static readonly Color fields.
- `MechControl/UI/LightingZonePanel.cs` — ~120 LOC, UserControl.

**Modified files:**
- `UiHelpers.cs`: +~150 LOC (6 primitives + `TryUi` + `FontMediumBold`).
- All 9 tab/panel files: net reduction per file.

**Expected net LOC delta:**
- LightingTab: 545 → ~220 (-325)
- LiquidCoolerTab: 720 → ~520 (-200)
- PowerTab: 297 → ~180 (-117)
- FanTab: 530 → ~380 (-150)
- BatteryTab: 212 → ~180 (-32)
- SystemInfoTab: 101 → ~90 (-11)
- MonitorPanel: 71 → ~65 (-6)
- UiHelpers: 41 → ~200 (+160)
- New: UiMetrics (+40), UiColors (+20), LightingZonePanel (+120)

**Net: ~-540 LOC**, with the visible win being that adding a new tab is now a matter
of calling primitives instead of fighting a grid of magic numbers.

## Risk register

| Risk | Phase | Mitigation |
|------|-------|------------|
| GroupBox + AutoSize + TrackBar misbehaves | 4 | Pilot in BatteryTab first (no TrackBars); test TrackBar case in 5 before committing pattern |
| Double AutoScroll (TabPage + FlowLayoutPanel) | 4 | Pick one owner during Phase 4; document in helpers |
| FanCurvePanel min-size fights GroupBox AutoSize | 6 | Give `[Custom Fan Curve]` GroupBox a fixed height equal to `FanCurvePanel.MinimumSize.Height + padding` |
| DPI scaling breaks at 125%/150% | 4, 11 | Test after Phase 4 pilot and again in Phase 11 |
| Visual regression hard to spot | all | Phase 0 baseline screenshots per tab, compared at Phase 11 |
| Per-phase validation is manual | all | Accept — WinForms UI testing is painful; use diagnostic log for EC-write verification |

## Task tracking during execution

Use `TaskCreate` / `TaskUpdate` per phase when executing. Mark each phase
`in_progress` on entry, `completed` only after validation passes. Do NOT batch
completion at the end — one phase at a time, so a crash/rollback has a clean boundary.

## Rollback strategy

Each phase is one commit (or a small series). If a phase's validation fails and the
cause isn't obvious within 30 min:

1. `git stash` the in-progress work.
2. Reset to the last known-good commit.
3. Re-attempt with smaller scope (e.g., convert half of FanTab's sections first).

## Model-compatibility note

Nothing in this plan reads or writes EC registers, profile data, or hardware
detection. The `MachineProfile` flow stays identical. New UI helpers are model-agnostic
— they receive limits/values from the profile and render them, exactly as today.
Verify after each phase by loading the app on the test machine and checking that all
existing features still work for that model.

---

# Reference Appendix

The project has `ImplicitUsings=enable` and `UseWindowsForms=true` in
`MechControl.csproj`, so `System.Drawing.*` and `System.Windows.Forms.*` types
(`Label`, `TrackBar`, `Color`, `Font`, etc.) don't need using statements in any of
the files below.

## Appendix A — UiMetrics.cs (full source, Phase 1)

```csharp
namespace MechControl.UI;

static class UiMetrics
{
    // Section / row spacing
    public const int MarginLeft = 16;
    public const int SectionGap = 12;
    public const int RowGap = 6;

    // GroupBox interior padding (Top accounts for the GroupBox title)
    public const int GroupPadX = 12;
    public const int GroupPadTop = 22;
    public const int GroupPadBottom = 10;

    // Slider
    public const int SliderHeight = 30;
    public const int SliderMinWidth = 160;

    // Buttons
    public const int ButtonApplyWidth = 110;
    public const int ButtonReadWidth = 140;
    public const int ButtonHeight = 32;
    public const int ButtonGap = 10;

    // Small controls
    public const int NumericWidth = 68;
    public const int RgbPreviewSize = 32;
}
```

## Appendix B — UiColors.cs (full source, Phase 1)

```csharp
namespace MechControl.UI;

static class UiColors
{
    public static readonly Color StatusOk      = Color.FromArgb(46, 125, 50);   // green
    public static readonly Color StatusWarn    = Color.FromArgb(230, 124, 0);   // amber
    public static readonly Color StatusError   = Color.FromArgb(198, 40, 40);   // red
    public static readonly Color StatusNeutral = SystemColors.GrayText;
    public static readonly Color Hint          = SystemColors.GrayText;
    public static readonly Color Disabled      = SystemColors.GrayText;
}
```

## Appendix C — UiHelpers.cs additions (Phase 1 + Phase 3)

Add to the existing `static class UiHelpers`:

```csharp
// Phase 1: new font variant to replace MonitorPanel.StatusFont
public static readonly Font FontMediumBold =
    new("Segoe UI", 9.5f, FontStyle.Bold);

// Phase 3: records returned from the builders
public sealed record SliderRow(Label Label, TrackBar Slider, Label Value);
public sealed record RgbRow(
    NumericUpDown R, NumericUpDown G, NumericUpDown B, Panel Preview);

// Phase 3: section stack (outer layout per tab)
public static FlowLayoutPanel BuildSectionStack() => new()
{
    Dock = DockStyle.Fill,
    FlowDirection = FlowDirection.TopDown,
    WrapContents = false,
    AutoScroll = true,
    Padding = new Padding(0),
};

// Phase 3: standardized section GroupBox
public static GroupBox BuildSection(string title, bool autoSize = true) => new()
{
    Text = title,
    Font = FontBold,
    Dock = DockStyle.Top,
    AutoSize = autoSize,
    AutoSizeMode = AutoSizeMode.GrowAndShrink,
    Padding = new Padding(
        UiMetrics.GroupPadX, UiMetrics.GroupPadTop,
        UiMetrics.GroupPadX, UiMetrics.GroupPadBottom),
    Margin = new Padding(0, 0, 0, UiMetrics.SectionGap),
};

// Phase 3: slider row — caller places the three controls in a 3-column
// TableLayoutPanel row (Label AutoSize | Slider Fill | Value AutoSize).
// Formatter overrides the default "{v} {unit}" formatting.
public static SliderRow BuildSliderRow(
    string label, int min, int max, string unit,
    int? initial = null, Func<int, string>? formatter = null)
{
    var lbl = new Label { Text = label, AutoSize = true, Font = FontBold };
    var trk = new TrackBar
    {
        Minimum = min,
        Maximum = max,
        Value = initial ?? min,
        TickFrequency = Math.Max(1, (max - min) / 10),
        LargeChange  = Math.Max(1, (max - min) / 10),
        SmallChange = 1,
        AutoSize = false,
        Height = UiMetrics.SliderHeight,
        Dock = DockStyle.Fill,
    };
    var fmt = formatter ?? (v => string.IsNullOrEmpty(unit) ? v.ToString() : $"{v} {unit}");
    var val = new Label { Text = fmt(trk.Value), AutoSize = true, Font = FontMedium };
    trk.ValueChanged += (_, _) => val.Text = fmt(trk.Value);
    return new SliderRow(lbl, trk, val);
}

// Phase 3: RGB triplet + live preview
public static RgbRow BuildRgbRow(byte r, byte g, byte b)
{
    NumericUpDown Make(int initial) => new()
    {
        Minimum = 0, Maximum = 255,
        Value = initial,
        Width = UiMetrics.NumericWidth,
        Font = FontNormal,
        TextAlign = HorizontalAlignment.Right,
    };
    var nudR = Make(r);
    var nudG = Make(g);
    var nudB = Make(b);
    var preview = new Panel
    {
        Width = UiMetrics.RgbPreviewSize,
        Height = UiMetrics.RgbPreviewSize,
        BorderStyle = BorderStyle.FixedSingle,
        BackColor = Color.FromArgb(r, g, b),
    };
    void Update(object? s, EventArgs e) =>
        preview.BackColor = Color.FromArgb(
            (int)nudR.Value, (int)nudG.Value, (int)nudB.Value);
    nudR.ValueChanged += Update;
    nudG.ValueChanged += Update;
    nudB.ValueChanged += Update;
    return new RgbRow(nudR, nudG, nudB, preview);
}

// Phase 3: Apply + optional Read button pair
public static (Button Apply, Button? Read) BuildApplyReadRow(
    EventHandler onApply, EventHandler? onRead = null)
{
    var btnApply = new Button
    {
        Text = "Apply",
        Width = UiMetrics.ButtonApplyWidth,
        Height = UiMetrics.ButtonHeight,
        Font = FontNormal,
    };
    btnApply.Click += onApply;

    Button? btnRead = null;
    if (onRead != null)
    {
        btnRead = new Button
        {
            Text = "Read from EC",
            Width = UiMetrics.ButtonReadWidth,
            Height = UiMetrics.ButtonHeight,
            Font = FontNormal,
        };
        btnRead.Click += onRead;
    }
    return (btnApply, btnRead);
}

// Phase 3: unified try/catch + MessageBox for UI write paths
public static void TryUi(Action action, string operation, string? title = null)
{
    try { action(); }
    catch (Exception ex)
    {
        MessageBox.Show(
            $"Failed to {operation}:\n{ex.Message}",
            title ?? "Error",
            MessageBoxButtons.OK, MessageBoxIcon.Error);
    }
}
```

## Appendix D — Slider inventory (12 total, Phase 5/6/7/8)

| Tab | Slider | Current (file:line) | Replacement call |
|-----|--------|---------------------|-------------------|
| PowerTab | PL1 | `PowerTab.cs:78` via `AddSlider` | `BuildSliderRow("PL1 (Sustained)", pl1Limit.Min, pl1Limit.Max, "W")` |
| PowerTab | PL2 | `PowerTab.cs:80` | `BuildSliderRow("PL2 (Burst)", pl2Limit.Min, pl2Limit.Max, "W")` |
| PowerTab | PL4 | `PowerTab.cs:82` | `BuildSliderRow("PL4 (Peak)", pl4Limit.Min, pl4Limit.Max, "W")` — keep PL4 doubled-tooltip logic in the existing tooltip handler, not the formatter |
| PowerTab | Temp | `PowerTab.cs:84` | `BuildSliderRow("CPU Temp Limit", tempLimit.Min, tempLimit.Max, "°C")` |
| PowerTab | TGP | `PowerTab.cs:102` | `BuildSliderRow("GPU TGP (Total Graphics Power)", tgpLimit.Min, tgpLimit.Max, "W")` |
| PowerTab | DB Max | `PowerTab.cs:120` | `BuildSliderRow("Dynamic Boost Max", dbLimit.Min, dbLimit.Max, "W")` |
| FanTab | Switch Speed | `FanTab.cs:199-212` | `BuildSliderRow("Switch Speed", 1, 127, "", formatter: v => $"{v * 100} ms")` |
| LightingTab | Logo Brightness | `LightingTab.cs:95-109` | `BuildSliderRow("Brightness", 0, 100, "%")` |
| LightingTab | Keyboard Brightness | `LightingTab.cs:166-186` | `BuildSliderRow("Brightness", 0, 100, "%")` |
| LightingTab | Light Bar Brightness | `LightingTab.cs:250-266` | `BuildSliderRow("Brightness", 0, 100, "%")` |
| LiquidCoolerTab | Fan Duty | `LiquidCoolerTab.cs:166-176` | `BuildSliderRow("Fan Duty", 0, 100, "%")` |
| LiquidCoolerTab | Pump Duty | `LiquidCoolerTab.cs:234-244` | `BuildSliderRow("Pump Duty", 0, 100, "%")` |

All callers place the returned `SliderRow.Label / Slider / Value` into a 3-column
`TableLayoutPanel` row (`AutoSize | Percent 100 | AutoSize`) so the slider fills
whatever horizontal space the section GroupBox gives it.

## Appendix E — Blocks being replaced (file:line → what)

**LightingTab.cs (545 LOC → ~220):**
- Lines 68-137: Logo zone block → one `new LightingZonePanel("Logo LED (Rear Cover)", hasSaveButton: false)`
- Lines 139-221: Keyboard zone block → `new LightingZonePanel("Keyboard Backlight", hasSaveButton: true)`
- Lines 223-302: Light Bar zone block → `new LightingZonePanel("Light Bar (Front)", hasSaveButton: true)`
- Lines 393-424: `CreateColorRow` static method → deleted; `BuildRgbRow` used internally by `LightingZonePanel`
- Line 389-390: `FixZOrder` call → deleted

**PowerTab.cs (297 LOC → ~180):**
- Lines 180-204: local `AddSectionHeader`, `AddLabel`, `AddDisabledLabel` → deleted; use `BuildSection` + plain Labels
- Lines 219-249: local `AddSlider` → deleted; use `BuildSliderRow`
- Line 177: `FixZOrder` call → deleted

**FanTab.cs (530 LOC → ~380):**
- Lines 199-212: inline switch-speed slider → `BuildSliderRow(...)` with formatter
- Lines 240-271: `Resize += (s,e) => { ... manual positioning ... }` — **entire handler deleted**, reflow handled by `GroupBox + TableLayoutPanel`
- Line 275: `FixZOrder` call → deleted

**LiquidCoolerTab.cs (720 LOC → ~520):**
- Lines 147-185: inline fan slider + label + apply button → `BuildSliderRow` + `BuildApplyReadRow`
- Lines 198-253: inline pump slider + voltage combo + label + apply → similar
- Lines 266-337: RGB section (R/G/B numerics + preview + mode combo + apply) → `BuildRgbRow` + apply
- Line 389: `FixZOrder` call → deleted

**BatteryTab.cs (212 LOC → ~180):**
- Lines 38-104: section titles + radios with manual Y positioning → `BuildSection("Battery Charging Mode")` + `TableLayoutPanel` rows
- Lines 127-135: telemetry title → `BuildSection("Battery Telemetry")`
- Line 160: `FixZOrder` call → deleted

**SystemInfoTab.cs (101 LOC → ~90):**
- Lines 57-65, 72-80: section title Labels → `BuildSection()` wrappers
- Line 87: `FixZOrder` call → deleted

**MonitorPanel.cs (71 LOC → ~65):**
- Lines 63-64: `StatusFont` static field → deleted, replace all uses with `UiHelpers.FontMediumBold`

**Error-handling sweep (Phase 10):**
Search `MechControl/UI/*.cs` for `MessageBox.Show($"Failed to` — each is a
`TryUi` candidate. Known sites:
- `LightingTab.cs`: BtnApplyLogo_Click, BtnApplyKeyboard_Click, BtnApplyLightBar_Click,
  BtnSaveKeyboard_Click, BtnSaveLightBar_Click, ChkDimming_CheckedChanged
- `PowerTab.cs`: BtnApply_Click (CPU + GPU branches), RefreshFromEc (CPU + GPU)
- `FanTab.cs`: BtnApplyBoost_Click, BtnApply_Click, BtnApplySwitchSpeed_Click,
  ReadCurvesFromEc, RefreshFromEc, OnLinkFansChanged (Enable/Disable)
- `BatteryTab.cs`: BtnApply_Click, RefreshFromEc (not RefreshTelemetry — that one
  silently swallows)
- `LiquidCoolerTab.cs`: BtnFanApply_Click, BtnPumpApply_Click, BtnRgbApply_Click

## Appendix F — BatteryTab conversion example (Phase 4 pilot pattern)

Shows the general shape. Apply the same structure to every tab conversion.

**Before** (abridged, `BatteryTab.cs:28-124`):
```csharp
Dock = DockStyle.Fill;
AutoScroll = true;

int y = 16;

var lblTitle = new Label
{
    Text = "Battery Charging Mode",
    Location = new Point(16, y),
    AutoSize = true,
    Font = UiHelpers.FontHeader
};
Controls.Add(lblTitle);
y += 32;

_radioHighCapacity = new RadioButton
{
    Text = "High Capacity (Performance)",
    Location = new Point(16, y),
    AutoSize = true,
    Checked = true
};
Controls.Add(_radioHighCapacity);
y += 22;

Controls.Add(new Label
{
    Text = "Charges to 100%. Best for maximum runtime per charge.",
    Location = new Point(36, y),
    AutoSize = true,
    ForeColor = SystemColors.GrayText
});
y += 30;

// ... 2 more radios, 2 more hints, Apply/Read row, telemetry title + 10 rows ...

UiHelpers.FixZOrder(Controls);
```

**After:**
```csharp
Dock = DockStyle.Fill;

var stack = UiHelpers.BuildSectionStack();

// Charging mode
var grpMode = UiHelpers.BuildSection("Battery Charging Mode");
var modeLayout = new TableLayoutPanel
{
    Dock = DockStyle.Top,
    AutoSize = true,
    AutoSizeMode = AutoSizeMode.GrowAndShrink,
    ColumnCount = 1,
};
_radioHighCapacity = new RadioButton
    { Text = "High Capacity (Performance)", AutoSize = true, Checked = true };
modeLayout.Controls.Add(_radioHighCapacity);
modeLayout.Controls.Add(new Label
{
    Text = "Charges to 100%. Best for maximum runtime per charge.",
    AutoSize = true,
    Margin = new Padding(20, 0, 0, UiMetrics.RowGap),
    ForeColor = UiColors.Hint,
});
// ... 2 more radio/hint pairs ...

var (btnApply, btnRead) = UiHelpers.BuildApplyReadRow(BtnApply_Click, BtnRead_Click);
var buttonRow = new FlowLayoutPanel
    { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, Margin = new Padding(0, UiMetrics.RowGap, 0, 0) };
buttonRow.Controls.Add(btnApply);
buttonRow.Controls.Add(btnRead!);
modeLayout.Controls.Add(buttonRow);
grpMode.Controls.Add(modeLayout);
stack.Controls.Add(grpMode);

// Telemetry
var grpTelemetry = UiHelpers.BuildSection("Battery Telemetry");
var telemetryLayout = new TableLayoutPanel
{
    Dock = DockStyle.Top,
    AutoSize = true,
    AutoSizeMode = AutoSizeMode.GrowAndShrink,
    ColumnCount = 2,
};
telemetryLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
telemetryLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));
_lblPowerSource = AddTelemetryRow(telemetryLayout, "Power Source [0x490]:");
// ... 9 more ...
grpTelemetry.Controls.Add(telemetryLayout);
stack.Controls.Add(grpTelemetry);

Controls.Add(stack);
// no FixZOrder — reflow is automatic
```

Where `AddTelemetryRow` is a local 2-liner that adds two labels to consecutive
columns and returns the value label. (The existing `UiHelpers.AddLabeledRow` is
coupled to absolute positioning — it can stay for now since SystemInfoTab still
uses it, or get a TableLayoutPanel-friendly replacement.)

## Appendix G — TryUi conversion example (Phase 10)

**Before** (`FanTab.cs:525-536`):
```csharp
private void BtnApplyBoost_Click(object? sender, EventArgs e)
{
    try
    {
        _control.SetFanBoost(_chkFanBoost.Checked);
    }
    catch (Exception ex)
    {
        MessageBox.Show($"Failed to set fan boost:\n{ex.Message}",
            "EC Write Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
    }
}
```

**After:**
```csharp
private void BtnApplyBoost_Click(object? sender, EventArgs e) =>
    UiHelpers.TryUi(() => _control.SetFanBoost(_chkFanBoost.Checked),
        "set fan boost", "EC Write Error");
```

Skip sites where the catch does something non-trivial (state revert, logging + UI
update, etc.). Example: `LightingTab.ChkDimming_CheckedChanged` reverts the
checkbox in its catch — leave alone or refactor separately.

## Appendix H — LightingZonePanel skeleton (Phase 8)

New file `MechControl/UI/LightingZonePanel.cs`:

```csharp
namespace MechControl.UI;

class LightingZonePanel : UserControl
{
    private readonly CheckBox _chkEnabled;
    private readonly UiHelpers.SliderRow _brightness;
    private readonly UiHelpers.RgbRow _rgb;
    private readonly Button _btnApply;
    private readonly Button _btnPickColor;
    private readonly Button? _btnSave;

    public event EventHandler? ApplyClicked;
    public event EventHandler? SaveClicked;

    public bool ZoneEnabled
    {
        get => _chkEnabled.Checked;
        set => _chkEnabled.Checked = value;
    }
    public Color ZoneColor
    {
        get => Color.FromArgb((int)_rgb.R.Value, (int)_rgb.G.Value, (int)_rgb.B.Value);
        set { _rgb.R.Value = value.R; _rgb.G.Value = value.G; _rgb.B.Value = value.B; }
    }
    public int Brightness
    {
        get => _brightness.Slider.Value;
        set => _brightness.Slider.Value = Math.Clamp(value, 0, 100);
    }

    public LightingZonePanel(string title, bool hasSaveButton,
        byte initialR = 0, byte initialG = 0, byte initialB = 0,
        int initialBrightness = 50, bool initialEnabled = false)
    {
        // Root is a GroupBox via BuildSection(title). Inside, a TableLayoutPanel
        // with 3 rows:
        //   Row 1: [chkEnabled (col-span 2)] [brightness slider row (col-span 3)]
        //   Row 2: [RGB row R G B Preview (col-span 4)]
        //   Row 3: [Apply] [Pick Color] [Save] (if hasSaveButton)
        //
        // Wire pick-color: opens ColorDialog seeded from preview.BackColor,
        // writes result to R/G/B nudS (which triggers the preview update).
        //
        // Buttons raise ApplyClicked / SaveClicked events — business logic
        // stays in LightingTab.
        ...
    }
}
```

LightingTab becomes:
```csharp
var zoneLogo = new LightingZonePanel("Logo LED (Rear Cover)", hasSaveButton: false);
zoneLogo.ApplyClicked += (s, e) =>
    ApplyZone(LightingControl.ZoneLogo, zoneLogo, _state.Logo, "logo");
stack.Controls.Add(zoneLogo);

var zoneKeyboard = new LightingZonePanel("Keyboard Backlight", hasSaveButton: true);
zoneKeyboard.ApplyClicked += (s, e) => ApplyZone(...);
zoneKeyboard.SaveClicked += (s, e) => SaveZoneToBios(...);
stack.Controls.Add(zoneKeyboard);

// ... Light Bar identical ...
```

The `ApplyZone` / `SaveZoneToBios` helpers in `LightingTab` get simpler — they
accept a `LightingZonePanel` instead of the 6-parameter control tuple currently
in `LightingTab.cs:573-607`.

## Appendix I — Baseline screenshots (Phase 0)

Before starting Phase 1, launch the app elevated on the test machine and take one
screenshot per tab at default window size. Store in `notes/ui-baseline/` (local
only — do not commit). Used as the visual regression reference at Phase 11.

Tabs (conditional on hardware):
- Power
- Fan Control
- Battery
- Lighting
- Liquid Cooler (only if 0x7A4 bit 7 is set on the test machine)
- System Info

Also screenshot the tray icon and monitor panel at the bottom.
