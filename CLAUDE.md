# MechControl Project

## Documentation (read in this order for new sessions)

- `register_reference.md` — **Read first for feature work.** Single source of truth for all EC registers, encodings, write sequences, and HID protocol.
- `architecture.md` — App architecture, component design, and build instructions.
- `report.md` — RE background: system info, OEM architecture, methodology, project IDs, safety rules.

# IMPORTANT!! WHEN MAKING ANY CHANGES, FIXING BUGS, OR ADDING FEATURES, KEEP IN MIND WE WANT TO CONSIDER COMPATABILITY WITH OTHER MODELS.  For example,  how is model detection done to choose which profile to load at startup? How do we know which features apply to that model? Don't make code changes that appear specific to my machine that we're testing on.



## Evidence Archive (consult when debugging or doing new RE)

All in `captures/`:
- `frida_capture_annotated_X6AR55xY.md` — Raw annotated Frida capture (EC + HID + WMI) from Intel IDY_7Y system.
- `api_monitor_results.md` — Frida EC-only capture (ground truth for buffer format).
- `dll_decompile_results.md` — ACPIDriverDll.dll disassembly (buffer format proof).
- `ec_trace_decoded.md` — ECSpec register address dump.

## Organized Subdirectories

- `tools/` — RE tool source code (.cs, .py, .js, .ps1). Compile .cs with `csc.exe -nologo <file>.cs`.
- `captures/` — Frida logs, IOCTL dumps, EC snapshots, WMI traces.
- `decompilation/` — Decompiled GCUService/GCUBridge/ACPIDriverDll output.
- `acpi/` — DSDT/SSDT tables (.aml, .dat, .dsl).
- `notes/` — Secondary RE notes (dimming, ECSpec, startup, battery investigation, etc.).
- `screenshots/` — UI screenshots.

## Project Data

- `MechControl/` — The standalone replacement app (C# .NET 8.0 WinForms).
  - `MechControl/Profiles/MECHREVO/` — **App runtime profiles.** Compact JSONs (~35 lines) with standardized register names (e.g. `cpuFanDuty`). This is what `ProfileLoader` actually loads. One per model.
- `Profiles/MECHREVO/` — **RE archive / model onboarding source.** Full OEM-extracted profiles (~400 lines) with complete `ecRegisters` dumps, mode constants, limits, and feature flags per model. **Not loaded by the app**, but essential when adding support for a new model — you map the OEM register names to canonical names to create the compact app profile.
- `UserFanTables-OEM/` — OEM factory fan tables for 24 models (from driver package). Contains per-mode fan curves, PL defaults, active point counts. Critical for multi-model support.

## OEM App Backup

- `C:\Users\Jon\clean\ControlCenter-V1` — Copy of the installed OEM app. All our Frida captures, DLL disassembly, and profile extraction were done against this version. DO NOT DELETE.

## Safety

- EC writes can brick the machine. Never write to a register without verified ground truth.
- All writes must go through WriteGuard (allowlist + clamping + verify).
- Read-modify-write required for all bitfield registers.

## Build

```powershell
cd C:\Users\Jon\clean\MechControl
pwsh -File build-scripts\build-release.ps1
# Exes (run elevated):
#   bin\Release\net8.0-windows10.0.19041.0\MechControl.exe (needs .NET 8)
#   bin\selfcontained\MechControl.exe (standalone)
# Zips: bin\release-zips\MechControl-release.zip (~0.7 MB)
#       bin\release-zips\MechControl-selfcontained.zip (~14 MB)
#       bin\release-zips\MechControl-selfcontained.7z  (~10 MB, only when 7-Zip is installed)
```

## Compilation (standalone .cs tools in `tools/`)

Use `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe` (C# 5 only).
In Git Bash use `-nologo` not `/nologo` (forward slash gets eaten).
