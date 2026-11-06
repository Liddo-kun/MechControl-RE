# MechControl Native

A Windows x64 C++20 replacement for the C# MechControl app, built without .NET or
WinForms. It controls MECHREVO laptops through the OEM EC driver (fans, power limits,
thermal failsafe, battery, lighting, NVIDIA overrides and the BLE liquid cooler).

## Reverse-engineering material and the C# app

`re/` holds the hardware knowledge and evidence; `re/README.md` lists it. Read
`re/register_reference.md` first for EC work. It is the single source of truth for EC
registers, encodings, write sequences and the HID protocol. `re/decompilation/` is
local only: never commit decompiled OEM code, because the repository is public.

The C# app this replaced is on the `legacy-csharp` branch, and tag `v1.6.0` marks its
last release. "The reference" in code comments means that app. Read its source with
`git show v1.6.0:<path>`, for example `MechControl/Features/FanControl.cs` or its
`architecture.md`. Historical notes can be stale. Verify claims against current source.

## Goals

- Keep packaging light, memory use low and idle overhead minimal. Measure results
  instead of assuming native means faster.
- Preserve existing features and configuration semantics. Do not add features
  without asking Jon.
- Write simple, maintainable code. Do not add generic frameworks, speculative
  abstractions, redundant guards or compatibility machinery without a concrete need.
- Use native Windows controls, the Windows SDK, C++/WinRT for BLE and the pinned
  nlohmann/json dependency. Keep static runtime linking (`/MT`) consistent.
- Tests should cover consequential regressions or hard-to-observe behavior. Prefer
  the existing behavioral fixtures and real workflow checks over many small unit tests.

## Model compatibility

Always consider other models when changing code. Do not write code that only fits
Jon's test machine (Intel IDY_7Y, baseboard `YAOSHI Series-X6AR55xY`).

- **Detection** (`src/profiles.cpp`): WMI baseboard against profile `baseboardPatterns`
  (longest match wins), then EC 0x740 direct `projectIdValue` match, then shifted
  match (`raw >> 1`), then baseboard against profile filename, then manual selection.
  Every automatic step skips profiles whose `cpuVendor` contradicts CPUID.
- Baseboard patterns go first because Intel and AMD project IDs collide (X6FR5xxY AMD
  and IDY_7Y both report shifted ID 13). `X6FR5xxY.json` resolves this.
- Per-model features, registers, encodings and limits come from the profile JSON in
  `Profiles/MECHREVO` or from runtime detection (ECSpec, support bytes, CPU vendor).
  Never hardcode values that belong in one of those places.

## Safety

- EC writes can brick the machine. Never write a register without verified ground truth,
  and do not infer new register semantics.
- All writes go through the central WriteGuard in `src/ec_session.*` (allowlist,
  clamping and verification).
- Use read-modify-write for bitfield registers and keep model-derived encodings and limits.
- EC reads and other hardware checks are allowed. Never run two hardware-controlling
  instances at once. `MechControl.exe --diagnose <file.json>` is read-only and may run
  beside the installed app.

## Build and test

Put temporary work and build directories in `C:/Temp/tandem`. See `docs/build.md`.

```powershell
./tools/build-native.ps1 -Configuration Release -WithCli -Check
```

`-Check` runs the CTest fixtures, which use synthetic hardware. Do not claim physical
hardware coverage from them. `ui-pages-check.exe --ui-check <fresh directory>`
exercises the real pages with fake hardware on an interactive desktop.
`docs/architecture.md` describes component ownership and threading.

## Deployed app

- The `MechControl` scheduled task (logon trigger, highest privileges) starts the
  installed copy. Its action names the install directory. If the task is ever recreated,
  set its execution time limit to `PT0S`, because the 72-hour default stops the app.
- When Jon asks to deploy, run the checks relevant to the change, then run
  `./tools/deploy.ps1` from an elevated shell. It builds the CLI Release, closes the app,
  copies the runtime files over the installed copy and restarts it through the task.
  Settings, slots, peripheral state and logs stay in place. Do not add backups, status
  snapshots or deployment notes.
- After deploying, check `mechcontrol.log` beside the executable for `[OK]` EC init
  steps, the applied slot and a sane fan table. For problems after sleep, search for
  `EC activation lost` and `Re-applying slot`. `Get-Counter '\MechControl\CPU Fan RPM'`
  shows that the hidden monitor is running.
- `mcctl.exe` in the install directory controls the running app from an elevated shell:
  `mcctl status --json`, `mcctl gpu --nvvdd 240`, `mcctl help`. Its changes are not saved.
- `docs/handoff.md` covers only switching between the C# app and the native app.
- The OEM `UWACPIDriver` is the only OEM component required. A backup is at
  `D:\BU\UWACPIDriver-backup\` (`pnputil /add-driver UWACPIDriver.inf /install`).

## Documentation

Keep `docs/architecture.md` and `docs/build.md` accurate when a change affects them.
Keep documents short and current: replace stale statements instead of appending history.
Raw logs, transcripts and measurements belong in `C:/Temp/tandem`, not the repository.
