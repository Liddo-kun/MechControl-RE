# User Profile System — Implementation Plan

## Read First
1. `CLAUDE.md` — project rules, build instructions, multi-model compatibility requirement
2. `architecture.md` — app architecture, component design
3. `register_reference.md` — EC register encodings (needed to understand save/load value encoding)
4. This file

## Overview

5 saveable profile slots. Save reads current state from EC. Load writes all settings to EC immediately. Last-used profile auto-loads on startup after EC init.

## New Files

### 1. `MechControl\Profile\UserProfile.cs`
Data class, JSON-serializable. Stores all user-adjustable settings as **decoded human-readable values** (watts, degrees C, percent), not raw EC bytes. Encoding/decoding happens at save/load time using existing `RegisterEncoding` methods.

```json
{
  "name": "My Gaming Profile",
  "projectId": "IDY_7Y",
  "savedAt": "2026-04-01T12:00:00",
  "cpu": {
    "pl1": 210, "pl2": 210, "pl4": 346, "tempLimit": 85
  },
  "gpu": {
    "tgp": 150, "dynamicBoostEnabled": true, "dynamicBoostWatts": 25
  },
  "fan": {
    "boostEnabled": false,
    "curveEnabled": true,
    "switchSpeedEnabled": true,
    "switchSpeedMs": 100,
    "cpuCurve": {
      "tempUp": [48, 52, 56, 60, 64, 68, 72, 76, 80, 85, 255, 255, 255, 255, 255, 255],
      "tempDown": [0, 46, 50, 54, 58, 62, 66, 70, 74, 78, 82, 255, 255, 255, 255, 255],
      "duty": [0, 64, 70, 80, 100, 110, 120, 140, 160, 180, 180, 180, 180, 180, 180, 180]
    },
    "gpuCurve": {
      "tempUp": [46, 50, 54, 58, 62, 66, 70, 74, 78, 81, 255, 255, 255, 255, 255, 255],
      "tempDown": [0, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 255, 255, 255, 255, 255],
      "duty": [0, 60, 70, 80, 102, 110, 120, 140, 160, 180, 180, 180, 180, 180, 180, 180]
    }
  },
  "battery": {
    "mode": 2
  },
  "lighting": {
    "logoEnabled": true,
    "logoR": 0, "logoG": 50, "logoB": 0,
    "logoBrightness": 100,
    "keyboardBrightness": 3
  }
}
```

Fan curve values are raw EC bytes (tempUp/tempDown are degrees C, duty is raw 0-200 where raw/2=percent). These come straight from `FanCurveModel` which already stores them this way.

### 2. `MechControl\Profile\UserProfileManager.cs`
Manages `%APPDATA%\MechControl\` directory.

**Responsibilities:**
- Create appdata dir on first run
- Load/save profile slots (1-5) as `userprofile_1.json` through `userprofile_5.json`
- Load/save `settings.json` (last-used slot number, profile names)
- Expose: `SaveProfile(int slot, UserProfile profile)`, `LoadProfile(int slot) -> UserProfile?`, `GetProfileNames() -> string[]`, `RenameProfile(int slot, string name)`, `LastUsedSlot` property

**settings.json format:**
```json
{
  "lastProfile": 2,
  "profileNames": ["Gaming", "Office", "Quiet", "Profile 4", "Profile 5"]
}
```

### 3. `MechControl\UI\ProfileBar.cs`
Horizontal `UserControl` strip, docked below menu bar.

**Layout:** 5 profile buttons + 1 Save button, left-to-right in a FlowLayoutPanel.

**Behavior:**
- Each button shows the profile name
- Empty (never saved) slots: grayed out text, click does nothing
- Click a non-empty slot: loads and applies profile to EC immediately, highlights as active
- "Save" button: reads all current values from EC, saves to the active slot
- Right-click a button: rename dialog
- Active slot button stays visually highlighted (bold or different background)

## Modified Files

### `MainForm.cs`
- Instantiate `UserProfileManager`
- Create `ProfileBar` and dock it below menu bar (above tabs)
- Pass all feature controllers to ProfileBar (or a helper class) so it can read/write
- After EC init: check `settings.json` for last-used slot, if exists load and apply it
- Wire up ProfileBar events

## Startup Flow (Complete)

1. Open EC device, load machine profile, create WriteGuard
2. **EC init** — full handshake + default fan table (already implemented in `EcActivationControl.InitializeEc()`)
3. Create all feature controllers
4. Create ProfileBar + UserProfileManager
5. Check `settings.json` → last-used slot number
6. If slot file exists → **load and apply profile to EC** (overwrites the default fan table from step 2)
7. Refresh all UI tabs from EC
8. Start monitor

The default fan table from EC init (step 2) acts as a safety net — it gives the EC valid data immediately. The user's saved profile (step 6) then overwrites it with their actual settings.

## Apply Order (on Profile Load)

Order matters to avoid the EC using half-written state:

1. Fan curve enable **OFF** (prevent EC from acting on partial fan table)
2. Write full CPU fan curve (48 registers)
3. Write full GPU fan curve (48 registers)
4. Fan curve enable **ON**
5. CPU power (PL1, PL2, PL4, temp limit)
6. GPU power (TGP, DB ctrl, DB watts)
7. Fan boost
8. Fan switch speed
9. Battery mode
10. Lighting (logo LED + keyboard brightness)

## Save Flow (on Save Button Click)

Read all current values from EC using existing methods:

1. `CpuPowerControl.ReadCpuPower()` → PL1, PL2, PL4, TempLimit
2. `GpuPowerControl.ReadGpuPower()` → TGP, DB enabled, DB watts
3. `FanControl.ReadFanBoost()` → bool
4. `FanControl.ReadFanCurveEnabled()` → bool
5. `FanControl.ReadCpuCurve()` → FanCurveModel
6. `FanControl.ReadGpuCurve()` → FanCurveModel
7. `FanSwitchSpeedControl.ReadState()` → (bool, int ms)
8. `BatteryControl.ReadBatteryMode()` → int
9. `LightingControl.ReadLogoState()` → (bool, R, G, B, brightness)
10. Keyboard brightness — **see Known Gap below**

Populate a `UserProfile` object and pass to `UserProfileManager.SaveProfile(slot, profile)`.

## Known Gap: Keyboard Brightness Read

Keyboard brightness is HID-based (ITE USB controller), not EC. There is no `ReadKeyboardBrightness()` method. The ITE controller may not support reading back the current level via `HidD_GetFeature`.

**Recommended approach:** Track the last-written keyboard brightness in memory. When the user changes it through the UI, store the value. On Save, use that stored value. On first launch with no saved profile, default to level 3 (mid-range). This avoids needing to reverse-engineer a HID read command.

## Multi-Model Compatibility

- `UserProfile` stores `projectId` for reference but we don't enforce cross-model checks (app is fresh installed per machine)
- All read/write goes through existing feature controllers which already use profile-driven register addresses
- Fan curve arrays are always 16 entries (padded) matching the profile's `entriesPerArray`
- No hardcoded register addresses in the profile system — it's all decoded values
