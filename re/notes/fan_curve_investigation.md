# Fan Curve Investigation

**Last updated:** 2026-04-12

---

## TL;DR — Current State

**ROOT CAUSE: Fan table format.** On X6FR5xxY, the EC rejects custom
fan curves written with MechControl's current format (96 bytes, flat
duty, low temp thresholds). Switching to the OEM's exact table layout
(94 bytes, duty[0]=0x00, tempDown[0] skipped) produces an immediate
HIT — fans ramp toward target duty within seconds.

The init register hypothesis (12 missing registers) was a **red
herring**. Probe testing confirmed all 12 were already set from
previous sessions — Phase 2 writes were all no-ops. The table format
change alone fixed the problem.

**What still needs testing:** Whether the full OEM init sequence
(mode profile, edge pulse ritual, etc.) is actually required, or
whether MechControl's existing `EcActivationControl.InitializeEc()`
+ the corrected table format is sufficient. The probe included
everything; we need to strip it down to find the minimum.

**Next action:** Test with MechControl's own init sequence (no mode
profile, no edge pulse ritual) + the corrected fan table format.
If that works, the fix is purely in `FanControl.cs` table encoding.

---

## Problem Statement

Custom fan curves don't take effect on **X6FR5xxY** (collaborator's
machine, Ryzen 9 9955HX3D). MechControl works on **X6AR55xY** (my
machine, same IDY_7Y profile). Writes land in EC RAM (verified by
read-after-write) but the EC ignores the custom table and runs the
builtin curve.

---

## Root Cause: Fan Table Format

### What failed (all combos 1-13, old format)

MechControl's `FanControl.WriteCurve()` writes 96 bytes with:
- tempDown[0] written (slot 0 included)
- duty[0] = target duty (e.g., 80% flat across all slots)
- tempUp starting at 5°C with 5°C steps
- Padding duty = target duty

The EC on X6FR5xxY silently ignores this table. All control bits
(0x7C5=0x80, 0x7C6=0x04, 0x741=0x81) are set correctly, all 96
bytes verify on readback, but the fans follow the builtin curve.

### What works (combo 13a, OEM format)

Switching to the OEM's exact table layout produces an immediate ramp
toward target duty. CPU went from 34% → 59% in 15s and was still
climbing toward 80%. The key differences:

| Aspect | MechControl (MISS) | OEM format (HIT) |
|--------|-------------------|-------------------|
| Bytes written | 96 | **94** (tempDown[0] skipped) |
| duty[0] | Target (80%) | **0x00** (0% below first threshold) |
| tempUp range | 5-50°C | **48-85°C** (CPU), **46-81°C** (GPU) |
| tempUp step | 5°C | **4°C** |
| Padding duty | Target | **Last active duty** |
| tempDown[0] | Written | **Not written** |

### Likely culprits (need isolation testing)

1. **tempDown[0] skip** — Writing to tempDown[0] may corrupt the EC's
   table parser. The OEM writes 94 bytes, never touching 0xF10 or
   0xF40.
2. **duty[0] = 0x00** — The EC may validate that duty[0] is 0 (fan
   off below first threshold). A non-zero duty[0] may signal an
   invalid table.
3. Both together.

The temp ranges and step sizes are less likely to matter (they're
just data values, not structural), but haven't been ruled out.

---

## OEM Fan Table Layout (Byte-Exact Reference)

From Frida capture on X6FR5xxY. This is the format that works.

```
CPU TEMP_UP   (0xF00): 30 34 38 3C 40 44 48 4C 50 55  FF FF FF FF FF FF
CPU TEMP_DN   (0xF10): ?? 2E 32 36 3A 3E 42 46 4A 4E  52 FF FF FF FF FF
CPU DUTY      (0xF20): 00 A0 A0 A0 A0 A0 A0 A0 A0 A0  A0 A0 A0 A0 A0 A0

GPU TEMP_UP   (0xF30): 2E 32 36 3A 3E 42 46 4A 4E 51  FF FF FF FF FF FF
GPU TEMP_DN   (0xF40): ?? 2C 30 34 38 3C 40 44 48 4C  50 FF FF FF FF FF
GPU DUTY      (0xF50): 00 A0 A0 A0 A0 A0 A0 A0 A0 A0  A0 A0 A0 A0 A0 A0
```

