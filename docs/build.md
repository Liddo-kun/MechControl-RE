# Build, package and measure

Use Windows x64, Visual Studio 2022 Build Tools (v143 x64 C++, CMake tools) and the
Windows SDK with C++/WinRT and `ctrpp`. The API baseline is build 19041.
CMake selects C++20 and static `/MT` (`/MTd` for Debug) consistently. It fetches
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

Format `src` and `tests` with the `clang-format` from the Build Tools (`VC/Tools/Llvm/x64/bin`); the
style is `.clang-format`. Wrap comments by hand, because it leaves them as written.

## Release packages

```powershell
./tools/package-native.ps1 -OutputDirectory C:/Temp/tandem/release-<version>
./tools/package-native.ps1 -WithCli -BuildDirectory C:/Temp/tandem/native-cli -OutputDirectory C:/Temp/tandem/release-<version>-cli
```

Each output directory must be new. The script builds and checks the Release, then
writes `MechControl-win-x64[-cli].zip`, a separate symbols ZIP and `artifacts.json`
with every file's hash. CMake install rules choose the shipped files: the executables,
`Profiles/MECHREVO`, `Profiles/boards.json`, the counter manifest, `counters.ps1`, the notices and
`docs/release.md` as `README.md`. Set the version in `CMakeLists.txt` and
`resources/app.rc` before packaging. The app compares the CMake version with the tag of the latest
GitHub release, so tag releases `vMAJOR.MINOR.PATCH` and do not mark a release as latest before its
assets are uploaded.

`resources/app.ico` is committed and rendered from the SVGs in `resources/app-icon/`:
`app-16px.svg` for 16, 20, 32 and 40 px, `app-24px.svg` for 24 and 48 px and `app.svg`
for 64 and 256 px. The small sources sit on the pixel grid so the letter stays sharp.
The About tab's marks in `resources/about/` are rendered from the Simple Icons paths beside them
at 16, 20, 24, 32, 40, 48 and 64 px: `github-light.ico` in the light theme's text color
(#1C1E21), `github-dark.ico` in the dark theme's (#E8EAEE) and `discord.ico` in Discord's blurple
(#5865F2).


## Measuring

Compare executables under the same settings, power source, GPU state, window
visibility and warm-up. Never run two controllers together.

```powershell
./tools/measure.ps1 -ProcessId <PID> -Seconds 180 -NoGpu -OutputDirectory C:/Temp/tandem/idle-<label> -Conditions '<settings/power/GPU/visibility/warm-up>'
```

`-NoGpu` stops the measuring script itself from querying the GPU; it does not show
that the app never wakes it. CPU is reported per core and for the whole machine.
Short runs are dominated by startup and background activity.
