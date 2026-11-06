# EC Activation Issue — Standalone Operation

## Goal

MechControl must work on a machine where the OEM Control Center was never installed.

## Current Status

**UEFI NVRAM is solved but not sufficient.** We can read/write the OEM's UEFI variables, but setting them alone doesn't make the EC apply PL register values to the CPU. Something else the OEM service does at runtime is required.

## What We Know

### UEFI NVRAM Discovery

The OEM stores persistent config in a UEFI variable:
- **Variable:** `OemMagicVariable` (180 bytes)
- **GUID:** `{9f33f85c-13ca-4fd1-9c4a-96217722c593}`
- **Also under same GUID:** `OemMagicDoor`, `OemLightbarPresent`, `OemLightbarReset`, `OemEdpReset`
- **Separate variable:** `TpvSetup` (11 bytes) under GUID `{1c3483d5-1e7e-4450-9806-dede002c974b}`

The variable maps to `NVRAM_STRUCT` in the decompiled OEM code. Key fields:

| Offset | Field | Uninstalled | Installed+Custom |
|--------|-------|-------------|-----------------|
| 0x2F | PowerMode | 1 (Balance) | 3 (Custom) |
| 0x33 | MemoryOverClockSwitch | 0 | 1 |
| 0x5D | ApExistFlag | 0 | 1 |
| 0x61 | ApUseFlag | 0 | 1 |

PowerMode values on this machine: 0=Office, 1=Balance, 2=Turbo (sub-modes: Quiet Perf, Extreme Perf), 3=Custom.

The uninstaller clears ApExistFlag, ApUseFlag, and resets PowerMode to 1. Our `uefi_fix.exe` tool restores all 4 bytes. The BIOS reads these at boot and initializes the EC accordingly.

**With correct UEFI flags:** BIOS boots EC with valid fan tables, 0x741=0x01 (APPE set), 0x7C6=0x04. Without them, EC boots into failsafe (zeroed PLs, zeroed fan tables, 0x741=0x00).

### DSDT Revelation: 0x741 = APPE (1-bit field)

The DSDT defines register 0x741 as `APPE, 1` — a single bit. Writing 0x05 and reading back 0x01 is **normal** (only bit 0 matters). When the EC "transforms" 0x05→0x01, it means the EC firmware is actively processing the register. When it stays at 0x05, the EC is NOT processing — the value just sits in RAM.

### The Core Problem

EC register writes succeed (PL1=0xD2=210W in EC RAM), but the **actual CPU stays at 75W**. The EC firmware is not applying register values to the CPU via PECI. Two states observed:

| State | 0x741 readback | PL registers | Actual CPU power | Working? |
|-------|---------------|-------------|-----------------|----------|
| OEM service ran (ever) | 0x01 (transformed) | Values applied | Follows EC | YES |
| UEFI flags only, no OEM | 0x01 (transformed) | Values in RAM | Stuck at 75W | NO |

Both states have 0x741=0x01, correct UEFI flags, and PL registers showing 210W. But only the first state actually controls the CPU.

### EC Register Differences (Working vs Not-Working)

Snapshot comparison of OEM Custom-mode working state vs our standalone state (filtering out thermal volatiles and fan table differences):

| Register | OEM (working) | Standalone (broken) | Notes |
|----------|--------------|-------------------|-------|
| 0x726 | 0x80 | 0x00 | OEM sets bit 7 during Phase 5 init |
| 0x727 | 0xC0 | 0x80 | OEM sets bit 6 during Phase 5 init |
| 0x743 | 0x05 | 0x02 | tgpDbCtrl (DB off vs different state) |
| 0x766 | 0x93 | 0x90 | Support2 write-back (OEM Phase 3) |
| 0x787 | 0x81 | 0x00 | fanSwitchSpeed (enabled vs off) |
| 0x7E6 | 0xBC | 0x00 | Unknown — set during Custom mode selection |
| 0x7E7 | 0x88 | 0x00 | Unknown — set during Custom mode selection |
| 0x7E8 | 0x20 | 0x00 | Unknown — set during Custom mode selection |

