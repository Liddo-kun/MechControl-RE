# NVIDIA power/current integration

How the app uses the NVIDIA driver for TGP above the EC range and for the NVVDD, MSVDD and
board input current limits. Design background: `nvidia_power_override_research.md`. How
the limits act on the clock: `nvidia_gpu_limiters.md`.

## When NVIDIA is touched

- Startup checks for an NVIDIA adapter through PnP only (can't wake a sleeping dGPU).
  NVAPI/NVML open at startup **only** when
  `C:\ProgramData\MechControl\NvidiaOverrides\originals.txt` exists (overrides are or were
  staged), so a previous session can be resumed or cleaned up. Otherwise the driver opens
  on first use: the unlock checkbox or a manual load of a profile that needs the unlock.
- Models without an NVIDIA GPU show nothing about the unlock.
- If opening fails (old driver, unsupported BIOS layout, external overrides,
  staged-but-not-active state), the unlock is withdrawn for the session with the reason on
  the Power tab. EC controls are unaffected.
- The driver power request (F8) is written only while unlocked. Stock users get EC
  registers only.

## Unlock and GPU restart

- Turning the unlock on stages software BIOS overrides in the GPU's registry key and
  restarts that GPU (not a ROM flash): power ceiling = original max + 75 W; each current
  maximum = 140% of its stock default (never below the stock maximum), with the default at
  the chosen current. So any slider value applies live and the GPU starts at the chosen
  current; only toggling the unlock restarts the GPU. A maximum staged by an older build
  needs one restart when exceeded.
- **Before** the restart: the raised laptop budget (F7/platform) is returned, the original
  values at every patchable offset are saved to `originals.txt` with the BIOS version
  (missing offsets from older builds appended atomically), then the registry is written.
- Ownership: a `romOverride*` record is ours when it patches the saved BIOS version at a
  saved offset. Anything else (or a `vbios` value) is external and refused.
- **After** the restart: wait for NVAPI and 5 s for Dynamic Boost to publish its stock F7,
  verify the GPU consumed exactly the expected image, then re-apply the EC values through
  `ApplyGpuPower` so a failsafe hold that started meanwhile still wins. The restart runs on
  a worker thread behind a modal notice; NVIDIA calls during it fail fast.
- Overrides persist across reboots and sleep. They are lost only on driver reinstall or
  VBIOS replacement; both are detected at open, owned records are removed with
  `originals.txt` and the unlock shows as off.
- Turning the unlock off restarts the GPU without overrides and deletes `originals.txt`.
  Firmware Defaults does the same after the EC restore.

## Laptop budget (F7) and the EC (measured)

- Any change to the EC TGP / Dynamic Boost registers makes firmware republish its own F7
  1-2 s later (identical rewrites don't), replacing our raised F7. Firmware also resets F7
  on charger plug/unplug and both budgets at boot.
- So raises set F7/platform and F8 at once, then check ~3 s later (5 s after a power-source
  change, 8 s after resume) and reapply through `ApplyGpuPower` (a thermal hold still wins)
  when F8 differs or F7 is below a request above stock.
- Original F7/platform values are kept in memory only; headroom derives from them, so it
  never accumulates. Restoring returns only fields that still hold our value and never
  blocks lowering or unlock removal.
- With Dynamic Boost working, F7 = 95 + EC 0x788 (cTGP) + EC 0x7D5 (DB).
- **Stuck Dynamic Boost** (seen once, 2026-10-29, trigger unknown): F7 stayed at 150 W
  regardless of EC changes, with the app stopped too. `pnputil /restart-device
  "ACPI\NVDA0820\NPCF"` fixed it at once. Not reproduced by GPU restarts, budget writes,
  failsafe, charger, sleep, reboot or cold boot. While NVIDIA is open, the delayed check
  runs after every apply, power-source change and resume, and on AC logs "Dynamic Boost
  may be stuck" when F7 is below EC TGP + DB. Log only.

## Thermal failsafe

- While unlocked, every apply (including holds) also sets F8 to the EC request, so the
  driver request never sits above a hold. Lowering sets F8 first and never depends on the
  budget interfaces.
- Hold maintenance checks the EC registers and, when unlocked, that F8 and the enforced
  limit are at or below the minimum.
- EC minimum alone (F8 left high): firmware dropped F7 to 95 W and NVIDIA enforced 95 W
  within 2 s.

## TGP below the EC minimum (unlocked only)

- The EC TGP register is offset-encoded from the model minimum, so it can't go lower. While
  unlocked the TGP range starts at 60% of that minimum (`TgpUnlockMinimumPercent`; never
  below the driver's floor): 95 → 57 W here. Below the minimum the EC stays at its minimum
  and F8 = TGP + DB; readback reports that request.
- The thermal hold uses the lower of the EC minimum and the current request.
- Locked: EC range only. An automatic load of a sub-minimum profile is raised to the minimum
  with a notice; a manual load offers the unlock.
- Measured: F8 60 W held a load at 60.0 W; F8 survived EC changes and Modern Standby.

## Profiles

- Profiles store TGP, Dynamic Boost, exact NVVDD/MSVDD/board-input requests (mA) and
  `biosOverrideEnabled` (the values need the unlock).
- Loading never removes the unlock. Automatic loads (startup, EC re-activation, failsafe
  reset) never restart the GPU: values needing the unlock or a higher ceiling are limited
  to the active range, the rest applies, and a notice is shown. Manual loads ask.
- A profile that doesn't need the unlock behaves like stock while unlocked; currents it
  didn't save are set to stock defaults live. Power-tab Apply keeps unmoved currents.
- A board-input value on a GPU without board input is skipped with a notice.

## UI

- NVVDD/MSVDD sliders move in whole amps; board input in exact milliamps (arrow keys 0.1 A).
  None goes below 1 A. Positions round down, and an unmoved slider keeps the exact detected
  value and writes nothing.
- Board input is optional: BIT P slot 9 must be type 3 / flags 1, and the driver must expose
  exactly one HW_THRESHOLD mA policy on a SENSOR_CLIENT_ALIGNED monitor whose
  min/default/max equal the BIOS field. Anything else disables only that slider (logged);
  a recognized slot 9 with a bad range or channel still rejects the BIOS.
- **GPU Limits (live)**, below Apply: one row per limit (GPU clock, GPU power with core rails
  and memory/other, NVVDD, MSVDD, board input) showing setting, reading, reading as a share
  of the setting, and the controller's clock cap with its gap above the clock (one cap cell
  spanning both rail rows; board input shows IPC activity there). Rows are colored green →
  amber → red by reading/limit (power 94-99%, NVVDD 89-99%, MSVDD 90-99%, board input 68-81%),
  the IPC cell by activity (52-80%). No "is limiting" verdicts: the limits blend. A note asks
  shunt-modders to keep the three currents solid green 99% of the time.
  - Reads: each policy's STATUS value and limit, the board power policy's value (GPU power),
    the power controller's observed total (core rails), the current/power controller caps
    and ceiling, NVML clock and enforced limit, and ADC_IPC count deltas
    (`NvidiaInlinePower.cs`). ~6-8 ms per read on the UI thread; never waits for a GPU restart.
  - Each read is checked: controller totals equal their two rails, caps within the ceiling,
    the current controller's rails equal the NVVDD/MSVDD readings, exactly one candidate each.
    A failed check shows "--" for that value only. The controller union layout was mapped on
    617.14 (the public header's differs).
  - Gating: read every 0.5 s only while the Power tab is visible (another tab or the tray
    hides it), only when the driver is already open and Windows reports the GPU in D0. The
    first read after showing is posted, so the startup show-then-hide reads nothing. IPC
    monitors are looked up on the first read. `mcctl status` reads the same on demand.
- System Info, Power Delivery: the laptop GPU budget (F7) and the platform budget (TPP; EC
  0x7F7 reads 0 here). Same open/D0 gate; read on show, then every 5 s while visible.

## Compatibility and recovery

No mVolt executable or new kernel driver is needed. NVAPI and NVML load from the installed
driver. Versioned structures, PCI identity and BIOS policies are checked; unsupported layouts
disable these controls while EC controls remain. The BIT P parser discovers its layout and
location through ROM image translation; no wattages or offsets are specific to Jon's laptop.
Laptops only; one NVIDIA dGPU is assumed.

Preserve `C:\ProgramData\MechControl\NvidiaOverrides\` when moving or uninstalling an
installation with active overrides. External overrides (mVolt etc.) are not adopted: remove
them with their tool and restart the GPU first.

Layout evidence: mVolt 0.50.8 decompilation and `nohuto/nvapi-cli/include/nvapi.h`. No NVIDIA
module RVAs are used; the F7 transport is discovered from GET/SET wrapper signatures.

## Verification

- Tests: NvidiaPower.Tests (BIOS parsing/ownership, current policies, live readings at
  alternate indices, IPC deltas), GpuFailsafe.Tests (F8 holds, planning, lazy open),
  ThermalUi.Tests (sliders, live limits view, unopened driver, sleeping GPU, timer gating).
- Live-verified on Jon's laptop: unlock on/off restarts, F7 re-raise after EC changes and
  charger events, live current changes without restart, failsafe minimum, sub-minimum TGP,
  board input SET/readback, live limits reads under load.
- Not tested live: driver reinstall / VBIOS replacement cleanup, driver 621+.
