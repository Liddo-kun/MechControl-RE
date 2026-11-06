# EC DeviceIoControl Buffer Format — Ground Truth Capture

## Tool & Setup

**Tool:** Frida 17.9.1 (Python dynamic instrumentation)
**Script:** `hook_deviceio.py` — hooks `ntdll!NtDeviceIoControlFile` in GCUService.exe, filters for ACPI IOCTL codes (0x9C40xxxx), logs input/output buffers.
**Method:** Attach to GCUService.exe by PID, intercept at ntdll syscall level.

**Why not API Monitor:** API Monitor v2 was tried first. It hooks via IAT (Import Address Table) patching, but GCUService.exe is a .NET application whose P/Invoke calls resolve `kernel32!DeviceIoControl` at runtime via `GetProcAddress`, bypassing the IAT. API Monitor only captured network socket calls (mswsock.dll). Frida uses inline hooking (patches function prologue), which catches all calls regardless of how they're dispatched.

## Read Format (confirmed)

```
NtDeviceIoControlFile(..., IoControlCode=0x9C40A488, ...)
  Input buffer:  4 bytes — int32 LE (register address)
  Output buffer: 4 bytes — int32 LE (low byte = register value)
  NT status:     0 (STATUS_SUCCESS)
```

Example — read PL1 register:
```
in(4) = [83 07 00 00]    → addr = 0x0783
out(4) = [96 00 00 00]   → value = 0x96 = 150W
```

## Write Format (confirmed)

```
NtDeviceIoControlFile(..., IoControlCode=0x9C40A48C, ...)
  Input buffer:  8 bytes — [int32 LE address][int32 LE value]
  Output buffer: 4 bytes — returns 0xFFFFFFFE (constant)
  NT status:     0 (STATUS_SUCCESS)
```

Buffer layout:
```
Offset  Size    Content
0-3     int32   Register address (little-endian)
4-7     int32   Value to write (little-endian, only low byte is meaningful)
```

**The write format hypothesis is confirmed: 8 bytes = [int32 address][int32 value], both little-endian.**

**Output buffer:** All writes return `FE FF FF FF` (0xFFFFFFFE). This is a constant driver response, not a meaningful status code.

---

## Captured Operations

### 1. CPU Power Limits (PL1 / PL2 / PL4)

**Registers:** 0x783 (PL1), 0x784 (PL2), 0x785 (PL4)
**Encoding:** Direct — 1 byte = 1 watt

| Test | Change | Buffer | Decoded |
|------|--------|--------|---------|
| PL1 10→210W | `[83 07 00 00 D2 00 00 00]` | addr=0x783, val=0xD2=210 |
| PL1 210→117W | `[83 07 00 00 75 00 00 00]` | addr=0x783, val=0x75=117 |
| PL1 149→150W | `[83 07 00 00 96 00 00 00]` | addr=0x783, val=0x96=150 |
| PL2 210→209W | `[84 07 00 00 D1 00 00 00]` | addr=0x784, val=0xD1=209 |

**Behavior:** PL1/PL2/PL4 are always written as a group of 3, even when only one value changed. The group is written twice per change (double-write pattern).

### 2. GPU Configurable TGP

**Register:** 0x744 (ADDR_ConfigurableTGP_VALUE)
**Encoding:** Delta above base TDP (base TDP = 95W for this GPU)

| Test | Change | Buffer | Decoded |
|------|--------|--------|---------|
| TGP 150→149W | `[44 07 00 00 36 00 00 00]` | addr=0x744, val=0x36=54 |

Register stores `TGP - base_TDP`: 149W total - 95W base = 54W delta. Previous value was 55 (150W total).

### 3. GPU Dynamic Boost

**Register:** 0x746
**Encoding:** Direct — 1 byte = 1 watt

| Test | Change | Buffer | Decoded |
|------|--------|--------|---------|
| DB 25→24W | `[46 07 00 00 18 00 00 00]` | addr=0x746, val=0x18=24 |

### 4. CPU Temperature Limit (TCC Offset)

**Register:** 0x786 (ADDR_TimAP_TccOffset_Setting)
**Encoding:** Bit 7 = enable flag, bits 0-6 = offset in degrees C. Actual temp = 105 - offset.

| Test | Change | Buffer | Decoded |
|------|--------|--------|---------|
| 95→94°C (offset 10→11) | `[86 07 00 00 8B 00 00 00]` | addr=0x786, val=0x8B = 0x80 \| 11 |

### 5. Fan Control Enable/Disable

**Register:** 0x7C5 (ADDR_AP_OEM_BYTE5)
**Encoding:** Bit 7 = fan control enabled. Other bits preserved.

| Test | Read Before | Write After | Bit Change |
|------|-------------|-------------|------------|
| Disable | 0xE8 (11101000) | 0x68 (01101000) | Bit 7 cleared |
| Enable  | 0x68 (01101000) | 0xE8 (11101000) | Bit 7 set |

Pattern: read-modify-write. Only bit 7 changes, all other bits preserved.

### 6. Battery Health Protection Mode

**Register:** 0x7A6
**Encoding:** Bits 4-5 encode the mode (maps to BatteryHealthProtection_Status enum). Bit 3 always set.

| Mode | Value | Binary | Enum |
|------|-------|--------|------|
| High Capacity | 0x08 | 00**00**1000 | PERFORMANCEDMODE (0) |
| Balanced | 0x18 | 00**01**1000 | BALANCEDMODE (1) |
| Stationary | 0x28 | 00**10**1000 | HEALTHYMODE (2) |

All three transitions captured and verified bidirectionally.

### 7. Performance Mode Switch (Turbo → Balanced)

A full mode switch writes ~100+ registers including:

| Register | Value | Purpose |
|----------|-------|---------|
| 0x07C6 (OEM_BYTE6) | 0x00 → 0x04 | Clear then set apply flag (bit 2) |
| 0x0706 (BIOS_CONTROL) | 0x40 | FanBoost mode flag |
| 0x0751 (FAN_CONTROL) | 0x00 | Normal mode (balanced) |
| 0x0783 (PL1) | 0x00 | PL1 = 0 (use EC default) |
| 0x0784 (PL2) | 0x00 | PL2 = 0 (use EC default) |
| 0x0785 (PL4) | 0x00 | PL4 = 0 (use EC default) |
| 0x0744 (TGP) | 0x37 (55) | GPU TGP = 55W delta = 150W total |
| 0x0748 (LIGHTBAR) | 0x8D → 0x81 | Lightbar control sequence |
| 0x0769/A/B (LOGO RGB) | 0,0,22 | Logo color = deep blue |
| 0x078C (OEM_BYTE2) | 0x10 | Turbo mode flag |
| 0x0F00-0x0F5F | (fan table) | Full CPU + GPU fan curves (96 entries) |

**Key insight:** Balanced mode sets PL1/PL2/PL4 to 0 (EC defaults), while custom/turbo modes write explicit watt values.

### 8. Fan Table Structure

**Layout (RamFan1, 0x0F00-0x0F5F):**

| Range | Count | Content |
|-------|-------|---------|
| 0x0F00-0x0F0F | 16 | CPU temp UP thresholds (°C) |
| 0x0F10-0x0F1F | 16 | CPU temp DOWN thresholds (°C, hysteresis) |
| 0x0F20-0x0F2F | 16 | CPU fan duty (PWM, 0-200) |
| 0x0F30-0x0F3F | 16 | GPU temp UP thresholds (°C) |
| 0x0F40-0x0F4F | 16 | GPU temp DOWN thresholds (°C, hysteresis) |
| 0x0F50-0x0F5F | 16 | GPU fan duty (PWM, 0-200) |

- **10 active points** (indices 0-9), **6 padding slots** (indices 10-15)
- Padding: temp slots = 0xFF (255), duty slots = 0xB4 (180) or 0xC8 (200)
- Written in interleaved triplets: TEMP_UP[n], TEMP_DOWN[n+1], DUTY[n]
- TEMP_DOWN[0] is never written (can't step down from level 0)
- CPU and GPU sections are independent — changing a GPU fan point only rewrites 0x0F30-0x0F5F

**Behavior:** The OEM app rewrites the entire fan table even when:
- Only one point changes
- The user merely clicks the fan graph without changing anything

### 9. Keyboard Backlight

No EC writes observed when toggling keyboard backlight in the OEM app. The backlight did not actually change — either the toggle was non-functional or the keyboard backlight uses a different control path (HID, WMI, or separate RGB controller).

---

## Observed Read Polling

GCUService continuously polls these registers (approx every 250ms):

| Address | Register | Observed Value | Purpose |
|---------|----------|----------------|---------|
| 0x07CC | (status) | 0x81 (129) | Primary status/heartbeat — polled constantly |
| 0x0727 | ADDR_ESHUTTER_STATUS | 0xC0 (192) | Camera shutter / status |
| 0x07CA | (unknown) | 0x01 | Flag register |
| 0x074C | (near TGP) | 0xC6 (198) | Unknown |
| 0x0766 | ADDR_SUPPORT_BYTE2 | 0x93 (147) | Feature support flags |
| 0x07C5 | ADDR_AP_OEM_BYTE5 | varies | Fan control flags (bit 7 = enabled) |
| 0x07A6 | (battery) | varies | Battery health mode |

---

## Behavioral Patterns

1. **Read-before-write:** The service reads a register before writing to it (observed for bitmask registers like 0x7C5, 0x7A6).
2. **Group writes:** PL1/PL2/PL4 are always written as a trio. Fan tables are always written in full.
3. **Double-write:** PL changes produce two identical batches. Purpose unclear — possibly firmware handshake.
4. **Aggressive re-application:** Opening a settings page, clicking a fan graph, or switching tabs triggers a full profile re-write (100+ EC writes), even with no user changes.
5. **Single-register toggles:** Fan control and battery mode are efficient single-register writes using read-modify-write bitmask operations.

---

## Summary Table

| Parameter | ECREAD | ECWRITE |
|-----------|--------|---------|
| IOCTL code | 0x9C40A488 | 0x9C40A48C |
| Input size | 4 bytes | **8 bytes** |
| Input format | int32 LE (addr) | **int32 LE (addr) + int32 LE (value)** |
| Output size | 4 bytes | 4 bytes |
| Output format | int32 LE (low byte = value) | 0xFFFFFFFE (constant) |
| NT status | 0 (success) | 0 (success) |

## Register Encoding Summary

| Register | Address | Encoding | Notes |
|----------|---------|----------|-------|
| PL1 | 0x783 | Direct watts | 1 byte = 1W |
| PL2 | 0x784 | Direct watts | 1 byte = 1W |
| PL4 | 0x785 | Direct watts | 1 byte = 1W |
| GPU cTGP | 0x744 | Delta above base TDP | val + 95W = total TGP |
| GPU Dynamic Boost | 0x746 | Direct watts | 1 byte = 1W |
| TCC Offset | 0x786 | bit7=enable, bits0-6=offset | temp = 105 - offset |
| Fan Control | 0x7C5 | Bitmask | bit7 = enabled |
| Battery Mode | 0x7A6 | bits4-5 = mode enum | 0=perf, 1=balanced, 2=health |
| Fan Table | 0xF00-0xF5F | 6 arrays x 16 entries | temp°C or PWM duty |
