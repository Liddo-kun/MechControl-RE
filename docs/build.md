# Build and measurement

The build commands below run from the source repository, not the runtime package.
Use Windows x64, Visual Studio 2022 Build Tools (v143 x64 C++, CMake tools), and
Windows SDK with C++/WinRT and `ctrpp`. The API baseline is build 19041 with system
ICU. CMake selects C++20 and static `/MT` (`/MTd` for Debug) consistently. It fetches
nlohmann/json 3.12.0 with a pinned SHA256; retain its notice. Runtime needs supported
hardware, elevation and the OEM UWACPIDriver, not a bundled driver or .NET/VC runtime.

```powershell
./tools/build-native.ps1 -Configuration Release -Check
./tools/build-native.ps1 -BuildDirectory C:/Temp/tandem/native-cli -Configuration Release -WithCli -Check
./tools/build-native.ps1 -BuildDirectory C:/Temp/tandem/native-cli -Configuration Debug -WithCli -Check
./tools/package-native.ps1 -OutputDirectory C:/Temp/tandem/new-default-package
./tools/package-native.ps1 -WithCli -BuildDirectory C:/Temp/tandem/native-cli -OutputDirectory C:/Temp/tandem/new-cli-package
```

Keep build/output directories outside the source tree. Package outputs must be fresh. `artifacts.json` identifies runtime and separate PDB archives
and every payload hash. Runtime is allowlisted by CMake install rules; checks,
libraries, sources and symbols do not ship. Default packages omit client/server;
`-WithCli` adds both. Verify a fresh extraction with `tools/handoff.ps1 -Action Stage`.
Runtime includes `handoff.ps1`, corrected `counters.ps1`, README, notices and the
architecture/build/handoff docs. Packaged copies of the scripts are at the root.
Changing packaged documentation/scripts creates a different package identity even
when its EXE is unchanged; do not attribute the old archive's checks to it.

CTest checks are isolated behavioral/synthetic coverage. Run `ui-pages-check.exe
--ui-check <fresh-existing-scratch-directory>` on an interactive desktop for
production-widget checks and captures. No fixture pass establishes physical parity.
Normal production startup is hidden; there is no production `--show` option.

Measure the actual identified EXE under recorded equal settings, power, GPU,
visibility, process age and warmup conditions. Never run two controllers together.

```powershell
./tools/measure-reference.ps1 -ProcessId <PID> -Seconds 180 -NoGpu -OutputDirectory C:/Temp/tandem/new-idle -Conditions '<artifact/settings/power/GPU/visibility/warmup>'
./tools/measure-reference.ps1 -ProcessId <PID> -Seconds 262800 -IntervalSeconds 5 -NoGpu -OutputDirectory C:/Temp/tandem/new-native-73h -Conditions '<artifact/settings/power/GPU/visibility/warmup>'
```

Use fresh directories and retain application logs. Long runs stream samples and
record exits/misses; short observer success is not elapsed qualification. `-NoGpu`
avoids GPU queries by the observer, not proof that the application never wakes it.
Collect device-write/cadence traces separately. CPU is reported per core and for
the whole logical-processor machine; startup/JIT/cache and background clients can
dominate short comparisons. Size and memory observations in the repository's `docs/phase-8-result.md`
are measured reductions, not reliable CPU-speedup or long-term stability claims.
