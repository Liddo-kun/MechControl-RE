# UI Refactor — Finishing Plan

**Goal:** Complete the UI refactor that stopped halfway. Finish migrating the
unconverted bits of `FanTab` and `LiquidCoolerTab`, tighten helpers, and address
readability/performance/memory items along the way. LOC reduction is one
metric, not the sole one.

---

## 1. Session context (read this first in a fresh session)

- **Repo:** `C:\Users\Jon\clean - Copy\` (this is where the work happens).
  Sibling `C:\Users\Jon\clean\` is a *different* agent's implementation of the
  same original refactor goals — do NOT edit that one, it's a reference only.
- **Original plan:** `ui_refactor_plan.md` at repo root (797 lines). It
  specified 11 phases targeting `GroupBox + TableLayoutPanel + Dock` layout,
  shared primitives, elimination of absolute-positioning/`FixZOrder`/Resize
  handler, and ~-540 net LOC.
- **What's already been done (two passes):**
  - Pass 1 (pre-compact): implemented the plan's structural goals — new
    `UiMetrics`, `UiColors`, `LightingZonePanel` extracted, all tabs using
    `BuildSectionStack`/`BuildSection`, `TryUi` wrapped around sync write/read
    paths, magic Y-cursor / `FixZOrder` / `FanTab.OnResize` removed.
  - Pass 2 (just completed): consolidated helpers (`AddSliderRow`,
    `AddKeyValueRow`, `BuildKeyValueTable`, `BuildSliderTable`,
    `MakeInlineLabel`, `BuildRgbFlow`, `TryUiAsync`), migrated
    `BatteryTab`/`SystemInfoTab`/`PowerTab`/`LightingZonePanel`/`LiquidCoolerTab`
    to use them, replaced hardcoded `grpCurve.Height = 400` with
    `FanCurvePanel.MinHeight + chrome`, dropped dead `autoSize` param from
    `BuildSection`.
- **Current LOC:** 4426 across 14 UI `.cs` files.
  - Baseline at `HEAD` (initial commit): 4524 across 11 files.
  - Net delta so far: **−98 LOC**.
  - Sibling agent's run: 4105 (**−419 LOC**) with a different strategy —
    they kept absolute positioning and wrapped it in helpers rather than
    switching to `TableLayoutPanel`. Their LOC win is real but they kept
    `FixZOrder` and magic pixel offsets.
- **Visual verification status:** User confirmed the app opens and all tabs
  render correctly after Pass 2. Continue to verify after each phase with
  material layout change (C, D below).

---

## 2. Architecture (for fresh-session grounding)

- **Outer stack per tab:** `UiHelpers.BuildSectionStack()` returns a 1-col
  `TableLayoutPanel` with `Dock.Fill`, `AutoScroll=true`, `Percent 100` column.
- **Per section:** `UiHelpers.BuildSection(title)` returns a `GroupBox` with
  `Dock.Top`, `AutoSize = GrowAndShrink`, consistent padding.
- **Row layouts inside a section:**
  - `BuildSliderTable()` → 3-col (`AutoSize | Percent 100 | AutoSize`) for
    label/slider/value triplets.
  - `BuildKeyValueTable()` → 2-col (`AutoSize | Percent 100`) for
    caption/value labels.
  - `AddSliderRow(layout, SliderRow)` and `AddKeyValueRow(layout, caption)`
    append to the above.
- **Design tokens:**
  - `MechControl/UI/UiMetrics.cs` — sizes/spacing (`MarginLeft`, `SectionGap`,
    `RowGap`, `GroupPadX/Top/Bottom`, `SliderHeight`, `ButtonApplyWidth`,
    `ButtonReadWidth`, `ButtonHeight`, `ButtonGap`, `NumericWidth`,
    `RgbPreviewSize`).
  - `MechControl/UI/UiColors.cs` — semantic colors (`StatusOk`, `StatusWarn`,
    `StatusError`, `StatusNeutral`, `Hint`, `Disabled`).
  - Fonts live in `UiHelpers` statics (`FontTiny` through `FontHeader`).
- **Error surface:** `UiHelpers.TryUi(Action, operation, title?)` for sync
  handlers, `TryUiAsync(Func<Task>, ...)` for async.
- **`FanCurvePanel.MinWidth` / `MinHeight`** public consts (400, 250) exposed
  so callers can size chrome around them without hardcoding.

---

## 3. Available helpers (current surface of `UiHelpers.cs`, 294 LOC)

```
FontTiny, FontSmall, FontSmallBold, FontNormal, FontBold,
FontMedium, FontMediumBold, FontHeader

