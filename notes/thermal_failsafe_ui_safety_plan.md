# Thermal Failsafe UI And Safety Handoff

## Goal And Scope

Implement this plan in the current MechControl repository. Jon will supply screenshots of the current UI and a preferred reference UI. Match the preferred screenshot's layout, styling, controls, and explanatory presentation, while preserving and fixing this repository's stronger safety behavior.

This document is self-contained. The reference implementation will no longer exist; do not look for another repository or require its source. All paths below are relative to this repository, whatever its folder is named when you receive it.

Deliver working code, tests, documentation, and a visual comparison against the supplied screenshots. Do not deploy to live hardware or deliberately trigger hardware protection without Jon's approval.

### Decisions For This Plan

- Keep per-profile protection settings; do not silently convert them to global settings. The durable active safety interlock remains app-level and cannot be cleared by profile selection.
- Keep the existing immediate temperature trigger, fresh pre-apply checks, invalid-sample policy, reset hysteresis, and shared monitor. Matching a screenshot does not authorize weakening these behaviors.
- Add optional CPU power reduction, onboard fan boost (the requested max-fans feature), optional main-window activation, peak temperature, and last-trip details.
- Keep the real Test Failsafe button and a separate manual reset. Do not combine reset with automatic restoration of a high-power profile.
- Preserve existing saved configurations. New CPU/fan action fields default to off when absent, so upgrading an already-enabled GPU-only profile does not silently change hardware behavior. Users can enable and save them explicitly. Default the new main-window preference to on; retain the independent safety alert regardless of that preference.
- Match screenshot text wherever it is behaviorally accurate. The reference description contains a three-second delay and automatic re-arm claim that conflict with the retained safety contract. Section 2 provides the reference text and the corrected text to ship. Do not change the controller to make those unsafe claims true.

## 1. Establish Context

Read `AGENTS.md`, `register_reference.md`, `architecture.md`, and `notes/gpu_temperature_failsafe.md`. Follow the repository's remaining reference-reading instructions as relevant.

The reviewed starting point was commit `fd5ebe7b0b273f2c6043ce1fc3170a76c5a2adce` plus dirty GPU failsafe work. Inspect current status/diffs first: that work belongs to Jon and must not be reverted. This handoff is not permission to reset the tree or replace the implementation wholesale.

Relevant files and responsibilities:

| File | Responsibility |
| --- | --- |
| `MechControl/Features/GpuTemperatureFailsafe.cs` | Injected temperature/action/persistence controller; trip, reset, retry, status |
| `MechControl/Features/GpuPowerControl.cs` | Common GPU write gate, validated minimum, Dynamic Boost off |
| `MechControl/Features/CpuPowerControl.cs` | Grouped CPU writes, temperature-limit and PL4 encoding |
| `MechControl/Features/FanControl.cs` | Existing `SetFanBoost`, fan curves and control mode |
| `MechControl/Features/LinkedFanController.cs` | Existing linked-fan activity to audit for interference |
| `MechControl/Monitoring/EcMonitor.cs` | Shared one-second polling and early `GpuTemperatureSample` event |
| `MechControl/Profile/GpuFailsafeSettings.cs` | Per-profile protection settings |
| `MechControl/Profile/UserProfile.cs` | Saved user-profile schema |
| `MechControl/Profile/UserProfileManager.cs` | Settings/profile persistence, atomic interlock writes and recovery |
| `MechControl/AppJsonContext.cs` | Source-generated JSON |
| `MechControl/UI/FailsafeTab.cs` | Tab to redesign |
| `MechControl/UI/MainForm.cs` | Construction, alerts, tray, startup/recovery, defaults |
| `MechControl/UI/ProfileBar.cs` | Profile apply/save, selected slot, failsafe configuration ordering |
| `MechControl/UI/PowerTab.cs`, `MechControl/UI/FanTab.cs` | Explicit user restoration and other writes |
| `MechControl/UI/UiHelpers.cs`, `UiMetrics.cs`, `UiColors.cs`, `Section.cs` | Existing layout, fonts, controls, colors |
| `tests/GpuFailsafe.Tests/` | Hardware-free console suite, including real GPU guard/encoding integration |
| `tests/PackagedAlertSmoke/`, `tests/packaged-alert-smoke.ps1` | Packaged-runtime alert regression test |

