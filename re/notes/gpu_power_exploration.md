# GPU Power & EC Exploration Notes

## Context

Historical IDY_7Y snapshots with a shunt-modded GPU (GFID=0x1B). Values below
are from those snapshots, not current measurements. NPCF is NVIDIA's platform
power-control interface. A shunt mod changes power measurement; it does not
establish that every GPU or platform limit is bypassed.

**Units corrected 2026-11-06:** NVPCF dynamic power parameters use **125 mW
(1/8 W) units**, not watts. ACPI multiplies EC values by 8 to encode them.
The configurable-TGP field is an offset relative to the GPU's static TGP,
not the total CPU+GPU budget. Sources: NVIDIA
[`nvpcf.h`](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e6a2e01579fbbd1ca2ed43fef2ee2589e9bd8bd9/src/nvidia/inc/kernel/platform/nvpcf.h)
(`NVPCF_DYNAMIC_PARAMS_2X_POWER_UNIT_MW`) and
[`client_resource.c`](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e6a2e01579fbbd1ca2ed43fef2ee2589e9bd8bd9/src/nvidia/src/kernel/rmapi/client_resource.c)
(`ctgpOffsetmW`, `targetTppOffsetmW`, `maxOutputOffsetmW`).

## DSDT-Discovered GPU Power Registers

The DSDT defines a complete GPU power management system in the EC memory region (ECMG at 0xFE410000). These registers are NOT exposed in MechControl's UI or the OEM app's Custom mode.

### GPU Power Allocation Registers

| Register | DSDT Name | Description | OEM Value | Scaling |
|----------|-----------|-------------|-----------|---------|
| `0x743` | (bits) | DB ctrl — GNEN (bit 0), ECDC (bit 1) | 0x05 | Bitfield |
| `0x744` | CTVA | Configurable TGP input offset | 0x37 (55) | +55 W relative to model/VBIOS base; see register reference |
| `0x745` | DBCT | dGPU dynamic power control | 0x00 | Direct W |
| `0x746` | MXDB | Max dGPU power | 0x05 (5) | Direct W |
| `0x747` | MIDB | Min dGPU power | 0x00 | Direct W |
| `0x7D5` | DBAP | EC-produced Dynamic Boost maximum offset; ACPI sends `DBAP * 8` as AMAT/MAGA | 0x05 (+5 W) | 1 W per EC count |
| `0x7D6` | DBSP | dGPU spec/sustain power | 0x00 | Unknown |

### Published NVIDIA Power Parameters

| Register | DSDT Name | Description | OEM Value | Scaling |
|----------|-----------|-------------|-----------|---------|
| `0x788` | CTWA | EC-produced configurable-TGP offset; ACPI sends `CTWA * 8` as ACBT/TGPA | 0x37 (+55 W) | 1 W per EC count |
| `0x7F7` | ETPP | EC-produced TPP parameter; ACPI sends `ETPP * 8` as ATPP/TPPA under its CUME condition | 0x00 | 1 W per EC count |

### GPU Identity & Status

| Register | DSDT Name | Description | Value |
|----------|-----------|-------------|-------|
| `0x7D0` | DBD1 | dGPU data 1 | 0x46 (70) |
| `0x7D1` | DBD2 | dGPU data 2 | 0x00 |
| `0x7D2` | GFID | GPU family ID | 0x1B (27) |
| `0x7C3` | DBEN/DBST | dGPU enable + status byte | 0x07 |
| `0x7CF` | FPMS | Fan/power management status | 0x00 |

### Overcurrent Protection

| Register | DSDT Name | Description | Value |
|----------|-----------|-------------|-------|
| `0x763` | OCP1 | Overcurrent protection domain 1 | 0x00 (OEM), 0x2D (standalone) |
| `0x764` | OCP2 | Overcurrent protection domain 2 | 0x00 (OEM), 0x2D (standalone) |

Note: OCP1/OCP2 are 0x00 in the OEM working state but 0x2D (45) in standalone — suggests the OEM clears them. These could be power limits the EC enforces.

### VRM Registers

| Register | Name | Description | Value |
|----------|------|-------------|-------|
| `0x753` | VRM current limit | CPU VRM current limit | 0x00 |
| `0x754` | VRM current max | CPU VRM maximum | 0x00 |

## NPCF Power Allocation System (from DSDT)

The EC communicates GPU power needs to the OS via ACPI:

1. EC receives power state change → triggers `_Q84` query
2. `_Q84` reads `DBEN` and `DBAP` from EC
3. Sets `NPCF.DBAC` (disable flag: 0=enabled) and `NPCF.AMAT = DBAP * 8` (encoded in 1/8 W)
4. OS/driver receives notification and adjusts GPU power delivery

Other EC queries:
- `_Q80`/`_Q81`: GPU power state changes, triggers `NPCF.NPCF()` via dGPU UUID `36b49710-2483-11e7-9598-0800200c9a66`
- `_Q83`: Configurable-TGP update — `NPCF.ACBT = CTWA * 8`
- `_Q9C`: TPP update — `NPCF.ATPP = ETPP * 8` when CUME is clear

### GPU Model TPP Parameter (from DSDT init)

GFID-based thermal power point assignment:
- GFID 0x19: `ATPP = 0x04D8` = 155 W in protocol units
- GFID 0x1A: `ATPP = 0x0640` = 200 W in protocol units
- **GFID 0x1B: `ATPP = 0x0640` = 200 W in protocol units** (this machine)

The NVIDIA driver interprets this as a TPP offset parameter; these numbers alone
do not establish an absolute total-system or adapter limit.

## Interpretation Limits

- 0x788 and 0x7D5 are EC-produced outputs, not established independent host controls.
- The OCP1/OCP2 snapshot difference does not prove that the OEM clears them or
  that writing zero disables a limit. Current native initialization does not
  explicitly write these addresses.
- `PCIEPowerControl` UEFI variable value 0x35, under GUID
  `{7a09bc55-83e8-4d4d-93b0-89d80eedb720}`, remains uninterpreted.
- Cooler/OCPL firmware analysis: [`liquid_cooler_ec_policy.md`](liquid_cooler_ec_policy.md).

## Saved Data

| File | Description |
|------|-------------|
| `tools/ec_snap_7.bin` | EC state: OEM installed + Custom mode (working, GPU active) |
| `tools/ec_snap_8.bin` | EC state: standalone, UEFI cleared (broken state) |
| `captures/uefi_full_dump_all.txt` | Complete UEFI variable dump with all contents |

## Tools

- `ec_snapshot.exe snapshot` / `ec_snapshot.exe diff` — full EC dump and comparison
- `ec_write.exe <addr> <value>` — write individual EC registers (use with caution)
- `fan_table_dump.exe` — quick view of key power + fan registers
