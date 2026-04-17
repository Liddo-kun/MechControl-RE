# Liquid Cooler BLE Protocol — Reverse Engineering Notes

## Overview

The OEM app (GCUService) communicates with an external liquid cooler over Bluetooth Low Energy (BLE) using Windows GATT APIs. The `LiquidCooling` class manages the lifecycle; the `BLE` class handles device discovery, GATT service connection, and characteristic read/write.

**Obfuscation:** GCUService.exe is aggressively obfuscated — all method bodies replaced with `throw new Exception("Runtime exception")` at IL level. The GATT UUIDs (`UUID_SERVICE`, `UUID_SEND_GATT_CHARACTERISTIC`, `UUID_EVENT_GATT_CHARACTERISTIC`) and device names (`DEVICE_NAME`, `DEVICE_NAME2`) are declared as `private readonly` fields but their values cannot be extracted from the binary. Runtime sniffing (nRF Connect, Wireshark BLE) is needed to capture them.

---

## Telemetry Packet Format (from DSDT firmware)

Source: `acpi/dsdt.dsl:74309-74338` — the `BTLC` method constructs an 11-byte payload.

```
Name (BTLY, Package (0x02) { One, Package (0x0B) { 0x12, 0,0,0,0,0,0,0,0,0,0 } })

Method (BTLC, 0, Serialized)
{
    DerefOf (BTLY [One]) [One] = TILE
    DerefOf (BTLY [One]) [0x02] = TIS0
    DerefOf (BTLY [One]) [0x03] = TS0X
    DerefOf (BTLY [One]) [0x04] = TIS4
    DerefOf (BTLY [One]) [0x05] = TIS5
    DerefOf (BTLY [One]) [0x06] = BLEC
    DerefOf (BTLY [One]) [0x07] = BLPD
    DerefOf (BTLY [One]) [0x08] = BLPI
    Return (BTLY)
}
```

### Byte Map

| Byte | DSDT Variable | Size | Meaning |
|------|---------------|------|---------|
| 0 | `0x12` (constant) | uint8 | Header / packet type identifier |
| 1 | `TILE` | uint8 | Temperature — likely inlet or identifying temp |
| 2 | `TIS0` | uint8 | Temperature sensor 0 (likely **CPU**) |
| 3 | `TS0X` | uint8 | Temperature sensor 0 variant (secondary CPU or package) |
| 4 | `TIS4` | uint8 | Temperature sensor 4 (likely **GPU**) |
| 5 | `TIS5` | uint8 | Temperature sensor 5 (possibly SSD or ambient) |
| 6 | `BLEC` | uint8 | BLE error/status code |
| 7 | `BLPD` | uint8 | Pump duty (0-100%) |
| 8 | `BLPI` | uint8 | Pump current/indicator |
| 9 | — | uint8 | Unused (initialized zero) |
| 10 | — | uint8 | Unused (initialized zero) |

All temperature values are **raw unsigned Celsius** (e.g., `0x55` = 85C). Same encoding as the EC thermal registers.

---

## GNVS Variable Definitions

Source: `acpi/dsdt.dsl:3515-3522` — these live in the Global NVS memory region (`OperationRegion GNVS, SystemMemory, 0x61C96000`).

```
TILE,   8,    // Temperature inlet / identifier
TIS0,   8,    // Temperature input sensor 0
TS0X,   8,    // Temperature sensor 0 extended
TIS4,   8,    // Temperature input sensor 4
TIS5,   8,    // Temperature input sensor 5
BLEC,   8,    // Bluetooth error code
BLPD,   8,    // Bluetooth pump duty
BLPI,   8,    // Bluetooth pump indicator
```

These are populated by firmware from the EC thermal sensor registers.

---

## EC Thermal Sensor Registers

Source: `acpi/dsdt.dsl:104539-104550` — EC memory-mapped region (`OperationRegion ECMG, SystemMemory, 0xFE410000`).

| EC Offset | DSDT Name | Meaning |
|-----------|-----------|---------|
| 0xE0D | `CPUT` | CPU temperature |
| 0xE0E | `PCHT` | PCH temperature |
| 0xE10 | `SN1T` | Sensor 1 temperature |
| 0xE12 | `SN2T` | Sensor 2 temperature |
| 0xE14 | `SN3T` | Sensor 3 temperature |
| 0xE16 | `SN4T` | Sensor 4 temperature |
| 0xE18 | `SN5T` | Sensor 5 temperature |

The firmware maps a subset of these into the GNVS `TIS*` variables for the BLE packet. The most probable mapping:
- `TIS0` = `CPUT` (CPU)
- `TIS4` = `SN4T` (GPU — sensor 4 is typically dGPU on these boards)
- `TIS5` = `SN5T`

---

## EC Liquid Cooling Registers

Source: `register_reference.md` section 11.9, confirmed across all 46 OEM profiles.

| EC Address | Name | Access | Meaning |
|------------|------|--------|---------|
| 0x7E6 | `ADDR_LC_FAN_VALUE` | R/W | Liquid cooling fan duty value |
| 0x7E7 | `ADDR_LC_PUMP_VALUE` | R/W | Liquid cooling pump duty value |

---

## BLE Class Structure

Source: `decompilation/gcuservice_full_decompile.txt:11505-11950`

