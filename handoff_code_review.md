# Code Review Handoff — MechControl

*Written 2026-07-17 by a Claude session with Jon. Status: EC re-activation work is
**done and deployed**; this doc carries the remaining review findings for a fresh
session to pick up.*

## Critical context (read before touching anything)

1. **Two synced source copies.** `~/code/Mechcontrol` (tablet, edit here) and
   `C:\Users\Jon\code\Mechcontrol` (Windows build PC,
   build here). After editing, copy changed files over the `~/win` rclone mount
   and keep the trees identical (`diff -rq` excluding `obj/`/`bin/`).
2. **A previous session reviewed and patched the WRONG source** (a stale copy).
   That copy is gone. Everything in this doc was re-verified against the correct
   source on 2026-07-17. Distrust any older notes that conflict with this doc.
3. **Build:** `powershell -ExecutionPolicy Bypass -File C:/Users/Jon/code/Mechcontrol/MechControl/build-scripts/build-release.ps1`
   (autodetects dotnet; SDK 8 installed per-user at `C:\Users\Jon\.dotnet`).
   Produces framework-dependent + **trimmed self-contained** builds. The trimmed
   build relies on `ILLink.Descriptors.xml` and source-generated JSON
   (`AppJsonContext.cs`) — keep new `System.Text.Json` usage on the context, and
   check trim-compat when adding reflection.
4. **Deployed app:** `C:\Program_Files\MechControl`, launched by Scheduled Task
   `MechControl` (logon trigger, RunLevel Highest). When redeploying from
   `bin/selfcontained`, **preserve user data in the deploy dir**:
   `settings.json`, `userprofile_*.json`, `lighting_state.json`,
   `mechcontrol.log`. Backups of prior deploys live in `C:\tmp\MechControl-backup-*`.
5. **Model compatibility rule (from CLAUDE.md):** no machine-specific hardcoding.
   Model differences flow from profile JSONs (`MechControl/Profiles/MECHREVO/`)
   + EC detection (`HardwareDetection`, ECSpec). Jon's test machine is IDY_7Y.
6. **Safety:** all EC writes go through `WriteGuard` (allowlist + verify).
   Read-modify-write for bitfields. EC writes can brick the machine.

## Completed (do not redo)

### 1. EC re-activation system (problem: "profile apply fails until restart")
Root cause: EC clears activation byte 0x741 (sleep/resume & other events), then
silently ignores PL/fan/GPU writes → `WriteGuard` verify failures. Implemented
(by Jon, reviewed & verified in session):

- `Features/EcActivationWatchdog.cs` — three detection paths, one serialized
  recovery (`TryRecover`): WriteGuard verify-failure hook (immediate),
  `SystemEvents.PowerModeChanged` resume hook (3s settle, off UI thread), 30s
  spot-check timer (1 EC byte read when healthy; deliberately NOT the OEM's
  1.5s 0x7CC keepalive — Jon rejected that). `_lastRecoveryUtc` 10s window lets
  a caller that lost the race to another recovery still retry its write.
- `Driver/WriteGuard.cs` — `VerifyFailureRecovery` hook; `catch (IOException)
  when (!_inRecovery && ...)` + one retry; `EnterRecoveryScope()` (ThreadStatic)
  prevents recursive recovery from init's own writes.
- `Features/EcActivationControl.cs` — `InitializeEc()` is repeat-safe by
  construction in this source (no fan table / 0x7C6 writes; those live in
  FanControl/FanSnapshot).
- `UI/MainForm.cs` — hook wired AFTER startup init (~line 177–184) so boot
  failures surface normally; `OnEcReactivated` → `ReapplyAfterReactivation`
  (~line 582–639): re-applies active/last-used profile slot, else validates EC
  fan table via `FanSnapshot` and rewrites from panels only if garbage.
  Watchdog disposed before `DiagnosticLog.Close()` in FormClosing.
- `skipVerify` in profiles covers 0x741/handshake regs so init can't
  self-trigger recovery.
- Recovery events are logged with detection path — grep `mechcontrol.log` for
  `EC activation lost` to learn what actually causes drops in the wild.

### 2. Task Scheduler fix (problem: "app randomly not running")
Root cause found: task had default `ExecutionTimeLimit PT72H` → Windows killed
the app after exactly 3 days; logon-only trigger meant nothing restarted it.
Set to `PT0S` on 2026-07-17. Battery conditions were already off. **If the task
is ever recreated, disable the 72h limit again.** (RestartCount is still 0 —
optional hardening: add restart-on-failure.)