Prefer small changes in these existing components. Keep the controller testable through injected dependencies; do not introduce another scheduler, generic workflow framework, or broad unrelated MainForm refactor.

## 2. Target UI And Exact Copy

### Overall Layout

- Tab title: `Thermal Failsafe`; retain a visible attention marker while tripped or enforcement is unverified.
- Use the existing light WinForms styling and `BuildSectionStack()`.
- Two main sections, in order: `GPU Thermal Failsafe` (full width) and `Status` (fit content).
- Follow the supplied reference screenshot for spacing, section widths, alignment, button appearance, and wrapping. Use existing helpers rather than screenshot-sized absolute coordinates.
- Replace the current numeric temperature spinner with the same label/TrackBar/value row used elsewhere in the app.
- Put the purpose description above the enable checkbox. Do not retain the current large, always-expanded third `How It Works` block. Keep essential limitations accessible in a compact expandable details area or link below the main controls.
- Keep the Test button alongside the reset/re-arm button even if the reference screenshot only shows one button.

### Configuration Section, Top To Bottom

1. Wrapped muted description paragraph, normal font.
2. Bold checkbox: `Enable thermal failsafe`.
3. Slider row: `GPU trigger temperature`, degree-C value at the right. Retain the current 30-95 C range, default 62 C, and 1 C steps. Use `\u00B0C` in C# strings where appropriate.
4. Checkbox: `Also drop CPU power limits (PL1 / PL2 / PL4) to minimum`.
5. Checkbox: `Turn on fan boost when tripped`.
6. Checkbox: `Bring up the MechControl window when tripped (safety alert, tray alert and sound remain enabled)`.
7. Muted live preview: `On trip: GPU TGP {minimum} W, Dynamic Boost off; CPU PL1/PL2/PL4 {p1}/{p2}/{p4} W; fan boost on.` Include only selected, supported optional actions. These are EC setting values, not measured/shunt-corrected power.
8. `Apply` button, followed by a compact save-state hint.

The reference had these two checkbox labels verbatim:

> Turn on fan boost when tripped
>
> Bring up the MechControl window when tripped (tray alert + sound always happen)

Use the corrected main-window wording above to distinguish the independent safety alert and avoid guaranteeing delivery by Windows. Actual notifications and sound remain best-effort.

### Description Text

Reference screenshot/source wording, retained here so no other repository is needed:

> For machines that depend on an external / liquid cooler to run their high-power profile. If the EC GPU temperature stays at or above the trigger for 3 consecutive seconds (a level it never reaches with the cooler working), power limits are dropped to this model's minimums and you are alerted. Loading a profile or applying power settings re-arms it.

Ship this closely matching, behaviorally accurate version instead:

> For machines that depend on an external / liquid cooler to run their high-power profile. Set the trigger above the EC GPU temperature normally reached with the cooler working. If a reading reaches or exceeds the trigger, the failsafe latches, requests this model's minimum GPU power and alerts you. Optional actions also reduce CPU power and turn on fan boost. After checking cooling, manually reset the failsafe, then separately reapply your chosen profile or power/fan settings to restore normal operation.

Below Apply, use:

> Applies to the current profile. Apply takes effect now; use the profile bar's Save button to keep these settings.

Compact details must still explain:

- EC temperature checks run approximately once per second, including in the tray; there is no intentional three-sample delay.
- Five unavailable/invalid samples trip protection; arming or applying protected power requires an immediately valid reading. A sleeping GPU reporting zero can cause a conservative trip.
- Reset requires a fresh valid reading at least 3 C below the applicable threshold and does not restore power or turn off emergency fan boost.
- Minimum settings are verified EC requests, not proof of actual cooling or physical power enforcement. No shunt multiplier is assumed.
- No protection while the application/OS cannot run or hardware cannot accept writes; a plausible frozen sensor cannot be detected from this register alone. No BLE cooler connection is required.

