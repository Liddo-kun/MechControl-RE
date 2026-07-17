# Battery Charge Limiting Investigation

## Status: REOPEN — Critical bug invalidated all write tests

Last updated: 2026-04-03

---

## CRITICAL: IOCTL Write Buffer Format Bug

**All standalone test tools used the wrong ECWRITE buffer format.** This was discovered late in the investigation and invalidates most "write failed" conclusions.

### The bug
```
WRONG (all standalone .cs test tools):
  byte[5]: [addr_lo, addr_hi, value, 0, 0]  →  writes silently dropped by driver

CORRECT (MechControl's EcDevice.WriteByteInternal):
  int[2]:  [addr_as_int32, value_as_int32]   →  writes work
  i.e., 8 bytes: the address as a 32-bit LE int, then the value as a 32-bit LE int
```

### What this means
- **Reads were always correct** — the read format (int addr → byte result) worked fine
- **Every "write didn't stick" result from standalone tools was WRONG** — the driver was silently ignoring our malformed writes
- **MechControl's writes always worked** — it uses the correct format via `EcDevice.WriteByteInternal`

### Confirmed with correct format
- `0x7A6` (battery mode): **writable** — old format rejected, new format sticks
- `0x7B9` (CGLM charge limit up): **writable** — value 80 stuck, persists 9+ seconds
- `0x7D0` (charge limit down): **writable** — value 70 stuck, persists
- BUT: writing CGLM=80 while SOC was 89% did NOT stop charging. Rate continued at ~1300mW. The EC firmware stores the value but does not enforce it on this model.

### Tools that need fixing (all use the wrong byte[5] format)
- `bat_charge_test.cs` / `bat_charge_test2.cs`
- `wkbc_charge_test.cs`
- `bat_mode_explore.cs`
- `bat_4ad_test.cs`
- `bat_snapshot_mode.cs`

### Tool with CORRECT format
- `bat_cglm_test.cs` — uses `int[2]` format, confirmed working
- `bat_write_test.cs` — side-by-side comparison proving the format difference
- `MechControl/Driver/EcDevice.cs` — the reference implementation

---

## What Needs Retrying (with correct write format)

### Priority 1: EC Command Interface (0x48A-0x48E)
We tried writing to the WKBC command protocol registers with the OLD format — all writes were silently dropped. With the CORRECT format, the WKBC protocol might work. This matters because:
- WKBC sends a **command** to the EC firmware, not just a raw register write
- The EC might enforce CGLM differently when set via WKBC vs direct register write
- The DSDT uses WKBC for all EC writes through the WMI path
- Registers: LDAT(0x48A), HDAT(0x48B), FLAGS(0x48C), CMDL(0x48D), CMDH(0x48E)
- Protocol: write addr to LDAT/HDAT, write data to CMDL/CMDH, set WFLG bit in FLAGS, poll DRDY

### Priority 2: 0x4AD charge state flag
Diffing High mode vs Stationary mode snapshots showed 0x4AD changing (0x33 vs 0x13, bit 5 = 0x20 difference). We tried toggling it with OLD format — didn't stick. But 0x4AD also appears to be a rapidly-updating counter/status register (low bits increment each second). Still worth retrying the write with correct format to see if it sticks even briefly.

### Priority 3: Battery zone registers (0x400-0x4BF)
We assumed these were all read-only, but we never properly tested writes (only read them). With the correct format, some might be writable. Specifically:
- Near 0x490 (power source) — could have charge control bits
- Near 0x494 (battery alert) — could have charge control flags
- 0x460 (changed in mode diff: 0x03 vs 0x02, fan-related FFAN)

### Priority 4: 0x7A6 mode scan (all 8 values)
The mode scan was done with OLD format. Mode 0 appeared to stick but was likely a coincidence (register was already 0x00 from being on battery). The other values (2, 4, 6, 7) were never tested because the scan broke. Need to redo with correct format, with proper restore between each test. The EC needs ~3 seconds between mode writes.

### Priority 5: Undocumented registers 0x7AC-0x7AF
4-byte gap between known registers. All read as 0x00. Never tried writing (and if we had, wrong format). Located right next to battery mode 0x7A6. Could be voltage/threshold config.

