# GPU Power & EC Exploration Notes

## Context

This machine has a **shunt-modded GPU** (GFID=0x1B, ModuleIdGpu=0x1B). The shunt mod removes the GPU-side power limit detection, allowing it to draw more power. However, the EC still manages power delivery between CPU and GPU via the NPCF (Notify PCIe Function) system. Understanding and tuning these EC registers could unlock more GPU power.

## DSDT-Discovered GPU Power Registers

The DSDT defines a complete GPU power management system in the EC memory region (ECMG at 0xFE410000). These registers are NOT exposed in MechControl's UI or the OEM app's Custom mode.

### GPU Power Allocation Registers

| Register | DSDT Name | Description | OEM Value | Scaling |
|----------|-----------|-------------|-----------|---------|
| `0x743` | (bits) | DB ctrl — GNEN (bit 0), ECDC (bit 1) | 0x05 | Bitfield |
| `0x744` | CTVA | GPU control value A (base TGP) | 0x37 (55) | Direct W |
| `0x745` | DBCT | dGPU dynamic power control | 0x00 | Direct W |
| `0x746` | MXDB | Max dGPU power | 0x05 (5) | Direct W |
| `0x747` | MIDB | Min dGPU power | 0x00 | Direct W |
| `0x7D5` | DBAP | **dGPU amperage/power** — EC uses `DBAP * 8` for NPCF power allocation | 0x05 (40W) | x8 watts |
| `0x7D6` | DBSP | dGPU spec/sustain power | 0x00 | Unknown |

### Thermal Budget Registers

| Register | DSDT Name | Description | OEM Value | Scaling |
|----------|-----------|-------------|-----------|---------|
| `0x788` | CTWA | **Thermal workload adjustment** — EC uses `CTWA * 8` for AC thermal budget | 0x37 (440W) | x8 watts |
| `0x7F7` | ETPP | **Extended thermal power point** — EC uses `ETPP * 8` | 0x00 | x8 watts |

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
3. Sets `NPCF.DBAC` (enable) and `NPCF.AMAT = DBAP * 8` (power in watts)
4. OS/driver receives notification and adjusts GPU power delivery

Other EC queries:
- `_Q80`/`_Q81`: GPU power state changes, triggers `NPCF.NPCF()` via dGPU UUID `36b49710-2483-11e7-9598-0800200c9a66`
- `_Q83`: Thermal management — `NPCF.ACBT = CTWA * 8`
- `_Q9C`: Extended thermal power point — `NPCF.ATPP = ETPP * 8`

### GPU Model Thermal Ceiling (from DSDT init)

GFID-based thermal power point assignment:
- GFID 0x19: `ATPP = 0x04D8` = 1240W
- GFID 0x1A: `ATPP = 0x0640` = 1600W
- **GFID 0x1B: `ATPP = 0x0640` = 1600W** (this machine)

## Exploration Ideas

### 1. CTWA (0x788) — Total Thermal Budget
Currently 55 → 440W total budget for CPU+GPU. With a shunt mod, the GPU can physically draw more than its allocated share. Increasing CTWA could tell the EC to allow a higher total power envelope. **Caution:** the AC adapter has a physical watt limit.

### 2. DBAP (0x7D5) — GPU Power Allocation
Currently 5 → 40W via NPCF. This seems dynamic (EC adjusts it based on load). Writing a higher value might increase the GPU's power allocation, but the EC may overwrite it.

### 3. OCP1/OCP2 (0x763/0x764) — Overcurrent Protection
These are 0x00 when the OEM app runs but 0x2D (45) in standalone. The OEM clears them during init. If these are enforced power limits, clearing them removes a cap. Already cleared by MechControl's EC init in working state.

### 4. MXDB (0x746) — Max dGPU Power
Currently 5W — but this is the Dynamic Boost max, not total GPU power. With shunt mod, DB isn't the limiting factor.

### 5. PCIEPowerControl UEFI Variable
4 bytes = `0x35` (53). Unknown meaning. Under GUID `{7a09bc55-83e8-4d4d-93b0-89d80eedb720}`. Could be a PCIe slot power budget that limits GPU power delivery at the BIOS level.

### 6. ETPP (0x7F7) — Extended Thermal Power Point
Currently 0. The EC uses `ETPP * 8` to set `NPCF.ATPP`. Writing a value here could override the GFID-based default (1600W). Not useful unless the default is too low.

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

## Safety Notes

- **CTWA controls total system power budget.** Setting it above the AC adapter's capacity could cause shutdown under load.
- **OCP registers may exist for hardware protection.** Clearing them on untested hardware could damage VRMs.
- **DBAP is likely EC-managed.** The EC updates it dynamically; manual writes may be immediately overwritten.
- **Always take an EC snapshot before experimenting** so you can compare and revert.
