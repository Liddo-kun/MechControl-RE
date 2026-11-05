# MechControl Native

A native Windows C++20 rewrite of MechControl, focused on small deployment size,
low memory use, low background overhead, and straightforward code.

The original source repository is a read-only reference. Phase plans live in
`plans/`, and implementation evidence lives in `docs/`. The coordinator runs
each phase in a fresh agent session, followed by a separate retrospective and
an update to subsequent phases where the findings require it.

Implementation has started. A finished phase does not by itself imply that the
whole application is ready to replace the installed release.

Current handoff: **no production cutover is authorized by the recorded checks**.
See [architecture](docs/architecture.md), [build and measurement](docs/build.md),
[deployment/rollback](docs/handoff.md), and [phase 9 status](docs/phase-9-result.md).
The final default/CLI candidates are in `C:/Temp/tandem/mechcontrol-native-phase9-final/`
under `final-default` and `final-cli`, with adjacent inventories and separate symbols.
These supersede the frozen phase-8 archives for handoff, not the open qualification.

## Native development build

Use Windows x64 and Visual Studio 2022 Build Tools with the MSVC v143 x64 tools,
C++ CMake tools, and Windows SDK (including C++/WinRT and `ctrpp`). No .NET runtime
or third-party runtime DLL is required by the produced applications.

```powershell
./tools/build-native.ps1 -Configuration Release -WithCli -Check
./tools/build-native.ps1 -Configuration Debug -WithCli -Check
```

The default build directory is `C:/Temp/tandem/MechControl-native-build`.
Create checked, allowlisted runtime and separate developer-symbol ZIPs with:

```powershell
./tools/package-native.ps1 -OutputDirectory C:/Temp/tandem/native-release-default
./tools/package-native.ps1 -WithCli -BuildDirectory C:/Temp/tandem/native-build-cli -OutputDirectory C:/Temp/tandem/native-release-cli
```

Each output directory must be new. Release deployment instructions ship as the
package's `README.md`; their source is [`docs/release.md`](docs/release.md).
The supported Windows API baseline is Windows 10 build 19041 (system ICU).

Omit `-WithCli` to omit the pipe server, `mcctl` and its protocol fixture. Run
`<build>/Release/MechControl.exe` from a writable directory and approve elevation.
Normal startup is tray-hidden. Use the tray icon to open the window. Closing or
minimizing hides it; the tray menu's Exit command stops the application. The
production application does not accept the fixture's `--show` option.

This is **not a replacement release**. Normal startup discovers the model,
activates the EC, enforces recovered thermal holds and applies supported core
settings from the selected saved slot on the serialized worker. It uses
`Global\MechControl`, so an installed controller excludes native startup before
log/hardware access. The native Power, Fans, Thermal Failsafe, Battery, Lighting,
System Info and Cooler pages use the same real controllers as the optional CLI.
The five-slot bar supports selecting empty save targets, saving, and right-click
Rename/Clear actions. Defaults are model-derived and remain unsaved.
With `-WithCli`, the native server implements `status`, `cpu`, `gpu` and `fan boost`
through the same controllers. CLI changes are live and unsaved. Unlock and GPU
restart confirmations remain in the UI, not implicit CLI operations.
The client uses the production pipe name, so it addresses whichever controller
currently owns that pipe. Physical coverage and remaining qualification gaps are
recorded in `docs/phase-8-result.md`; an implemented control is not proof of
physical coverage on every model or peripheral.

`MechControl.exe --diagnose <output.json>` collects one read-only
model/ECSpec/fan/sensor report and exits. It does not acquire the controller mutex
and may run alongside the installed controller. It does not run
UEFI setup, activation, saved-state apply, BLE, HID output or NVIDIA access.
The output directory must exist. Success exits 0; missing/malformed diagnostic
arguments exit 2, and a worker/report failure exits 1. Normal controlling startup
retains global exclusion and a second normal launch exits 0 before log/EC access.
EC calls use the OEM driver's synchronous interface. Worker shutdown waits for
the current call to return; blocked kernel I/O is not canceled or given an
unproven timeout guarantee.

