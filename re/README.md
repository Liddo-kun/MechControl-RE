# Reverse-engineering material

Hardware knowledge and evidence behind MechControl. The app itself does not load
anything in this directory.

- `register_reference.md`: EC registers, encodings, write sequences and the HID
  protocol. Read this first for any EC work.
- `report.md`: OEM architecture, methodology, project IDs and safety background.
- `Architecture_BLE.md` and `notes/liquid_cooler_ble_protocol.md`: the BLE liquid cooler.
- `notes/`: findings by topic. Multi-model state is in `notes/other_machines.md` and
  `notes/ec_firmware_X6FR5_findings.md`. Intel EC power transitions are in
  `notes/intel_x6ar_power_sequencing.md`.
- `Profiles/MECHREVO/`: full OEM-extracted profiles. Use these
  to create a compact runtime profile in the repository's root `Profiles/MECHREVO/`
  when adding a model.
- `UserFanTables-OEM/`: OEM factory fan tables and PL defaults for 24 models.
- `captures/`: Frida, EC, WMI and UEFI captures. `frida_capture_annotated_X6AR55xY.md`
  (EC, HID and WMI) and `api_monitor_results.md` (EC buffer format) are the main ground
  truth. Raw `*.log` captures are kept locally and are not published.
- `acpi/`: DSDT and SSDT tables.
- `tools/`: RE tools. Compile a `.cs` tool with
  `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe -nologo <file>.cs` (C# 5 only).
  Compiled `.exe` files are kept locally and are not published.
- `decompilation/`: decompiled GCUService, GCUBridge and ACPIDriverDll output. It is
  local only and git-ignored. Never commit decompiled OEM code, because the repository
  is public. Notes refer to it as `decompiled/` or `decompilation/`.

Notes written for the C# app refer to its source (`MechControl/...`, `FanControl.cs`,
`architecture.md` and so on). That source is on the `legacy-csharp` branch, and tag
`v1.6.0` marks the last C# release. Read a file with
`git show v1.6.0:MechControl/Features/FanControl.cs`. Other bare file names in notes
were temporary scratch files that were not kept.

The OEM Control Center installer was lost in a Windows rebuild. These decompilations
and captures are the surviving record. New RE needs a fresh Control Center download,
which will be a different version, so treat old offsets as stale. `UWACPIDriver` is
the only OEM component MechControl needs.
