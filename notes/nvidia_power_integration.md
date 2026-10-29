# NVIDIA power/current integration

2026-10-28. Implementation follows `nvidia_power_override_research.md`. Revised the
same day after review (lazy open, profile loading, F7 re-raise, live rail lowering).

## When NVIDIA is touched

- Startup checks for an NVIDIA display adapter through PnP only (cannot wake a
  sleeping dGPU). NVAPI/NVML are opened at startup **only** when
  `C:\ProgramData\MechControl\NvidiaOverrides\originals.txt` exists (overrides are or
  were staged), so a previous session can be resumed or cleaned up. Otherwise the
  driver is opened on first use: the unlock checkbox or a manual load of a profile
  that needs the unlock.
- Models without an NVIDIA GPU show nothing about the unlock on the Power tab.
- If opening fails (old driver, unsupported BIOS layout, external overrides,
  staged-but-not-active state), the unlock is withdrawn for the session and the
  Power tab shows the reason. EC controls are unaffected.
- The driver power request (F8) is written only while the unlock is on. Stock users
  keep the HEAD behavior: EC registers only.

## Unlock, GPU restart and when it must be redone

- Turning the unlock on stages software BIOS overrides in the GPU's registry key
  (power ceiling = original max + 75 W; rail ceilings only when above stock) and
  restarts that GPU. It is not a ROM flash.