If Jon explicitly changes behavioral requirements when supplying screenshots, resolve that conflict with him. Otherwise screenshots govern appearance; this plan governs safety semantics and truthful copy.

### Status Section

Use `BuildKeyValueTable()` with these captions in this order:

- `State:`
- `GPU temperature (EC):`
- `Peak since armed:`
- `Last trip:`

Show current temperature only from a valid current monitor sample; display `--` or `Unavailable` when invalid, not an old temperature presented as current. Peak can retain historical valid samples, but it must be clearly historical. Compute it in memory from the shared samples; no extra reads. Reset peak on a genuine new arm/protection cycle, not on every UI refresh or Apply click.

Last trip should include date/time, reason, temperature/threshold when known, actions attempted, and per-action verification/errors. Sensor-loss and manual tests must not invent a temperature or falsely say an over-temperature threshold was crossed. Retain the last-trip record after acknowledgement; persisted active trips must still be understandable after restart. An unbounded trip-history database is out of scope.

State presentation:

| Condition | Presentation |
| --- | --- |
| Disabled, no outstanding interlock | Neutral `Disabled` |
| Armed, valid current sensor | Green `Armed - trips at {threshold} C` |
| Armed, sensor temporarily unavailable | Amber `Temperature unavailable - {n}/5 failed checks` |
| Tripped, selected actions verified | Bold red `TRIPPED - emergency settings written and verified` |
| Any required action unverified | Bold red `PROTECTION INCOMPLETE - {action} not verified`, with actionable details |
| Reset, still awaiting explicit restoration | Amber `Reset - emergency settings retained; restore explicitly` |
| Unsupported hardware/model | Neutral/amber `Unavailable`, explaining the missing capability |

The status must distinguish GPU, CPU, fan, and persistence failures. Never show a blanket minimum-power success statement when a required action failed. Persisted verification flags must not be trusted after restart/EC reactivation: reverify.

Buttons:

- Primary: `Re-arm` to match the reference, with adjacent help/confirmation explicitly stating that it acknowledges/resets only and does not restore power. Keep the fresh-temperature/hysteresis requirements. An explicit `Reset Failsafe` label is acceptable if the screenshot leaves room and Jon prefers clarity; do not change its semantics to a profile reload.
- Secondary: `Test Failsafe`; use the controller's applied runtime configuration, not pending checkbox edits. Confirmation must list the real applied GPU/CPU/fan actions and identify any unsupported options. If controls have unapplied changes, explicitly say they are not part of the test. Disable while already tripped, awaiting restoration, or unavailable, so it cannot produce a no-op confirmation or overwrite an unresolved event.
- Closing any alert is not acknowledgement or restoration.

### Small UI Fixes Included

- Distinguish unsaved control edits (`Changes not applied`) from runtime configuration differing from the saved profile (`Applied - not saved to profile`). Only a successful Apply/Save clears the corresponding indicator.
- Read runtime configuration when saving a profile, not un-applied control edits. Switching tabs/telemetry updates must not overwrite pending edits or trigger hardware writes.
- Show the post-reset state in warning colors; do not make it look disabled or fully restored.
- Capability-gate CPU/fan options with a clear unsupported explanation. Do not block core GPU protection because an optional feature is unavailable.
- Keep the tab visible with an explanation on unsupported models, as the current implementation does.
- Refresh hidden-tab state when shown, avoid periodic hidden-label work, and unsubscribe events on disposal.
- Verify keyboard focus order, slider keyboard use, text wrapping, and 100/150/200 percent DPI layouts against screenshots. Do not clip long error/last-trip text.

## 3. Add Optional Emergency Actions Safely

### Configuration And Persistence

Extend `GpuFailsafeSettings` with CPU-reduction, fan-boost, and show-main-window options. Store them with the existing per-profile settings. Validate loaded values and handle missing/null blocks safely. Keep source-generated serialization trim-compatible; update `AppJsonContext` or descriptor roots only as actually needed.

Snapshot the selected emergency actions when a trip occurs. An active interlock's obligations must not disappear when another profile is selected, options are unchecked, or the app restarts. Persist enough information to restore the active/pending action set; derive safe hardware targets from the currently validated machine rather than trusting arbitrary saved wattages as hardware authority. For an old saved trip that has only reason/threshold, preserve GPU-only protection; do not invent previously selected CPU/fan actions.