### Fields
```csharp
private readonly string DEVICE_NAME;          // BLE advertised name (primary)
private readonly string DEVICE_NAME2;         // BLE advertised name (alternate)
private readonly Guid UUID_SERVICE;           // GATT service UUID
private readonly Guid UUID_SEND_GATT_CHARACTERISTIC;  // Write characteristic
private readonly Guid UUID_EVENT_GATT_CHARACTERISTIC;  // Notify characteristic
private DeviceWatcher m_deviceWatcher;
private string m_matched_device_id;
public string m_matched_dev_mac;
public string m_matched_dev_name;
public bool m_matched_diff_fw;
public string AP_FWver;                       // App firmware version
public string LC_FWver;                       // Liquid cooler firmware version
private BluetoothLEDevice m_bluetoothLeDevice;
private GattDeviceService m_gatt_service;
private GattCharacteristic m_selected_gatt_character;
public List<string> DeviceMacList;
public int meter_state;                       // Telemetry state
```

### State Machine
```csharp
public enum BLE_STATE
{
    DeviceNotReady,   // 0
    DeviceIsReady,    // 1
    Disconnected,     // 2
    Scanning,         // 3
    Connecting,       // 4
    IsConnectable,    // 5
    Connected         // 6
}
```

### Key Methods (signatures only — bodies obfuscated)
- `BLE_Init()` — Initialize Bluetooth radio and device watcher
- `BLE_Write(byte[] array)` — Send data to cooler via GATT write characteristic
- `BLE_Manual_Connect()` / `BLE_Manual_DisConnect()`
- `BLE_Manual_SaveDevMAC(string DevMac)` / `BLE_Manual_ClearDevMAC()`
- `Characteristic_ValueChanged(...)` — Incoming GATT notification handler (parses telemetry)
- `ConnectionStatusChanged(...)` — Connection state monitor

---

## LiquidCooling Class

Source: `decompilation/gcuservice_full_decompile.txt:16885-17135`

### Control Methods (signatures only — bodies obfuscated)
```csharp
private void FanControl(bool onoff, byte duty)
private void PumpControl(bool onoff, byte duty, byte Volt)
private void LEDControl(bool onoff, byte R, byte G, byte B, byte M)
private void FanLEDControl(bool onoff, byte R, byte G, byte B, byte M)
public void Get_Meter_State()           // Request telemetry from cooler
public void ConnectLC() / DisconnectLC()
public void GoToSleepLC() / GoToLineoffLC()
public void RunFanLED()
public void RunCoolingAuto()
public void PumpFanAuto()
```

### Saved Settings

File: `LCSavingSettings/LCSavingSettings.json`
```json
{
  "LCSaving": {
    "AutoConnect": false,
    "Connected": false,
    "LedR": 0, "LedG": 128, "LedB": 0,
    "LedMode": 0,
    "FanLedMode": 0,
    "LC_action": true,
    "CoolingAuto": true,
    "PumpV": 8,
    "FanD": 50,
    "PumpCtrl": "1",
    "FanCtrl": "4",
    "FanBoost": false,
    "PumpBoost": false,
    "DeviceMacList": []
  }
}
```

---

## Internal Messaging (MQTT Topics)

Source: `decompilation/gcuservice_full.txt:33695-33697`

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `BT_LC/Control` | App -> Cooler | Commands to liquid cooler |
| `BT_LC/Status` | Cooler -> App | Status from liquid cooler |
| `LCHWOC/Control` | App -> HWOC | Liquid cooler GPU overclock commands |
| `LCHWOC/Status` | HWOC -> App | Liquid cooler GPU overclock status |

---

## Feature Detection

The OEM app checks `LiquidCoolingSupport` and `LiquidCoolingAutoModeSupport` from the registry at `SOFTWARE\OEM\GamingCenter2\ItemSupport` (source: `decompilation/defaulttool_decompile.txt:17559-17560`). These are also visible in the MQTT `Customize/SupportInfo` status message.

The ACPI scan code `0x0C1` (`SC_PumpFanAuto`) triggers automatic pump/fan control mode (source: `register_reference.md:801`).

---

## Decoding on ESP32

If the ESP32 is already receiving GATT notifications from the OEM app, verify the packet starts with `0x12` (the BTLC telemetry header). Then decode:

```c
void onBLENotify(uint8_t* data, size_t len) {
    if (len >= 9 && data[0] == 0x12) {
        uint8_t temp_inlet = data[1];  // TILE
        uint8_t cpu_temp   = data[2];  // TIS0 — most likely CPU temp in C
        uint8_t cpu_temp2  = data[3];  // TS0X — secondary CPU / package temp
        uint8_t gpu_temp   = data[4];  // TIS4 — most likely GPU temp in C
        uint8_t temp5      = data[5];  // TIS5 — SSD or ambient
        uint8_t ble_error  = data[6];  // BLEC — 0 = OK
        uint8_t pump_duty  = data[7];  // BLPD — pump duty 0-100%
        uint8_t pump_curr  = data[8];  // BLPI — pump current indicator
    }
}
```

**To confirm the mapping:** Log raw bytes while monitoring HWiNFO temperatures. Byte 2 should closely track CPU temp; byte 4 should track GPU temp. If bytes are swapped vs expectation, the firmware sensor-to-GNVS mapping may differ by model.

**Caveat:** The DSDT defines the firmware-level packet structure. The OEM app's `BLE_Write()` may use additional framing or different command types for fan/pump/LED control. Packets with a first byte other than `0x12` are likely control commands, not telemetry.
