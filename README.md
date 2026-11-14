# MechControl

An open-source replacement for the OEM Control Center on Uniwill-built gaming laptops sold as **MECHREVO**, **XMG** and **Eluktronics**. It talks to the laptop's embedded controller (EC) directly, so you don't need the Control Center app or its background services.

MechControl is a native C++ Windows app. It needs no .NET runtime and keeps its memory use and background overhead low. The original C# version is on the [`legacy-csharp`](https://github.com/Liddo-kun/MechControl-RE/tree/legacy-csharp) branch; releases up to v1.6.0 are that version.

> [!WARNING]
> MechControl writes to the EC, the chip that runs your fans and power limits. Use it at your own risk. Only the MECHREVO IDY_7Y (board `X6AR55xY`) has been tested on hardware. Other models load a profile built from OEM data. At startup MechControl checks each feature's registers and turns off any feature that doesn't pass. The reasons are written to `mechcontrol.log`. Send this log if you want to improve compatibility for your model.

## Features

- **CPU power:** PL1, PL2, PL4 and the CPU temperature limit
- **GPU power:** TGP and Dynamic Boost. An optional NVIDIA unlock raises the power ceiling and lets you change the GPU current limits.
- **Fans:** boost, custom fan curves, linked CPU/GPU fans, fan ramp speed
- **Thermal failsafe:** holds the GPU at minimum power if it stays too hot
- **Battery:** charge modes and battery info
- **Lighting:** keyboard, light bar, logo, idle timeout and mini-LED local dimming
- **Bluetooth liquid cooler:** fan, pump and RGB
- **Advanced (Power tab):** EC power-limit bypass where the model supports it (never saved; turns itself off on sleep, display off, AC loss and exit), and the EC's liquid cooler connection flag
- **Profiles:** 5 slots. The last one used loads at startup. You can switch profiles from the tray icon.
- **Monitoring:** live temperatures, fan speeds and power. Fan RPM can also be published as Windows performance counters.
- **Command line (optional):** `mcctl.exe` reads and changes GPU/CPU power, NVIDIA current limits, fan boost and the advanced settings on the running app, for scripts and agents.

## Requirements

- Windows 10 version 2004 (build 19041) or later, or Windows 11 (x64). MechControl must run as Administrator.
- `UWACPIDriver`, the Uniwill driver that comes with the OEM Control Center. MechControl needs only this driver, not the app.

## Install

1. Download the latest build from [Releases](https://github.com/Liddo-kun/MechControl-RE/releases/latest), or build it yourself (see below).
   - `MechControl-win-x64.zip`: the app
   - `MechControl-win-x64-cli.zip`: the app plus the `mcctl` command line
2. Extract it to a folder you can write to. Settings, profiles and the log are kept next to `MechControl.exe`. Keep the `Profiles` folder next to it.
3. Stop the OEM Control Center services (`GCUService`, `GCUBridge`), or they may overwrite your settings. MechControl warns you if they're running.
4. Run `MechControl.exe`. It starts in the system tray. Closing the window hides it; use **Exit** in the tray menu to stop it.

On first run, MechControl sets a few OEM UEFI flags so the BIOS prepares the EC at boot the same way it does with the Control Center installed.

**Upgrading from the C# version (v1.6.0):** exit it from the tray, then extract the new release over its folder. Settings, profile slots and lighting and cooler state carry over. Files last saved by a release older than v1.6.0 may not: a slot with a failsafe trigger below 50 °C has to be saved again. The old `.dll` files are no longer used and can be deleted.

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

### Command line

`mcctl.exe` is in the `-cli` download and in builds made with `-WithCli`. It talks to the running app. Run it from an elevated shell:

```powershell
mcctl status                         # settings with the ranges this machine accepts, live readings
mcctl status --json                  # the same as one line of JSON
mcctl watch --interval 1000          # live readings until Ctrl+C
mcctl gpu --tgp 150 --db 25          # GPU TGP and Dynamic Boost (watts)
mcctl gpu --nvvdd 240 --msvdd 75 --board-input 10.25   # NVIDIA current limits (amps)
mcctl cpu --pl1 80 --pl2 120         # CPU power limits (watts)
mcctl fan boost on                   # maximum fan speed
mcctl ec-bypass on                   # EC power-limit bypass, where the model supports it
mcctl cooler-flag on                 # tell the EC a liquid cooler is connected
mcctl help                           # everything else
```

Changes apply immediately but aren't saved. Values outside your model's ranges are refused, and nothing from the command line restarts the GPU or turns the NVIDIA unlock on or off. Current limits need the unlock to have been turned on in the Power tab.

### Diagnostic report

`MechControl.exe --diagnose report.json` writes a read-only report of the model, EC power limits, fan tables, sensors and the EC's memory, then exits. It can run while MechControl is running. The first session in `mechcontrol.log` records the EC's memory too.

## Supported models

Each model has a profile in [`Profiles/MECHREVO/`](Profiles/MECHREVO/). At startup MechControl picks the right one from the motherboard name, the EC project ID and the CPU vendor. If none matches, it asks you to choose. If your model loads the wrong profile or features are turned off, please open an issue and attach `mechcontrol.log`.

## Building

You need Visual Studio 2022 Build Tools with the MSVC v143 x64 C++ tools, the C++ CMake tools and the Windows SDK. Then run:

```powershell
./tools/build-native.ps1 -Configuration Release -Check            # app and checks
./tools/build-native.ps1 -Configuration Release -WithCli -Check   # adds mcctl.exe
```

The default build directory is `C:/Temp/tandem/MechControl-native-build`; change it with `-BuildDirectory`. [`docs/build.md`](docs/build.md) covers packaging and measurement.

## Documentation

- [`re/register_reference.md`](re/register_reference.md): EC registers, encodings and write sequences
- [`re/`](re/README.md): reverse-engineering notes, captures, OEM profiles and tools
- [`docs/architecture.md`](docs/architecture.md): how the app is put together
- [`docs/release.md`](docs/release.md): the notes shipped with each release, including performance counters

MechControl isn't affiliated with MECHREVO, XMG, Eluktronics or Uniwill.