Capability policy: before a trip, distinguish saved desired options from supported effective actions. An unsupported desired CPU/fan option remains visible as unavailable and is excluded from the executable action set, with an explicit warning such as `CPU reduction unavailable on this model; GPU protection remains active`. Do not silently overwrite the saved preference or claim full requested coverage. Test/preview/trip details must identify the unavailable option. Once a supported action has been captured in a durable active interlock, later capability/read/write failure does not remove it: retain an incomplete obligation and block a falsely successful reset. GPU capability failure retains the existing stricter behavior for profiles requiring GPU protection.

### GPU

Retain `GpuPowerControl`'s validated, exactly representable model minimum and Dynamic Boost-off write sequence. Do not weaken the provenance check to accept generic UI fallback limits. Keep all writes routed through `WriteGuard`.

### CPU

- Use `CpuPowerControl.ApplyCpuPower()` and effective model/ECSpec PL1/PL2/PL4 minimums. Validate limits and encodability; never hardcode Jon's wattages or CPU family.
- Preserve the user's valid CPU temperature limit using a current successful read for each grouped emergency write. Do not silently substitute the model maximum or a stale cached value when reading it fails. On failure, report this action incomplete and retry without skipping GPU/fan protection. Do not add a cache unless its authority and invalidation across every firmware/manual/recovery writer can be demonstrated; that complexity is not needed for this plan.
- Preserve grouped writes, Intel/AMD temperature encoding, PL4 scaling, and verification behavior.
- Attempt independent emergency actions even if one throws. GPU reduction is first; attempt fan boost promptly as CPU verification can block the UI thread.

### Fans / Max Fans

- Use the established `FanControl.SetFanBoost(true)` path. The current controller writes the profile's fan-control register using the already documented boost command; do not add raw register writes or rewrite curves to 100 percent as a substitute.
- This is the laptop's supported onboard EC fan-boost mode, not the external BLE cooler's fan/pump controls. Do not claim a specific physical RPM or universal 100-percent response without model evidence.
- Verify the mode's supported meaning in `register_reference.md` and the effective model. Keep the UI label `fan boost`; a max-fans explanation may describe the supported firmware boost mode.
- Audit Fan tab, profiles, defaults, and linked-fan activity so they cannot turn off the emergency fan action while it remains protected. Avoid a new background timer or disabling unrelated fan logic without a demonstrated conflict.

### Enforcement And Notifications

Keep per-action success/error state and rate-limited retries on the existing five-sample cadence. Retry for as long as an obligation remains; there is no total retry-count cutoff. Each attempt uses the existing bounded driver/verification behavior, not a new tight retry loop. A CPU/fan failure must not prevent a successful GPU action; retry incomplete actions and reassert needed settings after EC reactivation. Avoid repeated success log spam or alert storms.

Gate CPU and fan writes at common write boundaries where practical, just as GPU writes are gated now. Do not rely only on disabled buttons: tray/profile/defaults/recovery paths also write hardware. Protect only selected emergency actions; do not silently start constraining unrelated controls.

Keep the persistent tray warning, Windows native `MessageBeep`, and independent topmost nonmodal safety alert. Do not replace `MessageBeep` with `System.Media.SystemSounds`: the release packaging strips `System.Windows.Extensions.dll`, which previously broke alerts. The new foreground checkbox controls only opening/activating the main form on the failsafe tab; it must not suppress incomplete-protection warnings or the independent safety alert. OS notification restrictions must not pause sampling.

## 4. Fix The Existing Safety Defects

### A. Make Reset-Awaiting-Restoration Durable

Current defect: `Reset()` clears persisted `GpuFailsafeTripReason`, but `NeedsPowerRestore` is only in memory. On restart, startup profile autoload can raise power without the promised explicit action.

Represent at least these interlock stages explicitly, separately from whether temperature detection is enabled:

1. No outstanding interlock.
2. Tripped / manual acknowledgement required.
3. Reset / emergency settings held pending explicit restoration.

