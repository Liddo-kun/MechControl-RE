# Practical Safety And Simplification Handoff

Date: 2026-09-08. Status: phases 1-6 implemented and headlessly verified; built,
not deployed. The original audit/starting context below describes the baseline.

## Implementation Result (2026-09-08)

- Phases 1-2: independent operational recovery, 50-95 C/default 50 migration,
  truthful requested-slot failures/warnings, model identity preflight, atomic
  profile/settings saves and explicit partial-save handling implemented.
- Phases 3-5: effective model/encoding limits, DB sequence consolidation,
  once-per-operation emergency CPU temperature choice, complete GPU/fan/CPU
  maintenance readback, fresh-check re-arm and actions-before-flush implemented.
- Phase 6: shared synchronous linked UI operation, recursion-free failure repair,
  truthful pending/applied/saved state and starter-curve copy implemented.
- Direct-readback skip/redirect exceptions retain full guarded writes; no new
  hardware semantics were inferred. Unsupported known holds remain unresolved.
- Regression suite: 38/38 safety tests; expanded real ProfileBar/FanTab headless
  UI tests passed. Release packaging: zero errors, the two existing CS8602 warnings.
  Shipped-runtime source-generated serialization/migration/atomic-save smoke
  passed with `tests/packaged-alert-smoke.ps1 -Headless` (no alerts/sound/hardware).
- No commit, deployment, installed-file migration, live trip or sleep test.
  Detailed results, limitations and code-size accounting are appended to
  `notes/thermal_failsafe_validation.md`. Historical validation is unchanged.

## Goal

Implement the practical-safety review without weakening protections backed by real
hardware behavior. Remove redundant checks and inappropriate restrictions, fix
actual profile/limit handling gaps, and simplify the affected code. Do not build
another generic safety framework to do it.

Jon's direction:

- Look for "safety" additions with little real-world benefit, not just performance
  micro-optimizations.
- Also look for simplification and code-quality improvements.
- Jon subsequently requested 50 C instead of the previously discussed lower bound.
  This plan uses exactly 50 C as the configurable lower bound, retaining 95 C
  as the upper bound and using 50 C as the default. This is a product-use decision,
  NOT a claim that 50 C is a universal hardware safety limit.
- Write a plan that a fresh session can implement. This document supplies the
  proposed implementation policy; distinguish it from already-shipped behavior.

## Starting Context

Read `AGENTS.md`, then `register_reference.md`, `architecture.md`, and
`notes/gpu_temperature_failsafe.md`. Consult `notes/thermal_failsafe_validation.md`
for historical validation and the immediately preceding overhead work.

The preceding implementation and this handoff are being checkpointed before the
next session's work. Inspect `git status` and recent commits first: if the worktree
is clean, HEAD is the baseline for implementing this plan. Preserve any newer local
changes; do not reset them or commit unless asked. If the session starts dirty,
snapshot relevant files in `C:/Temp/tandem` before measuring its changes, since a
diff against HEAD would then include earlier work too.

Already implemented; do not redo:

- Per-profile detection/settings, durable tripped/reset-pending per-action holds,
  guarded restoration, optional CPU minimum/fan boost, and nonmodal alerts.
- Shared one-second monitor for failsafe AND linked fans. GPU safety callbacks
  precede CPU sampling/fan control, which precedes unrelated telemetry. Linked
  mode remains active tray-hidden even with the failsafe disabled.
- Previously verified CPU holds use fresh readback on five-sample maintenance;
  unchanged requests avoid the repeated 450 ms verification sleep.
- Hidden UI guards and separate temperature/state notifications.
- Shared typed subscriber dispatch; linked-fan enable directly applies fresh
  curves, propagates original failures, and subscribes only after success.
- Partial fan-table write failure invalidates the skip state. App exit does not
  rewrite the last firmware-controlled safety-ramp curves.

The four overhead improvements plus their cleanup added 56 net production lines
relative to the pre-overhead worktree, not HEAD. Last results: safety suite 34/34,
headless monitor/fan/UI regressions passed, release packages built with zero errors
and two existing CS8602 warnings in MainForm (then lines 631 and 652).

The candidate is BUILT, NOT DEPLOYED. The older installed app was still running
from `C:/Program_Files/MechControl/MechControl.exe`, last observed PID 23200. Recheck
process identity rather than assuming that PID persists. Do not alter installed
settings, trigger live protection, or interrupt gaming without approval.
Keep verification windowless by default. Work without subagents.