Optional passive counters publish 64-bit RPM at `\MechControl\CPU Fan RPM` and
`\MechControl\GPU Fan RPM`. Registration is explicit, never a startup action.
Stop all publishers and use an elevated shell. The migration script backs up
the legacy category and counter registration before replacing it:

```powershell
./tools/counters.ps1 -Action Install -Manifest <build>/Release/MechControl-counters.man -BackupDirectory C:/Temp/tandem/counter-backup
# Start MechControl, then read its passive counters with Get-Counter.
# Stop MechControl before removing or rolling back registration.
./tools/counters.ps1 -Action Rollback -Manifest <build>/Release/MechControl-counters.man -BackupDirectory C:/Temp/tandem/counter-backup
```

Use a new backup directory for each migration. `-Action Remove` removes only the
native registration; `Rollback` restores the saved legacy registration. No shared
system DLL is deleted. The resource-bearing executable must remain beside its
manifest. For logon startup, Task Scheduler can run that executable with highest
privileges in the interactive user's session, no execution time limit, and battery
operation enabled. There is no in-application startup installer.

`ui-pages-check.exe --ui-check C:/Temp/tandem/<fresh-existing-directory>` exercises
the production pages and controllers with synthetic hardware, injected live-read
failures and 100/150/200% DPI captures. Run it on an interactive desktop and inspect
the captures; it does not establish physical hardware qualification.
Final candidate identities/checks and open qualification are recorded in
`docs/phase-9-result.md`; prior measurements remain in `docs/phase-8-result.md`.
`mechcontrol.log` retains the first session and five recent
sessions beside the executable.

## Configuration library

Phase 2 adds the `configuration` static library, the single imported runtime
profile set under `Profiles/MECHREVO`, and `configuration-check`. Normal `-Check`
builds run its profile/detection and real filesystem recovery checks. The JSON
dependency is nlohmann/json 3.12.0, fetched at configure time with a pinned
SHA-256; see `THIRD_PARTY_NOTICES.md`. No JSON runtime DLL is shipped.

The library does not open an EC driver or change hardware. Its identity and
startup-validation APIs are connected to the serialized EC owner. Phase 4 connects
the store to startup, core slot application/capture and thermal enforcement.
Persistence defaults to the executable directory; development checks always pass
an isolated scratch directory instead. Read [`docs/phase-2-result.md`](docs/phase-2-result.md)
for the configuration API boundaries and the isolated C# interoperability command.

Phase 3 adds `src/ec.*`, `src/ec_session.*`, and `ec-check` for the transport,
guard, model preparation and read-only diagnostics. Builds stage the single
`Profiles/MECHREVO` source set beside the application. See
[`docs/phase-3-result.md`](docs/phase-3-result.md) for the physical Intel read/reference
comparison, fake fan-batch checks and remaining controller/hardware coverage gaps.

Phase 4 adds `src/controller.*`, `src/controller_cli.cpp`, `src/thermal.*` and
`controller-check`. The shared monitor delivers safety, linked fans and telemetry
in that order, with four RPM reads plus subscribed temperatures while hidden and
full reads while visible. Hidden ticks do not post presentation samples, and a
stopped monitor retries after sixty seconds or immediately on Show. Startup fan
capture follows activation; an invalid no-slot table is parked out of custom
mode, with an editable fallback requiring deliberate review/Apply rather than
automatic qualification. Core-only saves preserve known target-slot peripheral
intent and refuse unreadable targets; they are not full peripheral capture.
The optional server is `src/cli_server.*`. Actual
hardware writes, fixture coverage, measurements and remaining boundaries are in
[`docs/phase-4-result.md`](docs/phase-4-result.md).