record SliderRow(Label, TrackBar, Label Value)
record RgbRow(NumericUpDown R, G, B, Panel Preview)
record RgbFlow(FlowLayoutPanel Flow, NumericUpDown R, G, B, Panel Preview)

BuildSectionStack()        -> TableLayoutPanel (outer)
BuildSection(title)        -> GroupBox
BuildSliderTable()         -> 3-col TableLayoutPanel
BuildKeyValueTable()       -> 2-col TableLayoutPanel

BuildSliderRow(label, min, max, unit, initial?, formatter?) -> SliderRow
AddSliderRow(layout, SliderRow)
AddKeyValueRow(layout, caption, initial = "--") -> Label

MakeInlineLabel(text)      -> Label (for "R:" "G:" etc.)
BuildRgbRow(r, g, b)       -> RgbRow (just controls)
BuildRgbFlow(r, g, b)      -> RgbFlow (flow panel + controls)

BuildApplyReadRow(onApply, onRead?) -> (Button, Button?)

TryUi(Action, operation, title?)
TryUiAsync(Func<Task>, operation, title?)
```

Missing (to add in Phase A): a single-button `BuildApplyButton(text, onClick,
width?)` for apply-only sites.

---

## 4. Remaining work by phase

Each phase lists exact file locations, what to change, what to check, and
expected LOC delta.

### Phase A — Apply-button helper + migrate button boilerplate

**Add to `UiHelpers.cs`:**
```csharp
public static Button BuildApplyButton(string text, EventHandler onClick,
    int width = 0, Padding? margin = null)
{
    var b = new Button
    {
        Text = text,
        Width = width > 0 ? width : UiMetrics.ButtonApplyWidth,
        Height = UiMetrics.ButtonHeight,
        Font = FontNormal,
        Margin = margin ?? new Padding(0, UiMetrics.RowGap, 0, 0),
    };
    b.Click += onClick;
    return b;
}
```

**Migration sites:**

1. `FanTab.cs:54-75` — Fan Boost "Apply Boost" button. Replace the 12-line
   inline `new Button` block with:
   ```csharp
   var btnApplyBoost = UiHelpers.BuildApplyButton("Apply Boost", BtnApplyBoost_Click,
       margin: new Padding(0));
   ```
2. `FanTab.cs:174-194` — curve section `_btnApply` and `_btnRead`. Use
   `BuildApplyReadRow`, then set `Visible = _curveEditingEnabled` on both.
3. `FanTab.cs:303-313` — switch speed Apply button. Use `BuildApplyButton`.
4. `LiquidCoolerTab.cs` fan apply (~171-180), pump apply (~246-255), RGB apply
   (~313-321) — 3 buttons, each ~10 lines. Use `BuildApplyButton`.
5. `LightingZonePanel.cs:95-129` — Apply, Pick Color, Save buttons. Apply and
   Save use `BuildApplyButton`; Pick Color stays hand-built (different width,
   different semantics).

**Expected delta:** ~−60 LOC. No layout change — visual identical.

**Verification:** Build passes. No visual check needed; mechanical.

### Phase B — LC Bluetooth unavailable overlay

**File:** `LiquidCoolerTab.cs:330-354`

**Current (absolute positioning, architectural island):**
```csharp
_btUnavailablePanel = new Panel { Dock = DockStyle.Fill, ... };
var lblBtMsg = new Label {
    Text = "...",
    Location = new System.Drawing.Point(24, 24),
    ...
};
var btnBtRetry = new Button {
    Text = "Retry",
    Location = new System.Drawing.Point(24, 100),
    Size = new System.Drawing.Size(80, 36)
};
```

**Replace with FlowLayoutPanel TopDown stack:**
```csharp
_btUnavailablePanel = new Panel {
    Dock = DockStyle.Fill,
    BackColor = System.Drawing.SystemColors.Control,
    Padding = new Padding(24),
    Visible = false,
};
var stack = new FlowLayoutPanel {
    Dock = DockStyle.Fill,
    FlowDirection = FlowDirection.TopDown,
    AutoSize = true,
    WrapContents = false,
};
stack.Controls.Add(new Label {
    Text = "Bluetooth is not available.\n\nEnable Bluetooth in Windows Settings or Device Manager to use the liquid cooler.",
    AutoSize = true,
    Font = UiHelpers.FontMedium,
    Margin = new Padding(0, 0, 0, 16),
});
var btnBtRetry = UiHelpers.BuildApplyButton("Retry",
    async (_, _) => await CheckBluetoothAsync(),
    width: 80);