This plan supersedes the older plan's 30 C corrupt-state fallback and its requirement
to flush intent before emergency hardware actions. Other existing contracts remain
unless explicitly changed below. Do not silently rewrite historical validation.

## Evidence And Entry Points

Line numbers describe the audited source and may move.

| Finding | Current entry points | Evidence |
| --- | --- | --- |
| Bad history/unrelated settings manufacture a 30 C trip | `Profile/UserProfileManager.cs:123-177`, `Profile/ThermalProtectionState.cs:44-75`, `Features/GpuTemperatureFailsafe.cs:45` | Hardware-free production loader: both examples below produce Tripped/Gpu/reset threshold 30, requiring a 27 C reading. |
| Ordinary GPU requests ignore the model maximum | `Features/GpuPowerControl.cs:47-75`, `Profile/RegisterEncoding.cs:21-27` | Real controller/guard with fake EC, offset 70/min 80/max 100, accepts and reads back a 125 W request. |
| Failed saved-profile load masquerades as success | `Profile/UserProfileManager.cs:93-114`, `UI/ProfileBar.cs:148-151`, `UI/MainForm.cs:549-555` | A present malformed slot returns null; ProfileBar returns normally; startup then logs success. With no durable hold, the detector can remain unarmed. |
| Emergency CPU temperature limit is read twice | `Features/CpuPowerControl.cs:84-87,109-117` | ApplyMinimumPower supplies a fresh argument which the held-action path immediately replaces with another read. |
| Adjacent GPU checks/validation repeat | `Features/GpuTemperatureFailsafe.cs:115-133,351-353`, CPU/GPU Apply methods, `UI/FailsafeTab.cs:185-189` | Trace Configure -> ApplyExplicit -> feature apply. Some checks have no intervening hardware work; others follow substantial writes and are useful. |
| Unchanged GPU/fan holds and reset force rewrites | `Features/GpuTemperatureFailsafe.cs:186,249-253,307` | CPU-only maintenance fast path exists; GPU/fan still rewrite every five samples, and reset forces all groups. No periodic keepalive requirement was found for these registers. |
| Disk flush precedes power reduction | `Features/GpuTemperatureFailsafe.cs:243-253`, `Profile/UserProfileManager.cs:186-196` | SaveSettings uses Flush(true) synchronously before the emergency action loop. No problematic flush latency was measured on Jon's installation. |

Minimal bad-settings reproducers, each with a readable no-hold operational state:

```json
{"thermalProtection":{"Stage":0,"Held":0,"LastTrip":{"Time":"not-a-date","Reason":"Old diagnostic record"}}}
```

```json
{"lastProfile":"not-a-number","thermalProtection":{"Stage":0,"Held":0}}
```

Scratch fixtures were under `C:/Temp/tandem/thermal-safety-audit/`. They are not a
dependency of this plan; put reproducible cases in repository tests. No MainForm
constructor or real EC was used to reproduce these findings.

## Non-Negotiable Boundaries

- Addresses, encodings and limits come from the detected machine profile plus
  validated runtime hardware/ECSpec data. Respect CPU-only/unsupported models.
- All actual EC writes still go through WriteGuard. Preserve read-modify-write
  masks, allowlists, verified CPU group ordering, CPU commit bit and redirected
  readback. Do not replace the measured redirect backoff with an immediate check.
- Emergency CPU writes preserve a freshly read valid CPU temperature limit; do
  not substitute a cached pre-sleep limit or invented generic fallback.
- Keep immediate inclusive trip detection, five invalid samples, fresh checks
  before relevant restoration, and 3 C hysteresis for genuine trips. At a 50 C
  threshold, 47 C is the reset boundary, not 50 C.
- Keep actual outstanding holds across restart, partial failures and failed
  durable releases. Reset acknowledges; it does not automatically restore power.
- Keep epoch invalidation, per-action completion, and in-process write gates.
  Do not assume UI-thread code cannot re-enter: native/COM UI work can pump
  messages, and watchdog invalidation can occur on another thread.
- Do not delete safeguards simply because a synthetic test seems artificial.
  Conversely, tests asserting the old 27 C recovery policy must be changed rather
  than treated as evidence that the policy is desirable.

