# NVIDIA BIOS and ownership-plan tests

```powershell
dotnet run --project tests/NvidiaPower.Tests/NvidiaPower.Tests.csproj
```

Dependency-free console tests link the production parser/store sources. The ROM
fixtures are generated structural inputs, not proprietary firmware. Two layouts
vary device/version, BIT location, legacy/EFI sizes, logical policy addresses,
monitor channels and native limits to catch machine-specific assumptions.

Tests cover direct-record serialization, paired rail and optional board-input
DEFAULT/MAX patches, unchanged unrelated bytes, unknown input identities versus
recognized malformed ranges/channels, malformed/profile rejection and ownership
recovery plans. Legacy five-offset recovery is upgraded to seven offsets, retaining
saved values, accepting new owned records and restoring the exact original ROM.
Store tests invoke only pure private validation/migration helpers through reflection;
they never instantiate the store or open GPU, SetupAPI or registry handles.

Current-policy tests exercise the production NVAPI buffer path through managed
callbacks, bypassing native initialization. They vary policy indices and monitor
channels, verify board-only SET masks and exact metadata preservation, and reject
unsupported/out-of-range requests or failed readback without real hardware writes.
Missing, duplicate or out-of-range board-input policies leave the rails readable
and writable.

Optional local-only captured-ROM comparison (files are not committed):

```powershell
dotnet run --project tests/NvidiaPower.Tests/NvidiaPower.Tests.csproj -- --captured-roms C:/path/original.rom C:/path/active.rom 180000
```

This proves a generated power-max patch matches the entire supplied active image.
It does not exercise live registry writes, restart or hardware enforcement.