### 3. Environment repairs
- .NET 8 SDK reinstalled at `C:\Users\Jon\.dotnet` (Windows was rebuilt at some
  point; the rebuild also deleted `C:\Users\Jon\clean\` including the
  `ControlCenter-V1` OEM backup). Full-drive search 2026-07-18 confirmed the
  OEM binaries are gone everywhere — see the updated "OEM App Backup" section
  in CLAUDE.md for what survives (decompilations, 2019 Eluktronics installer,
  and a fresh `UWACPIDriver` backup at `D:\BU\UWACPIDriver-backup\`).
- Deployed the correct-source trimmed build 2026-07-17 23:52; startup log clean.

### 4. Already fixed in the correct source (were findings against the wrong copy)
- `DiagnosticLog`: thread-safe (`_sync` lock), `append: true`, session rotation
  (keeps first session + 5 recent).
- `WriteGuard` redirect verify: empirical backoff table instead of naive sleeps.

## Open findings (the actual handoff)

Ordered by value. Line numbers verified 2026-07-17.

### A. Crash-proofing bundle (high value, small diff) — `Program.cs`
Still missing in `AppMain()`:
1. **Global exception handlers.** No `Application.ThreadException`,
   `AppDomain.CurrentDomain.UnhandledException`, or
   `Application.SetUnhandledExceptionMode`. Any unhandled UI-event exception can
   kill the tray-hidden app invisibly. Handlers should `DiagnosticLog.Error`
   with stack trace; ThreadException can show a dialog and continue.
2. **Single-instance guard.** If the task fires while an instance runs, the new
   instance dies ugly: `DiagnosticLog.Start()` throws (log file locked —
   StreamWriter shares Read only), and a second instance would fight over the
   EC and the PerfCounter category (`EnsureCategory` deletes/recreates).
   Named `Mutex` (e.g. `Global\MechControl`) at top of `AppMain`; on conflict,
   exit quietly (optionally signal the running instance to show its window).

### B. TjMax hardcoding (cross-model correctness) — `Features/CpuPowerControl.cs:41–42,68`
`ApplyCpuPower`/`ReadCpuPower` call `RegisterEncoding.EncodeCpuTempLimit(tempC)`
/ `DecodeCpuTempLimit(raw)` with the **default tjMax=105** instead of the
profile's `TjMax`; encode also hard-clamps 80–95 (`RegisterEncoding.cs:32–34`)
regardless of `profile.Limits["cpuTempLimit"]`. A TjMax=100 model gets limits
off by 5°C. Note the newer `_absoluteTemp`/`EncodeCpuTempLimitDirect` path is
handled; only the offset path is wrong. `EcMonitor` already passes profile
TjMax — mirror that. Pass profile into `CpuPowerControl` (it already receives
it in ctor) and clamp from profile limits.

### C. Baseboard profile matching (cross-model detection) — `Profile/ProfileLoader.cs:178–187`
`FindProfileByBaseboard` = raw `boardLower.Contains(filename)`. Two-letter
profile names (`GI`, `GJ`, `PF`…) false-match any baseboard containing those
letters, and `Directory.GetFiles` order makes ties nondeterministic. This is
fallback #2 after EC 0x740 detection, so a wrong match is possible on models
whose 0x740 has no profile. Fix: require token/word-boundary match; prefer the
longest match; log candidates. (Detection order & methods documented at top of
`ProfileLoader.DetectAndLoad`.)

### D. Smaller correctness nits
- `Monitoring/EcMonitor.cs:99` — `Start()` doesn't reset `_consecutiveFailures`;
  after a 5-failure auto-stop, reopening the window can insta-stop again and
  re-fire the `Stopped` MessageBox (which `MainForm.cs:154` shows even when the
  form is tray-hidden). Reset counter in `Start()`; route `Stopped` to
  DiagnosticLog + tray balloon instead of MessageBox.
- `Driver/EcDevice.cs:38–68,136` — `ReadByte`/`WriteByte` ignore `_disposed`; a
  late timer tick after dispose makes `TryReconnect` silently reopen the driver
  handle (leak). Also the blanket catch-retry re-issues **writes** after
  reconnect (double-write; benign for these registers but sloppy) and discards
  the original exception. Check `_disposed` (throw ObjectDisposedException),
  and log the swallowed first error.
- `Driver/HidDevice.cs:260` — usage-based zone mapping uses first-wins
  `byUsage.TryAdd` over unsorted enumeration; two interfaces sharing usage
  0x0001 → arbitrary keyboard pick. Sort `lightingDevices` by path before
  mapping (the fallback path at :283 already sorts). Also: HID handles are
  never reopened after sleep/device re-enumeration — failures only log. Optional:
  reopen-on-failure.
- `Driver/WriteGuard.cs:112–132` — redirect verify sleeps up to ~1.5s (×2 with
  recovery retry) **on the UI thread**; used by cpuTempLimit (0x786→0x463
  redirect in IDY_7Y.json). Acceptable today; worth async-ifying if profile
  apply ever feels frozen.
- `EcActivationWatchdog.Reactivated` fires while `_recoveryLock` is held —
  current subscriber only does `BeginInvoke` (fine); add a doc comment warning
  future subscribers not to block or write EC synchronously.

### E. Structural (only with Jon's buy-in)
- `UI/MainForm.cs` is a 971-line god-constructor (EC I/O, detection, dialogs,
  init). Extract a startup pipeline; also lets the tray icon appear instantly.
- `EcMonitor` + `PerfCounterPublisher` both read the same fan RPM bytes on
  separate 1s timers; merging into one always-on monitor would drop duplicate
  EC traffic and give the watchdog richer context for free.
- Scattered `Console.WriteLine` in a WinExe goes nowhere (notably `[HID]`
  probing in `HidDevice.Open` and profile-detection lines in `ProfileLoader`) —
  convert to `DiagnosticLog.Write` where diagnostically useful.

## Verification recipe

1. Build: run `build-release.ps1` over SSH (expect only the pre-existing
   CS8602 warning at `MainForm.cs` FormClosing lambda).
2. Deploy: swap `bin/selfcontained` into `C:\Program_Files\MechControl`
   preserving the user-data files; `Stop-ScheduledTask`/`Start-ScheduledTask`.
3. Check `mechcontrol.log`: EC init steps `[OK]`, `0x741 transformed`,
   validations PASS, `Auto-loaded user profile slot N`, post-write fan table
   dump sane.
4. For watchdog changes: sleep/resume the machine and grep the log for
   `EC activation lost` / `Re-applied user profile`.
