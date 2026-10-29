# MechControl Project

## Documentation (read in this order for new sessions)

- `register_reference.md` — **Read first for feature work.** Single source of truth for all EC registers, encodings, write sequences, and HID protocol.
- `architecture.md` — App architecture, component design, and build instructions.
- `report.md` — RE background: system info, OEM architecture, methodology, project IDs, safety rules.
- `handoff_code_review.md` — 2026-07 full-app review. **As of 2026-07-18 all findings (A–E) are implemented** except the MainForm startup-pipeline extraction (skipped by choice). Still worth reading for context, file:line background, and the verification recipe.
- `notes/practical_safety_simplification_plan.md` — 2026-09-08 implementation handoff: phases 1-6 implemented, headlessly verified and built, not deployed. Covers 50 C lower-bound recovery, profile loading/saves, model-limit enforcement and redundant safety work. Supersedes the older failsafe plan only where explicitly stated; results in `notes/thermal_failsafe_validation.md`.

# IMPORTANT!! WHEN MAKING ANY CHANGES, FIXING BUGS, OR ADDING FEATURES, KEEP IN MIND WE WANT TO CONSIDER COMPATABILITY WITH OTHER MODELS.  For example,  how is model detection done to choose which profile to load at startup? How do we know which features apply to that model? Don't make code changes that appear specific to my machine that we're testing on.

Answers to the above (verify in `Profile/ProfileLoader.cs` if code has moved on):
- **Detection order** (`ProfileLoader.DetectAndLoad`): EC 0x740 direct `projectIdValue` match → shifted match (`raw >> 1`) → WMI baseboard vs. profile `baseboardPatterns` (longest match wins) → baseboard vs. profile filename (token match) → manual picker. Real baseboards (e.g. `YAOSHI Series-X6AR55xY` = IDY_7Y) don't contain profile names — `baseboardPatterns` in the profile JSON is the reliable WMI mapping; add patterns as field logs come in.
- **Per-model behavior** flows from the profile JSON (registers, limits, encodings, TjMax, allowlist) + runtime `HardwareDetection` (support bytes, ECSpec limits, PL4 double flag, Intel/AMD temp encoding). Never hardcode values that belong in one of those two places.
- Known trap: ProjectID collisions exist (X6FR5xxY AMD matches IDY_7Y via shifted ID 13). See `notes/other_machines.md`.

## Dev Workflow (tablet edits → Windows build)