### Lower priority: WKBC via WMI (already partly works)
WMI calls (AcpiTest_MPackage.SetPackage) DO reach the ACPI methods — confirmed by GetPackage returning our buffer. But the WKBC protocol via WMI didn't change CGLM. This might be because WKBC's writes to 0x48A-0x48E go through ACPI SystemMemory writes (not our driver), and those work independently of our buffer format issue. So the WMI path was probably tested correctly and genuinely doesn't work for charge control. Lower priority to retest.

---

## What We Know For Sure

### Battery mode (0x7A6) IS functional — controls charge voltage
- User observation: switching from Stationary to High causes battery to start charging to a higher voltage
- Confirmed by register diff: voltage (0x438) changes, charge rate (0x434) changes, mode (0x7A6) changes
- **Only 0x7A6 changed in the writable config range (0x7A0-0x7FF)** — voltage targets are hardcoded in EC firmware, not in any accessible register
- 3 known modes: 0x08=High, 0x18=Balanced, 0x28=Stationary
- 5 untested mode values: 0, 2, 4, 6, 7 (need rescan with correct format)

### Mode diff results (High at 100% vs Stationary at 100%)
| Address | High | Stationary | Meaning |
|---------|------|------------|---------|
| 0x434-0x435 | 0x01BA (442mW) | 0x0000 (0mW) | Charge rate — Stationary stopped charging |
| 0x438 | 0x9D | 0x56 | Voltage low byte (17053 vs 16982 mV) |
| 0x449 | 0x36 | 0x2D | Unknown counter |
| 0x44C | 0x69 | 0x4D | Unknown |
| 0x460 | 0x03 | 0x02 | Fan-related |
| 0x4AD | 0x33 | 0x13 | Charge state (bit 5 = 0x20 differs) |
| 0x4AE | 0x16 | 0x17 | Unknown (near battery temp) |
| 0x7A6 | 0x08 | 0x28 | Mode register (expected) |

### CGLM (0x7B9) is writable but not enforced
- DSDT confirms it IS the charge limit register (_BST checks CGLM 1-99)
- With correct write format, value sticks (tested: CGLM=80 persisted 9+ seconds)
- But EC firmware does NOT stop charging when SOC exceeds CGLM value
- MyBat=0 (support byte 2 bit 6) confirms OEM knows this model doesn't enforce it

### DSDT architecture (confirmed, doesn't need retesting)
- CGLM at 0x7B9: 7-bit field in ECMG, checked by _BST
- EC command interface at 0x48A-0x48E: LDAT/HDAT/CMDL/CMDH/FLAGS
- WMI classes: AcpiTest_MPackage (WMBC), AcpiTest_MULong (WMBA), AcpiTest_MString (WMBB)
- BAT0 device: standard _BIF/_BST/_BTP, no custom charge methods
- No charge control ACPI methods in DSDT

### OEM app analysis (confirmed, doesn't need retesting)
- BatteryProtection2 gated by MyBat=0 — disabled on this model
- All BatteryProtection2 method bodies are obfuscated
- Support byte enum names are misleading

---

## Register Snapshots on File

Saved for future diffing:
- `snap_high.txt` — EC register dump in High mode at 100% SOC, AC connected
- `snap_stationary.txt` — EC register dump in Stationary mode at 100% SOC, AC connected
- Ranges captured: 0x400-0x4BF (battery), 0x7A0-0x7FF (config)

---

## Changes Made During This Investigation

### MechControl code changes
1. **EcMonitor.cs** — Added `ChargeLimitUp`, `ChargeLimitDown`, `BatteryModeRaw` to `MonitorData`. Reads 0x7B9 and 0x7D0 every tick.
2. **MonitorPanel.cs** — Added "Charge Limits" display showing 0x7B9 and 0x7D0 values with hex, plus raw 0x7A6 hex in battery display.

