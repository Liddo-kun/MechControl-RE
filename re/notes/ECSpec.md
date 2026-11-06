# ECSpec Configuration — Reverse Engineering Report

## Summary

The ECSpec IOCTL (`0x9C40A500`) reads a 9-byte `SMAPCTABLE_STRUCT` from the EC firmware containing **per-model power limits**. This was fully decoded from a single machine (IDY_7Y) with 6/6 known parameters matching perfectly. Implementing runtime extraction would eliminate the need for hardcoded profile limits — the EC itself tells us the valid ranges.

## How It Works

### IOCTL Interface

```
IOCTL:  0x9C40A500  (called "SMAPCTABLE" in driver)
Input:  [0xBB, index]  packed as int32: 0xBB | (index << 8)
Output: 4 bytes (int32, little-endian)
```

The driver has a 9-byte internal buffer. Each IOCTL call reads a **32-bit DWORD at byte offset `index`**, creating a sliding window. Only the **LSB** (`result & 0xFF`) is the actual value at that index. The upper 3 bytes are just the next bytes in the buffer leaking through.

### Reading in Code

Current code in `EcDevice.ReadSmapcTable(int index)`:
```csharp
int inBuf = 0xBB | (index << 8);
// ... DeviceIoControl ...
return (uint)outBuf;
```

To extract the actual byte at each index:
```csharp
byte value = (byte)(ReadSmapcTable(index) & 0xFF);
```

### Out-of-Range Detection

When `index >= 9`, the driver echoes the input command:
```
Index 9  -> 0x000009BB  (contains 0xBB in LSB = echo)
Index 10 -> 0x00000ABB
Index 11 -> 0x00000BBB
```

Check: `if ((result & 0xFF) == 0xBB)` means the index is out of range.

## Buffer Layout: SMAPCTABLE_STRUCT

| Index | Field Name | IDY_7Y Raw | Decoded | Encoding | Verified |
|-------|-----------|-----------|---------|----------|----------|
| 0 | PL1 (max) | 0xD2 | 210W | direct: raw = watts | Yes, matches profile |
| 1 | PL2 (max) | 0xD2 | 210W | direct: raw = watts | Yes, matches profile |
| 2 | PL4 (max) | 0xD2 | 420W | double: raw * 2 = watts | Yes, matches profile |
| 3 | TccOffset (max) | 0x0A | 95C | inverted_offset: tjMax(105) - raw = max temp | Yes, matches profile |
| 4 | ConfigurableTGP (max) | 0x37 | 150W | offset: raw + DefaultTGP[7] = watts | Yes, matches profile |
| 5 | DynamicBoost (max) | 0x19 | 25W | direct: raw = watts | Yes, matches profile |
| 6 | TargetTemperature | 0x57 | 87C | direct: raw = degrees C | New — GPU thermal target |
| 7 | DefaultTGP | 0x5F | 95W | direct: raw = watts (TGP base/minimum) | Yes, matches TGP offset |
| 8 | DynamicBoostCpuTdp | 0xFF | unused | 0xFF = not applicable | Padding for this model |

### Raw ECSpec Dump (IDY_7Y)

```
ECSpec[0x00]: 0x0AD2D2D2   -> buf[0]=0xD2 (PL1=210W)
ECSpec[0x01]: 0x370AD2D2   -> buf[1]=0xD2 (PL2=210W)
ECSpec[0x02]: 0x19370AD2   -> buf[2]=0xD2 (PL4 raw=210, 420W)
ECSpec[0x03]: 0x5719370A   -> buf[3]=0x0A (TccOffset=10, temp=95C)
ECSpec[0x04]: 0x5F571937   -> buf[4]=0x37 (ConfigTGP raw=55, 150W)
ECSpec[0x05]: 0xFF5F5719   -> buf[5]=0x19 (DynBoost=25W)
ECSpec[0x06]: 0xFFFF5F57   -> buf[6]=0x57 (TargetTemp=87C)
ECSpec[0x07]: 0xFFFFFF5F   -> buf[7]=0x5F (DefaultTGP=95W)
ECSpec[0x08]: 0xFFFFFFFF   -> buf[8]=0xFF (unused)
ECSpec[0x09]: 0x000009BB   -> echo (out of range)
ECSpec[0x0A]: 0x00000ABB   -> echo (out of range)
ECSpec[0x0B]: 0x00000BBB   -> echo (out of range)
```

## Key Discoveries

### 1. TGP Offset Is Not Hardcoded

The IDY_7Y profile hardcodes `"offset": 95` for TGP encoding. This 95 actually comes from ECSpec index 7 (`DefaultTGP`). The TGP register (0x744) stores `watts - DefaultTGP`, and `DefaultTGP` varies per model. On the E23 test machine, this value will likely differ.

**Impact:** The TGP encoding offset should be read from ECSpec[7] at runtime, not hardcoded in profiles.

### 2. GPU Thermal Target

Index 6 = 87C is the GPU thermal throttle target. Previously unknown. Could be used to:
- Display in the monitor panel
- Validate fan curve sanity (fan should be at 100% before this temp)
- Detect GPU thermal headroom

