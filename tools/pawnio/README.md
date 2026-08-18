# NVIDIA GB20x Thermal via PawnIO.Modules PR #77

MechControl consumes the upstream NVIDIA module from
[PawnIO.Modules PR #77](https://github.com/namazso/PawnIO.Modules/pull/77)
(`Nvidia.p` -> `Nvidia.bin`), not a module of our own. See
`notes/gpu_hotspot_pr77_plan.md` for the full plan and
`notes/gpu_hotspot_pr77_validation.md` for the pinned head SHA and artifact
hashes. There is no version-controlled module source in this directory.

The module ABI: `ioctl_read_thermal_registers` takes `[bus, device, function]`
and returns six raw DWORDs from `BAR0+0xAD0A90..0xAD0AA4`. All decoding
(bit 30 validity, `(raw & 0xFFFF) / 256.0`, plausibility, aggregate/fallback
selection) happens in the shared C# provider.

## Development module build

Check out the pinned PR #77 branch in a `PawnIO.Modules` clone
(`pr77-<sha>`), then compile with Pawn 4.1.7152 and the exact CI flags:

```powershell
pawncc Nvidia.p "-iinclude" "-C64" "-;+" "-(+" "-p"
```

For the unrestricted development driver only, prepend a four-byte zero
signature length to the AMX image:

```powershell
[IO.File]::WriteAllBytes("Nvidia.bin", [byte[]](@(0,0,0,0) + [IO.File]::ReadAllBytes("Nvidia.amx")))
```

The zero-signature `Nvidia.bin` is ignored by git and must never enter a
production MechControl build. Production pins a signed `Nvidia.bin` from an
official PawnIO.Modules release (Phase 7 of the plan).

## Probe

The console probe links the same client/provider sources used by MechControl.
PR #77 requires the caller to supply the PCI BDF, so pass it explicitly
(hex `bus:device.function`, as shown by Device Manager / validation notes):

```powershell
dotnet run --project tools/pawnio/BlackwellThermProbe -c Release -- --self-test
dotnet run --project tools/pawnio/BlackwellThermProbe -c Release -- tools/pawnio/Nvidia.bin 02:00.0 180
```

The hardware run emits CSV containing all six raw and decoded channels. Follow
the validation matrix in `notes/gpu_hotspot_pr77_plan.md` (section 7) before
exposing or deploying the sensor.
