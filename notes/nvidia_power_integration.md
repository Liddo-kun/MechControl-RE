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
  (power ceiling = original max + 75 W; all three current maximums = 140% of stock default,
  never below the stock maximum) and
  restarts that GPU. It is not a ROM flash.
- **Before** the restart: the raised laptop budget (F7/platform) is returned, the
  original values at every patchable offset (power max, NVVDD/MSVDD and supported
  board-input default/max) are saved to `originals.txt` with the BIOS version,
  then the registry is written. Missing original offsets from older builds are
  appended atomically before any new overrides, preserving all existing originals.
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
- Currents (2026-10-29 revision): while unlocked the registry holds each current maximum at
  the top of the unlocked slider range (140% of stock default) and its default at the chosen
  current, so any slider value applies live and the GPU starts at the chosen current.
  Only toggling the unlock restarts the GPU. Before, default and maximum were both the
  chosen value: lowering applied live, but raising above the last restart's value
  restarted the GPU again (Jon's report). A live maximum staged by such a build still
  needs one restart when exceeded; any apply restages the full range for the next start.
  Stock BIOS here has default = maximum (210/210, 60/60 A). Unlocked range was 2x stock
  default until 2026-10-29, then 130%; the board-input addition uses 140% for all three.
  Live 2026-10-29 10:43 (`C:\Temp\tandem\rail-ceiling-test\run1.txt`, app stopped): old live
  maximums 241/73 A; 250/75 A restarted the GPU once, which accepted default 250 < maximum
  273 A (image verified); then 273/78 -> 230/65 -> 273/78 -> 210/60 A all live, no restart.
  Registry afterwards: power 250 W, maximums 273/78 A, stock defaults. App restart: profile
  slot 2 applied 210/60 A live, F8/F7/enforced 175 W.
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
  F8 175 W) the GPU was held at 150 W (games and an OpenCL load), with the unlock off
  too and with the app stopped. Cause: the Dynamic Boost driver (PnP device
  `ACPI\NVDA0820\NPCF`, "NVIDIA Platform Controllers and Framework") had stopped adding
  DB. Broken: F7 150 W, platform 345 W, enforced 150 W; EC changes (DB off/5/25) did not
  revive it. `pnputil /restart-device "ACPI\NVDA0820\NPCF"` (app stopped) revived it at
  once: F7 175 W, platform 350 W, enforced 175 W, ~170 W drawn. With DB working, none of
  these broke it again (each followed by a 10 s load at enforced 175 W): NVAPI/platform
  reads, GPU restart, same-value F7 and TPP writes, F7/platform raise to 185 W and
  restore, EC failsafe minimum and back, F8 175 W, app restart, charger unplug/replug,
  sleep/resume, reboot. With DB working, F7 = 95 + EC 788 (cTGP) + EC 7D5 (DB): boot
  defaults 788=50, 7D5=5 give 150 W. Writing F7 = 150 caps enforced at 150 W only until
  the next EC change republishes (then 175 W). Timeline evidence: it was already broken
  at 8:45 (charger test while F7 was raised to 185 W by the app: replug gave 124 W with
  EC 160+25, which should be 185 W) and at 9:4x (replug gave 150 W, same EC). Also not
  reproduced with the current build: app at 185 W, unplug (re-raise on battery), replug,
  back to 150+25; F7/platform raise to 250 W and restore; GPU restart while raised (F7
  republished 175 W after the restart). Trigger unknown; possibly a pre-simplification
  build (budget journal/rollback, blocking re-raise loop) or a morning event not repeated.
  Manual recovery: `pnputil /restart-device "ACPI\NVDA0820\NPCF"` (GPU keeps running).
  Draw figures in these runs may include a game sharing the GPU; enforced/F7 are the
  measure.
