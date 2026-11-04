# MechControl Native — Windows x64

Extract the entire archive into a writable directory. Windows 10 version 2004
(build 19041) or later is the supported API baseline. The application uses native
Windows controls, C++/WinRT, WMI, PerfLib and system ICU. It requires neither .NET
nor a separately installed Visual C++ runtime. It still requires the compatible
OEM UWACPIDriver and a supported model profile; the driver is not redistributed.
Do not run another laptop hardware controller at the same time.

Run `MechControl.exe` and approve elevation. It starts hidden in the notification
area. Use the tray icon to show it. Closing/minimizing hides it; choose Exit in the
tray menu to stop it. Profiles, settings, saved device state and rotating logs
are relative to the executable, not the shell's current directory. Keep the
`Profiles/MECHREVO` directory intact. Its reference-only capture is deliberately
not a selectable model.

This is a qualification candidate, not a declaration of hardware parity. Physical
cooler connections, GPU ROM/restart/restoration, AMD models, sleep/AC transitions,
clean-host/19041 execution and a greater-than-72-hour soak remain unqualified.
Developer-host checks cannot establish those results.
Keep the functioning release until qualification, daily use and rollback are
proven. The included [handoff procedure](docs/handoff.md), `handoff.ps1` and corrected
`counters.ps1` provide staging/current-state transfer and counter rollback preflight,
not an in-application installer or qualification approval. See also
[architecture](docs/architecture.md) and [build/measurement](docs/build.md).
Do not replace active thermal obligations or shared NVIDIA originals with
pre-cutover settings during rollback. These final candidates supersede the frozen
phase-8 archives for handoff, not their remaining physical/elapsed qualification.

For logon startup, create an interactive Task Scheduler task targeting the full
executable path, with highest privileges, no execution time limit and battery
operation enabled. There is no in-application startup installer. When moving the
directory, update that task and counter registration before removing the old copy.

Optional passive fan counters require explicit registration in an elevated shell.
Stop all publishers first and choose a fresh backup directory. Keep the executable
and manifest together at the registered path:

```powershell
./counters.ps1 -Action Install -Manifest ./MechControl-counters.man -BackupDirectory C:/Temp/tandem/counter-backup
# Start MechControl and use Get-Counter '\MechControl\CPU Fan RPM','\MechControl\GPU Fan RPM'.
# Stop MechControl before removal or rollback.
./counters.ps1 -Action Rollback -Manifest ./MechControl-counters.man -BackupDirectory C:/Temp/tandem/counter-backup
```

The default archive has no CLI client or pipe server. The optional CLI archive
includes `mcctl.exe`; an elevated `mcctl status --json` addresses the active
controller. CLI changes are live and unsaved. `mcctl watch --interval 1000 --json`
stops on Ctrl+C. Never substitute an ordinary second controller for a client.

`MechControl.exe --diagnose C:/Temp/tandem/report.json` collects a write-free EC
report and exits. The output directory must exist; this mode may coexist with a
controller. Shutdown joins synchronous device calls and cannot promise to cancel
a blocked OS/driver call. Separate symbols archives are for developers only.