### 3. DynamicBoostCpuTdp

Index 8 = 0xFF on IDY_7Y (unused). On models with Dynamic Boost affecting CPU TDP, this likely contains the CPU TDP limit when Dynamic Boost is active. Models with `"DB": "NA"` in OEM fan tables may have 0xFF here.

### 4. These Are MAX Values

The buffer contains maximums, not min/max pairs. The minimums appear to be implicit:
- PL1/PL2 min: likely a fixed lower bound (10W or 15W)
- PL4 min: likely PL1 min * 2
- TGP min: likely DefaultTGP (index 7) — i.e., the base TGP with zero offset
- Dynamic Boost min: likely 0 or 5
- CPU temp min: likely a fixed lower bound (80C)

## Implementation Plan

### Step 1: Extract Bytes from ECSpec

In `HardwareDetection.cs`, after reading the raw ECSpec entries, extract the individual bytes:

```csharp
public byte[] EcSpecBytes { get; } = new byte[9];

// After reading EcSpecConfig[]:
for (int i = 0; i < 9; i++)
    EcSpecBytes[i] = (byte)(EcSpecConfig[i] & 0xFF);
```

### Step 2: Populate Limits from ECSpec

Create a method that builds `LimitInfo` entries from the extracted bytes:

```csharp
public Dictionary<string, LimitInfo> ExtractLimits()
{
    byte pl1Max = EcSpecBytes[0];
    byte pl2Max = EcSpecBytes[1];
    byte pl4MaxRaw = EcSpecBytes[2];  // double encoding
    byte tccOffset = EcSpecBytes[3];
    byte tgpMaxRaw = EcSpecBytes[4];
    byte dbMax = EcSpecBytes[5];
    byte gpuTargetTemp = EcSpecBytes[6];
    byte defaultTgp = EcSpecBytes[7];

    int tgpMin = defaultTgp;  // base TGP = minimum
    int tgpMax = tgpMaxRaw + defaultTgp;
    int tempMax = 105 - tccOffset;  // inverted offset from TjMax

    return new Dictionary<string, LimitInfo>
    {
        ["PL1"] = new() { Min = 10, Max = pl1Max, Encoding = "direct" },
        ["PL2"] = new() { Min = 10, Max = pl2Max, Encoding = "direct" },
        ["PL4"] = new() { Min = 20, Max = pl4MaxRaw * 2, Encoding = "double" },
        ["cpuTempLimit"] = new() { Min = 80, Max = tempMax, Encoding = "inverted_offset", TjMax = 105 },
        ["TGP"] = new() { Min = tgpMin, Max = tgpMax, Encoding = "offset", Offset = defaultTgp },
        ["dynamicBoost"] = new() { Min = 0, Max = dbMax, Encoding = "direct" },
    };
}
```

### Step 3: Priority in Limit Resolution

```
1. Profile JSON limits (verified profiles — highest trust)
2. ECSpec-extracted limits (runtime, per-model — high trust)
3. Conservative defaults from UniversalDefaults.cs (fallback — lowest trust)
```

For unverified profiles, ECSpec limits should override the conservative defaults since they come directly from the EC firmware for that specific machine.

### Step 4: Validation

ECSpec values should be sanity-checked before use:
- PL1/PL2 max: 15-300W
- PL4 max raw: 15-250 (30-500W)
- TccOffset: 0-25 (temp 80-105C)
- TGP max raw: 0-200
- DB max: 0-50W
- DefaultTGP: 50-200W
- TargetTemp: 60-105C
- All non-0xFF (0xFF likely means "not applicable")

### Step 5: Log the Decoded Values

Add to `DiagnosticLog`:
```
--- ECSpec Decoded Limits ---
  PL1 max:     210W (raw=0xD2)
  PL2 max:     210W (raw=0xD2)
  PL4 max:     420W (raw=0xD2, x2)
  CPU temp max: 95C (TccOffset=10, TjMax=105)
  TGP range:   95-150W (default=95, configurable=55)
  DB max:      25W (raw=0x19)
  GPU target:  87C (raw=0x57)
```

## Confirmation Needed

This mapping was derived from a **single machine**. To fully confirm:

1. **E23 test machine** — read ECSpec, check if the decoded limits match the machine's actual capabilities
2. **OEM fan table cross-reference** — for the 24 models with OEM fan tables, the PL1/PL2/TGP/DB values in the fan tables should be <= the ECSpec maximums
3. **Models with DB="NA"** — should have 0xFF or 0x00 at index 5 (DynamicBoost)
4. **Models with different TGP bases** — index 7 (DefaultTGP) should differ from 95

## Files to Modify

- `Features/HardwareDetection.cs` — extract bytes, decode limits
- `Profile/UniversalDefaults.cs` — add ECSpec-based limit population as priority 2
- `UI/MainForm.cs` — use ECSpec limits when available for unverified profiles
- `DiagnosticLog.cs` — log decoded ECSpec values
- `Features/GpuPowerControl.cs` — read TGP offset from ECSpec[7] instead of profile