- `??` = not written by OEM (tempDown[0] skipped)
- 10 active entries (indices 0-9), 6 padding (indices 10-15)
- Padding: tempUp/tempDown = 0xFF, duty = last active duty (0xA0)
- tempDown[i] = tempUp[i-1] - 2 (hysteresis pattern)
- tempDown[10] bridges into padding (value from tempUp[9] - N)
- duty[0] = 0x00 (0% below first threshold)
- Total: 94 bytes written (6 arrays × 16 entries − 2 skipped)

---

## Full OEM Init + Apply Sequence (Frida Capture Reference)

Captured from GCUService startup on X6FR5xxY after MechControl's
broken apply. This is the complete sequence that produces a HIT.
Registers marked **[SKU]** have per-model values from profiles.

### Phase 1 — EC Handshake + Activation

```
W 0x709 = 0x05              handshake step 1
W 0x708 = 0x01              handshake step 2
W 0x741 = 0x05              EC activation (transforms to 0x01)
W 0x748 = 0x81              feature enable (written 4x by OEM)
```

Already in MechControl's `EcActivationControl.InitializeEc()`.

### Phase 2 — Service Init Registers

```
R 0x7A6 -> W 0x7A6          battery mode RMW writeback
W 0x749 = 0x00              cmd clear 1
W 0x74A = 0x00              cmd clear 2
W 0x74B = 0x00              cmd clear 3
R 0x766 -> W 0x766          Support2 RMW writeback
W 0x767 = 0x00              trigger clear
W 0x74E = 0x00              cmd clear 4
```

NOT in MechControl. But probe testing showed these were already set
from previous sessions — all writes were no-ops. **May not be needed.**

### Phase 3 — Mode Profile (SKU values)

```
R 0x728 -> W 0x728          RMW preserve firmware bits
W 0x727 = [SKU]             ESHUTTER (X6FR5xxY=0x40, X6AR55xY=0xC0)
W 0x751 = 0x00              FAN_CTRL normal
W 0x726 = 0x80              OEM9 + power mgmt bit
W 0x783 = [SKU]             PL1 (X6FR5xxY=50, X6AR55xY=75)
W 0x7C6 = 0x00              clear apply latch
W 0x784 = [SKU]             PL2 (X6FR5xxY=50, X6AR55xY=85)
R 0x7C5 -> W 0x7C5 | 0x80   set master bit via RMW
W 0x785 = [SKU]             PL4 (X6FR5xxY=60, X6AR55xY=145)
W 0x786 = [SKU]             TCC_OFFSET (X6FR5xxY=0xDF)
W 0x744 = [SKU]             TGP (X6FR5xxY=40, X6AR55xY=55)
W 0x745 = 0x00              DB_TOTAL
W 0x746 = 0x00              DB_MAX
R 0x743 -> W 0x743          DB_CTRL RMW touch
W 0x787 = 0x00              FAN_SWITCH_SPEED
```

NOT in MechControl's fan curve path. MechControl's init does some of
these (0x726, 0x727 via power mgmt activation; 0x7C5 via fan curve
init). **May not be needed** — probe included it but the format fix
may be what actually mattered.

### Phase 4 — Fan Table (94 bytes, OEM format)

Write all 6 arrays (tempUp, tempDown, duty × CPU+GPU), skipping
tempDown[0] for both. See "OEM Fan Table Layout" section above.

### Phase 5 — Finalize + Edge Pulse

```
R 0x7C7 -> W 0x7C7          fan curve param RMW
W 0x706 = 0x41              BIOS_CTRL (written LATE, after table)
W 0x7C6 = 0x04              edge activate
  sleep 32ms
W 0x7C6 = 0x00              edge release
R 0x7C5 -> W 0x7C5          re-master RMW
```

MechControl's init writes 0x7C7 and 0x706 but BEFORE the fan table.
The OEM writes them AFTER. **Ordering may not matter** — need testing.

### Phase 6 — Second Pass (OEM double-write)

```
[94-byte table rewrite — identical to Phase 4]
W 0x7C6 = 0x04              edge activate (HELD, not released)
```

OEM writes the table twice with two edge pulses. Likely paranoia —
single pass may suffice.

---

## What MechControl Currently Does

### At startup (`EcActivationControl.InitializeEc()`, 7 steps)

```
1. W 0x709=0x05, W 0x708=0x01   handshake
2. W 0x741=0x05                  activation
3. W 0x748=0x81                  feature enable
4. R 0x726 |= 0x80, R 0x727 |= 0x40   power mgmt
5. R 0x7C5 |= 0x60              fan curve init (bits 5+6)
6. W 0x7C7=0x0C                 fan curve param
7. W 0x706=0x41                 BIOS_CTRL init complete
```

