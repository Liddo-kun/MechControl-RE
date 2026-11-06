# MECHREVO Control Center -- Reverse Engineering Report

**Last updated:** 2026-04-01
**Status:** EC protocol fully decoded. USB HID protocol fully decoded (keyboard, logo, light bar). Service init sequence decoded and implemented -- standalone operation without OEM service verified working. Local dimming implemented.

All register addresses, encodings, and write sequences are in `register_reference.md`. App architecture is in `architecture.md`.

---

## Quick Status

| Feature | Control Path | Status |
|---------|-------------|--------|
| CPU PL1/PL2/PL4 + Temp Limit | EC | Fully decoded, implemented |
| GPU TGP + Dynamic Boost | EC | Fully decoded, implemented |
| Fan Boost / Curve / Table | EC | Fully decoded, implemented |
| Fan Switch Speed | EC `0x787` | Fully decoded, implemented |
| Battery Health Mode | EC `0x7A6` | Fully decoded, implemented |
| Logo LED | EC + USB HID | Fully decoded, implemented |
| Keyboard Backlight | USB HID only | Fully decoded, implemented |
| Light Bar | USB HID only | Fully decoded, implemented |
| Local Dimming | WMI | Implemented |
| Service Init / Standalone | EC `0x741` + `0x7C6` | **Decoded.** 0x741=0x05 (activation) + 0x7C6=0x04 (fan curve mode). See section 11. |
| Mode Switching | EC (100+ registers) | Captured, not replicated (we write individual features) |

---

## 1. System Info

- **Machine:** MECHREVO laptop (Uniwill ODM)
- **GPU:** NVIDIA GeForce RTX 5090 Laptop GPU + Intel Graphics
- **Display:** 60Hz / 240Hz mini LED panel
- **BIOS Project ID:** `IDY_7Y` (ProjectID enum = 13)
- **EC Register 0x740:** Raw value 26 (0x1A). Transform: `projectId = raw >> 1` = 13 = IDY_7Y
- **Baseboard:** YAOSHI Series-X6AR55xY
- **BIOS Version:** N.1.13MRO16 (AMI, 2025-04-17)
- **CPU TjMax:** 105 C

---

## 2. Architecture

### OEM App Stack (what we replaced)
```
UWP App -> MQTT (port 13688) -> GCUBridge.exe -> GCUService.exe -> ACPIDriverDll.dll -> UWACPIDriver.sys -> EC
```

### MechControl
```
MechControl.exe -> DeviceIoControl(\\.\ACPIDriver) -> UWACPIDriver.sys -> EC
MechControl.exe -> HidD_SetFeature -> ITE USB HID controller (lighting)
```

Only kernel dependency: `UWACPIDriver.sys` (49KB, signed by Uniwill Technology Inc., binds to ACPI\INOU0000).

### OEM Component Details

| Component | Role | Notes |
|-----------|------|-------|
| `UWACPIDriver.sys` | Kernel driver -- hardware access | 49KB, KMDF 1.15. We depend on this. |
| `ACPIDriverDll.dll` | Native x64 C++ DLL wrapping DeviceIoControl | 4.1MB, 19 exports. We don't need this. |
| `GCUService.exe` | .NET business logic | 14.7MB, Fody-obfuscated. Our RE target. |
| `GCUBridge.exe` | MQTT broker service | 2.1MB, port 13688. |

---

## 3. Driver Interface

### Device Access
```csharp
IntPtr hDevice = CreateFile(@"\\.\ACPIDriver",
    0xC0000000, 0x3, IntPtr.Zero, 0x3, 0, IntPtr.Zero);
```

### Buffer Format (confirmed from DLL disassembly + Frida capture)

Two independent verification sources:
1. **DLL disassembly** (`ACPIDriverDll.dll`) -- ReadEC/WriteEC functions show exact buffer layout at global buffer RVA 0x27CCA0
2. **Frida live capture** -- Hooked `ntdll!NtDeviceIoControlFile` in GCUService.exe during OEM app usage

```csharp
// READ: 4-byte int address in, 4-byte int out (low byte = value)
// WRITE: 8 bytes = int addr (offset 0, LE) + int value (offset 4, LE)
// Confirmed: Frida capture in(8)=[83 07 00 00 D2 00 00 00] = addr 0x783, value 210
```