stack.Controls.Add(btnBtRetry);
_btUnavailablePanel.Controls.Add(stack);
```

**Expected delta:** ~−15 LOC. Only visible when BT adapter is disabled.

**Verification:** Toggle Bluetooth off in Windows Settings. Confirm overlay
renders with label + Retry button stacked and properly padded.

### Phase C — Flatten `LiquidCoolerTab` Connection + Pump sub-layouts

**C.1 Connection section (`LiquidCoolerTab.cs:64-139`):**
Current: 4-col `TableLayoutPanel` with three `SetColumnSpan` calls (status
spans 4, auto-connect spans 2, firmware spans 2).

Split into three sequential `TableLayoutPanel`s docked top:
- Row 1: status label (single full-width)
- Row 2: devices combo (fill) + Scan + Connect (3-col, `Percent 100 | Auto | Auto`)
- Row 3: auto-connect checkbox + firmware label (2-col, `Auto | Fill`)

This reads linearly instead of mentally tracking column spans.

**C.2 Pump section (`LiquidCoolerTab.cs:186-259`):**
Current: `pumpLayout` (3-col) with nested `pumpHeader` (3-col for `Pump On` +
"Voltage:" label + voltage combo), then slider row, then Apply.

Flatten: eliminate `pumpHeader`; the "Pump On + Voltage" row is inline enough
that a single `FlowLayoutPanel` LeftToRight with the three controls is
simpler than a nested table.

**Expected delta:** ~−30 LOC total.

**Verification:** Open LiquidCooler tab. Confirm Connection and Pump
sections render with same controls in the same order. Check that Scan button
fires.

### Phase D — Flatten `FanTab` nested layouts

**File:** `FanTab.cs:80-264`

**Current:** 4 nested `TableLayoutPanel`s inside `grpCurve`:
- `curveInner` (3 rows: header / curves / footer)
- `headerLayout` (3-col: checkbox + link fans checkbox + status panel)
- `curvesLayout` (1 or 2 col: CPU curve panel, optional GPU curve panel)
- `footerLayout` (3-col: Apply + Read + validation label)

**Target:** `curveInner` stays (needed for the 3-row vertical structure
because `FanCurvePanel` is `Dock.Fill` and the header/footer are fixed
height). Convert `headerLayout` and `footerLayout` to `FlowLayoutPanel`
LeftToRight — no column math, no `SetColumnSpan`, children auto-flow.
`curvesLayout` stays (column math is meaningful here: 50/50 split for CPU/GPU).

Also: use `BuildApplyReadRow` for the Apply/Read buttons in the footer.

**Expected delta:** ~−40 LOC.

**Verification:** Open Fan tab. Confirm:
- Custom Fan Curve checkbox and Link Fans checkbox render inline.
- Status panel renders to the right and paints when Link Fans is toggled.
- CPU and GPU curve panels split 50/50.
- Apply, Read buttons render next to the validation label in the footer.

### Phase E — Extract `LinkedFanStatusBar` control

**Extract from:** `FanTab.cs:249-260` (panel wiring) + `FanTab.cs:521-555`
(`PaintLinkedStatus` method, ~35 lines of custom Graphics code).

**New file:** `MechControl/UI/LinkedFanStatusBar.cs`

```csharp
using MechControl.Features;

namespace MechControl.UI;

class LinkedFanStatusBar : Panel
{
    private readonly LinkedFanController _controller;
    private static readonly System.Drawing.Color s_bgColor =
        System.Drawing.Color.FromArgb(245, 245, 245);
    private static readonly System.Drawing.SolidBrush s_fillBrush =
        new(System.Drawing.Color.FromArgb(80, 65, 105, 225));