- **Before** the restart: the raised laptop budget (F7/platform) is returned, the
  original values at every patchable offset (power max, both rails' default/max) are
  saved once to `originals.txt` with the BIOS version, the registry is written.
- Ownership: a `romOverride*` record is ours when it patches the saved BIOS version at
  a saved offset. Any other record (or a `vbios` value) is external and refused. The
  original BIOS is the active image with the saved values written back.
- **After** the restart: wait for NVAPI and 5 s for Dynamic Boost to publish its
  stock F7, verify the GPU consumed exactly the expected image, then re-apply the
  current EC values through `ApplyGpuPower`, so a failsafe hold that started
  meanwhile still wins. The restart runs on a worker thread behind a modal notice;
  the UI thread (monitor + failsafe) keeps running. NVIDIA calls made during the
  restart fail fast ("GPU is restarting") instead of hanging.
- Overrides persist across reboots and sleep; nothing needs redoing. They must be
  redone only if the driver drops them (driver reinstall/clean install) or the
  VBIOS is replaced. Both are detected at open (BIOS version differs, or no records
  left and the original values active): owned records that no longer apply are
  removed with `originals.txt` and logged, and the unlock shows as off.
- The registry always holds exactly the chosen ceilings. Raising a rail above its
  live maximum needs a restart; lowering applies live and the lower ceiling is
  staged for the next GPU start, without a restart.
- Turning the unlock off restarts the GPU with no overrides and deletes `originals.txt`.
  Firmware Defaults does the same after the EC restore (best effort, separate error).

## Laptop budget (F7) and EC interaction (measured)

- Any change to the EC TGP / Dynamic Boost registers makes firmware republish its
  own F7 about 1-2 s later; identical rewrites do not. The value varies (observed
  175, 95 and 120 W in different runs), but it replaces our raised F7, so without a
  fix an unlocked 185 W request was silently capped lower.
- Firmware also resets F7 on charger plug/unplug (measured 185 -> 99 -> 60 W on
  unplug, 124 W on replug; platform and F8 unchanged) and both budgets at boot
  (cold boot: F7 150 W, platform 345 W; an earlier boot gave 99/299 W).
- Raises therefore: raise F7/platform and set F8 without waiting, then check about
  3 s later (5 s after a power-source change, 8 s after resume). The check reapplies
  the last request through `ApplyGpuPower` (so a thermal hold still wins) when F8
  differs or F7 is below a request above stock. Battery included, no AC check.
  Identical EC rewrites don't trigger a reset, so a reapplied raise holds.
- The original F7/platform values and the last applied ones are kept in memory only.
  New raises derive headroom from the original, so it never accumulates and can also
  come down. If firmware or another tool moved the platform budget, the current
  values become the new baseline. Restoring returns only fields that still hold our
  value and never blocks lowering or unlock removal. After an app restart without a
  reboot while raised, the raised values become the baseline until the next boot
  (accepted: F8 and the CPU PLs still cap the actual draw).

- **Open issue (Jon, 2026-10-29):** at 150 W + 25 W Dynamic Boost (a stock request,
  F8 175 W) the GPU is now held about 20-25 W lower than before the unlock work. Probe:
  F7 150 W (firmware value since boot), platform 345 W, enforced 150 W. The app only
  raises F7 for requests above stock. Not yet investigated: compare with the unlock
  off / the app stopped, and with HEAD (EC only).

## Thermal failsafe

- While unlocked, every apply (including holds) also sets F8 to the EC request, so
  the raised driver request never sits above the hold. Lowering sets F8 first and
  never depends on the budget interfaces.
- Hold maintenance checks the EC registers and, when unlocked, that F8 and the
  enforced limit are at or below the minimum.
- EC minimum alone (F8 left at 185 W), re-measured 2026-10-29: firmware dropped F7
  to 95 W and NVIDIA enforced 95 W within 2 s. An earlier unsaved run reported
  150 W; it could not be reproduced and should not be relied on.

## TGP below the EC minimum (unlocked only)

- The EC TGP register is offset-encoded from the model minimum (its base TGP), so it
  cannot go lower. While unlocked, the TGP range starts at 60% of that minimum
  (`TgpUnlockMinimumPercent`, rounded up; never below the driver's own floor):
  95 -> 57 W here. Below the minimum the EC stays at its minimum (Dynamic Boost as
  chosen) and the driver request (F8) is TGP + DB. Readback reports that request
  when the EC sits at its minimum, so the slider and saved profiles show it.
- The thermal hold uses the lower of the EC minimum and the current request; it never
  raises a lower request.
- Locked: unchanged (EC range only). An automatic load of a sub-minimum profile while
  locked is raised to the minimum with a notice; a manual load offers the unlock.
- Resume: 8 s after wake the same request check runs (see the laptop budget section).
- Measured 2026-10-29 (app stopped, OpenCL load): F8 60 W -> enforced 60 W, draw held
  at 60.0 W (SW power cap) at ~1.9 GHz; F8 40 W -> 40.0 W; raising F8 again let the same
  load jump to 124 W within 1 s. EC changes to its minimum and back republished F7
  (99/124 W) but F8 60 W stayed enforced. Two ~5 min Modern Standby periods kept F8
  60 W; the dGPU (driving the panel) was active 85% of them, so a full GPU power-off
  was not proven. Draw figures are GPU telemetry; this GPU is shunt-modded.

## Profiles

- Profiles store TGP, Dynamic Boost, exact rail requests (mA) and
  `biosOverrideEnabled` = the values need the unlock. Legacy keys (`nvidiaAdapter`)
  are ignored.
- Loading never removes the unlock. Automatic loads (startup, EC re-activation,
  failsafe reset) never restart the GPU: values needing the unlock or a higher rail
  ceiling are limited to the active range, everything else in the profile still
  applies, and a notice is logged and shown on the Power tab. Manual loads ask to
  turn the unlock on or restart the GPU.
- A profile that doesn't need the unlock behaves like stock while unlocked: power is
  capped by its own TGP + DB, and currents it didn't save (legacy/stock-saved
  profiles) are set to the stock defaults, live. The lower rail ceilings are staged,
  so after a reboot an unlocked profile's higher currents need a manual load (GPU
  restart) again. Power-tab Apply is not a profile and keeps unmoved currents.
- TGP values beyond even the unlocked range are refused as before.

## UI

- Rail sliders move in whole amps. An unmoved slider keeps the exact detected value
  (e.g. 70.313 A) and writes nothing. Unlocked range is up to twice the stock
  default (UI policy, not a hardware rating).

## Compatibility and recovery

No mVolt executable or new kernel driver is needed. NVAPI and NVML are loaded from
the installed NVIDIA driver. Versioned structures, PCI identity and BIOS policies
are checked; unsupported layouts/active BIOS-profile copies disable these controls
while ordinary EC controls remain. The parser discovers the BIT P policy layout and
its location through ROM image translation; no wattages or offsets are specific to
Jon's laptop. Laptops only: one NVIDIA dGPU is assumed.

