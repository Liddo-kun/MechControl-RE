# NVIDIA BIOS and ownership-plan tests

```powershell
dotnet run --project tests/NvidiaPower.Tests/NvidiaPower.Tests.csproj
```

Dependency-free console tests link the production parser/store sources. The ROM
fixtures are generated structural inputs, not proprietary firmware. Two layouts
vary device/version, BIT location, legacy/EFI sizes, logical policy addresses,
monitor channels and native limits to catch machine-specific assumptions.

Tests cover direct-record serialization, paired rail DEFAULT/MAX patches,
unchanged unrelated bytes, malformed/profile rejection and ownership recovery
plans. Store tests invoke only pure private validation helpers through reflection;
they never instantiate the store or open GPU, SetupAPI or registry handles.

Optional local-only captured-ROM comparison (files are not committed):

```powershell
dotnet run --project tests/NvidiaPower.Tests/NvidiaPower.Tests.csproj -- --captured-roms C:/path/original.rom C:/path/active.rom 180000
```

This proves a generated power-max patch matches the entire supplied active image.
It does not exercise live registry writes, restart or hardware enforcement.