**DLL behavior notes:**
- Opens and closes `\\.\ACPIDriver` handle per call (no persistent handle)
- Buffer size passed is 0x400000 (4MB) -- driver only uses first 4-8 bytes; smaller sizes work
- WMI alternative path exists: `WMIReadECRAM`/`WMIWriteECRAM` in managed code (potential fallback)

---

## 4. Local Dimming (WMI)

**NOT EC, NOT HID, NOT DDC/CI.** Confirmed via exhaustive EC scan, Frida full capture, and checking that dxva2.dll is not loaded.

Control path: `CoCreateInstance(WbemLocator)` -> WMI -> ACPI method. The actual WMI call goes out-of-process through COM to wmiprvse.exe. Implemented in `LocalDimmingControl.cs`.

---

## 5. Mode Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| FAN_GAMING_MODE | 0 | Gaming mode (app-level) |
| FAN_OFFICE_MODE | 1 | Office mode |
| FAN_TURBO_MODE | 2 | Turbo mode |
| FAN_CUSTOM_MODE | 3 | Custom mode |
| FAN_MODE_NORMAL | 0 | Normal fan (register uses 0x00) |
| FAN_MODE_BOOST | 1 | Fan boost (register uses 0x40) |
| FAN_MODE_CUSTOMIZE | 2 | Custom fan curve |

---

## 6. Project IDs -- All 44 Known Uniwill Models

Extracted from `Define.ProjectID` enum in GCUService.exe:

| Project ID | Value | | Project ID | Value |
|-----------|-------|---|-----------|-------|
| GI | 1 | | PH4TRX1 | 18 |
| GJ | 2 | | PH4TUX1 | 19 |
| GK | 3 | | PH4TQx1 | 20 |
| GICN | 4 | | PH6TRX1 | 21 |
| GJCN | 5 | | PH6TQxx | 22 |
| GK5CN_X | 6 | | PHxAxxx | 23 |
| GK7CN_S | 7 | | PHxPxxx | 24 |
| GK7CPCS_GK5CQ7Z | 8 | | PH4ARxx | 5889 |
| PF | 9 | | PH4AUxx | 5890 |
| GK5CP_4X_5X_6X | 10 | | PH4AXxx | 5891 |
| IDP | 11 | | PH6AQxx | 5892 |
| **IDY_6Y** | **12** | | PH6ARxx | 5893 |
| **IDY_7Y** | **13** | | PH6AGxx | 5894 |
| PF4MU_PF4MN_PF5MU | 14 | | PH4AUxf | 5895 |
| CML_Gaming | 15 | | PH4PRxx | 6145 |
| GK7NXXR | 16 | | PH4PUxx | 6146 |
| GM5MU1Y | 17 | | PH4PGx1-PH6PG7x150W | 6147-6157 |

### Profile JSON Files

44 RE data profiles at `Profiles/MECHREVO/`, one per project ID. Generated from GCUService.exe via reflection (`generate_profiles.cs`). These are raw reference data -- the app uses simplified profiles at `MechControl/Profiles/MECHREVO/`.

**Identical across all 44 profiles:** IOCTL codes, EC register addresses, fan table addresses, system registers, mode constants, battery enums.

**Differs for IDY_6Y/IDY_7Y:** Features, limits, verified encodings (other 42 profiles have these as "unknown").

### OEM Factory Fan Tables (UserFanTables-OEM/)

Factory default fan curves for 24 models from the driver package. Each file includes PL1, PL2, TCC, DB, CTGP, active point count, and full 16-entry CPU+GPU fan curves.

**Key cross-model variations:**

| Parameter | Range | Notes |
|-----------|-------|-------|
| Active fan points | 6-11 | IDY uses 10. Must be per-model config. |
| GPU fan curve | 0-11 points | Some models are iGPU-only |
| Dynamic Boost | 0-25W or "NA" | "NA" = not supported |
| Hysteresis gap | 1-15 C | NOT fixed -- varies per point and per model |

---

## 7. MQTT Protocol (Reference)

The OEM app uses MQTT internally between its UWP frontend and GCUService backend. Documented for completeness -- MechControl does not use MQTT.

- **Broker:** localhost:13688 (GCUBridge.exe)
- **Credentials:** ClientId=`OcTool`, Username=`OcToolUser`, Password=`OcToolPwd123`
- **Topics:** `Fan/Control`->`Fan/Status`, `Setting/Control`->`Setting/Status`, `BatteryProtection/Control`, `Keyboard/Ctrl`->`Keyboard/Status`