### Test tools created
| Tool | Status | Format | Purpose |
|------|--------|--------|---------|
| `bat_charge_test.cs` | **BROKEN** (wrong format) | byte[5] | Read/write 0x7B9 and 0x7D0 |
| `bat_charge_test2.cs` | **BROKEN** (wrong format) | byte[5] | Delayed readback test |
| `wkbc_charge_test.cs` | **BROKEN** (wrong format) | byte[5] | EC command interface test |
| `bat_mode_explore.cs` | **BROKEN** (wrong format) | byte[5] | Mode value scanner |
| `bat_4ad_test.cs` | **BROKEN** (wrong format) | byte[5] | 0x4AD charge state toggle |
| `bat_snapshot_mode.cs` | OK (read-only) | byte[4] read | Register snapshot for diffing |
| `bat_voltage_monitor.cs` | OK (read-only) | byte[4] read | Live voltage/rate monitor |
| `bat_cglm_test.cs` | **CORRECT** | int[2] | CGLM write test (confirmed working) |
| `bat_write_test.cs` | **CORRECT** | int[2] | Format comparison proof |
| `bat_rapid_write.cs` | **BROKEN** (wrong format) | byte[5] | Rapid write timing test |

### DSDT extraction
- `dsdt.dat` — Raw ACPI DSDT binary (594KB)
- `dsdt.dsl` — Decompiled ASL source (105K lines)
- `iasl.exe` — Intel ACPI decompiler (was already present)

---

## Test Tools Reference

```bash
# CORRECT format tools:
./bat_cglm_test.exe            # read CGLM state
./bat_cglm_test.exe 80         # set CGLM=80 (correct format)
./bat_cglm_test.exe reset      # set CGLM=0

./bat_write_test.exe           # proves old vs new format difference

# Read-only tools (work fine):
./bat_voltage_monitor.exe 2    # live monitor, 2s interval
./bat_snapshot_mode.exe        # register dump for diffing

# BROKEN tools (wrong write format — DO NOT TRUST write results):
./bat_charge_test.exe          # reads work, writes broken
./wkbc_charge_test.exe         # reads work, all writes broken
./bat_mode_explore.exe         # reads work, mode writes broken
./bat_4ad_test.exe             # reads work, toggle broken
```

---

## Correct EC Write Format Reference

When building new tools or fixing existing ones, use this pattern:

```csharp
// CORRECT: matches MechControl's EcDevice.WriteByteInternal
[DllImport("kernel32.dll", SetLastError = true)]
static extern bool DeviceIoControl(IntPtr h, uint code,
    int[] lpIn, uint inSz, ref int lpOut, uint outSz, out uint ret, IntPtr ov);

static void WriteEC(int addr, byte val)
{
    int[] inBuf = new int[] { addr, val };
    int outBuf = 0;
    uint r;
    DeviceIoControl(hDev, IOCTL_ECWRITE, inBuf, 8, ref outBuf, 4, out r, IntPtr.Zero);
}

static byte ReadEC(int addr)
{
    int outBuf = 0;
    uint r;
    DeviceIoControl(hDev, IOCTL_ECREAD, new int[] { addr }, 4, ref outBuf, 4, out r, IntPtr.Zero);
    return (byte)(outBuf & 0xFF);
}
```

---

## Files to Read for Context
- `register_reference.md` section 11.6 — Battery & Charge registers
- `interestingregisterstoprobe.md` — Undocumented EC register candidates
- `dsdt.dsl` — Full decompiled ACPI DSDT (105K lines). Key sections:
  - Line 104417: ECMG field definitions (EC register map including CGLM)
  - Line 104609: ECMP/ECXP regions
  - Line 105622: BAT0 device (_BIF, _BST, _BTP)
  - Line 105702: CGLM charge limit check in _BST
  - Line 103025: AMW0 WMI _WDG definitions
  - Line 103466: WMBA/WMBB/WMBC WMI methods
  - Line 103732: WKBC protocol implementation
  - Line 103794: SCMD protocol implementation
- `MechControl/Driver/EcDevice.cs` lines 90-100 — **Reference for correct IOCTL buffer format**
- `MechControl/Features/BatteryControl.cs` — Current (cosmetic) implementation
- `MechControl/Monitoring/EcMonitor.cs` — Monitors 0x7B9/0x7D0
- `gcuservice_full.txt` lines 14276-14582 — BatteryProtection2 class (obfuscated)
- `defaulttool_decompile.txt` lines 14000-14019 — SupportByteOneFlag/TwoFlag enums
