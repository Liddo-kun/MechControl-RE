# Build, package and measure

Use Windows x64, Visual Studio 2022 Build Tools (v143 x64 C++, CMake tools) and the
Windows SDK with C++/WinRT and `ctrpp`. The API baseline is build 19041 with system
ICU. CMake selects C++20 and static `/MT` (`/MTd` for Debug) consistently. It fetches
nlohmann/json 3.12.0 with a pinned SHA256; keep its notice. The app needs supported
hardware, elevation and the OEM UWACPIDriver, not a bundled driver or .NET/VC runtime.

```powershell
./tools/build-native.ps1 -Configuration Release -Check
./tools/build-native.ps1 -BuildDirectory C:/Temp/tandem/native-cli -Configuration Release -WithCli -Check
./tools/build-native.ps1 -BuildDirectory C:/Temp/tandem/native-cli -Configuration Debug -WithCli -Check
```

`-WithCli` adds the pipe server and `mcctl.exe`. Keep build directories outside the
source tree. `-Check` runs the CTest checks, which use synthetic hardware and do not
prove physical behavior. `ui-pages-check.exe --ui-check <fresh existing directory>`
renders the real pages with fake hardware at 100, 150 and 200% DPI; run it on an
interactive desktop and inspect the captures.

## Release packages

```powershell
./tools/package-native.ps1 -OutputDirectory C:/Temp/tandem/release-<version>
./tools/package-native.ps1 -WithCli -BuildDirectory C:/Temp/tandem/native-cli -OutputDirectory C:/Temp/tandem/release-<version>-cli
```

Each output directory must be new. The script builds and checks the Release, then
writes `MechControl-win-x64[-cli].zip`, a separate symbols ZIP and `artifacts.json`
with every file's hash. CMake install rules choose the shipped files: the executables,
`Profiles/MECHREVO`, the counter manifest, `counters.ps1`, the notices and
`docs/release.md` as `README.md`. Set the version in `CMakeLists.txt` and
`resources/app.rc` before packaging.

`tools/check-serialization.ps1` builds the C# app's serializers from tag `v1.6.0` and
checks that both apps read each other's settings and slots. It needs the .NET 8 SDK.

## Measuring

Compare executables under the same settings, power source, GPU state, window
visibility and warm-up. Never run two controllers together.

```powershell
./tools/measure.ps1 -ProcessId <PID> -Seconds 180 -NoGpu -OutputDirectory C:/Temp/tandem/idle-<label> -Conditions '<settings/power/GPU/visibility/warm-up>'
```

`-NoGpu` stops the measuring script itself from querying the GPU; it does not show
that the app never wakes it. CPU is reported per core and for the whole machine.
Short runs are dominated by startup and background activity.