## Implementation Order

### 1. Separate Recovery From Diagnostic Metadata

Files: `UserProfileManager`, `ThermalProtectionState`, `GpuFailsafeSettings`,
`GpuTemperatureFailsafe`, `FailsafeTab`, relevant source-generated JSON context.

Parse operational protection state independently of optional history and unrelated
settings fields. A bad timestamp, diagnostic string or profile name must not
invalidate otherwise readable Stage/Held values. Preserve the existing JSON schema
where practical; do not add a database or a second journal framework.

Proposed recovery policy:

| Input | Required outcome |
| --- | --- |
| No settings files on a clean install | Existing new-install defaults; no invented trip. |
| Valid no-hold Stage/Held, bad history or unrelated setting | No thermal hold; discard/repair only bad optional data and report configuration warnings. Do not claim a profile loaded if its identity cannot be recovered. |
| Valid held Stage/Held, bad history | Preserve the actual stage and all known action obligations. Missing history is diagnostic, not a new overheating event. |
| Valid committed/pending operational states | Keep conservative merging for unfinished operations; a pending release must not erase committed obligations. Different historical timestamps alone must not escalate otherwise valid operational states. |
| Operational state genuinely unreadable/inconsistent | Retain an explicitly identified configuration-recovery hold: preserve recoverable action bits and the existing GPU fallback where supported, with a usable reset boundary. Do not invent CPU/fan obligations without evidence. |
| Legacy GPU-only trip fields | Migrate as GPU-only holds, preserving the actual history/reason where readable. |

For unknown recovery thresholds, use the supported 50 C default, not the minimum
of an obsolete UI range. Keep the recovery warning distinct from a measured thermal
event; no fabricated temperature/time. Reuse existing state/diagnostic mechanisms
where possible instead of adding a parallel state machine. Unsupported emergency
actions must remain visibly unresolved, never be silently reported as applied.

Centralize the 50-95 range. Update duplicated literal bounds in recovery/salvage,
validation, UI, error text, tests and docs together. For older otherwise-valid
30-49 C profiles/operational reset thresholds, migrate to 50 C with a clear warning,
preserving Enabled and held-action bits. Historical recorded thresholds remain
historical facts. Treat invalid types/values outside the old supported range as
configuration errors, not arbitrary clamps to seemingly valid settings.

Do not mass-edit installed files or turn previously disabled detection on during
migration. A valid existing hold must never disappear because its optional history
or old low threshold was repaired.

### 2. Fix Profile Loading And Atomic Saves

Files: `UserProfileManager`, `ProfileBar`, MainForm startup/recovery callers.

- Distinguish an intentionally empty slot from a failed load. Prefer simple
  exceptions for corrupt present files and an explicit success result where needed;
  do not create a large result hierarchy.
- Never log/highlight/mark a profile as applied when load or application failed.
  Handle the file-disappeared-between-exists-and-load case as well.
- Surface a persistent, nonmodal startup configuration warning when the requested
  profile cannot load, including that its protection settings were not armed.
  Preserve known runtime/durable protection. Do not invent a high-power fallback
  profile or pretend to know the missing file's enabled/options values.
- Use atomic replacement for user profiles too. A small shared atomic-file writer
  is appropriate because settings and profiles both need it. Keep temporary files
  in the destination directory; use source-generated JSON serialization.
- Only report save success/update selected-slot bookkeeping after the relevant
  save steps succeed. On failure, keep truthful partial/unsaved UI state.
  If the profile replacement succeeds but the later settings save fails, report
  that partial result; do not claim the profile was unwritten or invent a multi-file
  transaction/rollback mechanism just to make both files appear atomic together.
- Do not automatically load an unfinished profile temporary file as authorization
  for new power settings. Preserve the last committed profile on failure.
- Inspect saved ProjectId handling. A mismatch should not silently apply another
  machine's settings; prefer refusal with an actionable message for explicit
  mismatches. Legacy missing IDs need a documented policy and current-model
  validation, not an unconditional new compatibility exception.

### 3. Put Model Limits At The Actual Write Boundary

Files: `CpuPowerControl`, `GpuPowerControl`, `RegisterEncoding`, profile preflight.

- Ordinary application/restoration must honor effective model limits, not merely
  generic byte/encoding clamps or the UI slider's bounds.