An enum or equally clear minimal representation is fine. Persist the stage, active action set, and required event information atomically. Keep startup and watchdog recovery distinguishable from deliberate user actions. Neither may release the hold or increase protected settings.

Interrupted-write recovery must be conservative across both valid committed and valid pending records, not just choose the latest timestamp:

- Order stages as tripped > reset-pending > no hold. Use the stronger stage and the union of still-held actions from both records. A pending reset/release never clears a committed trip or action.
- Use the lower applicable recorded reset threshold when combining protected records. Never import a threshold from an unrelated unprotected/default record as evidence of the original trip.
- If only a pending held record exists, recover it. Missing both records is ordinary first-run state. An old reason-only record maps to tripped with a GPU obligation.
- On corrupt/unreadable or inconsistent state, retain recoverable known action obligations plus a conservative GPU trip, and surface `Saved protection state uncertain`. Do not guess CPU wattages or manufacture a thermal event from incomplete metadata.
- If records describe different events, label the recovered state explicitly, retain available original event details for diagnostics, and do not present the merged action set as an exact historical event. Recommit the recovered state before treating persistence as healthy.
- A complete successful atomic replacement removes the pending file and is authoritative on its own. These merge rules are for interrupted/ambiguous writes, not a reason to restore already-released historical actions from unrelated history records.

### B. Enforce The Post-Reset Hold

Current defect: `EnforceMinimum()` returns when `IsTripped` is false. Recovery also skips GPU profile writes while `NeedsPowerRestore` is true, so lost firmware limits are not reinstated.

Keep enforcement/subscription active for both tripped and reset-pending stages, even if new temperature triggering is disabled. Reassert protected GPU/CPU/fan settings before ordinary profile recovery can raise power or lower emergency fans. Retry failures and keep accurate unverified status. A cooling GPU is not permission to release the hold.

### C. Refuse A Falsely Successful Reset

Current defect: a fresh cool temperature permits reset even if emergency writes never succeeded; the resulting status claims minimum power and stops retries.

Before transitioning to the acknowledged hold, require a fresh cool sample and a successful current enforcement/verification attempt for required actions. If either fails, retain the trip, errors, persistence state, and retries. Reset must never raise power or switch off emergency fans. A failed save of the reset must retain the runtime trip.

### D. Make Restoration Explicit And Success-Dependent

The current `GpuPowerControl.OnPowerApplied()` callback fires for any successful GPU write; it is not sufficient authorization once the post-reset hold is enforced. Distinguish a deliberate restoration operation from an emergency retry, startup autoload, or recovery apply. A narrowly scoped authorization or explicit operation parameter is preferable to global suppression flags.

- While tripped, even deliberate high-power requests remain clamped or refused. The user must reset first.
- After reset, a deliberate profile/power/fan apply can restore the affected settings, subject to a fresh pre-apply safety check whenever protection is enabled.
- Only successfully completed, verified restoration clears the corresponding outstanding hold. A GPU-only apply cannot falsely mark CPU/fan settings restored. Either track the still-held actions or require an explicit complete restoration flow; keep the UI and Save lock truthful.
- Protect multi-action restoration from partial failure: reassert still-protected settings on failure, retain an unresolved state, and report which steps failed. Never announce full restoration after exceptions were swallowed by `TryUi`.
- Commit durable release only after successful hardware restoration. If persisting release fails, retain a conservative interlock and reassert emergency settings rather than reporting normal operation. Test crash/failure boundaries; do not promise impossible hardware/disk atomicity.
- Keep profile Save blocked while tripped or any restoration obligation remains, so emergency settings cannot overwrite the user's normal profile.

### E. Fix Failed Profile-Switch Disarming

Current defect: `ProfileBar.ApplyProfile()` immediately configures the destination failsafe before fan/CPU/GPU writes. A destination with protection off can disable the current protection, then fail on hardware writes, leaving the old protected profile highlighted.

Validate destination settings/capabilities before writes. Stage safety policy so stronger destination requirements are active before power can rise, but failed application cannot leave protection weaker than the previous committed policy. Do not simply postpone all configuration until after writes; that creates the opposite gap when loading a protected high-power profile.

