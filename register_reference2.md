# EC Register Reference

Register-centric reference for MechControl. Each row below states what the
byte is and whether any MechControl code reads or writes it. Supporting
sections at the end cover the fan table layout, multi-register write
sequences, EC init, the HID lighting protocol, and WriteGuard rules.

For RE methodology and evidence, see `report.md` and the files under
`captures/`. For architecture and build instructions, see `architecture.md`.

---

## 1. Driver Interface

| Field | Value |
|-------|-------|
| Device path | `\\.\ACPIDriver` (UWACPIDriver.sys) |
| IOCTL read  | `0x9C40A488` |
| IOCTL write | `0x9C40A48C` |

**Read:** input is a 4-byte little-endian int (EC address); output is a
4-byte little-endian int whose low byte is the register value.

**Write:** input is 8 bytes — `int addr` at offset 0 plus `int value` at
offset 4, both little-endian. The output buffer is always `0xFFFFFFFE` and
carries no meaning.

The EC driver exposes 28 IOCTLs in total. MechControl only uses these two
plus `0x9C40A500` (ECSpec batch config read) during startup hardware
detection.

---

## 2. Register Access Model

MechControl never hard-codes register addresses in feature code. Instead,
features call `profile.GetRegister("canonicalName")` to look up the address
from the active profile (`MechControl/Profiles/MECHREVO/*.json`). The
profile maps short canonical names like `cpuFanDuty` to the model-specific
address.

The **OEM extraction profiles** under `Profiles/MECHREVO/*.json` at the repo
root use the original OEM symbol names (`ADDR_EC_MAIN_FAN_L_DUTY_BYTE`,
etc.) and are only used during model onboarding to build the compact app
profile. They are not loaded at runtime.

The tables below list the canonical profile name when one exists,
followed by the OEM symbol name from the extraction profile.

**Legend for R/W column:**
- `R` — read by MechControl
- `W` — written by MechControl
- `RW` — both
- `F&F` — fire-and-forget write (WriteGuard skips verification)
- `—` — not accessed by MechControl (OEM symbol only, or reserved)

---

## 3. Register Map

Registers are listed in ascending address order within each EC page.
"Unknown" rows mark address ranges that fall inside a functional block but
have no documented meaning. The EC exposes a sparse 4 KiB address space;
only the addresses below have ever been observed in Frida captures or the
OEM symbol table.

### 3.1 ACPI Mirror Page (0x402–0x4AB)

These registers are exposed by the DSDT `OperationRegion ECXP`
(`SystemMemory` at `ECMA+0x400`). They mirror standard SBS/ACPI battery
info plus some sensor and identity bytes. All 16-bit pairs are
**little-endian** — lower address holds the low byte — even though the OEM
symbol names label `BYTE1` as "high byte". Fan RPM registers in this range
(0x464–0x46C) use the opposite convention; see the comment in
`EcMonitor.cs`.

DSDT `_BIF` sets `PowerUnit = 1` (mAh), so capacity values are mAh and rate
values are mA. Voltage is mV. Battery temperature is SBS-standard 0.1 K.

| Addr | R/W | Name | Description | MechControl usage |
|------|-----|------|-------------|-------------------|
| `0x402-0x403` | R | `ADDR_EC_BIF_DC` | Battery design capacity (16-bit LE, mAh) | `BatteryControl` telemetry |
| `0x404-0x405` | R | `ADDR_EC_BIF_LFC` | Battery last full charge capacity (16-bit LE, mAh). `SOH% = LFC / DC × 100`. | `BatteryControl` telemetry |
| `0x406-0x407` | — | — | unknown | — |
| `0x408-0x409` | R | `ADDR_EC_BIF_DV` | Battery design voltage (16-bit LE, mV) | `BatteryControl` telemetry |
| `0x40A-0x433` | — | — | unknown | — |
| `0x434-0x435` | R | `ADDR_EC_BST_BPR` | Battery present rate — charge/discharge (16-bit LE, mA) | `BatteryControl` telemetry |
| `0x436-0x437` | R | `ADDR_EC_BST_BRC` | Battery remaining capacity (16-bit LE, mAh) | `BatteryControl` telemetry |
| `0x438-0x439` | R | `ADDR_EC_BST_BPV` | Battery present voltage (16-bit LE, mV) | `BatteryControl` telemetry |
| `0x43A-0x43D` | — | — | unknown | — |
| `0x43E` | R | `cpuTemp` | CPU temperature, °C (EC sensor, direct byte) | `EcMonitor` poll, `LinkedFanController` |
| `0x43F-0x448` | — | — | unknown | — |
| `0x449` | R | — | Adapter current; `raw / 10 = amps` | `EcMonitor` poll |
| `0x44A-0x44E` | — | — | unknown | — |
| `0x44F` | R | `gpuTemp` | GPU temperature, °C (EC sensor, direct byte) | `EcMonitor` poll, `LinkedFanController` |
| `0x450-0x453` | — | — | unknown | — |
| `0x454` | R | `ecOEM_EC_VER` | EC firmware version | `HardwareDetection` at startup |
| `0x455` | — | — | unknown | — |
| `0x456` | R | `ADDR_SystemID` | System identification byte | `HardwareDetection` at startup |
| `0x457-0x45A` | — | — | unknown | — |
| `0x45B` | R | `ADDR_SILENTMODE_STATUS_BYTE` | Silent mode status (0 = off, nonzero = active) | OEM symbol only; not used |
| `0x45C-0x462` | — | — | unknown | — |
| `0x463` | R | `cpuTempLimitRead` | CPU temp limit readback: `105 - raw` = °C limit (Intel). Written via `0x786`. | `EcMonitor` (limit display), `WriteGuard` verify redirect for `0x786` |
| `0x464` | R | `cpuFanRpmLo` (`ADDR_EC_MAIN_FAN_RPM_BYTE1`) | CPU fan RPM — byte A. `RPM = (0x464 << 8) | 0x465`. EC firmware byte order is inconsistent across models; see `EcMonitor.cs:113`. | `EcMonitor` poll, `PerfCounterPublisher` |
| `0x465` | R | `cpuFanRpmHi` (`ADDR_EC_MAIN_FAN_RPM_BYTE2`) | CPU fan RPM — byte B | `EcMonitor` poll |
| `0x466` | R | `ADDR_EC_BIOS_INFO5` | BIOS info register 5 | OEM symbol only; not used |
| `0x467-0x46A` | — | — | unknown | — |
| `0x46B` | R | `gpuFanRpmLo` (`ADDR_EC_SECOND_FAN_RPM_BYTE2`) | GPU fan RPM — byte A. `RPM = (0x46C << 8) | 0x46B`. | `EcMonitor` poll |
| `0x46C` | R | `gpuFanRpmHi` (`ADDR_EC_SECOND_FAN_RPM_BYTE1`) | GPU fan RPM — byte B | `EcMonitor` poll |
| `0x46D-0x475` | — | — | unknown | — |
| `0x476` | R | `BIOSFuncReg` | BIOS function register | OEM symbol only; not used |
| `0x477-0x47C` | — | — | unknown | — |
| `0x47D` | R | `ecOEM_SUB_VER2` | EC sub-version 2 | `HardwareDetection` at startup |
| `0x47E-0x48F` | — | — | unknown | — |
| `0x490` | R | `ecPowSource` | Power source (bit 0 = AC connected) (DSDT `ACIC`) | `BatteryControl` telemetry |
| `0x491-0x493` | — | — | unknown | — |
| `0x494` | R | `ADDR_BATTERY_ALERT_BYTE` | Battery alert flags | OEM symbol only; not used |
| `0x495-0x49E` | — | — | unknown | — |
| `0x49F` | R | `ADDR_BIOS_INFO_3_BYTE` | BIOS info byte 3 | OEM symbol only; not used |
| `0x4A0-0x4A1` | — | — | unknown | — |
| `0x4A2-0x4A3` | R | `ecBt1Temperature` | Battery temperature (16-bit LE, 0.1 K). `°C = raw/10 - 273.15`. | `BatteryControl` telemetry |
| `0x4A4-0x4A5` | — | — | unknown | — |
| `0x4A6-0x4A7` | R | `ADDR_EC_BT1CycleCount` | Battery cycle count (16-bit LE) (DSDT `CYCN`) | `BatteryControl` telemetry |
| `0x4A8-0x4AA` | — | — | unknown | — |
| `0x4AB` | R | `batterySoc` (`ecBt1RSOC`) | Battery relative state of charge, % | `EcMonitor` poll, `BatteryControl` telemetry |