Preserve `C:\ProgramData\MechControl\NvidiaOverrides\` when moving or uninstalling
an installation with active overrides. External overrides (mVolt etc.) are not
adopted: remove them with their owning tool and restart the GPU first.

Private interface layout evidence: mVolt 0.50.8 decompilation and
`nohuto/nvapi-cli/include/nvapi.h`. No NVIDIA module RVAs are used. The F7 transport
is discovered from matching GET/SET wrapper signatures; unrecognized driver code
disables the feature.

## Verification (revision)

- GpuFailsafe.Tests 41/41 (adds: no F8 while locked, platformChanged flag, automatic
  vs manual profile planning, lazy open, open failure), NvidiaPower.Tests 15/15
  (budget can come down, never below baseline), ThermalUi tests (whole-amp sliders,
  unmoved slider writes nothing, unopened driver), packaged trimmed smoke passed.
- Release build: 0 errors, the 2 pre-existing CS8602 warnings.
- Live, new code via `C:\Temp\tandem\nvidia-integration-smoke\` (deployed app
  stopped ~30 s): EC change + 185 W raise re-raised F7 after the firmware reset and
  held 185 W; identical rewrite needed no re-raise; NVVDD 220 -> 215 -> 220 A live
  without restart; lowering to the failsafe minimum gave F8/enforced 95 W, platform
  365 -> 355 W and deleted the budget journal.
- Not live-tested in this revision: unlock on/off GPU restart (unchanged sequence,
  tested in the first round), driver reinstall / VBIOS replacement cleanup.
- Deployed 2026-10-29 07:18.
- Sub-minimum TGP revision: GpuFailsafe.Tests 41/41 (unlocked floor, EC stays at its
  minimum, readback, locked clamp/refusal, hold never raises, resume reassert),
  ThermalUi (slider floor, sub-minimum apply/readback/capture), NvidiaPower 15/15,
  packaged smoke, release build 0 errors / 2 known warnings. Deployed 2026-10-29 08:25.
  Live via the UI: Apply 60 W -> EC 95 W/DB off, F8 60, enforced 60, platform budget
  restored 360 -> 299 W. Saved to a slot (tgp 60, biosOverrideEnabled true); loading
  profile 2 (185 W, F7 re-raised) and back restored 60 W, no GPU restart.
- Simplification revision (2026-10-29): recovery file is `originals.txt` (version +
  5 offset/original pairs) instead of a binary file with two 1 MB BIOS images; no
  `.lock` file (the app's single-instance mutex covers it); budget journal removed;
  blocking raise loop replaced by the delayed check; one SetupAPI lookup; NvidiaApi
  locks removed (NvidiaPowerControl serializes). NvidiaPower.Tests 14/14 (ownership
  by version/offset, original rebuilt from a patched image), GpuFailsafe.Tests 41/41
  (delayed check scheduled only above stock, reapply on F7 reset), ThermalUi, packaged
  smoke, release build 0 errors / 2 known warnings. Old `.nvo` converted once with
  `C:\Temp\tandem\nvo-migrate` (dry run on the live GPU: original rebuilt byte-equal
  to the old file's ROM, existing record owned). Deployed 2026-10-29 09:40.
  Live (`C:\Temp\tandem\live-simplify-watch.txt`): Apply 160+25 W -> F7/enforced 185 W
  within 1 s, no firmware reset this time; unplug -> F7 45/60 W, EC zeroed 0x788/0x7D5,
  enforced 30-35 W; check re-raised F7 to 185 W but enforced stayed 35 W (battery
  limits elsewhere); replug -> F7 150 W, check re-raised to 185 W ~6 s later, enforced
  185 W; Apply 150+25 W -> F8 175 W, F7/platform back to 150/345 W. Unlock off -> GPU
  restart, no romOverride values, originals.txt deleted, max 175 W, no F8; unlock on
  -> GPU restart, romOverride00 = 250 W, originals.txt recreated identical, F8 175 W.
  The "Requested ... enforced" log line right after a raise shows the enforced value
  before it settles (logged 150 W, measured 185 W a second later).
