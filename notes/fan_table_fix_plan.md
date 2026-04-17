# Fan Table Fix — Implementation Plan

**Date:** 2026-04-13
**Status:** Ready to implement
**Evidence:** fan_curve_probe15 tests on X6FR5xxY

---

## Probe15 Results

| Test | Change | Result |
|------|--------|--------|
| 0 | Baseline (exact OEM table) | HIT |
| A | duty[0] = 0xA0 (non-zero) | **MISS** |
| B | 8°C spacing (was 4°C), 2°C hyst | HIT |
| C | Last active point shifted +2°C | HIT |
| D | 6°C spacing + 4°C hysteresis | HIT |

**Conclusions:**
1. `duty[0]` **must** be 0x00. The EC rejects the table if the first duty slot is non-zero.
2. Temp spacing, individual point positions, and hysteresis width are all flexible.
3. MechControl's existing 7-step init is sufficient (no OEM mode profile or edge pulse required).

---

## Architecture Overview

Two concerns, cleanly separated:

| Concern | Owner | Register |
|---------|-------|----------|
| Master enable (on/off toggle) | Callers (FanTab Apply, LinkedFanController.Enable) | `0x7C5` bit 7 (RMW) |
| Table write + apply latch | `WriteCurve()` (self-contained) | `0x7C6` (0x00 → write → 0x04) |

`WriteCurve()` never touches `0x7C5`. Callers never touch `0x7C6`.

### Call sites

**FanTab unified Apply button** (new):
1. RMW `0x7C5` bit 7 on/off (per checkbox state)
2. `ApplyCpuCurve()` → `WriteCurve()` brackets 0x7C6
3. `ApplyGpuCurve()` → `WriteCurve()` brackets 0x7C6

Always writes the table regardless of checkbox state. EC RAM stays in sync
with the UI. When checkbox is off, 0x7C5 bit 7 = 0 so EC ignores the table
even though 0x7C6 ends at 0x04 (both conditions required).

**LinkedFanController.Enable():**
1. `SetCustomFanCurveMode(true)` — sets 0x7C5 bit 7 + 0x7C6=0x04
2. Each tick: `ApplyCpuCurve()` + `ApplyGpuCurve()` (WriteCurve handles 0x7C6)

**LinkedFanController.Disable():**
- Writes panel curves back via `ApplyCpuCurve`/`ApplyGpuCurve`
- Doesn't touch 0x7C5 — custom mode stays on with real curves restored

**ProfileBar.ApplyProfile():**
- Calls `FanTab.LoadCurves()` → `ApplyCpuCurve` + `ApplyGpuCurve`
- Then `SetCustomFanCurveMode(saved state)`

---

## Changes to FanControl.cs

### Fix 1: Point 0 duty pinned to 0% (EC + UI + validation)

**EC requirement:** `duty[0]` must be 0x00 or the EC rejects the entire table.

**Current 1% floor enforcement (must add point-0 exception to each):**

| Location | Current code | Fix |
|----------|-------------|-----|
| `FanCurvePanel.cs:206` | `Math.Clamp(newDuty, 1, DutyMax)` | `i == 0 ? 0 : Math.Clamp(newDuty, 1, DutyMax)` |
| `FanCurvePanel.cs:225` | `Math.Max(1, pts[j+1].DutyPercent)` | `Math.Max(j == 0 ? 0 : 1, ...)` |
| `FanControl.cs:142-144` | `if (rawDuty == 0) rawDuty = 2` | Skip when `i == 0` (already handled by writing 0x00) |
| `LinkedFanController.cs:168` | `Math.Clamp(dutyPercent, 1, 100)` | No change needed — LinkedFanController builds its own 10-point safe curves starting at FixedTemps[0]=15°C. Point 0 of these curves will be forced to 0x00 by WriteCurve's `i == 0` path, which is correct (0% below 15°C). |

**UI behavior:** Point 0 is locked to 0% duty in the panel. User cannot drag
it above 0%. The curve visually starts at 0% and the first "real" duty is
point 1. This matches OEM behavior — fan is off below the first threshold.

**FanCurveModel.Validate():** Add rule: `Points[0].DutyPercent must == 0`.
This catches curves from profiles/EC that don't have point 0 at 0%.

**WriteCurve `i == 0` block** (unchanged from earlier in this plan):
```csharp
if (i == 0)
{
    _guard.Write(dutyBase + i, 0x00);  // EC requires duty[0] = 0
}
```
This is the backstop — even if the model somehow has a non-zero point 0,
the EC always gets 0x00.

### Fix 2: Skip tempDown[0] (write 94 bytes, not 96)

**Current:** Writes tempDown[0] = 0 (pinned by EnforceRange). OEM never writes this byte.

**Fix:** Skip the tempDown write when `i == 0`.

Combined with Fix 1, the active-point block for `i == 0` becomes:
```csharp
if (i < curve.ActiveEntries)
{
    var pt = curve.Points[i];
    _guard.Write(tempUpBase + i, (byte)pt.TempUp);
    if (i == 0)
    {
        // OEM format: tempDown[0] not written, duty[0] = 0x00
        _guard.Write(dutyBase + i, 0x00);
    }
    else
    {
        _guard.Write(tempDownBase + i, (byte)pt.TempDown);
        byte rawDuty = RegisterEncoding.EncodeFanDuty(pt.DutyPercent);
        if (rawDuty == 0) rawDuty = 2;
        _guard.Write(dutyBase + i, rawDuty);
    }
}
```

### Fix 3: Padding duty = last active duty (already correct)