Define the transition policy explicitly: triggering is enabled if either the current or destination policy enables it; the effective threshold is the lower threshold among enabled policies; optional emergency actions are the union of their supported requested actions. Ignore a disabled policy's threshold when computing that minimum. Preserve any already-held action snapshot independently. Keep current alert preferences during the transition. This handles mixed changes such as raising the threshold while adding fan boost without guessing which whole profile is stronger.

Commit the destination configuration only after its required synchronous profile application and slot/settings persistence succeed. If the operation fails, keep the conservative transition policy or another explicitly proven no-weaker policy, flag the configuration as partially applied/unsaved, and never roll back a trip that occurred during the attempt. Keep slot highlighting honest; do not imply the old slot exactly describes partially changed hardware/settings. Avoid attempting a dangerous automatic hardware rollback to a high-power profile merely to make the selection look consistent.

Do not broaden this into transactional BLE/lighting architecture. Existing asynchronous/noncritical work may remain separate, but it must not be mistaken for proof that safety-critical power/fan/configuration work completed.

### F. Fix Firmware Defaults And Other Bypass Paths

Current defect: after reset and disabling protection, `RestoreDefaults_Click()` directly hands GPU control back to firmware while leaving `NeedsPowerRestore` set. Status incorrectly says minimum power and Save remains blocked.

Keep firmware defaults blocked while tripped or temperature protection is armed. After an explicit reset and disable, allow a specifically confirmed restoration from the pending hold. Do not base eligibility solely on `RequiresMonitoring`, since the corrected hold also requires monitoring.

Use the deliberate-restoration path to handle direct GPU/CPU/fan writes. On successful relevant writes, update the durable hold and UI accurately; unrelated lighting failures may still need their own warning. On partial safety-critical failure, retain/reassert the hold and show incomplete restoration, not success. Audit Gaming/Office defaults, Power/Fan tabs, tray profile selection, startup, and watchdog paths for the same rules.

## 5. Resource And Polling Requirements

Keep `EcMonitor.GpuTemperatureSample` as the single recurring temperature source for this feature. Read it before unrelated telemetry, including in lightweight tray mode, and reuse the sample for normal telemetry while visible.

- No new WinForms timer, background thread, GPU API, BLE polling, sensor library, or disk polling for failsafe/status/history.
- Under healthy normal operation, monitor plus failsafe use one GPU-temperature read per tick while visible and one while tray-hidden. Existing other features and deliberate fresh pre-apply/reset checks are outside that count.
- With triggering disabled and no outstanding interlock, remove the safety subscriber: no additional periodic hardware reads, writes, or logs. Small resident UI/controller objects are acceptable; do not claim literally zero resource use.
- With an unresolved hold, continue sampling/enforcement regardless of the enable checkbox. Extra verification/retries in this state are intentional safety work.
- Unrelated telemetry failures must not stop safety sampling. Contain/report notification/UI subscriber failures so they cannot prevent subsequent enforcement; test this without adding another scheduler.
- Do not claim real-time protection: UI-thread stalls can delay the shared timer. Preserve the existing documented limitation.

## 6. Implementation Order

1. Establish the current build/test baseline and add failing regression tests for the reviewed reset/recovery/profile/defaults defects.
2. Correct durable interlock stages, explicit restoration, persistence recovery, and write-path gating. Keep UI minimally functional during this phase.
3. Add per-profile optional actions and capability validation; extend enforcement and restoration tests to CPU/fans.
4. Rebuild the tab to the supplied reference layout and copy in section 2. Add peak/last-trip presentation, the foreground option, and save-state feedback without another poller.
5. Complete integration, serialization, packaged-alert, and visual verification. Update `architecture.md` and `notes/gpu_temperature_failsafe.md` to describe the shipped behavior, setup, and limitations.

Prefer a small testable controller/state model with concise data structures. Do not let rendering code become the authority for safety state or let a settings checkbox clear active emergency obligations.

## 7. Verification And Acceptance

### Automated Tests

Retain all existing controller/guard tests and extend the real failure paths, not just happy-path fakes:

- Inclusive threshold, disabled no-work, invalid sample streak/reset, immediate invalid pre-apply trip, fresh cool reset, and manual test.
- Trip and reset-pending reconstruction; startup autoload and watchdog recovery never release held GPU/CPU/fan actions.
- Fresh enforcement failure refuses reset; failed disk writes never skip emergency hardware attempts or clear a trip.
- Successful/failed explicit restoration, individual versus multi-action restoration, and failure to persist release. Internal emergency writes cannot authorize release.
- Existing settings/profiles with missing new fields and old reason-only trips; null/invalid/out-of-range state; valid/corrupt/missing primary and pending `.tmp` files. Pending releases must not override a stronger committed trip; pending held states must not be lost.
- Protected-to-unprotected and unprotected-to-protected profile switches, with injected fan/CPU/GPU/settings-save failures. No protection gap, misleading slot selection, or latch rollback.
- Firmware and mode defaults under armed/tripped/reset-pending states; successful release versus partial failure. No unconditional success callback after caught write errors.
- Independent GPU/CPU/fan failures, recovery, model-derived limits/encodings, preservation of CPU temperature limit, unavailable optional hardware, and fan boost versus other writers.
- Saving while tripped/pending is blocked; ordinary Save stores configured settings, not un-applied edits or emergency hardware values.
- Shared polling read counts, tray/visible transitions, unrelated telemetry failures, subscriber exceptions, and no work while disabled/unheld. Peak/status/history introduce no reads or timers.
- Peak lifecycle, invalid current readings, truthful historical trip records, pending/apply/save indicators, and Test disabled for unresolved trips.

Extend the existing fake EC to inject read/write/verification failures. Include actual persistence and relevant integration logic in tests; the original suite links the controller/guard but not `MainForm`, `ProfileBar`, or the real settings manager. Extract only the minimal non-UI decision logic needed for deterministic tests.

### Builds And UI Checks

The reviewed source built with two pre-existing `CS8602` warnings in MainForm and no errors. The original hardware-free suite passed 16/16; that did not cover the defects above. Reestablish this baseline in the current environment.

```text
dotnet run --project tests/GpuFailsafe.Tests
dotnet build MechControl/MechControl.csproj -c Release
```

For compile-only Linux verification, the Windows-targeted build additionally needs `-p:EnableWindowsTargeting=true`; this is not Windows runtime verification. Use the repository's Windows `MechControl/build-scripts/build-release.ps1` for actual release packaging. Follow the local AGENTS workflow and verify synced source hashes before remote builds; do not assume an old absolute repository path still applies.

Run the packaged alert regression following `notes/gpu_temperature_failsafe.md`. It briefly shows warnings and plays sound but must not initialize hardware. Retain and extend it for the new alert/window choices. A successful development build is not proof the stripped self-contained package works.

For screenshot validation, use fake/sample states or a hardware-free UI harness where feasible. Capture disabled, armed, pending edits, applied-unsaved, verified trip, incomplete trip, reset-pending, and unsupported states. Compare against Jon's reference at matching size/DPI, including the description paragraph, slider, checkboxes, status rows, and buttons. Refresh/read paths can access hardware, so do not invoke the full application assuming it is a harmless visual mockup.

Hardware validation is separate and requires Jon's approval. Verify actual model-specific GPU reduction, optional CPU limits, onboard fan-boost response, tray alerting, and sleep/resume under supervised conditions with workloads stopped first. Do not deliberately heat the GPU to exercise the trigger. Report what was and was not verified.

### Definition Of Done

- The tab closely matches the supplied preferred screenshot, with the target description/presentation and all added controls.
- Added fan/CPU/window options work, persist per profile, and are gated by model capability.
- The durable trip and acknowledged hold cannot be bypassed by profile changes, reset/restart, EC recovery, partial failures, or unrelated writes.
- Status never confuses requested actions with verified actions, or acknowledgement with power restoration.
- Applied/unsaved feedback, warning colors, and Test-button behavior are fixed.
- The existing shared monitor is still used with no additional recurring poller.
- Tests/builds pass without new warnings; packaged and visual results are documented honestly.
- No unrelated changes reverted, no live deployment without permission, and no dependency on another repository.