### 3.2 System Power Page (0x60C–0x60D)

| Addr | R/W | Name | Description | MechControl usage |
|------|-----|------|-------------|-------------------|
| `0x60C-0x60D` | R | — | System power consumption (16-bit LE, watts) | `EcMonitor` poll |

No other addresses in the `0x600` range have been documented.

### 3.3 Control Page (0x706–0x7FB)

This is the main control page: handshake, activation, CPU/GPU power,
fan control, battery mode, lighting, and OEM config bytes.

| Addr | R/W | Name | Description | MechControl usage |
|------|-----|------|-------------|-------------------|
| `0x706` | W | `ADDR_AP_BIOS_CONTROL_BYTE` | BIOS control byte. Writing `0x41` signals "init complete". | `EcActivationControl` init (step 7) |
| `0x707` | — | — | unknown | — |
| `0x708` | RW | (handshake step 2) | Unlock step 2. Writes `0x01` after `0x709`; readback is `0x01`. | `EcActivationControl` init (step 1) |
| `0x709` | W | (handshake step 1) | Unlock step 1. Writes `0x05` before `0x708`. | `EcActivationControl` init (step 1) |
| `0x70A-0x725` | — | — | unknown | — |
| `0x726` | RW | `ADDR_AP_OEM_BYTE9` | Power-management activation. Bit 7 must be set (`\|= 0x80`) to enable the EC's PECI power-management loop. Persists in EC NVRAM. Clearing it deactivates PL enforcement immediately. | `EcActivationControl` init (read-modify-write) |
| `0x727` | RW | `ADDR_CPU_DOUBLE_FLAG_SUPPORT` / `ADDR_ESHUTTER_STATUS` | Multi-purpose DSDT-mapped byte. **Bit 6 = CUME** (CPU Management Enable) — must be set (`\|= 0x40`) to activate the PECI loop alongside `0x726` bit 7. **Bit 7 = PL4D** — when set, `0x785` encodes PL4 as raw × 2 watts; when clear, raw = watts. OEM symbol also names it as e-shutter status for polled reads. | `HardwareDetection` reads at startup to determine PL4 encoding; `EcActivationControl` init sets CUME bit |
| `0x728` | RW | `ADDR_AP_OEM_BYTEB` | OEM byte B | OEM symbol only; not used |
| `0x729-0x72A` | — | — | unknown | — |
| `0x72B` | R | — | Local dimming state (DSDT `CSMS`). Bit 1 reflects current dimming state. | `LocalDimmingControl.ReadState` |
| `0x72C-0x72F` | — | — | unknown | — |
| `0x730` | R | `ADDR_GAMING_PL1_DEFAULT_VALUE` | Gaming-mode PL1 default, watts | `HardwareDetection` at startup (mode defaults) |
| `0x731` | R | `ADDR_GAMING_PL2_DEFAULT_VALUE` | Gaming-mode PL2 default, watts | `HardwareDetection` at startup |
| `0x732` | R | `ADDR_GAMING_PL4_DEFAULT_VALUE` | Gaming-mode PL4 default (raw; scale depends on `0x727` bit 7) | `HardwareDetection` at startup |
| `0x733` | R | `ADDR_GAMING_D_DEFAULT_VALUE` | Gaming-mode "D" default | `HardwareDetection` at startup |
| `0x734` | R | `ADDR_OFFICE_PL1_DEFAULT_VALUE` | Office-mode PL1 default, watts | `HardwareDetection` at startup |
| `0x735` | R | `ADDR_OFFICE_PL2_DEFAULT_VALUE` | Office-mode PL2 default, watts | `HardwareDetection` at startup |
| `0x736` | R | `ADDR_OFFICE_PL4_DEFAULT_VALUE` | Office-mode PL4 default | `HardwareDetection` at startup |
| `0x737` | R | `ADDR_OFFICE_D_DEFAULT_VALUE` | Office-mode "D" default | `HardwareDetection` at startup |
| `0x738` | RW | `ADDR_MYFAN3_CPU_TAU` | MyFan3 CPU TAU (power integration time constant) | OEM symbol only; not used |
| `0x739-0x73F` | — | — | unknown | — |
| `0x740` | R | `projectId` (`ADDR_PROJECT_ID_BYTE`) | Model / project ID byte. `raw >> 1` = project enum value (e.g. `26 >> 1 = 13 = IDY_7Y`). | `ProfileLoader` at startup (model detection) |
| `0x741` | RW | (activation) (`ADDR_AP_OEM_BYTE`) | Activation byte. While `0x741 == 0`, the EC ignores PL/fan/GPU writes. `EcActivationControl` writes `0x05`; the EC asynchronously transforms it to `0x01` (post-init readback check). EC clears it when OEM service disconnects. | `EcActivationControl` init (step 2 + post-verify) |
| `0x742` | R | `ADDR_SUPPORT_BYTE5` | Support flags byte 5 | `HardwareDetection` at startup |
| `0x743` | RW | `tgpDbCtrl` (`ADDR_ConfigurableTGP_DynamicBoost_CTRL_BYTE`) | GPU TGP / Dynamic Boost control bitfield (mask `0x07`). Bit 0 + bit 2 = TGP enable; bit 1 = Dynamic Boost enable. `0x07` = both on; `0x05` = TGP only. OEM dual label: `ADDR_MYFAN2_L1_PWM` (not used on IDY_7Y). | `GpuPowerControl` write |
| `0x744` | RW | `tgp` (`ADDR_ConfigurableTGP_VALUE`) | Configurable TGP value. `watts = raw + 95`. | `GpuPowerControl` write |
| `0x745` | RW | `dbTotalPower` (`ADDR_DynamicBoost_TotalProcessingPowerTarget_VALUE`) | Dynamic Boost total-power target. `255` = on, `0` = off. | `GpuPowerControl` write |
| `0x746` | RW | `dbMaxTgp` (`ADDR_DynamicBoost_MaxinumTGP_VALUE`) | Dynamic Boost max TGP, watts (direct byte, typical range 5–25) | `GpuPowerControl` write |
| `0x747` | RW | `ADDR_MYFAN2_L5_PWM` | MyFan2 level 5 PWM | OEM symbol only; not used |
| `0x748` | W | `ADDR_LIGHTBAR_CONTROL_BYTE` | Feature-enable / lightbar control. Written `0x81` during init (OEM writes it four times; we write once). | `EcActivationControl` init (step 3) |
| `0x749` | W | `ADDR_REDBAR_CONTROL_BYTE` | Command-clear / red-bar control | OEM init writes `0x00`; not used by MechControl |
| `0x74A` | W | `ADDR_GREENBAR_CONTROL_BYTE` | Command-clear / green-bar control | OEM init writes `0x00`; not used by MechControl |
| `0x74B` | W | `ADDR_BLUEBAR_CONTROL_BYTE` | Command-clear / blue-bar control | OEM init writes `0x00`; not used by MechControl |
| `0x74C` | R | `ADDR_OEMSERVICE_PROJECT_ID_BYTE` | OEM service project ID | OEM polls; not used by MechControl |
| `0x74D` | — | — | unknown | — |
| `0x74E` | W | `ADDR_BIOS_OEM_BYTE` | BIOS OEM byte / command-clear 4 | OEM init writes `0x00`; not used by MechControl |
| `0x74F-0x750` | — | — | unknown | — |
| `0x751` | RW | `fanControlMode` (`ADDR_MAFAN_CONTROL_BYTE`) | Fan control mode. `0x00` = normal, `0x40` = full blast (bit 6). | `FanControl.SetFanBoost` |
| `0x752` | — | — | unknown | — |
| `0x753` | RW | `ADDR_CPU_VRM_CURRENT_LIMIT_BYTE` | CPU VRM current limit | `HardwareDetection` reads at startup |
| `0x754` | RW | `ADDR_CPU_VRM_MAXI_CURRENT_LIMIT_BYTE` | CPU VRM maximum current limit | `HardwareDetection` reads at startup |
| `0x755-0x75A` | — | — | unknown | — |
| `0x75B` | R | `cpuFanDuty` (`ADDR_EC_MAIN_FAN_L_DUTY_BYTE`) | CPU fan duty. `raw / 2 = percent` (0–200 raw). | `EcMonitor` poll |
| `0x75C` | R | `gpuFanDuty` (`ADDR_EC_MAIN_FAN_R_DUTY_BYTE`) | GPU fan duty. `raw / 2 = percent`. | `EcMonitor` poll |
| `0x75D` | RW | `ADDR_TRIGGER_BYTE2` | Secondary trigger byte | OEM symbol only; not used |
| `0x75E-0x764` | — | — | unknown | — |
| `0x765` | R | `ADDR_SUPPORT_BYTE1` | Feature support flags | `HardwareDetection` at startup |
| `0x766` | R | `ADDR_SUPPORT_BYTE2` | LED-capability flags (OEM app writes it back during init as part of its own init dance). | `HardwareDetection` at startup |
| `0x767` | F&F | `logoTrigger` (`ADDR_TRIGGER_BYTE`) | Logo LED trigger. `LightingControl.SetZoneOn` writes `0x00` before applying logo color. Fire-and-forget (skip verify). | `LightingControl` (logo path) |
| `0x768` | R | `statusByte` (`ADDR_STAUTS_BYTE`) | Status byte. Bit 2 = fan-boost active. | `EcMonitor` poll |
| `0x769` | RW | `logoR` (`ADDR_AP_EC_LOGO_R` / `ADDR_RGBKB_LEVEL_R`) | Logo LED red component, scaled 0–60. `EC = round(HID × 60 / 255)`; values above 60 are invalid per hardware testing. | `LightingControl` (logo path) |
| `0x76A` | RW | `logoG` (`ADDR_AP_EC_LOGO_G`) | Logo LED green (same scaling as `0x769`) | `LightingControl` |
| `0x76B` | RW | `logoB` (`ADDR_AP_EC_LOGO_B`) | Logo LED blue (same scaling as `0x769`) | `LightingControl` |
| `0x76C` | R | `ADDR_RGBKB_LEVEL_DEFAULT_R` | Default red for RGB keyboard | OEM symbol only; not used |
| `0x76D` | R | `ADDR_RGBKB_LEVEL_DEFAULT_G` | Default green for RGB keyboard | OEM symbol only; not used |
| `0x76E` | R | `ADDR_RGBKB_LEVEL_DEFAULT_B` | Default blue for RGB keyboard | OEM symbol only; not used |
| `0x76F` | RW | `ADDR_RGBKB_MUSIC_NO` | RGB-keyboard music-mode number (`0x00` = disable, `0xFE` = enable) | OEM symbol only; not used |
| `0x770` | — | — | unknown | — |
| `0x771` | R | `ADDR_ROMID` | ROM ID 1 (hardware identification) | `HardwareDetection` at startup |
| `0x772` | R | `ADDR_ROMID2` | ROM ID 2 (hardware identification) | `HardwareDetection` at startup |
| `0x773-0x781` | — | — | unknown | — |
| `0x782` | RW | `ADDR_BIOS_OEM_BYTE2` | BIOS OEM byte 2 | OEM symbol only; not used |
| `0x783` | RW | `PL1` (`ADDR_PL1_SETTING_VALUE`) | Live PL1, direct byte = watts (allowed range 10–210) | `CpuPowerControl` write |
| `0x784` | RW | `PL2` (`ADDR_PL2_SETTING_VALUE`) | Live PL2, direct byte = watts (10–210) | `CpuPowerControl` write |
| `0x785` | RW | `PL4` (`ADDR_PL4_SETTING_VALUE`) | Live PL4. Encoding depends on `0x727` bit 7: if set (`PL4D = 1`) then `watts = raw × 2` (range 10–420); if clear, `watts = raw` (10–210). | `CpuPowerControl` write |
| `0x786` | W | `cpuTempLimitWrite` | CPU temp-limit write. **Bit 7 is a required commit flag** — without it the EC stores the value but does not enforce the limit. Intel: `0x80 \| (105 - °C)`. AMD: `0x80 \| °C`. Readback is via `0x463` with bit 7 stripped. OEM dual label: `ADDR_L1_PWM_DEFAULT_MYFAN3` (not used on IDY_7Y). | `CpuPowerControl` write |
| `0x787` | RW | `fanSwitchSpeed` (`ADDR_TimAP_FanSwitchSpeedT100mSec`) | Fan switch speed: bit 7 = enable, bits 0–6 = time in units of 100 ms. OEM dual label: `ADDR_L2_PWM_DEFAULT_MYFAN3`. | `FanSwitchSpeedControl` write |
| `0x788` | R | — | NPCF `ACBT` shadow (DSDT `CTWA`). Direct mirror of `0x744` TGP offset byte (`watts = raw + TgpOffset`). DSDT populates `NPCF.ACBT = CTWA × 8` — the ×8 is NPCF's unit (1/8 W), not a watt conversion. OEM dual label: `ADDR_L3_PWM_DEFAULT_MYFAN3`. | `EcMonitor` poll |
| `0x789` | RW | `ADDR_L1_PWM_DEFAULT_MYFAN2` / `ADDR_L4_PWM_DEFAULT_MYFAN3` | MyFan2 / MyFan3 PWM default (dual-mapped) | OEM symbol only; not used |
| `0x78A` | RW | `ADDR_L2_PWM_DEFAULT_MYFAN2` / `ADDR_L5_PWM_DEFAULT_MYFAN3` | MyFan2 / MyFan3 PWM default (dual-mapped) | OEM symbol only; not used |
| `0x78B` | RW | `ADDR_L3_PWM_DEFAULT_MYFAN2` / `ADDR_MYFAN3_GPU_SETTING` | MyFan2 / MyFan3 setting (dual-mapped) | OEM symbol only; not used |
| `0x78C` | F&F | `logoCommit` (`ADDR_AP_EC_LOGO_CONFIRM`) | Logo commit trigger: write `0x10` to apply the staged logo RGB/brightness. OEM dual labels: `ADDR_L4_PWM_DEFAULT_MYFAN2`, `ADDR_AP_OEM_BYTE2`, `ADDR_SINGLEKBL_ENABLE` (not used on IDY_7Y). | `LightingControl` (logo path) |
| `0x78D` | RW | `ADDR_L5_PWM_DEFAULT_MYFAN2` | MyFan2 level 5 PWM default | OEM symbol only; not used |
| `0x78E` | R | `ADDR_SUPPORT_BYTE6` / `ADDR_SINGLEKBL_SUPPORTPOWER` | Support byte 6 | `HardwareDetection` at startup |
| `0x78F-0x79D` | — | — | unknown | — |
| `0x79E` | RW | `ADDR_MYFANI_MIN_SPEED` | MyFanI minimum speed | OEM symbol only; not used |
| `0x79F` | RW | `ADDR_MYFANI_MIN_TEMP` | MyFanI minimum temperature threshold | OEM symbol only; not used |
| `0x7A0` | RW | `ADDR_MYFANI_EXTRA_SPEED` | MyFanI extra speed | OEM symbol only; not used |
| `0x7A1-0x7A2` | — | — | unknown | — |
| `0x7A3` | RW | `ADDR_BIOS_OEM_BYTE3` | BIOS OEM byte 3 | OEM symbol only; not used |
| `0x7A4` | RW | `ADDR_AP_BIOS_BYTE` | AP BIOS byte | OEM symbol only; not used |
| `0x7A5` | RW | `ADDR_AP_OEM_BYTE3` | AP OEM byte 3 | OEM symbol only; not used |
| `0x7A6` | RW | `batteryMode` (`ADDR_AP_OEM_BYTE4`) | Battery-protection mode. Bits 3–5 (mask `0x38`): `0x08` = high-capacity (performance), `0x18` = balanced, `0x28` = stationary (healthy). | `BatteryControl` write (read-modify-write), `EcMonitor` poll |
| `0x7A7` | R | `ADDR_BATTERYSAVER_PL1_DEFAULT_VALUE` | Battery-saver mode PL1 default, watts | `HardwareDetection` at startup |
| `0x7A8` | R | `ADDR_BATTERYSAVER_PL2_DEFAULT_VALUE` | Battery-saver mode PL2 default, watts | `HardwareDetection` at startup |
| `0x7A9` | R | `ADDR_BATTERYSAVER_PL4_DEFAULT_VALUE` | Battery-saver mode PL4 default | `HardwareDetection` at startup |
| `0x7AA` | R | `ADDR_BATTERYSAVER_D_DEFAULT_VALUE` | Battery-saver mode "D" default | `HardwareDetection` at startup |
| `0x7AB` | RW | `logoOnOff` (`ADDR_AP_EC_LOGO`) | Logo brightness / on-off. Five levels, one-hot bits 5–7 plus bit 0 (LED-exists flag): `0x01` = off, `0x21` = 25%, `0x41` = 50%, `0x61` = 75%, `0x81` = 100%. OEM dual label: `ADDR_MyFanCCI_Mode_Index`. | `LightingControl` (logo path) |
| `0x7AC-0x7AF` | — | — | unknown | — |
| `0x7B0` | RW | `ADDR_MyFanCCI_Mode_Profile1` | MyFanCCI profile 1 | OEM symbol only; not used |
| `0x7B1` | RW | `ADDR_MyFanCCI_Mode_Profile2` | MyFanCCI profile 2 | OEM symbol only; not used |
| `0x7B2` | RW | `ADDR_MyFanCCI_Mode_Profile3` | MyFanCCI profile 3 | OEM symbol only; not used |
| `0x7B3-0x7B8` | — | — | unknown | — |
| `0x7B9` | RW | `ADDR_BATTERY_CHARGE_LIMIT_UP` | Battery charge upper limit. On IDY_7Y this register reads `0` — the OEM feature is not operational on that firmware. | OEM symbol only; not used by MechControl |
| `0x7BA-0x7C4` | — | — | unknown | — |
| `0x7C5` | RW | `fanCurveEnable` (`ADDR_AP_OEM_BYTE5`) | Multi-bitfield. **Bit 7** = custom fan curve enable (written by `FanControl.SetCustomFanCurveMode`). **Bits 5 & 6** are set together by `EcActivationControl` init via `\|= 0x60`; the OEM also sets these during its own init. Read-modify-write. | `FanControl` bit 7 write, `EcActivationControl` init for bits 5 & 6, `EcMonitor` poll for state |
| `0x7C6` | RW | `ADDR_AP_OEM_BYTE6` | Fan-curve commit / mode flag. `0x04` = custom fan table active, `0x00` = disabled. Persistent mode flag (not a trigger): must stay set while custom curves are in use. Without it the EC ignores the `0xF00-0xF5F` fan table. Skip-verify. | `FanControl.SetCustomFanCurveMode` write, `FanSnapshot` read |
| `0x7C7` | RW | `ADDR_AP_OEM_BYTE7` | OEM byte 7 | `EcActivationControl` init writes `0x0C` (step 6) |
| `0x7C8` | RW | `ADDR_AP_OEM_BYTE8` | OEM byte 8 | OEM symbol only; not used |
| `0x7C9` | — | — | unknown | — |
| `0x7CA` | R | `ADDR_BIOS_OEM_BYTE8` | BIOS OEM byte 8 | OEM polls; not used by MechControl |
| `0x7CB` | — | — | unknown | — |
| `0x7CC` | R | `ADDR_COMPLEX_POWER_STATUS` | Complex power status / watchdog keepalive | OEM polls ~1.5 s; not used by MechControl |
| `0x7CD-0x7CF` | — | — | unknown | — |
| `0x7D0` | RW | `ADDR_BATTERY_CHARGE_LIMIT_DOWN` | Battery charge lower limit. Reads `0` on IDY_7Y — OEM feature not operational. | OEM symbol only; not used by MechControl |
| `0x7D1` | R | `AP_SSDTempr` | SSD temperature | OEM symbol only; not used |
| `0x7D2` | R | `ADDR_ModuleID_GPU` | GPU module identification | `HardwareDetection` at startup |
| `0x7D3` | R | `ADDR_ModuleID` | CPU module identification | `HardwareDetection` at startup |
| `0x7D4` | — | — | unknown | — |
| `0x7D5` | R | — | NPCF `AMAT` shadow (DSDT `DBAP`). Direct mirror of `0x746` DB max byte (`watts = raw`, `0` = DB off). DSDT populates `NPCF.AMAT = DBAP × 8` — the ×8 is NPCF's unit (1/8 W), not a watt conversion. | `EcMonitor` poll |
| `0x7D6-0x7D7` | — | — | unknown | — |
| `0x7D8` | R | `ADDR_GAMING_TCC_OFFSET_DEFAULT_VALUE` | Gaming-mode TCC offset default | `HardwareDetection` at startup |
| `0x7D9` | R | `ADDR_OFFICE_TCC_OFFSET_DEFAULT_VALUE` | Office-mode TCC offset default | `HardwareDetection` at startup |
| `0x7DA` | R | `ADDR_TURBO_TCC_OFFSET_DEFAULT_VALUE` | Turbo-mode TCC offset default | `HardwareDetection` at startup |
| `0x7DB-0x7E5` | — | — | unknown | — |
| `0x7E6` | RW | `ADDR_LC_FAN_VALUE` | Liquid-cooling fan value | OEM symbol only; not used |
| `0x7E7` | RW | `ADDR_LC_PUMP_VALUE` | Liquid-cooling pump value | OEM symbol only; not used |
| `0x7E8-0x7FA` | — | — | unknown | — |
| `0x7FB` | RW | `ADDR_CHANGE_PORT_ID_WKD` | Change port ID (WKD) | OEM symbol only; not used |