Full MQTT capture: `mqtt_capture_output.txt`

---

## 8. Decompilation & Verification Notes

### GCUService.exe (managed .NET, Fody-obfuscated)
- **Tool:** ilspycmd 9.1.0.7988
- **Problem:** Fody (Costura.Fody) -- nearly all method bodies decompile as `throw new Exception("Runtime exception")`
- **What works:** Constants, enums, field definitions, type signatures
- **Workaround:** Live EC register monitoring with OEM app to observe actual behavior
- **Verification:** Independent ILSpy check confirmed 0 value discrepancies

### ACPIDriverDll.dll (native x64 C++, fully decompiled)
- **Key functions:** ReadEC (RVA 0x3410), WriteEC (RVA 0x34F0)
- **Buffer proof:** RIP-relative addressing confirms addr at globalBuf+0 and data at globalBuf+4

### Frida Captures
- **EC-only capture:** Hooked `ntdll!NtDeviceIoControlFile` -- captured PL, TGP, DB, fan, battery, mode switch writes
- **Full capture (EC + HID + WMI):** Additional hooks for HidD_SetFeature, CoCreateInstance. Decoded all lighting protocols, fan switch speed, local dimming transport

---

## 9. Files & Locations

### Documentation
```
report.md                      -- This file (RE background and methodology)
register_reference.md          -- Register addresses, encodings, write sequences, HID protocol
architecture.md                -- MechControl app architecture and build
CLAUDE.md                      -- Session instructions (also lists evidence archive files)
```

### OEM App
```
C:\Program Files\OEM\<...>\
  AiStoneService\
    GCUBridge.exe              -- MQTT broker
    MyControlCenter\
      GCUService.exe           -- Business logic (RE target)
      ACPIDriverDll.dll        -- Native EC access DLL
      UserFanTables\           -- Fan curve JSONs per mode

C:\Users\Jon\clean\ControlCenter-V1\  -- Our backup copy (DO NOT DELETE)
```

### RE Tools (`tools/`)
```
ec_read_test.cs                -- EC register read test
ec_monitor.cs                  -- Live EC register change monitor
ec_snapshot.cs                 -- EC register snapshot + diff tool
ec_handshake_test.cs           -- Test 0x708/0x709/0x741/0x748/0x7C5/0x706 init registers
ec_apply_test.cs               -- Test OEM6_APPLY (0x7C6) with PL writes
capture_startup.py             -- Automated Frida capture of GCUService startup
mqtt_capture.cs                -- MQTT capture tool
generate_profiles.cs           -- Per-model JSON profile generator
hook_deviceio.py               -- Frida EC hook script
frida_full_hook.js             -- Frida comprehensive hook script
```
Compile .cs tools with: `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe -nologo <file>.cs`

### Tools Installed
```
C:\Users\Jon\.dotnet\              -- .NET SDK 8.0.419
C:\Users\Jon\.dotnet\tools\        -- ilspycmd 9.1.0.7988
```

---

## 10. Safety Notes

**Never write to EC registers without first observing the OEM app write the same value.** Bad EC writes can brick the machine. Always: observe -> replicate -> verify.

### OEM Write Patterns to Replicate
- **0x743 (TGP/DB control):** Write full byte: 0x07 (DB+TGP on) or 0x05 (DB off, TGP on). All 3 bits set explicitly.
- **Fan table:** Interleaved triplet order: `tempUp[N], tempDown[N+1], duty[N]`. `tempDown[0]` never written.
- **PL1/PL2/PL4:** Always written as a group, even when only one changed.
- **Bitmask registers (0x7C5, 0x7A6, 0x7AB, 0x743):** Always read-before-write, only modify target bits.

### Compilation Note
C# RE tools compiled with: `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe` (C# 5 only). Use `-nologo` not `/nologo` in Git Bash.

---

## 11. Service Init / Standalone Operation

**Problem:** MechControl could not operate without GCUService running. CPU power limits and fan curves would not persist -- the EC reverted them immediately. On fresh boot (cold reboot with no OEM service), the system would enter failsafe (CPU locked at 800MHz) and fan table registers contained garbage.

