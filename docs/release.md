# MechControl for Windows x64

MechControl replaces the OEM Control Center on Uniwill-built MECHREVO, XMG and
Eluktronics laptops. Project page: https://github.com/Liddo-kun/MechControl-RE

## Requirements

- Windows 10 version 2004 (build 19041) or later, or Windows 11 (x64).
- The OEM `UWACPIDriver`, which comes with the Control Center. It is not included.
- No .NET or Visual C++ runtime is needed.

Do not run the OEM Control Center services (`GCUService`, `GCUBridge`) or another
hardware control tool at the same time.

## Install

Extract the whole archive into a folder you can write to, and keep the `Profiles`
folder next to `MechControl.exe`. Run `MechControl.exe` and approve elevation. It
starts hidden in the notification area. Use the tray icon to show it. Closing the
window hides it; choose **Exit** in the tray menu to stop it. Settings, the five
profile slots, lighting and cooler state and `mechcontrol.log` are kept next to the
executable.

To start MechControl at logon, create a Task Scheduler task for the full executable
path that runs with highest privileges, has no execution time limit and is allowed
on battery. The project page has a PowerShell command for this.

## Upgrading from the C# version (v1.6.0 or earlier)

This version reads the C# version's settings and profile files. Exit the old version
from the tray, then either extract this archive over the old folder, or copy
`settings.json`, `userprofile_*.json`, `lighting_state.json` and
`liquid_cooler_state.json` into the new folder. If a scheduled task starts
MechControl, make sure it points at the new `MechControl.exe`. The old version's
`.dll` files are no longer used and can be deleted.

## Command line

The `-cli` archive includes `mcctl.exe`. From an elevated shell it reads and changes
CPU and GPU power, NVIDIA current limits, fan boost, the EC power-limit bypass and
the liquid cooler flag on the running app. Run `mcctl help` for the commands. Changes apply immediately but
are not saved.

## Diagnostic report

`MechControl.exe --diagnose report.json` writes a read-only report of the model, EC
power limits, fan tables and sensors, then exits. It can run while MechControl is
running. Attach it, or `mechcontrol.log`, when reporting a problem with your model.

## Fan speed performance counters (optional)

MechControl can publish CPU and GPU fan RPM as Windows performance counters. This
needs a one-time registration from an elevated PowerShell in this folder. Exit
MechControl first, and use a new backup directory. Registration replaces the C#
version's counters if they were registered; the backup can restore them.

```powershell
./counters.ps1 -Action Install -Manifest ./MechControl-counters.man -BackupDirectory C:/Temp/counter-backup
# Start MechControl, then: Get-Counter '\MechControl\CPU Fan RPM','\MechControl\GPU Fan RPM'
# To remove the registration, exit MechControl and run:
./counters.ps1 -Action Remove -Manifest ./MechControl-counters.man -BackupDirectory C:/Temp/counter-backup
# Or, to restore the registration from before Install (including the C# version's counters):
./counters.ps1 -Action Rollback -Manifest ./MechControl-counters.man -BackupDirectory C:/Temp/counter-backup
```

Registration points at this folder. If you move the folder, remove the registration
first and register it again afterwards.