### 3.4 Extended Control Page (0xD00–0xD4F)

| Addr | R/W | Name | Description | MechControl usage |
|------|-----|------|-------------|-------------------|
| `0xD00` | RW | `ADDR_AP_OEM_SingleZone` | Single-zone keyboard backlight control | OEM symbol only; not used |
| `0xD01-0xD4D` | — | — | unknown | — |
| `0xD4E` | R | `EC_Color_Calibration_Support` | Color-calibration support flag | OEM symbol only; not used |
| `0xD4F` | RW | `ADDR_AP_MISCSWITCH2` | Miscellaneous switch register 2 | OEM symbol only; not used |

### 3.5 Fan Curve Tables (0xF00–0xFEF)

The fan tables occupy a dedicated EC page. Three formats are known in the
OEM symbol table — the active format for each model is selected via
`profile.fanTable.format`. MechControl currently implements `RamFan1`.

**RamFan1 — 2-fan layout (CPU + GPU), 6 base addresses × 16 bytes.**
Used by IDY_7Y and most 2-fan laptops.

| Base | Name | Purpose |
|------|------|---------|
| `0xF00+N` | `ADDR_CPU_FAN_TABLE_TEMP_UP0`   | CPU temp-up threshold, entry `N` |
| `0xF10+N` | `ADDR_CPU_FAN_TABLE_TEMP_DOWN0` | CPU temp-down threshold, entry `N` |
| `0xF20+N` | `ADDR_CPU_FAN_TABLE_DUTY0`      | CPU fan duty, entry `N` (`raw / 2 = %`) |
| `0xF30+N` | `ADDR_GPU_FAN_TABLE_TEMP_UP0`   | GPU temp-up threshold, entry `N` |
| `0xF40+N` | `ADDR_GPU_FAN_TABLE_TEMP_DOWN0` | GPU temp-down threshold, entry `N` |
| `0xF50+N` | `ADDR_GPU_FAN_TABLE_DUTY0`      | GPU fan duty, entry `N` |