### When user enables custom fan curves

`FanControl.SetCustomFanCurveMode(true)`:
```
WriteBitfield(0x7C5, 0x80, 0x80)   set master bit 7
Write(0x7C6, 0x04)                  set apply latch
```

### When user applies a curve

`FanControl.ApplyCpuCurve()` / `ApplyGpuCurve()`:
- Writes **96 bytes** (all entries including tempDown[0])
- Uses user curve values directly
- **No edge pulse, no 0x7C7, no 0x706**

### When user loads a saved profile

`ProfileBar.ApplyProfile()`:
- Calls `FanTab.LoadCurves()` → writes table (96 bytes)
- Calls `SetCustomFanCurveMode(true)` → sets 0x7C5/0x7C6

---

## Next Steps — Minimum Fix Testing

### Test 1: MechControl init + OEM table format only

The simplest possible fix. Use MechControl's existing 7-step init
(already runs at startup) + corrected table format in WriteCurve():
- Skip tempDown[0]
- Set duty[0] = 0x00
- Use OEM padding (0xFF temps, last-active duty)

If this works, the fix is purely in `FanControl.WriteCurve()` —
no init sequence changes needed.

### Test 2: If Test 1 fails, add edge pulse

After writing the table, add:
```
W 0x7C6 = 0x04   edge activate
  sleep 32ms
W 0x7C6 = 0x00   edge release
W 0x7C5 RMW      re-master
W 0x7C6 = 0x04   held
```

### Test 3: If Test 2 fails, add mode profile

Add the Phase 3 mode profile writes before the table.

### Test 4: If Test 3 fails, add Phase 2 service init registers

Add 0x749/74A/74B/74E/766/767 writes.

Each test builds on the previous, isolating exactly what's needed.

---

## Key Files

### Code
- `MechControl/Features/FanControl.cs` — **where the table format fix goes**
- `MechControl/Features/EcActivationControl.cs` — EC init (7-step)
- `MechControl/UI/FanTab.cs` — UI event handlers for apply/enable
- `MechControl/UI/ProfileBar.cs` — profile load → apply path
- `MechControl/Profile/UniversalDefaults.cs` — write allowlist

### Probes
- `tools/fan_curve_probe13.cs` / `.exe` — **current test tool**
  - `13a`: OEM table + standard timing (HIT on X6FR5xxY)
  - `13b`: OEM table + 5s activation delay
  - `13c`: OEM table + no neutralize

### Captures
- `captures/frida_full_capture.log` — OEM on X6AR55xY (ground truth)
- `frida_full_capture_X6FR5xxY.log` — OEM mode-switch on X6FR5xxY
- `frida_startup_capture2_startup.log` — OEM startup re-init on X6FR5xxY

---

## Confirmed Facts

- Both machines use the **same EC IOCTL channel** (ACPIDriver). Zero
  HID/DDC/WMI during fan curve apply.
- Writes **land in EC RAM** — read-after-write verify confirms all
  bytes match on both machines.
- **No heartbeat needed** on X6FR5xxY. EC runs curve autonomously.
- **PROJECT_ID** on X6FR5xxY is 0x1A (26), mapped to IDY_7Y profile.
- Fan duty encoding: `raw / 2 = percent`. 0xA0 = 80%, 0x6E = 55%.
- **0x741 is EC-managed.** Writing 0x00 doesn't clear it. Write 0x05
  at init; EC transforms to 0x01. Bit 7 (0x80) is a persistent fan
  alert flag — do not try to clear it.

---

## Ruled Out

1. Init register hypothesis — all 12 were already set; format was the issue
2. Timing (5s activation delay) — same result as standard timing
3. 0x741 deep neutralize — EC ignores the write; breaks activation
4. Heartbeat / WHMS refresh — not needed on X6FR5xxY
5. Non-EC channels (HID, DDC, WMI) — zero during apply
6. Write delivery — all bytes verified on readback
7. 0x7C5 bit 5/6 — bit 5 auto-clears, bit 6 not needed for fan curves
8. 0x743 state machine — OEM just RMW-touches at 0x05
9. Burst 1 RESET before apply — not in startup capture
10. 0x706 edge transition (0x40→0x41) — no edge in startup capture
