# Thermal Failsafe

## Setup

1. Load the desired profile. Open **Thermal Failsafe**, enable protection and set
   the GPU trigger above the EC temperature normally reached with cooling working.
   The range is 50-95 C, default 50 C, with one-degree slider steps. The lower
   bound is a product-use choice, not a universal hardware safety limit.
2. Optionally select CPU minimum power and/or onboard fan boost. Unsupported
   options explain the missing coverage; they do not disable core GPU protection.
   These are EC setting values, not measured or shunt-corrected watts.
3. **Apply** changes runtime configuration; the profile bar's **Save** persists it.
   `Changes not applied` means control edits only. `Applied - not saved to profile`
   means runtime settings differ from the saved profile, or application was partial.
   Save reads applied configuration, never pending checkbox/slider edits.
4. **Test Failsafe** really applies the currently applied emergency actions. Its
   confirmation lists them and warns about unapplied edits. Test is disabled while
   any protection hold remains. Stop workloads first; do not heat the GPU to test.
5. To recover (2026-10-14 simplification), either press **Reset** (clears the trip
   and reapplies the active profile in one step) or click the profile that matches
   the current cooling, e.g. an air-cooling profile (one confirmation; clears the
   trip and arms that profile's own trigger). Turning fan boost **on** is never
   blocked. Power-tab/mode-default/firmware-default applies and Save are refused
   only while tripped.

Existing profiles without the block default to disabled. Missing CPU/fan options
default **off**, including already-enabled GPU-only profiles. The foreground
preference defaults **on**. No BLE cooler connection is required.

Otherwise-valid legacy 30-49 C profile triggers and operational reset thresholds
migrate to 50 C with a configuration warning. Enabled/options and actual held
actions are preserved; historical recorded thresholds are not rewritten. Migrated
profiles show an unsaved hint until explicitly saved. Invalid types or values
outside the old supported range are configuration errors, not arbitrary clamps.

## Detection And Actions

- Trips use the rolling average of the last 5 one-second EC readings (at least 3),
  inclusive of the trigger. EC readings are noisy; a single spike is diluted to a
  fifth, while a sustained crossing trips within roughly 2-3 s. Fresh pre-apply
  checks add a reading to the same window.
- Five consecutive invalid/unavailable readings trip protection. A failed fresh
  pre-apply read falls back to the recent average; with no recent readings (just
  armed) it trips. Zero from a sleeping GPU counts as invalid.
- GPU action: the validated model/ECSpec minimum TGP, Dynamic Boost off, using the
  existing GPU sequence and `WriteGuard`. Generic UI fallback limits are not used.
- Fan action: `FanControl.SetFanBoost(true)`, the established onboard EC mode.
  This is not the BLE cooler's fan/pump control, and does not guarantee a specific
  physical RPM or universal 100-percent response.
- CPU action: validated effective PL1/PL2/PL4 minimums using existing grouped
  writes, Intel/AMD temperature encoding and runtime PL4 scaling. Every emergency
  group preserves a successfully read current CPU temperature limit. Invalid or
  failed reads make CPU protection incomplete; no cached/max-temperature fallback.
- Actions execute independently, GPU first and fan boost before CPU verification.
  A CPU/fan failure cannot prevent GPU reduction. Status identifies GPU, CPU, fan
  and persistence failures separately, rather than claiming blanket success.
- Desired unavailable optional actions remain in saved settings, explicitly
  excluded from the executable action set. Once captured in an active hold, later
  capability/write failures cannot remove that action's obligation.

## Durable Recovery

| Stage | Behavior |
| --- | --- |
| No hold | Enabled detection arms protection; disabled detection adds no recurring failsafe work. |
| Tripped | Emergency settings held and enforced until Reset or a deliberate profile load. |

The trip persists in `settings.json`, independently of selected profile, and
survives restart (startup autoload and EC-recovery reloads keep it). Reset checks
the **current** trigger: while detection is enabled it refuses if the rolling
average is at or above the trigger (it would trip again) and says how to get out;
with detection disabled it always succeeds. There is no latched lower reset point
and no extra 3 C margin. A failed release save keeps the trip.

Reset/profile load releases every held action at once (the old reset-pending stage
and per-action restoration permits were removed 2026-10-14). Settings files with
the retired Stage 1 load as already reset. A deliberate profile load uses the
destination's own failsafe settings, not a merge with the previous profile; it can
trip again on its own trigger, which is reported.

While tripped, deliberate Power-tab/mode-default/firmware-default applies and Save
are refused; automatic writes are coerced to the emergency values. Firmware
defaults no longer require detection to be disabled.

Fan audit: Fan tab/profile boost changes share the gated `SetFanBoost` boundary.
Linked-fan ticks only write curves/latches, not the boost register; no demonstrated
conflict warrants disabling that independent controller. Firmware defaults stop
linked mode before handing fan-curve control back to firmware. Profile-linked-mode
initialization failures now propagate instead of masquerading as successful applies.

### Persistence Recovery

Trip state/action obligations latch in memory before hardware work or notifications.
GPU, fan and CPU actions are attempted independently before synchronously flushing
the resulting state and errors. A save failure retains the hold and retries; a
crash between hardware reduction and the save can lose that new trip record.
Fresh startup/profile-apply checks remain essential. This is not asynchronous IO
or a bounded-latency guarantee. Replacement is atomic (`settings.json.tmp` to
`settings.json`). At an
interrupted boundary, a tripped record wins and held actions are unioned.
A pending reset never clears a committed trip. A completed atomic
replacement removes the pending file and is authoritative without historical merging.

Operational Stage/Held values are parsed independently of optional history and
unrelated app settings. A bad timestamp, diagnostic string or profile name does not
manufacture a trip or erase a real hold. Different historical events are reported
as merged but do not escalate otherwise valid operational stages.

Old reason/threshold-only trips migrate as GPU-only holds. Genuinely unreadable or
inconsistent operational state produces a clearly identified configuration-recovery
hold, preserving known actions and adding the GPU fallback only with detected
GPU/model support. CPU-only machines do not acquire invented GPU/CPU/fan actions;
known obligations that cannot be executed remain unresolved. Unknown recovery
thresholds use 50 C (reset at <=47 C), without invented measurements or times.
Recovery is recommitted before persistence is considered healthy. Verification
results from a prior process/EC activation are historical, not current authority.

### Profile Files

Missing/corrupt requested slots throw at the apply boundary; they never report
successful application or arming. Startup/recovery failures retain existing
protection and display a persistent, nonmodal configuration warning on the tab
and tray, distinct from a thermal trip. An intentionally empty slot can still be
selected as a save target. No loaded profile means no claim of saved-profile identity.

Displayed configuration warnings combine session-long startup settings/recovery
diagnostics with the current operation warning. Successful profile load/save clears
only the operation warning; a newly loaded migrated profile keeps its save-required
warning. Startup diagnostics are re-evaluated on next launch, not declared resolved
by a profile operation. Loader diagnostics are retained separately in the manager/log.
Warning changes update the tab title and tray through one presentation
path, including before tray initialization; clearing a warning never clears a hold.
Unsupported machines still have a controller, with unavailable capabilities and
disabled detection controls rather than a null-controller fallback.

Explicit ProjectId mismatches are refused. Legacy profiles without IDs are allowed
with a warning and current-model preflight. CPU/GPU requests are validated before
unrelated profile writes and again at the feature write boundary. Out-of-range or
unrepresentable requests are rejected, not silently clamped. Doubled PL4 retains
downward two-watt quantization, provided both requested and effective values remain
inside model bounds. DB-off writes exactly zero to both DB value registers. The
deliberate firmware-default zero handoff remains a separate guarded path.

Profiles and settings share a same-directory flush-and-replace writer. An unfinished
profile `.tmp` is never loaded. If profile replacement succeeds but the subsequent
selected-slot settings save fails, the profile is saved but the UI reports the
partial result and does not mark the whole operation saved or change selection.
There is no multi-file transaction or fictitious rollback.

The latest trip retains time, reason, known temperature/threshold and action results
after reset/restoration. Sensor-loss and manual events do not invent over-temperature
measurements. Peak is in memory, from shared samples, and resets on a new protection
cycle, not ordinary refresh/Apply. Invalid current readings display `--`.

## UI And Notifications

The tab follows the supplied mockup's two-section layout: **GPU Thermal Failsafe**
and fit-content **Status**, with a slider and three optional-action/preference
checkboxes. Accurate immediate-trigger/manual-reset copy replaces the mockup's
three-second/automatic-rearm claims. Details are expandable instead of a third
always-expanded section. Long status/history and checkbox text wrap and scroll.

Tripped/incomplete protection is red; acknowledged/restoration-pending state is
amber. A persistent tray warning and tab attention marker remain while held.
Native Windows `MessageBeep`, the tray balloon, and an independent topmost nonmodal
alert remain best-effort. The main-window checkbox controls only bringing up the
main form on this tab; it never suppresses the independent alert. Closing an alert
does not reset or restore protection. `System.Media.SystemSounds` must not be used:
release packaging intentionally removes its `System.Windows.Extensions` dependency.

## Resource Cost And Limits

The existing one-second `EcMonitor` timer is the sole recurring failsafe sampler.
Armed/held ticks read the GPU temperature before unrelated telemetry; visible
telemetry reuses that same byte. Tray mode adds one temperature read to normal RPM
reads, unless linked fans already need it. Linked control now shares this timer and
GPU sample, adding one CPU-temperature read in the tray rather than polling both
sensors on its own timer. Disabled and unheld protection removes the subscriber: no additional periodic
reads, writes, logs or hidden-label work. Small resident controller/UI objects remain.
No new timer, thread, GPU API, BLE polling or periodic disk reads are introduced.

Outstanding holds are maintained every five samples, without a total retry limit.
A previously verified action in the same hardware epoch uses fresh request readback:
CPU minimum PLs plus the valid current temperature limit; GPU masked control bits,
TGP and both DB bytes; fan boost's established request register (not RPM/status).
Matching requests need no rewrite or CPU redirect-verification sleep. Drift/read
failure repairs the full affected group when inputs are valid. Initial trips,
restart/recovery, unverified actions and forced rollback still write/verify.
Re-arm may use the same fresh maintenance checks; it never restores power.
Profiles declaring skipped/redirected readback for those direct request registers
retain the full-write path rather than inferring new readback semantics.
Invalid/unreadable CPU temperature limits remain failures, never fallback writes.
Actual writes retain the bounded `WriteGuard` verification delays and remain on
the UI thread; this removes redundant stable-hold stalls, not all blocking IO.
Fresh apply/reset/linked-enable checks are outside the recurring sample count.
Subscriber/notification and unrelated telemetry exceptions cannot stop safety work.

Profile transitions share configuration assignment without repeating the adjacent
initial GPU check. Direct feature-controller checks remain, including after slow
fan/CPU work and before reducing fan boost. No global temperature cache is used.
Linked-fan programmatic/user changes share one synchronous operation. Failed enable
repairs the checkbox without accidental disable/rollback writes; explicit re-enable
applies fresh curves and subscription/disposal contracts remain unchanged.

Temperature notifications refresh live readings without rebuilding the action
preview, saved/applied comparisons or historical record. Stable maintenance does
not emit full state-change notifications or save unchanged results. The bottom
monitor panel and hidden Failsafe tab skip label work without stopping monitoring.

This is not real-time protection or proof of coolant flow. UI-thread stalls can
delay sampling; there is no protection while the application/OS cannot run or the
EC cannot accept writes. A plausible frozen register is undetectable. Readback
verifies an EC request, not physical power consumption/cooling. Hardware and disk
cannot be made one atomic transaction; crashes or disk failure at a boundary remain
limitations even with conservative recovery and immediate reassertion on failure.

## Verification

```powershell
dotnet run --project tests/GpuFailsafe.Tests
dotnet run --project tests/ThermalUi.Tests -- --headless
dotnet run --project tests/ThermalUi.Tests -- C:/Temp/tandem/thermal-ui-validation
dotnet run --project tests/ThermalUi.Tests -- C:/Temp/tandem/thermal-ui-native --native
dotnet build MechControl/MechControl.csproj -c Release
```

Use `MechControl/build-scripts/build-release.ps1` for real release packaging, then:

```powershell
./tests/packaged-alert-smoke.ps1 -Package ./MechControl/bin/selfcontained -ScratchRoot C:/Temp/tandem -DotnetPath C:/Users/Jon/.dotnet/dotnet.exe
```

Add `-Headless` to run only shipped-runtime settings/profile serialization,
recovery, migration and atomic-save checks, with no windows, sound or hardware.

All harnesses are hardware-free. The UI suite links the actual tab/shared monitor
against fake hardware; it tests both sensor read counts and rendering/edit behavior.
The headless mode checks actual linked-fan/guard writes, shared sampling and UI
refresh behavior without showing windows or touching the running installation.
The packaged test invokes the real alert method using the shipped runtime and DLLs,
without constructing MainForm or opening the EC. It checks both foreground choices,
the independent nonmodal alert, native sound path and message-loop progress.

See `thermal_failsafe_validation.md` for results, screenshots and remaining checks.
This expanded implementation has **not been deployed or deliberately triggered on
live hardware**. Hardware power/fan response, sleep/resume and physical enforcement
require Jon's approval and supervised validation with workloads stopped first.