- Prefer rejecting out-of-range saved/API requests before writing, rather than
  silently applying a different profile while displaying the original as applied.
  UI sliders should already present valid values.
- Validate every member of a CPU/GPU group before its first write. Preflight known
  invalid loaded-profile power values before starting unrelated profile writes.
- Keep policy bounds in the feature layer and conversion in the encoding layer.
  Do not remove encoding caps in a way that broadens writes beyond verified profile/
  runtime authority. Expose unsupported/unrepresentable ranges rather than falsely
  claiming requested values were applied.
- Preserve/document PL4 quantization on doubled platforms. Check effective encoded
  values against model bounds; do not accidentally reject every odd slider step
  or silently round below an allowed minimum. Emergency minima remain strictly
  representable, as in the current capability checks.
- DB disabled must remain exactly zero in the two DB value registers; never run
  that zero through the existing enabled-DB encoder, which clamps it to at least 5.
- Preserve the distinct deliberate firmware-default path: zero handoff sentinels
  are not ordinary user-requested wattages and must not be accidentally forbidden
  by normal minimum checks.
- Collapse the duplicated DB-on/off write branches into one ordered sequence with
  precomputed values. Keep the same bitfield mask and verification behavior.

### 4. Remove Redundant Reads And Stable Rewrites

Files: CPU/GPU/fan controllers, failsafe controller and configuration call sites.

- Choose the emergency CPU temperature limit once per complete apply operation,
  at the common boundary. Do not replace the double read with a magic temperature
  argument or bypass the existing held-action gates.
- Consolidate settings validation at its authoritative boundary. Eliminate only
  adjacent duplicate fresh GPU checks; retain direct-controller protection and
  fresh checks after slow CPU/fan work and before reducing a held fan action.
  Do not add a global stale-temperature cache to save a few reads.
- Extend the existing same-epoch, previously-verified maintenance check to GPU
  and fan boost if the register reference supports reliable readback. Check the
  complete requested GPU group (masked control bits, TGP and both DB bytes), not
  just reported TGP or an unrelated status mirror. Check the established fan boost
  request register. Do not infer physical watts/RPM from these checks.
- Drift/read failures invalidate confidence; repair with the existing full guarded
  sequence when inputs are valid. Keep bounded retries and no total retry cutoff.
- Initial/restart/recovery enforcement still performs actual verified writes.
- Re-arm may use the same fresh maintenance verification rather than always
  rewriting unchanged groups. Retain its existing cool-reading, completion and
  durable-state requirements; this plan does not authorize restoring unverified
  actions merely because acknowledgement itself does not change hardware.
- Keep forced enforcement for rollback after interrupted/failed restoration.
  If any model lacks proven readback semantics, retain its existing rewrite path
  and document the limitation instead of guessing.

### 5. Lower Power Before Flushing The Trip Record

Files: failsafe enforcement/persistence ordering and persistence tests.

- Latch the in-memory stage/action obligations before any action or notification,
  so ordinary writes remain gated immediately.
- Attempt emergency actions independently, in the existing GPU/fan/CPU order,
  before the synchronous durable write. Record resulting state/errors afterward.
- A failed save keeps persistence visibly incomplete and retryable; it must never
  undo the hold or skip an emergency action. Do not mark failed durable release as
  successful. Keep durable release after verified explicit restoration.
- Document the tradeoff: a crash between hardware reduction and persistence can
  lose the new trip record. Retain fresh startup/profile-apply checks and do not
  claim hardware and disk are one atomic operation.
- This small ordering change does NOT make the UI or alert delivery nonblocking.
  Moving persistence to a worker/adding a journal queue is outside this plan.
  Do not promise bounded disk latency because an exception is caught.

### 6. Simplify UI Orchestration And Claims

- `FanTab.SetLinkedEnabled` and `OnLinkFansChanged` should share one synchronous
  operation; the UI event wrapper handles presentation/errors. Suppress programmatic
  checkbox recursion. Make any desired hardware rollback after failed enable
  explicit, not an accidental second event caused by unchecking the box.
- Preserve original error messages, successful-only enable/subscription, failed
  re-enable cleanup, last-successful written curves and safe disposal behavior.
- Keep pending/applied/saved settings distinct. A configuration warning is not
  evidence of overheating; failed profile load is not successful arming.
- Replace unsupported claims such as "safe on any laptop" for a generic starter
  fan curve. Keep the actual firmware-controlled ramp tails and existing UI warning.