**Tested individually:** Writing 0x7E6=0xBC, 0x7E7=0x88, 0x7E8=0x20, 0x766=0x93 one at a time did NOT fix the issue. Haven't tested 0x726, 0x727, or combinations.

### The Puzzle

When the OEM app was installed but service was **disabled** (never ran on boot), MechControl worked on fresh reboot. This means:
1. The OEM service didn't need to run each boot
2. Something the OEM service wrote **once** (on a previous boot) persisted and was sufficient
3. The uninstaller removed/cleared that persistent state
4. UEFI NVRAM alone doesn't restore it (we've matched all bytes)

**What persists from the OEM beyond UEFI NVRAM?** Possibilities not yet investigated:
- EC persistent storage (microcontroller NVRAM, separate from UEFI)
- A WMI/ACPI method call that changes EC firmware state (e.g., `Set_APExistToEC(true)`)
- A combination of register writes that activate the EC's power management loop
- The OEM's `UEFI_Firmware.dll` does something beyond what `SetFirmwareEnvironmentVariable` does (uses `ReadUefi`/`WriteUefi` with physical addresses, not variable names)

### OEM Init Sequence (from Frida Capture)

MechControl's 8-step init is simplified. The OEM does additional steps we skip:

**Phase 3 (missing from our init):**
- Write 0x748=0x81 four times interspersed (we do once)
- Write-back 0x766 = Read(0x766) — acknowledges support flags
- Clear 0x749=0, 0x74A=0, 0x74B=0, 0x767=0, 0x74E=0

**Phase 4-5 (missing):**
- Write-back 0x726 = Read(0x726) | 0x80
- Write-back 0x727 = Read(0x727) | 0x40
- Clear 0x7C6=0 before fan curve setup

**Phase 7-8 (missing):**
- Double fan table write (table → apply 0x7C6=0x04 → clear → table → apply again)

**Steady state (missing):**
- Read 0x7CC every ~1.5s (watchdog keepalive)

### Decompiled OEM Code: Key Functions

All obfuscated (Fody), but signatures visible:
- `Set_APExistToEC(bool bExist)` — writes to EC to signal app presence (NOT the same as UEFI ApExistFlag)
- `SetApExist(bool exist)` / `SetApExist(int mode)` — multiple overloads, called during mode switches
- `SetFwVarsForUninstall(string varname, byte value)` — clears UEFI NVRAM on uninstall
- `NvramVariable_Init()` — initializes UEFI NVRAM access
- `UEFI_Firmware.dll` P/Invokes: `ReadUefi(startAddr, endAddr)`, `WriteUefi(startAddr, endAddr, buffer)` — physical address access, DLL is gone (uninstaller removed it)

## Tools Built This Session