Already uses last active entry's duty for padding. No change needed.
Padding tempUp/tempDown already 0xFF. First padding tempDown slot already
does `lastActive.TempDown + 4`. All correct.

### Fix 4: WriteCurve() self-brackets with 0x7C6

**Current:** No 0x7C6 management in WriteCurve(). Table written while latch
may be active (set earlier by SetCustomFanCurveMode).

**Fix:** Bracket the table write:
```csharp
private void WriteCurve(FanCurveModel curve, int tempUpBase, int tempDownBase, int dutyBase)
{
    // ... existing guards (format check, 0 entries check) ...

    // Deactivate latch while updating table
    _guard.Write(AddrFanCurveMode, 0x00);

    // ... write 94 bytes (fixes 1-3 above) ...

    // Table complete — activate latch
    _guard.Write(AddrFanCurveMode, 0x04);
}
```

This prevents the EC from reading a half-written table. Every caller
(FanTab Apply, LinkedFanController tick, profile load) gets correct
bracketing automatically.

---

## Changes to FanTab.cs

### Merge three Apply buttons into one

**Remove:**
- `_btnApplyCurveEnable` ("Apply" next to Custom Fan Curve checkbox)
- `_btnApplyCpu` ("Apply CPU Curve")
- `_btnApplyGpu` ("Apply GPU Curve")

**Add:** Single `_btnApply` ("Apply") below the curve panels.

**Click handler:**
```csharp
private void BtnApply_Click(object? sender, EventArgs e)
{
    try
    {
        // 1. Master enable — RMW 0x7C5 bit 7 per checkbox state
        _control.SetCustomFanCurveMode(_chkFanCurve.Checked);

        // 2. Write both fan tables (WriteCurve handles 0x7C6 bracket)
        if (_cpuCurvePanel.Curve != null)
        {
            var errors = _cpuCurvePanel.Curve.Validate();
            if (errors.Count > 0) { /* show validation error, return */ }
            _control.ApplyCpuCurve(_cpuCurvePanel.Curve);
        }
        if (_gpuCurvePanel.Curve != null)
            _control.ApplyGpuCurve(_gpuCurvePanel.Curve);
    }
    catch (Exception ex) { /* error dialog */ }
}
```

**Note on SetCustomFanCurveMode:** This method currently writes both
0x7C5 bit 7 AND 0x7C6=0x04. Since WriteCurve() now owns 0x7C6,
`SetCustomFanCurveMode` should be simplified to only RMW 0x7C5 bit 7.
The 0x7C6 write in SetCustomFanCurveMode becomes redundant — WriteCurve
will set it to 0x04 at the end.

However, `LinkedFanController.Enable()` calls `SetCustomFanCurveMode(true)`
before the first tick. If SetCustomFanCurveMode no longer writes 0x7C6,
that's fine — the first tick's WriteCurve will set 0x7C6=0x04.

Updated `SetCustomFanCurveMode`:
```csharp
public void SetCustomFanCurveMode(bool enabled)
{
    _guard.WriteBitfield(_addrFanCurveEnable, _fanCurveEnableMask,
        enabled ? _fanCurveEnableMask : (byte)0x00);
    // 0x7C6 is now managed exclusively by WriteCurve()
}
```

### Linked mode button disabling

Currently disables all three apply buttons when linked. With one button,
just disable `_btnApply` when linked mode is on:
```csharp
private void SetLinkedControlsEnabled(bool linked)
{
    _btnApply.Enabled = !linked;
    _btnRead.Enabled = !linked;
    _chkFanCurve.Enabled = !linked;
}
```

---

## Files to Modify

| File | Change |
|------|--------|
| `MechControl/Features/FanControl.cs` | `WriteCurve()` — fixes 1-4. `SetCustomFanCurveMode()` — remove 0x7C6 write. |
| `MechControl/Features/FanCurveModel.cs` | `Validate()` — add rule: `Points[0].DutyPercent must == 0`. |
| `MechControl/UI/FanCurvePanel.cs` | Drag clamp and push-left-down: allow 0% for point 0, keep 1% floor for all others. |
| `MechControl/UI/FanTab.cs` | Merge 3 apply buttons into 1. Update click handler. Update linked-mode disable. |

No other files need changes. `LinkedFanController`, `ProfileBar`, `EcActivationControl`,
`UniversalDefaults` all remain unchanged.

---

## What NOT to Change

- **EcActivationControl.cs** — 7-step init is sufficient, no additions needed.
- **FanCurveModel.cs** — `RangeCelsius = 4` and `EnforceRange()` are fine. The EC accepts any hysteresis >= 2°C as long as spacing > hysteresis. 4°C range with the existing validation (points must be `RangeCelsius` apart) guarantees no overlap.
- **LinkedFanController.cs** — No changes. Calls `SetCustomFanCurveMode` (now 0x7C5 only) on Enable, and `ApplyCpuCurve`/`ApplyGpuCurve` (WriteCurve handles 0x7C6) on each tick.
- **ProfileBar.cs** — No changes. Still calls `FanTab.LoadCurves()` then `SetCustomFanCurveMode()`.
- **UniversalDefaults.cs** — Write allowlist already includes 0xF00-0xF5F and 0x7C6.

---

## Verification

After implementing, test on both machines:
- **X6AR55xY** (your machine) — regression test, should still work
- **X6FR5xxY** (collaborator) — the machine that was broken, should now work

Quick smoke test: enable custom curves, apply a non-flat curve (e.g. 20% at 40°C ramping to 80% at 85°C), verify fans follow the curve as temp changes.