**Root cause (captured 2026-03-31, resolved 2026-04-01):**

The OEM GCUService performs a multi-step EC handshake during startup. Initially we identified only 0x741 and 0x7C6 as required (by testing with stop/restart of the service). But those tests were misleading -- when you stop and restart the service, registers like 0x708/0x709/0x748 persist from the previous run. On a **cold reboot**, they're at power-on defaults, and the EC rejects all configuration.

**Full init sequence required (all steps, in order):**

| Step | Register | Value | Purpose |
|------|----------|-------|---------|
| 1 | 0x709, 0x708 | 0x05, 0x01 | EC handshake -- "OEM software present" |
| 2 | 0x741 | 0x05 | EC activation -- gates PL/fan/GPU writes |
| 3 | 0x748 | 0x81 | Feature enable register |
| 4 | 0x7C5 | OR 0xE0 | Fan curve enable + bits 5,6 (read-modify-write) |
| 5 | 0xF00-0xF5F | fan data | Write complete valid fan table (EC failsafes on garbage data) |
| 6 | 0x7C7 | 0x0C | Fan curve parameter |
| 7 | 0x7C6 | 0x04 | Fan curve mode flag (must stay set, NOT pulse-and-clear) |
| 8 | 0x706 | 0x41 | BIOS_CTRL -- init complete signal |

**Key finding: the default fan table write (step 5) is critical.** On cold boot, fan table registers (0xF00-0xF5F) contain uninitialized data. Setting the init flags without providing a valid fan table causes the EC to enter failsafe. The OEM service writes a complete fan table from its saved profile during init. MechControl now writes a default fan table from the machine profile's `defaultFanCurve` section, then overwrites it with the user's saved profile if one exists.

**What 0x7C6 actually is:** Initially we modeled 0x7C6 as a pulse-and-clear trigger (write 0x04, then 0x00) based on the Frida capture. This was wrong. The capture shows write 0x04 / write 0x00 mid-sequence during the OEM's double-write pattern, but the **final** state after full init is 0x7C6=0x04. It's a persistent mode flag that must stay set for the EC to use the custom fan table. Clearing it causes the EC to ignore the fan table and run fans at ~26% default.

**What went wrong during testing:**
1. Testing with service stop/restart instead of cold reboot -- missed that 0x708/0x709/0x748 need to be written from scratch on cold boot
2. `Oem6Apply()` was coded as pulse-and-clear (write 0x04 then 0x00) -- this immediately undid the mode flag
3. `ec_handshake_test.exe` 0x741 mode had a bug (wrote back 0x00 instead of 0x05), leading to false conclusion that OEM6_APPLY was also needed for PLs
4. Never tested writing the full fan table during init -- assumed the EC would accept curves written later

**Implementation in MechControl:** `EcActivationControl.InitializeEc()` performs all 8 steps on app startup with 100ms delays between steps. If a user profile is saved, it's applied after init (overwriting the default fan table). See `startup.md` for the concise spec.

**Testing methodology:**
- Stopped GCUBridge service, took EC snapshot (baseline)
- Started GCUBridge, immediately attached Frida to GCUService.exe (capture_startup.py)
- Captured all EC writes, HID commands, and WMI calls during init (~20s)
- Took second EC snapshot, diffed against baseline (95 registers changed)
- Isolated 0x741 for PLs via ec_isolate_test.exe (PL1 held 10+ seconds without 0x7C6)
- Isolated 0x7C6 for fan curves via ec_fan_test2.exe (fans only responded when 0x7C6=0x04 left set)
- Cold reboot testing confirmed full 8-step sequence required (2026-04-01)

**Capture files:**
```
notes/service_init_capture.md  -- Full analysis with annotated write sequence
notes/startup.md               -- Concise spec for standalone operation requirements
captures/frida_startup_log.txt -- Raw Frida capture (573 lines)
captures/ec_snap_1.bin         -- EC snapshot (service stopped)
captures/ec_snap_2.bin         -- EC snapshot (service running)
tools/capture_startup.py       -- Automated capture script
tools/ec_handshake_test.cs     -- Register test tool
tools/ec_apply_test.cs         -- OEM6_APPLY test tool
tools/ec_isolate_test.cs       -- 0x741 isolation test
tools/ec_fan_test2.cs          -- 0x7C6 persistent mode flag test
```