Each base spans 16 bytes (`N = 0..15`). The first `activeEntries` (usually
10) are active points; the remaining slots are padding. See §6 for the
write rules.

**RamFan1p5 extension** — some models add three status/control bytes at the
end of the same range (otherwise identical to RamFan1):

| Addr | Name | Purpose |
|------|------|---------|
| `0xF5D` | `ADDR_RAMFAN1P5_TABLE_STATUS1` | Fan table status register 1 |
| `0xF5E` | `ADDR_RAMFAN1P5_TABLE_STATUS2` | Fan table status register 2 |
| `0xF5F` | `ADDR_RAMFAN1P5_TABLE_CTRL`    | Fan table control (commit/apply) |

**RamFan2 — 3-fan layout** (Fan1/Fan2/Fan3 each with CPU up/down + GPU up/down + duty). Used on some chassis with a third fan. **Not currently supported by MechControl.**

| Base | Name |
|------|------|
| `0xF00+N` | `ADDR_TABLE_F1_CPU_TEMP_UP0` |
| `0xF10+N` | `ADDR_TABLE_F1_CPU_TEMP_DOWN0` |
| `0xF20+N` | `ADDR_TABLE_F1_DUTY0` |
| `0xF30+N` | `ADDR_TABLE_F1_GPU_TEMP_UP0` |
| `0xF40+N` | `ADDR_TABLE_F1_GPU_TEMP_DOWN0` |
| `0xF50+N` | `ADDR_TABLE_F2_CPU_TEMP_UP0` |
| `0xF60+N` | `ADDR_TABLE_F2_CPU_TEMP_DOWN0` |
| `0xF70+N` | `ADDR_TABLE_F2_DUTY0` |
| `0xF80+N` | `ADDR_TABLE_F2_GPU_TEMP_UP0` |
| `0xF90+N` | `ADDR_TABLE_F2_GPU_TEMP_DOWN0` |
| `0xFA0+N` | `ADDR_TABLE_F3_CPU_TEMP_UP0` |
| `0xFB0+N` | `ADDR_TABLE_F3_CPU_TEMP_DOWN0` |
| `0xFC0+N` | `ADDR_TABLE_F3_DUTY0` |
| `0xFD0+N` | `ADDR_TABLE_F3_GPU_TEMP_UP0` |
| `0xFE0+N` | `ADDR_TABLE_F3_GPU_TEMP_DOWN0` |