All in `C:\Users\Jon\clean\tools\`:

| Tool | Purpose |
|------|---------|
| `uefi_dump.exe` (.cs) | Enumerate all UEFI variables (name + GUID + hex for interesting ones) |
| `uefi_read_oem.exe` (.cs) | Read OEM UEFI variables + decode NVRAM_STRUCT fields |
| `uefi_fix.exe` (.cs) | Set ApExistFlag=1, ApUseFlag=1, PowerMode=3, MemoryOverClockSwitch=1 |
| `ec_write.exe` (.cs) | Write arbitrary EC registers via UWACPIDriver |
| `ec_snapshot.exe` (.cs) | Full EC dump (0x400-0xFFF) + diff (pre-existing, recompiled) |
| `fan_table_dump.exe` | Fan table + control register dump (pre-existing) |

## Saved Captures

| File | Description |
|------|-------------|
| `uefi_vars_before.txt` | Full UEFI variable list (uninstalled state) |
| `uefi_vars_current.txt` | Full UEFI variable list (current, same as before) |
| `ec_snap_1.bin` | EC state: OEM installed, GCUBridge just started (before Custom mode) |
| `ec_snap_2.bin` | EC state: GCUBridge running, before Custom mode switch |
| `ec_snap_3.bin` | EC state: after switching to Custom mode (partial settings) |
| `ec_snap_4.bin` | EC state: after finishing all Custom mode settings — **WORKING STATE** |
| `ec_snap_5.bin` | EC state: standalone with UEFI flags, no OEM — **NOT WORKING** |

## SOLVED — 2026-04-05

### Root Cause

Two EC registers must be set to activate the EC's PECI power management loop:

- **0x726 |= 0x80** — `ADDR_AP_OEM_BYTE9`, set bit 7
- **0x727 |= 0x40** — `ADDR_CPU_DOUBLE_FLAG_SUPPORT` / DSDT field `CUME` (bit 6)

Without these, the EC stores PL values in RAM but never applies them to the CPU via PECI, resulting in the CPU stuck at 75W regardless of PL1/PL2 register values.

### DSDT Field Mapping

At offset 0x727 in the EC memory region (ECMG at 0xFE410000):
- Bits 0-5: reserved
- Bit 6: **CUME** — likely "CPU Management Enable"
- Bit 7: **PL4D** — PL4 double flag

### How We Confirmed

1. Took EC snapshot with OEM uninstalled (broken state: 0x726=0x00, 0x727=0x80)
2. Reinstalled OEM, set Custom mode, took second snapshot (working: 0x726=0x80, 0x727=0xC0)
3. UEFI diff: zero changes (already correct from uefi_fix.exe)
4. Uninstalled OEM again, ran uefi_fix.exe, rebooted
5. Manually wrote `0x726=0x80`, `0x727=0xC0` via ec_write.exe
6. PL1 immediately applied to CPU (210W confirmed), fan control working

### Fix Applied

Added as step 4 in MechControl's 9-step EC init sequence (`EcActivationControl.cs`):
- Read-modify-write to preserve other bits (safe for all models)
- Added to `UniversalDefaults.InitAllowlist` and `SkipVerify`

### Full Standalone Recipe

MechControl works completely standalone — no UEFI flags, no OEM app, no special setup.
Tested with UEFI flags cleared (PowerMode=1, ApExistFlag=0, ApUseFlag=0) and confirmed working.

MechControl's 9-step EC init handles everything at runtime:
1. Handshake (0x709, 0x708)
2. Activation (0x741=0x05)
3. Feature enable (0x748=0x81)
4. **Power management activation (0x726|=0x80, 0x727|=0x40)** — the key discovery
5. Fan curve enable (0x7C5|=0xE0)
6. Write fan table
7. Fan curve param (0x7C7=0x0C)
8. Fan curve commit (0x7C6=0x04)
9. BIOS_CTRL (0x706=0x41)

`uefi_fix.exe` is no longer required for operation.

## Saved Captures

| File | Description |
|------|-------------|
| `captures/uefi_before_reinstall.txt` | UEFI OEM vars before OEM reinstall |
| `captures/uefi_full_before_reinstall.txt` | Full UEFI variable list before reinstall |
| `captures/uefi_after_reinstall.txt` | UEFI OEM vars after OEM reinstall (identical) |
| `captures/uefi_full_after_reinstall.txt` | Full UEFI variable list after reinstall (identical) |
| `captures/fantable_before_reinstall.txt` | Fan tables before reinstall |
| `captures/fantable_after_reinstall.txt` | Fan tables after reinstall (OEM Custom defaults) |
| `tools/ec_snap_6.bin` | EC state: before OEM reinstall (broken) |
| `tools/ec_snap_7.bin` | EC state: after OEM reinstall + Custom mode (working) |
| `tools/ec_snap_8.bin` | EC state: after second uninstall + reboot (broken again) |
