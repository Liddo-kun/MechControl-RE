# Local Dimming — Findings & Implementation Plan

## SOLVED: How Local Dimming Works

The BOE mini-LED panel has its own TCON (timing controller) that manages local dimming zones. It is controlled via a **GPIO signal** (pad `0x0016108C`), toggled through the WMI/ACPI interface.

### Working WMI Commands

| Action | SA00 | WMI Data (UInt64) | ACPI Effect | Display Effect |
|--------|------|-------------------|-------------|----------------|
| **Dimming ON** | 0x0E | `0x000005000000000E` | `PGPI.SGOV(0x0016108C, 1)` — GPIO HIGH | Panel TCON enables local dimming |
| **Dimming OFF** | 0x0D | `0x000005000000000D` | `PGPI.SGOV(0x0016108C, 0)` — GPIO LOW | Panel TCON disables local dimming |
| EC state ON | 0x02 | `0x0000050000000002` | `CTAF(1)` — sets CSMS=1, NPCF notify | No display change (state tracking only) |
| EC state OFF | 0x01 | `0x0000050000000001` | `CTAF(0)` — sets CSMS=0, NPCF notify | No display change (state tracking only) |

### What the OEM App Does (Probable)

The OEM app likely sends **both** commands per toggle:
1. SA00=0x02 or 0x01 — updates EC flag (CSMS at 0x72B bit 1) for state persistence and GPU power management
2. SA00=0x0E or 0x0D — toggles the actual GPIO to the panel's TCON

### Why SA00=1/2 Alone Don't Work

SA00=1/2 call `CTAF` → `PMSF`, which:
- Writes CSMS (EC 0x72B bit 1) — internal state flag
- Updates NPCF power parameters (ACBT, DBAC, AMAT, ATPP)
- Sends `Notify(NPCF, 0xC0)` and `Notify(RP09.PXSX, 0xC5)` to the GPU driver

None of this reaches the panel. The panel only responds to the GPIO hardware signal.

## Confirmed EC Register State (from diagnostic run)

| Register | Value | Meaning |
|----------|-------|---------|
| GFID (0x7D2) | 0x1B (27) | GPU Feature ID, non-zero (PMSF runs) |
| CSMS (0x72B bit 1) | Toggles with SA00=1/2 | Dimming state flag in EC (does NOT control panel) |
| CUME (0x727 bit 6) | 1 | Custom mode active |
| APTC (0x786 bits 0-6) | 20 (0x14) | Active point count |
| APTN (0x786 bit 7) | 1 | Active point toggle |
| TB43 (0x728 bit 0) | 0 | — |
| LCSE (0x7C7 bit 1) | 0 | — |

## Implementation Plan for MechControl

### WMI Interface

Use existing `root\WMI` / `AcpiTest_MULong` / `GetSetULong` (WmiMethodId=4) — same as all other WMI commands in MechControl.

```
Instance: ACPI\PNP0C14\1_1
```

### Toggle Dimming ON

```csharp
// 1. Set EC state (for persistence/power management)
WmiCall(0x0000050000000002UL);  // SA00=2: CTAF(1), returns 0xBB

// 2. Toggle panel GPIO (actually changes display)
WmiCall(0x000005000000000EUL);  // SA00=0x0E: GPIO HIGH, returns 0x0E
```

### Toggle Dimming OFF

```csharp
// 1. Set EC state
WmiCall(0x0000050000000001UL);  // SA00=1: CTAF(0), returns 0xAA

// 2. Toggle panel GPIO
WmiCall(0x000005000000000D UL);  // SA00=0x0D: GPIO LOW, returns 0x0D
```

### Read Current State

Read EC register 0x72B, check bit 1:
```csharp
byte val = ecDevice.ReadByte(0x72B);
bool dimmingOn = (val & 0x02) != 0;
```

Note: This reads the EC state flag (CSMS), which tracks intended state. It does not directly read the GPIO, but if both commands are always sent together, it stays in sync.

### Return Values

| Command | Expected Return |
|---------|----------------|
| SA00=1 (EC OFF) | 0xAA (170) |
| SA00=2 (EC ON) | 0xBB (187) |
| SA00=0x0D (GPIO LOW) | 0x0D (13) |
| SA00=0x0E (GPIO HIGH) | 0x0E (14) |

### Model Applicability

- GFID=0x1B identifies this GPU/model configuration
- PMSF has model-specific branches for MOID==0x1B and MOID==0x21 (different APTC values)
- The GPIO pad `0x0016108C` is likely consistent across models with the same panel, but profile JSON should gate whether local dimming is available

## Investigation History

See git history of this file for the full investigation log. Key milestones:
- Identified WMI namespace/class/method and SA00=1/2 constants from binary IL analysis
- Decompiled DSDT (105K lines) with iasl, traced full ACPI chain
- SA00=1/2 returned correct values but didn't change display — spent multiple sessions hooking WMI infrastructure (wmiprvse, svchost/winmgmt, GCUService IOCTLs, NtWmi syscalls, COM/RPC) to find the discrepancy
- Built EC register diagnostic tool (`dimming_diag.cs`), confirmed CSMS changes but panel doesn't respond
- Discovered SA00=0x0D/0x0E (GPIO commands) in DSDT `OEMG` method — these directly toggle the panel's TCON
- Confirmed: GPIO HIGH = dimming ON, GPIO LOW = dimming OFF

## Key Files

| File | Purpose |
|------|---------|
| `dimming_diag.cs` / `.exe` | EC register diagnostic + WMI test tool (all SA00 sub-commands) |
| `dsdt.dsl` | Decompiled DSDT (105K lines, iasl v20251212) |
| `dsdt.aml` | Raw ACPI DSDT dump (594KB) |
| `test_dimming.ps1` | Original PowerShell test (SA00=1/2 only — doesn't work alone) |
