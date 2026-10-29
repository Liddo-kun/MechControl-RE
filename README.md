# MechControl

An open-source replacement for the OEM Control Center on Uniwill-built gaming laptops sold as **MECHREVO**, **XMG** and **Eluktronics**. It talks to the laptop's embedded controller (EC) directly, so you don't need the Control Center app or its background services.

> [!WARNING]
> MechControl writes to the EC, the chip that runs your fans and power limits. Use it at your own risk. Only the MECHREVO IDY_7Y (board `X6AR55xY`) has been tested on hardware. Other models load a profile built from OEM data. At startup MechControl checks each feature's registers and turns off any feature that doesn't pass. The reasons are written to `mechcontrol.log`.

## Features

- **CPU power:** PL1, PL2, PL4 and the CPU temperature limit
- **GPU power:** TGP and Dynamic Boost. An optional NVIDIA unlock raises the power ceiling and lets you change the GPU current limits.
- **Fans:** boost, custom fan curves, linked CPU/GPU fans, fan ramp speed
- **Thermal failsafe:** holds the GPU at minimum power if it stays too hot
- **Battery:** charge modes and battery info
- **Lighting:** keyboard, light bar, logo, idle timeout and mini-LED local dimming
- **Bluetooth liquid cooler:** fan, pump and RGB
- **Profiles:** 5 slots. The last one used loads at startup. You can switch profiles from the tray icon.
- **Monitoring:** live temperatures, fan speeds and power. Fan RPM is also published as Windows performance counters.

## Requirements

- Windows 10 or 11 (x64). MechControl must run as Administrator.
- `UWACPIDriver`, the Uniwill driver that comes with the OEM Control Center. MechControl needs only this driver, not the app.
- Either the [.NET 8 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/8.0) or the self-contained download, which needs nothing extra.

## Install

1. Download the latest build from [Releases](https://github.com/Liddo-kun/MechControl-RE/releases/latest).
   - `MechControl-selfcontained.zip` or `.7z`: works without installing anything else
   - `MechControl-release.zip`: smaller, but needs the .NET 8 Desktop Runtime
2. Extract it to a folder you can write to. Settings, profiles and the log are kept next to `MechControl.exe`.
3. Stop the OEM Control Center services (`GCUService`, `GCUBridge`), or they may overwrite your settings. MechControl warns you if they're running.
4. Run `MechControl.exe`. It starts in the system tray.

On first run, MechControl sets a few OEM UEFI flags so the BIOS prepares the EC at boot the same way it does with the Control Center installed.

### Start with Windows

Create a scheduled task from an elevated PowerShell (change the path):

```powershell
$action    = New-ScheduledTaskAction -Execute 'C:\MechControl\MechControl.exe'
$trigger   = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
$settings  = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask MechControl -Action $action -Trigger $trigger -Principal $principal -Settings $settings
```

`-ExecutionTimeLimit` matters: without it, Windows stops the task after 3 days.

## Supported models

Each model has a profile in [`MechControl/Profiles/MECHREVO/`](MechControl/Profiles/MECHREVO/). At startup MechControl picks the right one from the motherboard name, the EC project ID and the CPU vendor. If none matches, it asks you to choose. If your model loads the wrong profile or features are turned off, please open an issue and attach `mechcontrol.log`.

## Building

You need the .NET 8 SDK. Then run:

```powershell
powershell -ExecutionPolicy Bypass -File MechControl/build-scripts/build-release.ps1
```

The release zips are written to `MechControl/bin/release-zips/`.

## Documentation

- [`register_reference.md`](register_reference.md): EC registers, encodings and write sequences
- [`architecture.md`](architecture.md): how the app is put together
- [`report.md`](report.md): reverse-engineering background

MechControl isn't affiliated with MECHREVO, XMG, Eluktronics or Uniwill.