- Log check (added after the above): while the NVIDIA driver is open, the delayed check
  runs after every apply (not only above stock), after a power-source change and after
  resume. On AC it warns in `mechcontrol.log` when F7 is below EC TGP + DB ("Dynamic
  Boost may be stuck"). Log only; no automatic device restart. Stock users who never
  opened NVIDIA get no check.
- Full power cycle (boot type 0x0, deployed log-check build, app not started at boot):
  EC boot defaults 743=02, 788=50, 7D5=5 -> F7 150 W, enforced 150 W, game ~147.5 W.
  App start (profile 150+25) -> EC 788=55, 7D5=25, F7 175 W, enforced 175 W within ~2 s,
  game ~170 W; no stuck warning. Boot does not break Dynamic Boost.
  Probe ops `loadsmi` (no NVAPI), `tppsame`, `f7same`, `raise`; `C:\Temp\tandem\boot-check.ps1`.

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

- Profiles store TGP, Dynamic Boost, exact NVVDD/MSVDD/board-input requests (mA) and
  `biosOverrideEnabled` = the values need the unlock. Legacy keys (`nvidiaAdapter`)
  are ignored.
- Loading never removes the unlock. Automatic loads (startup, EC re-activation,
  failsafe reset) never restart the GPU: values needing the unlock or a higher rail
  ceiling are limited to the active range, everything else in the profile still
  applies, and a notice is logged and shown on the Power tab. Manual loads ask to
  turn the unlock on or restart the GPU.
- A profile that doesn't need the unlock behaves like stock while unlocked: power is
  capped by its own TGP + DB, and currents it didn't save (legacy/stock-saved
  profiles) are set to the stock defaults, live. The rail maximums stay at the unlocked
  range, so a later unlocked profile applies its currents live. Power-tab Apply is not a
  profile and keeps unmoved currents.
- A board-input value on a GPU without board input is skipped with a notice (manual
  and automatic loads); the rest of the profile still applies.
- TGP values beyond even the unlocked range are refused as before.

## UI

- NVVDD/MSVDD sliders move in whole amps. Board input positions are exact milliamps (a whole
  amp would be ~10%, ~20 W, of its ~11 A; no round step divides 11053 mA), arrow keys 0.1 A;
  a mouse drag moves ~30 mA per pixel. None goes below 1 A. Positions are rounded down, so
  the detected value never lands above the slider maximum; an unmoved slider keeps that exact
  value (e.g. 187.5 A at position 187) and writes nothing. Unlocked range is up to 140% of
  the stock default (UI policy, not a hardware rating).
- Board input is optional: BIT P slot 9 must be type 3 / flags 1, and the driver
  must expose exactly one HW_THRESHOLD mA policy on a SENSOR_CLIENT_ALIGNED monitor
  whose min/default/max equal the BIOS field. Channel numbers are discovered. Anything
  else (no slot 9, no or duplicate driver policy, mismatched values, e.g. a driver that
  exposes it differently) disables only this slider, logged once per open/restart; TGP
  and NVVDD/MSVDD keep working, and no slot-9 record is staged. If the driver doesn't
  take a staged board-input override after a GPU restart, the same applies and the
  rails' requested values are still set. A recognized slot 9
  with a bad range or channel still rejects the BIOS, like the other fields.
  Its slider uses the same unlock/restart/live-current path as NVVDD and MSVDD;
  checking the unlock expands its maximum without forcing its chosen current up.
  The status line lists the stock board input after the rails when available.
