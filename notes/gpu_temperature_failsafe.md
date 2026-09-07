# Thermal Failsafe

## Setup

1. Load the desired profile. Open **Thermal Failsafe**, enable protection and set
   the GPU trigger above the EC temperature normally reached with cooling working.
   The range is 30-95 C, default 62 C, with one-degree slider steps.
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
5. After checking cooling, **Re-arm** acknowledges/resets only. Separately reapply
   the chosen profile to restore all its power/fan settings, or explicitly apply
   power and fan-boost settings individually. A Power-tab apply cannot release a
   remaining fan hold. Save remains blocked until every held action is restored.

Existing profiles without the block default to disabled. Missing CPU/fan options
default **off**, including already-enabled GPU-only profiles. The foreground
preference defaults **on**. No BLE cooler connection or hotspot driver is required.

## Detection And Actions

- The first valid EC reading at or above the threshold trips immediately. There is
  no intentional three-sample delay, averaging, or automatic reset/profile reload.
- Five consecutive invalid/unavailable readings trip protection. Arming and
  protected power application require an immediately valid fresh reading. Zero
  from a sleeping GPU can therefore conservatively trip the failsafe.
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
| Tripped | Manual acknowledgement required; protected settings remain gated. |
| Reset pending | Emergency settings still held and enforced; explicit successful restoration required. |

Both protected stages persist in `settings.json`, independently of selected
profile. Reset requires a fresh valid sample at least 3 C below the lower applicable
trip/current enabled threshold, plus successful current enforcement and persistence.
A failed save retains the runtime trip. Reset itself neither raises power nor
turns off emergency fan boost, and restart does not implicitly restore either.

Explicit profile/power/fan applies use short-lived per-operation restoration
permits after reset. Only verified corresponding writes release their actions.
Partial writes, missing completion, or failed durable release retain/reassert the
old hold. Re-enforcement or EC reactivation during a restoration invalidates its
completion. Startup autoload and watchdog recovery never obtain a permit.

Profile switching first applies a conservative transition policy: detection enabled
if either profile enables it, the lower threshold among enabled profiles, and the
union of their requested optional actions (capability-filtered when executed).
Current alert preference is retained during transition. Destination policy commits
only after synchronous application, slot persistence and any restoration release
succeed. Failed application leaves a conservative policy, an unsaved/partial hint,
and no misleading old-slot highlight. Trips during the attempt are never rolled back.

While tripped, deliberate restoration/profile/default applies are refused; ordinary
controller writes remain gated. Firmware defaults require acknowledgement and
disabled detection, plus a specific confirmation that firmware control releases the
hold. Safety-critical default writes must all succeed before release; unrelated
lighting errors are not proof of protection failure or successful full restoration.
Mode defaults release only power actions they actually apply, not fan boost.

Fan audit: Fan tab/profile boost changes share the gated `SetFanBoost` boundary.
Linked-fan ticks only write curves/latches, not the boost register; no demonstrated
conflict warrants disabling that independent controller. Firmware defaults stop
linked mode before handing fan-curve control back to firmware. Profile-linked-mode
initialization failures now propagate instead of masquerading as successful applies.

### Persistence Recovery

State writes flush intent before emergency hardware work, then record verification
results. Replacement is atomic (`settings.json.tmp` to `settings.json`). At an
interrupted boundary, valid records merge by **tripped > reset pending > no hold**,
union of held actions and the lower recorded threshold from held records only.
Pending release/reset never clears a stronger committed record. A completed atomic
replacement removes the pending file and is authoritative without historical merging.

Old reason/threshold-only trips migrate as GPU-only holds. Unreadable, null or
inconsistent records produce `Saved protection state uncertain`, preserving known
CPU/fan obligations plus conservative GPU protection. Different recoverable events
are explicitly identified as merged, not presented as an exact historical event.
If no valid protected reset threshold is recoverable, the conservative 30 C floor
is used; this may require support intervention if a fresh reading cannot reach 27 C.
Recovery is recommitted before persistence is considered healthy. Verification
results from a prior process/EC activation are historical, not current authority.

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
A CPU action already verified in the same hardware epoch uses fresh minimum-PL
readback plus validation of the current temperature limit. Matching requests need
no rewrite or redirect-verification sleep; drift rewrites the complete CPU group.
Initial trips, restart/recovery, failed verification, reset and explicit enforcement
cannot take this fast path. Invalid/unreadable temperature limits remain failures,
never fallback writes. GPU/fan holds still reassert on the existing cadence.
Actual writes retain the bounded `WriteGuard` verification delays and remain on
the UI thread; this removes redundant stable-hold stalls, not all blocking IO.
Fresh apply/reset/linked-enable checks are outside the recurring sample count.
Subscriber/notification and unrelated telemetry exceptions cannot stop safety work.

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