---

## 4. CPU Power Write Sequence

`CpuPowerControl` writes PL1/PL2/PL4/TempLimit as a group of four:

```
Write(PL1,            pl1_watts)           // 0x783
Write(PL2,            pl2_watts)           // 0x784
Write(PL4,            pl4_watts / scale)   // 0x785 — scale depends on 0x727 bit 7
Write(cpuTempLimit,   0x80 | offset)       // 0x786 — Intel: offset = 105 - °C, AMD: offset = °C
```

Bit 7 of the `0x786` write is a required commit flag. Without it the EC
stores the value but does not enforce the limit. Verification reads `0x463`
and compares `105 - readback` against the target (bit 7 is stripped).

---

## 5. GPU Power Write Sequence

`GpuPowerControl` writes the TGP/DB group of four:

**Dynamic Boost enabled:**
```
WriteBitfield(tgpDbCtrl, 0x07, 0x07)   // 0x743 — TGP bits 0,2 + DB bit 1
Write(tgp,               tgp_watts - 95) // 0x744
Write(dbTotalPower,      255)            // 0x745
Write(dbMaxTgp,          db_watts)       // 0x746
```

**Dynamic Boost disabled:**
```
WriteBitfield(tgpDbCtrl, 0x07, 0x05)   // TGP only
Write(tgp,               tgp_watts - 95)
Write(dbTotalPower,      0)
Write(dbMaxTgp,          0)
```

---

## 6. Fan Control Write Sequences

**Fan boost:** `Write(fanControlMode, boost ? 0x40 : 0x00)` — toggles bit 6 of `0x751`.

**Custom curve enable:** `FanControl.SetCustomFanCurveMode` writes BOTH:
- `WriteBitfield(fanCurveEnable, 0x80, enable ? 0x80 : 0x00)` — `0x7C5` bit 7
- `Write(0x7C6, enable ? 0x04 : 0x00)` — mode flag

Both must be set for the EC to honour the custom fan table.

**Fan switch speed:** `Write(fanSwitchSpeed, enabled ? (0x80 | ms/100) : 0x00)` — `0x787`.

**Fan curve table write** (interleaved triplets, matches OEM order):

```
active = curve.ActiveEntries   // typically 10 of 16
for i in 0..entriesPerArray-1:
  if i < active:
    Write(tempUpBase   + i, tempUp[i])
    Write(tempDownBase + i, i == 0 ? 0x00 : tempUp[i] - 5)  // fixed 5°C range; point 0 pinned to 0
    Write(dutyBase     + i, duty[i])                         // raw = percent × 2
  else:
    Write(tempUpBase   + i, 0xFF)
    Write(tempDownBase + i, i == active ? tempDown[active-1] + 4 : 0xFF)
    Write(dutyBase     + i, lastActiveRawDuty)               // padding duty = last active
```