- Do not broaden this into a MainForm rewrite, remove epoch/permit protections,
  or change linked-fan cadence.

## Required Regression Coverage

Extend existing fixtures instead of building a second test framework. Test observable
outcomes, read/write order and effective values, not only private helper structure.

| Area | Minimum cases |
| --- | --- |
| Thresholds | 50/95 accepted; new unsupported values rejected; enabled and disabled legacy low profiles migrate predictably; genuine 50 C trip requires <=47 C reset. |
| Recovery | Both JSON reproducers above stay unheld; actual GPU/CPU/fan holds survive bad optional metadata; committed/pending conflicts preserve obligations; unknown operational state has a usable, clearly identified recovery path; no fabricated measurements. |
| Profile loading | Malformed, missing and disappearing requested slots never report success; existing hold remains active; valid profile arms its configured detector; invalid cross-model requests do not write. |
| Atomic saves | Failed/interrupted profile-file replacement preserves the prior committed profile; unfinished temporary profile cannot authorize an apply; a completed profile replacement followed by settings-save failure is reported as partial, with truthful slot/highlight state. |
| Model limits | Fake offset-70/min-80/max-100 GPU refuses 125 W before writes; model-specific CPU/DB bounds, Intel/AMD encoding, direct/doubled PL4, quantization, unsupported capability and fallback authority; firmware zero handoff remains deliberate and valid. |
| Sampling | One shared sample per needed sensor/tick, tray and visible; GPU safety first; unrelated subscriber/telemetry errors do not stop control; a fresh post-slow-write check still catches a new threshold crossing. |
| Maintenance | Stable complete CPU/GPU/fan requests avoid writes; drift in each relevant register repairs the full affected group; unrelated control bits preserved; stale epoch/read failure cannot pass as verified; reset does not restore power; rollback remains forced. |
| Persistence order | All emergency actions attempted before the save callback, including when one action fails; save failure retains holds; restart/failed release behavior matches the documented durability boundary. |
| Linked UI | One intended enable/disable operation per user action; programmatic checkbox repair does not accidentally write; original exceptions survive; partial-write retry, re-enable cleanup and no-write disposal continue passing. |

Hardware-free Windows commands (per-user SDK is required here):

```powershell
& C:/Users/Jon/.dotnet/dotnet.exe run --project tests/GpuFailsafe.Tests
& C:/Users/Jon/.dotnet/dotnet.exe run --project tests/ThermalUi.Tests -- --headless
powershell -ExecutionPolicy Bypass -File C:/Users/Jon/code/Mechcontrol/MechControl/build-scripts/build-release.ps1 -DotnetPath C:/Users/Jon/.dotnet/dotnet.exe
git diff --check
```

Expect zero build errors and only the two existing CS8602 warnings. The release
script's auto-detection previously chose a runtime-only installation; use the
explicit SDK argument. Transient output-file locks previously needed a retry.
Keep logs in
`C:/Temp/tandem`. Check trimmed serialization and reflection-based test constructors
if DTOs/signatures change.

Native/scaled screenshots and `tests/packaged-alert-smoke.ps1` show windows/play
sound; run those when appropriate with Jon, not during the default headless pass.
No new live trip/sleep/deployment certification is implied by unit tests.

## Completion And Code-Size Accounting

Update `notes/gpu_temperature_failsafe.md`, relevant `architecture.md` sections,
and append results/limits to `notes/thermal_failsafe_validation.md`. Mark which
parts of this plan are complete or blocked by missing hardware evidence.

The cleanup portions should remove production lines. The complete plan also fixes
missing behavior: reliable profile loading, atomic profile saves, model-limit
enforcement and usable recovery. Expect production size roughly flat to modestly
larger overall, not a guaranteed net reduction. Tests will probably grow, so total
repository lines are likely to increase. Reduced duplication and clearer ownership
matter more than a lower raw line count.

Report actual net production/test/documentation changes separately against the
start-of-session snapshot. Include comments/blank lines consistently. Do not quote
the entire dirty Git diff as this pass's size, compress formatting to hit a target,
or remove tests/necessary checks merely to make the count smaller.

Deliver the implemented phases, regression results, build result, remaining limits,
and deployment status. No commit, deployment, installed-file migration or live
hardware test is authorized by this handoff alone.
