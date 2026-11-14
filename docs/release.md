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

## Upgrading from the C# version (v1.6.0)

This version reads the settings and profile files that v1.6.0 writes. Exit the old
version from the tray, then either extract this archive over the old folder, or copy
`settings.json`, `userprofile_*.json`, `lighting_state.json` and
`liquid_cooler_state.json` into the new folder. If a scheduled task starts
MechControl, make sure it points at the new `MechControl.exe`. The old version's
`.dll` files are no longer used and can be deleted.

Files last saved by a C# release older than v1.6.0 may not carry over. A profile slot
with a failsafe trigger below 50 C does not load and has to be saved again, and a
thermal trip recorded in the older format is not restored.

## Battery charge limit

**Battery > Charge Limit** stops charging at the level you choose and resumes five points
below it. It is offered where the EC firmware is recognized as having a charge hold. The
EC keeps charging blocked for up to a week while MechControl is not running, so the limit
holds while the laptop is off and plugged in. Unplugging the adapter lifts the block. The limit
is software-controlled, so in rare cases the battery will not charge while MechControl is
not running, such as when the laptop is put into deep sleep while plugged in, then
unplugged, allowed to drain, and plugged in again without being woken. A
laptop that was running on battery and is plugged in while it is off charges to full,
because nothing is running to stop it. One plugged in while asleep may do the same. Set the limit to
**Off** before you remove MechControl; otherwise charging stays blocked until that week
runs out.

## Display-off wake crash workaround

**Power > Display-off wake crash workaround (dGPU mode)** works around an NVIDIA driver fault
where the display may not come back (driver reset, black screen, sometimes a bluescreen) if
the GPU wakes just after Windows powered it down with the screen off. While the screen is
off and the PC is awake, MechControl gives the GPU a tiny piece of Direct3D work every 3
seconds. It acts only while the NVIDIA GPU drives the laptop screen directly (hybrid mode
off) and stops when the PC sleeps. It costs the GPU's idle power while the screen is off.
It is off by default.

## Command line

The `-cli` archive includes `mcctl.exe`. From an elevated shell it reads and changes
CPU and GPU power, NVIDIA current limits, fan boost, the EC power-limit bypass and
the liquid cooler flag on the running app. Run `mcctl help` for the commands. Changes apply immediately but
are not saved.

## Diagnostic report

`MechControl.exe --diagnose report.json` writes a read-only report of the model, EC
power limits, fan tables, sensors and the EC's memory, then exits. It can run while
MechControl is running. Attach it, or `mechcontrol.log`, when reporting a problem with
your model. The first session in the log records the EC's memory too.

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