    public LinkedFanStatusBar(LinkedFanController controller)
    {
        _controller = controller;
        Height = 26;
        Visible = false;
        BorderStyle = BorderStyle.FixedSingle;
        DoubleBuffered = true;          // perf: eliminate paint flicker
        _controller.Updated += () => { if (Visible) Invalidate(); };
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        if (!_controller.IsEnabled) { base.OnPaint(e); return; }
        var g = e.Graphics;
        int w = ClientSize.Width, h = ClientSize.Height;

        g.Clear(s_bgColor);
        float fillW = w * _controller.LinkedDuty / 100f;
        g.FillRectangle(s_fillBrush, 0, 0, fillW, h);

        var font = UiHelpers.FontSmall;
        var fontRamp = UiHelpers.FontTiny;
        float mainY = (h - g.MeasureString("X", font).Height) / 2f;
        float rampY = (h - g.MeasureString("X", fontRamp).Height) / 2f;
        float x = 4;

        void DrawMain(string s) {
            g.DrawString(s, font, System.Drawing.Brushes.Black, x, mainY);
            x += g.MeasureString(s, font).Width - 4;
        }
        void DrawRamp(string s) {
            g.DrawString(s, fontRamp, System.Drawing.Brushes.DimGray, x, rampY);
            x += g.MeasureString(s, fontRamp).Width - 2;
        }

        DrawMain($"{_controller.LinkedDuty}%  \u2502  CPU {_controller.CpuTemp}\u00B0\u2192{_controller.CpuDuty}% ");
        if (_controller.CpuRampTemp > 0)
            DrawRamp($"(safety ramp {_controller.CpuRampTemp}\u00B0C)");
        DrawMain($"  \u2502  GPU {_controller.GpuTemp}\u00B0\u2192{_controller.GpuDuty}% ");
        if (_controller.GpuRampTemp > 0)
            DrawRamp($"(safety ramp {_controller.GpuRampTemp}\u00B0C)");
    }
}
```

**In `FanTab.cs`:** Replace the panel + paint wiring with one line:
```csharp
_linkedStatusBar = new LinkedFanStatusBar(_linkedController) {
    Dock = DockStyle.Fill,
    Margin = new Padding(0, 2, 0, 2),
};
headerLayout.Controls.Add(_linkedStatusBar);
```

Delete `PaintLinkedStatus`, `_linkedStatusPanel`, and the `_linkedController.Updated`
subscription (moved into the bar's ctor).

**Expected delta:** ~−80 LOC in `FanTab.cs`, +~60 LOC new file. Net ~−20.
But `FanTab` becomes much easier to follow.

**Verification:** Enable Link Fans. Confirm the status bar appears, fills to
the right % of width matching linked duty, text updates on monitor tick.

---

## 5. Readability / performance / memory items (interleave with phases)

### Readability

- **R1.** `LightingZonePanel.cs:105-128` — Pick Color button has `Width = 110`
  hardcoded, Save button has `Width = 120` hardcoded. Add
  `UiMetrics.ButtonWideWidth = 120` and use for both (and rename if a better
  name fits the two widths; or use `UiMetrics.ButtonApplyWidth` = 110 for
  Pick Color since it's close enough).
- **R2.** Margin grab-bag. After phase A–E passes, sweep files for
  `Padding(0, 4, 0, 0)`, `Padding(0, 6, ...)`, `Padding(0, 8, ...)` — add a
  small set of named margin tokens (`UiMetrics.ControlTopMargin = 4`,
  `RowPaddingTop = 6`, etc.) or default to `UiMetrics.RowGap` where
  appropriate. Aim: reduce unique `new Padding(...)` shapes from ~15 to ~5.
- **R3.** `PowerTab.cs:162-186` tooltip wiring is inline. Extract a
  `WireRegisterTooltip(control, formatter)` helper if it shortens the block.
  Optional — keep inline if extraction obscures the per-slider logic.
- **R4.** Consider splitting `UiHelpers.cs` (now 294 LOC) if it grows further.
  Natural split: `UiFonts.cs` (the 8 font statics), keep the rest. Defer
  unless another ~50 LOC lands here.

### Performance

- **P1.** `FanCurvePanel` (and the new `LinkedFanStatusBar`) use
  `DoubleBuffered = true` — keep it. Check any other `OnPaint`-heavy custom
  panels for the same.
- **P2.** `PaintLinkedStatus` currently does `g.MeasureString("X", font)` on
  every paint to compute font heights. Cache per-font height once (either as
  statics in `LinkedFanStatusBar` or during first paint). Minor but cheap.
- **P3.** `BatteryTab._telemetryTimer` ticks every 2s continuously. Verify
  `MainForm` calls `StartTelemetry()`/`StopTelemetry()` on tab enter/leave
  (file: `MainForm.cs` around the `TabControl.SelectedIndexChanged` wiring).
  If not, add it — saves an EC read every 2s when user isn't looking at the
  battery tab. Do the same audit for any other per-tab timers.
- **P4.** `MonitorData` updates — if the monitor fires every 1s and updates
  all tabs via events, consider whether `SystemInfoTab.OnMonitorUpdated` is
  subscribed even when the tab isn't visible. If so, either subscribe/
  unsubscribe on tab change, or early-return when `!Visible`. Same for any
  other monitor-driven tab.

### Memory

- **M1.** `BatteryTab._tooltip = new ToolTip();` — `ToolTip` is `IDisposable`
  but never disposed. Dispose in `BatteryTab.Dispose` (override). Same for
  `PowerTab._tooltip`.
- **M2.** Static brushes/pens in `FanCurvePanel.cs:15-20` are app-lifetime —
  fine. Just confirm no new per-paint allocations get introduced during the
  Phase E extraction.
- **M3.** Event handler audit during Phase E: when `LinkedFanStatusBar`
  subscribes to `_controller.Updated` in its ctor, verify that when the tab is
  disposed, the controller is disposed too (it is, in `FanTab.Dispose`), which
  detaches all subscribers. No explicit unsubscription needed.
- **M4.** No known leaks elsewhere — `_ble.ConnectionLost` is explicitly
  detached in `LiquidCoolerTab.Dispose`, timers are disposed.

---

## 6. Execution order

Recommended order: **A → B → R1 → C → D → E → R2 → P2/P3 → M1**

- A first because it's mechanical and enables cleaner Phase B/E.
- B is small, visual but isolated (BT-off state only).
- R1 is a 5-minute token addition that Phase C/D/E will use.
- C, D are the two with real layout impact — verify after each.
- E is the biggest structural move; do last so FanTab's TableLayoutPanel
  structure is already settled.
- R2 sweep after all layout work settles.
- Perf/memory items can land in any order at the end.

---

## 7. Verification workflow

After **each** phase:
```powershell
cd "C:\Users\Jon\clean - Copy\MechControl"
& "$env:USERPROFILE\.dotnet\dotnet.exe" build -c Release -nologo
```
Expect: "Build succeeded" with only the 2 pre-existing `MainForm.cs`
`CS8602` warnings (lines 456, 472) — those are unrelated to the refactor.

After phases **B, C, D, E**: user opens the app and verifies the affected tab.
- Phase B: toggle Bluetooth adapter off, check LiquidCooler tab overlay.
- Phase C: LiquidCooler tab — connection + pump render and function.
- Phase D: Fan tab — curve editing works, Link Fans wires through.
- Phase E: Fan tab with Link Fans enabled — status bar renders and ticks.

Final LOC check:
```bash
cd "/c/Users/Jon/clean - Copy" && ls MechControl/UI/*.cs | xargs wc -l
```
Compare total against current 4426 and baseline 4524.

---

## 8. Realistic target

| Phase | Δ LOC |
|---|---|
| A BuildApplyButton + migrations | ~−60 |
| B BT overlay stack | ~−15 |
| C LC Connection/Pump flatten | ~−30 |
| D FanTab flatten | ~−40 |
| E LinkedFanStatusBar extract | ~−20 net (bigger in FanTab) |
| **Additional from finishing** | **~−165** |

Projected final: **~4261 LOC, net −263 vs baseline (−6% of 4524).**

Sibling `clean\` is at 4105 (−419). We will remain above them by design:
`TableLayoutPanel` + `Dock` is inherently more verbose than absolute
positioning, but it's responsive, DPI-aware, and doesn't carry `FixZOrder`.
The architectural trade-off is intentional; LOC parity is not the goal.

---

## 9. Git hygiene

- Current branch: `main`. Only commit: `67699f8 Initial commit`. All refactor
  work is uncommitted in the working tree.
- Proposed: one commit per phase (A–E) + one for the R/P/M sweep. Or a single
  squashed commit at the end if you prefer. No `--no-verify` or hook-skipping.
- Do NOT touch `C:\Users\Jon\clean\` — that's the sibling reference tree.

---

## 10. What to tell a fresh Claude session

> Read `ui_refactor_finishing_plan.md`. Then read `MechControl/UI/UiHelpers.cs`,
> `MechControl/UI/UiMetrics.cs`, `MechControl/UI/FanTab.cs`, and
> `MechControl/UI/LiquidCoolerTab.cs`. Execute Phase A. Build. Report result.
> Wait for user before proceeding to Phase B.

This file is the source of truth for context; everything needed to resume
after `/compact` is in sections 1–8.