**Write rules:**
- `tempDown[0]` is always `0x00` (matches OEM; the level has no step-down behavior but the EC expects the byte).
- `tempDown[i] = tempUp[i] - 5` for `i ∈ 1..active-1`. Fixed 5 °C range; not profile-configurable.
- Adjacent points must be ≥ 5 °C apart on `tempUp`. UI enforces; `FanCurveModel.Validate()` rejects violations.
- Padding `tempUp = 0xFF`. Padding duty = last active entry's raw duty (avoids a discontinuity if the EC ever evaluates a padding row).
- Padding `tempDown` for the first padding slot (`i == active`) = `tempDown[active-1] + 4`; remaining = `0xFF`. EC never consults these (tempUp 0xFF guarantees the row can't fire); mirrored purely for verify-diff cleanliness.
- Duty raw `0` triggers EC auto-override (~25%). Minimum safe raw is `2` (= 1%). `FanControl.WriteCurve` clamps zero up to 2.
- Writes take effect immediately; no trigger byte needed.
- CPU and GPU sections are independent — changing one GPU point only rewrites `0xF30-0xF5F`.

---

## 7. Battery Mode Write Sequence

`BatteryControl` sets `0x7A6` as a read-modify-write with mask `0x38`:

| Mode | Value | Binary |
|------|-------|--------|
| High Capacity (Performance) | `0x08` | 00**00**1000 |
| Balanced                    | `0x18` | 00**01**1000 |
| Stationary (Healthy)        | `0x28` | 00**10**1000 |

```
WriteBitfield(0x7A6, 0x38, mode_value)
```

---

## 8. Logo LED Write Sequence

The logo LED requires BOTH USB HID commands to the ITE controller AND EC
register writes. The HID commands physically drive the LED; the EC writes
persist state so it survives power cycles. `LightingControl.SetZoneOn(3)`
executes the combined sequence.

### Turn ON
```
HID: [00 1A 03 01 04 00 00 00 01]          // Power ON zone 3
HID: [00 14 03 01 R  G  B  00 00]          // Set color
HID: [00 08 23 01 01 BRIGHT 01 00 00]      // Apply with brightness
EC:  Write(logoTrigger,    0x00)            // 0x767 — clear trigger
EC:  WriteBitfield(fanCurveEnable, 0x20, 0x20) // 0x7C5 bit 5 = logo flag
EC:  Write(logoR,          scaleRgb(R))     // 0x769 — round(R × 60 / 255)
EC:  Write(logoG,          scaleRgb(G))     // 0x76A
EC:  Write(logoB,          scaleRgb(B))     // 0x76B
EC:  Write(logoOnOff,      brightness_enc)  // 0x7AB
EC:  Write(logoCommit,     0x10)            // 0x78C — commit
```

The EC writes to `0x767` and `0x7C5` bit 5 were not observed in the OEM
per-change Frida capture (the remaining EC writes match the OEM capture
exactly). MechControl writes them defensively.

### Turn OFF
```
HID: [00 12 00 03 00 00 00 00 00]          // Reset
HID: [00 08 05 00 00 00 00 00 00]          // Stop effects
HID: [00 08 01 03 00 00 00 00 00]          // Clear zone 3
HID: [00 1A 03 00 00 00 00 00 01]          // Power OFF
EC:  Write(logoOnOff,  0x01)                // Off (bit 0 = LED-exists flag)
EC:  Write(logoCommit, 0x10)                // Commit
```

### Logo brightness encoding (0x7AB)

| Level | Value | HID % |
|-------|-------|-------|
| 0 (off)    | `0x01` | 0%   |
| 1 (low)    | `0x21` | 25%  |
| 2 (med)    | `0x41` | 50%  |
| 3 (high)   | `0x61` | 75%  |
| 4 (max)    | `0x81` | 100% |

Bit 0 (`0x01`) is the LED-exists flag and is set in every value. Bits 5–7
are one-hot and encode the level.

### RGB scaling (0x769/0x76A/0x76B)

`LightingControl.ScaleRgbToEc`: `EC = round(HID × 60 / 255)`. Values above
60 are invalid per hardware testing (clamp in `LightingControl.cs:47`).

---

## 9. ITE USB HID Protocol

All lighting (keyboard, light bar, logo) uses ITE USB HID feature reports.
Every report is 9 bytes: `[ReportID=0x00, Command, ...params...]`.

### Device identification

- VID: `0x048D` (ITE Tech)
- Filter: `FeatureReportByteLength == 9`
- `HIDP_STATUS_SUCCESS = 0x00110000` (not 0)

**Device topology (IDY_7Y):**

| PID      | Interface | Usage    | UsagePage | Zone(s) |
|----------|-----------|----------|-----------|---------|
| `0x600B` | `mi_01`   | `0x0001` | `0xFF03`  | 0 (Keyboard) |
| `0x7001` | `mi_01`   | `0x0002` | `0xFF03`  | 2 (Light Bar) + 3 (Logo, separate handle) |

### Zones

| Zone | # | CMD ID | Device           | Transport |
|------|---|--------|------------------|-----------|
| Keyboard  | 0 | `0x02` | Keyboard backlight | HID only |
| (unused)  | 1 | `0x21` | Unknown — seen during mode switch only | — |
| Light Bar | 2 | `0x22` | Front/hinge strip | HID only |
| Logo      | 3 | `0x23` | Rear cover LED   | HID + EC |

Zone CMD ID formula: keyboard = `0x02` (special case); others = `0x20 + ZoneNumber`.

### Commands

**`0x1A` — Power control**
```
[00 1A ZONE_NUM POWER MODE 00 00 00 SAVE]
```
| Byte | Meaning | Values |
|------|---------|--------|
| 2 | Zone number | `0x00` KB, `0x02` LB, `0x03` Logo |
| 3 | Power       | `0x00` off, `0x01` on |
| 4 | Mode        | `0x00` when off, `0x04` when on |
| 8 | Save        | `0x01` (always persists to BIOS) |

**`0x14` — Set color**
```
[00 14 ZONE_NUM TARGET R G B 00 SAVE]
```
| Byte | Meaning | Values |
|------|---------|--------|
| 2 | Zone number | `0x00` KB, `0x02` LB, `0x03` Logo |
| 3 | Target      | `0x01` volatile, `0x09` startup/BIOS default |
| 4–6 | R, G, B   | `0x00-0xFF` |
| 8 | Save        | `0x00` volatile, `0x01` persist |

**`0x08` — Apply effect**
```
[00 08 ZONE_CMD_ID POWER EFFECT BRIGHTNESS SPEED PARAM SAVE]
```
| Byte | Meaning | Values |
|------|---------|--------|
| 2 | Zone CMD ID   | `0x02` KB, `0x22` LB, `0x23` Logo |
| 3 | Power state   | `0x00` off, `0x01` on |
| 4 | Effect mode   | see per-zone table below |
| 5 | Brightness    | see brightness scales below |
| 6 | Speed/param   | zone-specific |
| 8 | Save flag     | `0x00` volatile, `0x01` persist |

**Per-zone effect parameters:**

| Zone | CMD ID | Effect       | Speed | Brightness |
|------|--------|--------------|-------|------------|
| Keyboard  | `0x02` | `0x07` | `0x08` | keyboard scale |
| Light Bar | `0x22` | `0x01` (static) | `0x01` | 0x00-0x64 (percent) |
| Logo      | `0x23` | `0x01` (static) | `0x01` | 0x00-0x64 (percent) |

**Sub-commands (encoded in byte 2 of cmd `0x08`):**
- `0x05` — stop all effects: `[00 08 05 00 00 00 00 00 00]`
- `0x01` — zone mode off: `[00 08 01 ZONE_NUM 00 00 00 00 00]`

**`0x12` — Reset/prepare** — always `[00 12 00 03 00 00 00 00 00]` before OFF sequences.

**`0x09` — Query/sync** — `[00 09 ZONE_CMD_ID 00 00 00 00 00 00]`. Sent before keyboard OFF; other zones don't use it.

### Brightness scales

**Logo / Light Bar** (standard percent):

| Percent | HID value |
|---------|-----------|
| 0%  | `0x00` |
| 25% | `0x19` |
| 50% | `0x32` |
| 75% | `0x4B` |
| 100% | `0x64` |

**Keyboard** (custom scale, `8 + (level-1) × 14`):

| Level | HID value |
|-------|-----------|
| 1 (min) | `0x08` |
| 2       | `0x16` |
| 3       | `0x24` |
| 4 (max) | `0x32` |

### Standard sequences

**Turn ON any zone:**
```
1. [00 1A ZONE_NUM 01 04 00 00 00 01]       // Power ON + save
2. [00 14 ZONE_NUM 01 RR GG BB 00 00]       // Set color
3. [00 08 ZONE_CMD_ID 01 EFF BR SPD 00 00]  // Apply effect
4. (Logo only: EC writes — see §8)
```

**Turn OFF any zone:**
```
1. (Keyboard only: [00 09 02 00 00 00 00 00 00])
2. [00 12 00 03 00 00 00 00 00]
3. [00 08 05 00 00 00 00 00 00]
4. [00 08 01 ZONE_NUM 00 00 00 00 00]
5. [00 1A ZONE_NUM 00 00 00 00 00 01]
6. (Logo only: EC writes — see §8)
```

**Change color (zone already on):**
```
1. [00 14 ZONE_NUM 01 RR GG BB 00 00]
2. [00 08 ZONE_CMD_ID 01 EFF BR SPD 00 00]
```

**Save-to-BIOS flag locations:**

| Command | Normal | Save to BIOS |
|---------|--------|--------------|
| `0x14` Set Color | byte 3 = `0x01` | byte 3 = `0x09` (KB only) |
| `0x08` Apply     | byte 8 = `0x00` | byte 8 = `0x01` (all zones) |

---

## 10. EC Init Sequence

`EcActivationControl.InitializeEc` replicates the minimum subset of the OEM
`GCUService` startup handshake needed for PL/fan/GPU writes to take effect
on a machine with no OEM service running.

Three mechanisms together enable standalone operation:

1. **UEFI NVRAM flags** (one-time, auto-set by `UefiSetup`): four fields in
   `OemMagicVariable` (GUID `{9f33f85c-13ca-4fd1-9c4a-96217722c593}`, 180
   bytes) — `[0x2F] PowerMode=3`, `[0x33] MemoryOverClockSwitch=1`,
   `[0x5D] ApExistFlag=1`, `[0x61] ApUseFlag=1`. Pre-write sanity checks
   (size 180, PowerMode 0–3, booleans 0/1) and a verified readback. Not
   strictly required but ensures the EC boots into a sane state.
2. **EC power-management activation** (`0x726 \|= 0x80`, `0x727 \|= 0x40`).
   Bit 7 of `0x726` and bit 6 of `0x727` (DSDT `CUME`) together activate
   the EC's PECI power-management loop. Without these bits, PL register
   values sit in EC RAM but are never applied to the CPU — it stays at
   its default power. Persist in EC microcontroller NVRAM across reboots
   but clearing takes effect immediately.
3. **Activation byte** (`0x741 = 0x05`). While `0x741 == 0`, the EC ignores
   writes to PL / fan / GPU registers (simple direct registers like fan
   boost `0x751` still work). The EC asynchronously transforms `0x05` to
   `0x01`; the post-init check reads `0x741` and warns if it's not `0x01`.

### MechControl init steps

```
1. Write(0x709, 0x05), Write(0x708, 0x01)      // EC handshake
2. Write(0x741, 0x05)                          // Activation
3. Write(0x748, 0x81)                          // Feature enable
4. Write(0x726, Read(0x726) | 0x80)            // Power management (byte 9, bit 7)
   Write(0x727, Read(0x727) | 0x40)            // Power management (CUME bit)
5. Write(0x7C5, Read(0x7C5) | 0x60)            // OEM init bits 5 & 6 (preserves bit 7)
6. Write(0x7C7, 0x0C)                          // Fan curve parameter
7. Write(0x706, 0x41)                          // BIOS_CTRL init complete

Post-verify: Read(0x741) should be 0x01.
```

Fan table writes (`0xF00-0xF5F`) and the `0x7C6 = 0x04` commit happen
separately via `FanControl` whenever a custom curve is active.

Each step is wrapped in a try/catch so a failure in one step does not block
the rest. 150 ms inter-step sleep (OEM uses ~100 ms; margin covers the ~250 ms
async transform window on `0x741`).

### Full OEM init (for comparison)

```
Phase 1 — Handshake (+7s):
  Write(0x709, 0x05)
  Write(0x708, 0x01)

Phase 2 — Activation (+14s):
  Write(0x741, 0x05)
  Write(0x748, 0x81)              // written 4×
  Write(0x766, Read(0x766))       // write-back Support2
  Write(0x749, 0x00)              // clear command registers
  Write(0x74A, 0x00)
  Write(0x74B, 0x00)
  Write(0x767, 0x00)              // clear trigger
  Write(0x74E, 0x00)

Phase 3 — Apply settings (+17s):
  Write(0x726, Read(0x726) | 0x80)
  Write(0x727, Read(0x727) | 0x40)
  Write(0x7C5, Read(0x7C5) | 0xE0)   // bits 7+6+5 together
  Write PL1/PL2/PL4/TCC
  Write fan table (0xF00-0xF5F)
  Write TGP/DB/DB_MAX
  Write(0x787, 0x81)                  // fan switch speed
  Write(0x743, 0x07)                  // DB_CTRL (TGP+DB on)
  Write(0x7C6, 0x04)                  // APPLY
  Write(0x7C6, 0x00)

Phase 4 — Double-write (+19s):
  [Repeat full fan table + PL + GPU write]
  Write(0x7C6, 0x04)
  Write(0x7C6, 0x00)

Phase 5 — Logo + finalize:
  EC + HID logo writes
  Write(0x706, 0x41)

Phase 6 — Steady state:
  Read(0x7CC) every ~1.5 s  (watchdog keepalive)
```

The OEM app also polls these registers continuously at ~250 ms:
`0x7CC`, `0x727` (e-shutter), `0x7CA`, `0x74C`, `0x766`, `0x7C5`, `0x7A6`.
MechControl does not replicate this polling.

Additional IOCTL codes seen during OEM init:

| IOCTL | Input | Output | Purpose |
|-------|-------|--------|---------|
| `0x9C40A4A0` | `[00 00 00 00]` | `[86 80 1C 7D]` | PCI device ID read (`0x8086:0x7D1C` = Intel PCH) |
| `0x9C40A500` | `[BB 00..0B]` | 4 bytes each | ECSpec batch config read (12 indices) |

---

## 11. WriteGuard Rules

All EC writes from feature code go through `WriteGuard`, which enforces
the per-profile allowlist, applies bit-mask clamping for read-modify-write
registers, and verifies the readback after every write.

### IDY_7Y allowlist (115 addresses)

| Category | Addresses |
|----------|-----------|
| CPU power    | `0x783`, `0x784`, `0x785`, `0x786` |
| GPU power    | `0x743`, `0x744`, `0x745`, `0x746` |
| Fan control  | `0x751`, `0x7C5`, `0x787` |
| Fan table CPU| `0xF00-0xF2F` (48 bytes) |
| Fan table GPU| `0xF30-0xF5F` (48 bytes) |
| Battery      | `0x7A6` |
| Logo/Lighting| `0x767`, `0x769`, `0x76A`, `0x76B`, `0x7AB`, `0x78C` |
| Service init | `0x706`, `0x708`, `0x709`, `0x741`, `0x748`, `0x7C6`, `0x7C7` |

### Read-modify-write registers

| Register | Mask | Values |
|----------|------|--------|
| `0x7C5` | `0x80` | Fan curve: `0x80` = on, `0x00` = off |
| `0x7C5` | `0x20` | Logo flag: `0x20` = on |
| `0x7A6` | `0x38` | Battery: `0x08` perf / `0x18` balanced / `0x28` stationary |
| `0x743` | `0x07` | Dynamic Boost: `0x07` = on, `0x05` = off |

### Verification rules

| Register | Rule |
|----------|------|
| `0x786` (CPU temp write) | Redirect: read `0x463`, check `105 - readback == target_temp` |
| `0x767` (logo trigger)   | Skip verify — fire-and-forget |
| `0x78C` (logo commit)    | Skip verify — write-only trigger |
| `0x741` (activation)     | Skip verify — EC transforms value (write `0x05`, reads back `0x01`) |
| `0x7C6` (fan curve mode) | Skip verify — mode flag |
| `0x708`, `0x709`, `0x748`, `0x706`, `0x7C7` | Skip verify — init registers, not all readable on every model |
| Everything else          | Read same address, compare directly |

### OEM write patterns (observed via Frida)

1. **Read-before-write** on every bitmask register (`0x7C5`, `0x7A6`, `0x743`, `0x7AB`).
2. **Group writes** — PL1/PL2/PL4 + temp always as a group of 4. GPU TGP/DB as a group of 4. MechControl preserves both groupings.
3. **Double-write** — PL changes produce two identical batches. The fan table is also written twice with a `0x7C6` apply flag between them.
4. **LED capability check** — OEM reads `0x766` before every HID operation (always reads `0x93` on IDY_7Y).
5. **Mode switch** writes 100+ registers (PL + TGP + full fan table + lighting + apply flags). MechControl does not replicate mode switching; it writes individual features.

---

## Appendix A. OSD Event Codes

These are **not** EC register addresses — they are event-notification
constants used in software (e.g. scan codes for hotkeys, status codes for
OSD popups). They live alongside the EC registers in the OEM symbol table
and are preserved here for RE cross-reference.

| Code | Name | Description |
|------|------|-------------|
| `0x00` | Disable_RGB_Music | Command: disable RGB music-sync mode |
| `0x001` | OSD_CAPSLOCK | Caps Lock toggled |
| `0x002` | OSD_NUMLOCK | Num Lock toggled |
| `0x003` | OSD_SROLLOCK | Scroll Lock toggled |
| `0x004` | OSD_TPON | Touchpad enabled |
| `0x005` | OSD_TPOFF | Touchpad disabled |
| `0x006` | OSD_SILENTON | Silent mode enabled |
| `0x007` | OSD_SILENTOFF | Silent mode disabled |
| `0x008` | OSD_WLANON | WLAN enabled |
| `0x009` | OSD_WLANOFF | WLAN disabled |
| `0x00A` | OSD_WINMAXON | WinMax enabled |
| `0x00B` | OSD_WINMAXOFF | WinMax disabled |
| `0x00C` | OSD_BTON | Bluetooth enabled |
| `0x00D` | OSD_BTOFF | Bluetooth disabled |
| `0x00E` | OSD_RFON | RF on |
| `0x00F` | OSD_RFOFF | RF off |
| `0x010` | OSD_3GON | 3G/mobile data enabled |
| `0x011` | OSD_3GOFF | 3G/mobile data disabled |
| `0x012` | OSD_WEBCAMON | Webcam enabled |
| `0x013` | OSD_WEBCAMOFF | Webcam disabled |
| `0x014` | OSD_BRIGHTNESSUP | Screen brightness up |
| `0x14`  | OS_VK_CAPITAL | Virtual key code: Caps Lock |
| `0x015` | OSD_BRIGHTNESSDOWN | Screen brightness down |
| `0x01A` | OSD_RADIOON | Radio on |
| `0x01B` | OSD_RADIOOFF | Radio off |
| `0x031` | OSD_POWERSAVEON | Power save enabled |
| `0x032` | OSD_POWERSAVEOFF | Power save disabled |
| `0x034` | OSD_MENU | Menu key pressed |
| `0x035` | OSD_MUTE | Mute toggled |
| `0x036` | OSD_VOLUMEDOWN | Volume down |
| `0x037` | OSD_VOLUMEUP | Volume up |
| `0x038` | OSD_OSD_MENU_2 | Secondary menu |
| `0x039` | OSD_BREATH_LED_ON | Breathing LED effect enabled |
| `0x03A` | OSD_BREATH_LED_OFF | Breathing LED effect disabled |
| `0x03B` | OSD_KB_LED_LEVEL0 | Keyboard LED level 0 (off) |
| `0x03C` | OSD_KB_LED_LEVEL1 | Keyboard LED level 1 |
| `0x03D` | OSD_KB_LED_LEVEL2 | Keyboard LED level 2 |
| `0x03E` | OSD_KB_LED_LEVEL3 | Keyboard LED level 3 |
| `0x03F` | OSD_KB_LED_LEVEL4 | Keyboard LED level 4 (max) |
| `0x040` | OSD_WINKEY_LOCK | Windows key locked |
| `0x041` | OSD_WINKEY_UNLOCK | Windows key unlocked |
| `0x042` | OSD_MENU_JP | Menu key (Japanese layout) |
| `0x90`  | OS_VK_NUMLOCK | Virtual key code: Num Lock |
| `0x091` | OSD_CAMERAOFF | Camera disabled |
| `0x090` | OSD_CAMERAON | Camera enabled |
| `0x91`  | OS_VK_SCROLL | Virtual key code: Scroll Lock |
| `0x0A4` | OSD_AIRPLANEMODE | Airplane mode toggled |
| `0x0A5` | WinKey_Update | Windows key state update |
| `0x0A7` | OSD_FANBOOST_UPDATE | Fan boost state changed |
| `0x0A9` | OSD_LCD_SW | LCD switch (external display toggle) |
| `0x0AA` | TimAP_MyFanOT | MyFan over-temperature warning |
| `0x0AB` | OSD_MyBat_ACUpdate | Battery AC adapter status update |
| `0x0AC` | TimAP_MyBat_HPOff | Battery high-performance off |
| `0x0AD` | TimAP_ReleaseUSM | Release ultra-silent mode |
| `0x0AE` | OSD_Battery_Alert | Battery alert triggered |
| `0x0AF` | TimAP_HaierLB_Sw | Haier lightbar switch |
| `0x0B0` | OSD_FanModeSwitch | Fan mode switched |
| `0x0B3` | BacklightLevelChange | Keyboard backlight level changed |
| `0x0B4` | BacklightPowerChange | Keyboard backlight power state changed |
| `0x0B7` | TimAP_MicMute_Sw | Microphone mute switch |
| `0x0B8` | OSD_FnChange | Fn key behavior changed |
| `0x0BA` | CallAP | Call application (hotkey) |
| `0x0BB` | TimAP_HighTemp | High temperature warning |
| `0x0BC` | TimAP_WhisperUpdate | Whisper mode update |
| `0x0C0` | SC_SwitchOC | Switch overclock mode |
| `0x0C1` | SC_PumpFanAuto | Pump/fan auto mode |
| `0x0C2` | TimAP_HWOCScanCode | Hardware OC scan code |
| `0x0C3` | TimAP_LidClose | Lid closed |
| `0x0C4` | TimAP_LidOpen | Lid opened |
| `0x0C7` | PDWarning_Event / TimAP_AC_Chg | PD warning event / AC charger change |
| `0x0C8` | SC_ThermalProtect | Thermal protection activated |
| `0x0C9` | SC_CustomMode | Custom mode toggled |
| `0x0CB` | PHxGPURule1 | GPU power rule 1 (dGPU policy) |
| `0x0CD` | TimAP_ELU_Fn2 | ELU function 2 |
| `0x0CE` | TimAP_ELU_Fn3 | ELU function 3 |
| `0x0CF` | TimAP_NotifyEShutter | E-shutter notification |
| `0x0D0` | TimAP_NotifyFlipScreen | Flip screen notification |
| `0x0D1` | TimAP_DISPLAY_In | Display input change |
| `0xFE` | Enable_RGB_Music | Command: enable RGB music-sync mode |

---

## Appendix B. Fan / Power Mode Constants

Not EC registers — enum / flag values used in OEM software. Preserved here
for RE cross-reference.

| Code | Name | Description |
|------|------|-------------|
| 0 | FAN_GAMING_MODE | Fan profile: gaming |
| 1 | FAN_GAMING_MODE_SMART | Fan profile: gaming smart |
| 2 | FAN_GAMING_MODE_PERFENHANCED | Fan profile: gaming performance-enhanced |
| 1 | FAN_OFFICE_MODE | Fan profile: office |
| 1 | FAN_OFFICE_MODE_BASIC | Fan profile: office basic |
| 2 | FAN_OFFICE_MODE_ADVANCED | Fan profile: office advanced |
| 2 | FAN_TURBO_MODE | Fan profile: turbo |
| 3 | FAN_CUSTOM_MODE | Fan profile: custom |
| 0 | FAN_MODE_NORMAL | Fan mode switch: normal |
| 1 | FAN_MODE_BOOST | Fan mode switch: boost |
| 2 | FAN_MODE_CUSTOMIZE | Fan mode switch: customize |
| 0 | FAN_CUSTOMIZEMODE_NORMAL | Custom sub-mode: normal |
| 1 | FAN_CUSTOMIZEMODE_BASIC | Custom sub-mode: basic |
| 2 | FAN_CUSTOMIZEMODE_HIGH | Custom sub-mode: high |
| 1 | POWER_SETTING_MODE_ECO | Power: eco |
| 2 | POWER_SETTING_MODE_STD | Power: standard |
| 0 | TURBO_MODE_CPUBOOST | Turbo: CPU boost |
| 1 | TURBO_MODE_1 / TURBO_MODE_MIN | Turbo level 1 (min) |
| 2 | TURBO_MODE_2 / TURBO_MODE_DEFAULT | Turbo level 2 (default) |
| 3 | TURBO_MODE_3 | Turbo level 3 |
| 4 | TURBO_MODE_4 / TURBO_MODE_MAX | Turbo level 4 (max) |