- Live readings (2026-10-30), right of each current limit: "Measured" (the policy's STATUS
  valueCurr) and "At clock cap" (the current controller's prediction for NVVDD/MSVDD; board input
  is a hardware threshold with no prediction), then a line with the current and power controllers'
  GPU clock caps and their ceiling. Read every 0.5 s only while the Power tab is shown (minimizing
  hides the window), only when the driver is already open and Windows reports the GPU in D0
  (same gate as System Info). The driver publishes new values every 20 ms, so no alignment is
  needed. Reads never wait for a GPU restart; ~8 ms each on the UI thread.
  - How the driver limits (measured 2026-10-30, driver 617.14): STATUS type-0x15 policies are
    controllers that each set a clock cap where their prediction at that cap reaches their limit;
    the lowest cap applies. The current controller (index 10 here) observes NVVDD + MSVDD and
    predicts each rail's current at its cap; the power controller (index 0 here) takes its limit
    from the board power policy (PMU limit arbitration input) and predicts rail power. With a
    MSVDD 60 A limit in a game: current-controller cap 2820-2960 MHz below the power controller's
    2917-3022 MHz in 150/150 samples, predicted MSVDD 98.6-100% of 60 A, predicted NVVDD 62-78%,
    measured MSVDD only 45-51 A (the GPU ran 150-200 MHz below the cap; unexplained). Measured
    versus limit therefore does not show a current limit; the prediction and the caps do.
  - The controllers' union layout (offsets in `NvidiaApi.ReadTelemetry`) was mapped from live
    dumps; the public header's layout differs. Every read checks it: totals equal the two rails,
    the cap is within its ceiling, the current controller's observed rails equal the NVVDD/MSVDD
    readings, exactly one candidate each. Anything else shows "--" for that value only.
    3199/3199 back-to-back reads passed. Evidence: `C:\Temp\tandem\limit-probe\`.
- System Info, Power Delivery: "EC" and "NVIDIA Driver" headings; the driver part shows
  the laptop GPU budget (F7) and the CPU+GPU platform budget (TPP; EC 0x7F7 reads 0 here,
  so it is not readable from the EC). Never opens NVIDIA (rows show "NVIDIA not
  connected" until the unlock or saved state opened it), and never reads a GPU Windows
  reports asleep (`DEVPKEY_Device_PowerData` most recent state != D0; "GPU asleep, not
  read"). Reads once when the tab is shown, then every 5 s while it is visible and the
  window isn't minimized. A visible tab can still keep an awake but idle Optimus dGPU from
  sleeping. The asleep path was not observed live (this GPU stayed in D0).

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
- Board input revision (2026-10-29): NvidiaPower.Tests 20/20, GpuFailsafe.Tests 45/45,
  ThermalUi headless, packaged headless smoke, release build 0 errors / 2 known
  warnings. Read-only production-API check: BIOS and driver board input 1/11053/11053 mA,
  rail maximums 273/78 A (`C:\Temp\tandem\board-current-readonly\result-fixed.txt`).
  Live compact SET 11053 -> 11000 -> 11053 mA with an earlier variant of this change
  (same SET path; CONTROL readback and enforced limit followed, rails unchanged;
  `C:\Temp\tandem\bic-live\run1.txt`). That variant required board input for the whole
  unlock; it was deployed 19:02 and reverted 19:06 (`C:\Temp\tandem\bic-revert\revert.ps1`).
  This build deployed 19:34 (backup `C:\tmp\MechControl-backup-20261029-193405`; prior
  registry values and 5-offset `originals.txt` in `C:\Temp\tandem\board-input-deploy\`):
  EC init OK, validations PASS, profile slot 2 auto-loaded as 210/60/11.053 A without a
  GPU restart; `originals.txt` gained 360188/360192 = 11053 (saved values unchanged) and
  the registry now holds power 250 W, board input max 15474 mA, NVVDD/MSVDD max 294/84 A
  (defaults stock). Live maximums stay 273/78/11.053 A until the next GPU restart, so the
  first raise above them asks for one. **Not tested live**: that restart, a raised board
  input limit under load, driver 621+.
  0.1 A board input steps: ThermalUi (12.3/15.1 A applied, locked clamp writes nothing),
  release build, packaged smoke; deployed 19:39 (backup
  `C:\tmp\MechControl-backup-20261029-193953`), startup as above, overrides unchanged.
  Replaced by milliamp positions with rounded-down slider positions (no stored "unmoved"
  position): ThermalUi (exact 12.3/15.123 A, a 60.6 A rail at its locked ceiling
  writes nothing; a round-to-nearest mutant fails), GpuFailsafe 45/45, NvidiaPower 20/20,
  release build, packaged smoke. Deployed 19:52 (backup
  `C:\tmp\MechControl-backup-20261029-195254`), startup as above, overrides unchanged.
- Live readings revision (2026-10-30): NvidiaPower.Tests 22/22 (controllers at alternate policy
  indices; broken totals/cap/identity, ambiguity, missing board power source and mismatched board
  input each blank only their value; mismatched rail status refuses), GpuFailsafe.Tests 45/45,
  ThermalUi headless (columns, values, unopened driver, sleeping GPU not read, timer off while
  hidden), packaged headless smoke, release build 0 errors / 2 known warnings. Production
  `NvidiaApi.ReadTelemetry` read-only against the live GPU (`C:\Temp\tandem\telemetry-live\`):
  20 reads, all values present, 5.8-10.8 ms each. Deployed 2026-10-30 09:20 (backup
  `C:\tmp\MechControl-backup-20261030-092011`): EC init OK, validations PASS, NVIDIA open, profile
  slot 2 auto-loaded (150+25 W, 210/60/11.053 A) without a GPU restart.