- **Edit here** (`~/code/Mechcontrol` on the tablet). Syncthing mirrors to `C:\Users\Jon\code\Mechcontrol` on the Windows build PC. Never hand-copy files over the `~/win` SMB mount (sync-conflict risk).
- **Syncthing caveat:** the tablet-side watcher does NOT see Ubuntu writes (FUSE bypass); tablet→PC sync only happens on a periodic rescan (~30 min) or Jon pressing "Rescan All". Before building, **verify changed-file hashes match over SSH** (`Get-FileHash`) — the `~/win` mount shows stale cache and can't be trusted for verification. If sync is stalled, ask Jon to hit Rescan All.
- **Build (over SSH):** `powershell -ExecutionPolicy Bypass -File C:/Users/Jon/code/Mechcontrol/MechControl/build-scripts/build-release.ps1` — autodetects dotnet (SDK 8 per-user at `C:\Users\Jon\.dotnet`), produces framework-dependent + trimmed self-contained under `MechControl\bin\`. Expect **0 errors and exactly 2 pre-existing CS8602 warnings** (MainForm.cs FormClosing area); anything else is yours.
- **Trim compat:** the self-contained build is trimmed (`ILLink.Descriptors.xml`, source-generated JSON in `AppJsonContext.cs`). Keep new `System.Text.Json` usage on the context; check trim compatibility when adding reflection.
- PowerShell-over-SSH gotchas (no `&&`, scp a `.ps1` for anything non-trivial) are in the global agent notes.

## Deployed App (Jon's machine, IDY_7Y)

- Lives at `C:\Program_Files\MechControl`, launched by Scheduled Task `MechControl` (logon trigger, RunLevel Highest). **If the task is ever recreated, set ExecutionTimeLimit to PT0S** — the 72h default kills the app after exactly 3 days.
- **Redeploy:** stop task + process → back up deploy dir to `C:\tmp\MechControl-backup-<stamp>` → swap in `bin/selfcontained` contents, **preserving user data**: `settings.json`, `lighting_state.json`, `liquid_cooler_state.json`, `userprofile_*.json`, `mechcontrol.log` → start task. A helper doing all of this exists at `C:\tmp\deploy.ps1` (recreate from this recipe if gone).
- **Verify after deploy** in `mechcontrol.log`: EC init steps `[OK]`, `0x741 transformed`, validations PASS, `Auto-loaded user profile slot N`, sane fan table dump. Perf counters prove the tray-hidden monitor is alive: `Get-Counter '\MechControl\CPU Fan RPM'`.
- Post-sleep issues: grep the log for `EC activation lost` / `Re-applied user profile` (EC re-activation watchdog).

## Evidence Archive (consult when debugging or doing new RE)

All in `captures/`:
- `frida_capture_annotated_X6AR55xY.md` — Raw annotated Frida capture (EC + HID + WMI) from Intel IDY_7Y system.
- `api_monitor_results.md` — Frida EC-only capture (ground truth for buffer format).
- `dll_decompile_results.md` — ACPIDriverDll.dll disassembly (buffer format proof).
- `ec_trace_decoded.md` — ECSpec register address dump.

## Organized Subdirectories

- `tools/` — RE tool source code (.cs, .py, .js, .ps1). Compile .cs with `csc.exe -nologo <file>.cs`.
- `captures/` — Frida logs, IOCTL dumps, EC snapshots, WMI traces.
- `decompilation/` — Decompiled GCUService/GCUBridge/ACPIDriverDll output. Local only (git-ignored; the repo is public and must not contain OEM code).
- `acpi/` — DSDT/SSDT tables (.aml, .dat, .dsl).
- `notes/` — Secondary RE notes (multi-model state in `other_machines.md`, dimming, ECSpec, startup, battery, etc.).
- `screenshots/` — UI screenshots.

## Project Data

- `MechControl/` — The standalone replacement app (C# .NET 8.0 WinForms).
  - `MechControl/Profiles/MECHREVO/` — **App runtime profiles.** Compact JSONs (~35 lines) with standardized register names (e.g. `cpuFanDuty`). This is what `ProfileLoader` actually loads. One per model. May declare `baseboardPatterns` (WMI baseboard substrings) for detection.
- `Profiles/MECHREVO/` — **RE archive / model onboarding source.** Full OEM-extracted profiles (~400 lines) with complete `ecRegisters` dumps, mode constants, limits, and feature flags per model. **Not loaded by the app**, but essential when adding support for a new model — you map the OEM register names to canonical names to create the compact app profile.
- `UserFanTables-OEM/` — OEM factory fan tables for 24 models (from driver package). Contains per-mode fan curves, PL defaults, active point counts. Critical for multi-model support.

## OEM App Backup

- Lost in a Windows rebuild (searched all drives 2026-07-18). Decompilations/captures in the repo are the surviving record; new RE requires re-downloading Control Center (different version — treat old offsets as stale).
- `UWACPIDriver` (the only OEM piece MechControl needs) is still installed; backup at `D:\BU\UWACPIDriver-backup\` (`pnputil /add-driver UWACPIDriver.inf /install`).

## Safety

- EC writes can brick the machine. Never write to a register without verified ground truth.
- All writes must go through WriteGuard (allowlist + clamping + verify).
- Read-modify-write required for all bitfield registers.

## Compilation (standalone .cs tools in `tools/`)

Use `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe` (C# 5 only).
In Git Bash use `-nologo` not `/nologo` (forward slash gets eaten).
