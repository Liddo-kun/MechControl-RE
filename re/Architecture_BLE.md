# MechControl -- BLE Liquid Cooler Architecture

Extends MechControl with Bluetooth Low Energy support for the AQUARIS-compatible external liquid cooler. The cooler is a USB-powered radiator/pump unit that communicates over BLE using the Nordic UART Service.

Protocol details sourced from the Tuxedo Control Center (GPL) AQUARIS implementation, cross-referenced against the OEM GCUService decompilation and DSDT firmware tables, and validated via ETW BLE traffic capture from the OEM app.

---

## 1. Files

```
Driver/
  BleDevice.cs               WinRT BLE GATT: adapter check, scan, connect, write, notify, disconnect

Features/
  LiquidCoolerControl.cs     Protocol command layer: fan, pump, RGB, reset, commit, FW version
  LiquidCoolerState.cs       JSON persistence for saved MAC/name, auto-connect, last settings

UI/
  LiquidCoolerTab.cs         Full tab: scan/connect, fan/pump sliders, RGB, BT-unavailable overlay

Profile/
  UserProfile.cs             LiquidCoolerProfileSettings class (save/load in user profile slots)
```

---

## 2. Communication Layer (BleDevice.cs)

Third communication channel alongside EC (IOCTL) and HID (USB feature reports). Uses WinRT `Windows.Devices.Bluetooth` APIs via .NET 8 projection (`net8.0-windows10.0.19041.0` TFM). No external NuGet packages.

### Nordic UART Service (NUS)

Standard BLE serial-over-GATT profile used by the AQUARIS cooler's nRF MCU.

| Role | UUID |
|------|------|
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| TX (app writes) | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| RX (app receives) | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |

### API Surface

```
BleDevice : IDisposable
  -- Static --
  IsBluetoothAvailableAsync() -> bool       Adapter present + radio on
  ScanAsync(durationMs, ct) -> List<BleDeviceInfo>
                                            BluetoothLEAdvertisementWatcher, filters by
                                            device name containing "LCT21001" or "LCT22002"
  FormatAddress(ulong) -> string            AA:BB:CC:DD:EE:FF display format

  -- Instance --
  IsConnected -> bool                       True when GATT TX characteristic is resolved
  DeviceName -> string?                     BLE advertised name of connected device
  ConnectedAddress -> ulong                 Bluetooth address (persist for auto-reconnect)
  ConnectionLost : event Action             Fires on thread-pool thread when link drops

  ConnectAsync(ulong address, ct)            Primes Windows' BLE device cache with a 5s
                                            advertisement scan for the target address —
                                            FromBluetoothAddressAsync is a cache lookup and
                                            returns null on Task Scheduler cold boot before
                                            an advertisement has been processed. Then
                                            resolves NUS service + TX/RX characteristics.
                                            Auto-selects WriteWithResponse or WriteWithoutResponse
                                            based on characteristic properties. Enables RX
                                            notifications persistently (CCCD). Retries GATT
                                            resolution up to 3 times with 1.5s delay (GATT
                                            server may not be ready immediately after app
                                            restart). Concurrent calls serialized via
                                            SemaphoreSlim to prevent manual-click vs
                                            reconnect-timer state corruption.
                                            CancellationToken checked between each GATT
                                            operation — callers use Task.WhenAny to enforce
                                            a 15s hard timeout (WinRT BLE ops can hang
                                            indefinitely when device is unreachable).
  Disconnect()                              Synchronous: nulls chars, disposes service + device
  WriteAsync(byte[])                        Write to TX via DataWriter + WriteValueAsync
  WriteAndReceiveAsync(byte[], timeoutMs)   Attach RX handler, write, await response
                                            (notifications already enabled on connect)
  Dispose()                                 Calls Disconnect()
```

### Known Device Models

| Name | Model |
|------|-------|
| `LCT21001` | AQUARIS gen 1 |
| `LCT22002` | AQUARIS gen 2 |

Both Tuxedo and MECHREVO are Uniwill ODM customers using the same cooler hardware.

---

## 3. Protocol (LiquidCoolerControl.cs)

All commands are 8-byte frames:

```
[0xFE] [CMD] [ON/OFF] [P1] [P2] [P3] [P4] [0xEF]
 start                 payload                end
```

`ON/OFF`: `0x01` = enable, `0x00` = disable/off.

### Commands

| Command | CMD | P1 | P2 | P3 | P4 |
|---------|-----|----|----|----|-----|
| Fan on | `0x1B` | duty% (0-100) | 0 | 0 | 0 |
| Fan off | `0x1B` | 0 | 0 | 0 | 0 |
| Pump on | `0x1C` | duty% (0-100) | voltage | 0 | 0 |
| Pump off | `0x1C` | 0 | 0 | 0 | 0 |
| RGB on | `0x1E` | R (0-255) | G (0-255) | B (0-255) | mode |
| RGB off | `0x1E` | 0 | 0 | 0 | 0 |
| Reset | `0x19` | 0 | 1 | 0 | 0 |
| Commit | `0x33` | 0 | 0 | 0 | 0 |

**Commit (0x33)**: Discovered via ETW BLE capture of OEM app traffic. The OEM sends this after every settings batch. Sent once after all fan/pump/RGB commands in a group. Also sent after each individual Apply button press.

**Firmware version**: standalone 2-byte command `[0x73, 0x77]` (no framing). Send via TX, await response on RX notification. Returns UTF-8 string (e.g. "MCU F/W Version: 2.0.0.4").

### Cooler Responses

The cooler sends notifications on RX (handle 0x000F) in two cases:

| Response | Data | When |
|----------|------|------|
| Status | `FE 31 05 01 EF` | After CCCD notification enable |
| Status | `FE 31 05 02 EF` | After initial settings batch + commit |
| FW version | UTF-8 string | In response to `[0x73, 0x77]` query |

The `0x31` status responses are not used by our implementation. They appear to be a connection handshake — `05 01` = "notifications active", `05 02` = "settings received". Only sent during initial connect, not after every commit.

### Pump Note

The pump duty cycle byte is sent in the protocol but **has no effect on hardware** — the pump is voltage-controlled only. Confirmed by tester: changing duty produces no speed change, changing voltage does. The duty byte is still sent (default 60) for protocol compatibility.

### Pump Voltage

| Enum | Byte | Meaning |
|------|------|---------|
| `V11` | `0x00` | 11 volts |
| `V12` | `0x01` | 12 volts |
| `V7` | `0x02` | 7 volts |
| `V8` | `0x03` | 8 volts (OEM default) |

### RGB Modes

| Enum | Byte |
|------|------|
| `Static` | `0x00` |
| `Breathe` | `0x01` |
| `Colorful` | `0x02` |
| `BreatheColor` | `0x03` |

---

## 4. Feature Detection

### Gate: EC register `0x7A4` bit 7

Read during `HardwareDetection` at startup. This is the OEM's `IsSupportLiquidCooling()` check (confirmed in `defaulttool_decompile.txt:13315`). Hardware-authoritative, model-agnostic -- no model whitelist needed.

- Bit 7 set -> create `BleDevice` + `LiquidCoolerTab`, add "Liquid Cooler" tab
- Bit 7 clear -> no tab, no BLE initialization

### Bluetooth availability is NOT a feature gate

If `0x7A4` bit 7 is set but Bluetooth is unavailable (no adapter, adapter disabled, radio off), the tab still appears with a "Bluetooth is not available" overlay and a Retry button. This way:
- The user knows the feature exists on their machine
- They can enable Bluetooth without restarting the app (Scan button re-checks availability)

---

## 5. Connection Lifecycle

### Manual connect

```
User clicks Scan -> BluetoothLEAdvertisementWatcher runs 5s
                 -> filters by name containing LCT21001 / LCT22002
                 -> results shown in ComboBox with name, MAC, RSSI
User selects device, clicks Connect
                 -> ConnectAsync(address) -> resolve NUS -> resolve TX/RX
                    -> enable RX notifications (CCCD)
                 -> ApplyCurrentAsync() -> send fan/pump/RGB + commit
                 -> ReadFirmwareVersionAsync()
                 -> save address + name to liquid_cooler_state.json
```

On restart, the combo box is pre-populated with the saved device (name + address + "[saved]") so the user can click Connect without scanning first.

### Auto-reconnect

When `AutoConnect` is enabled and a saved address exists:
- On startup: `MainForm.Load` calls `LiquidCoolerTab.InitAsync()` (not tab Load, which only fires when the tab is first shown)
- InitAsync does a single BT availability check, then calls `TryConnectAsync(savedAddress)` if BT is up.
- If BT is down (e.g. Task Scheduler cold boot, slow BT stack init — some machines take 30s+), InitAsync starts the reconnect timer. The callback gates each attempt on `IsBluetoothAvailableAsync` so it silently backs off until the radio comes up, then connects and hides the "Bluetooth not available" overlay.
- All connect attempts use `ConnectWithTimeoutAsync` — 15s hard timeout via `Task.WhenAny` + `CancellationToken` (WinRT BLE GATT operations can hang indefinitely when the device is unreachable). On timeout, the in-flight `ConnectAsync` is left to finish in the background — WinRT BLE ops don't honour cancellation mid-op, so calling `Disconnect()` from the timeout path would race with the still-running connect. Stale `_device`/`_tx` state is cleaned up under `_connectLock` at the start of the next connect attempt (`if (_device != null) Disconnect()` inside `ConnectAsync`).
- On connect success: `ApplyCurrentAsync()` sends saved settings to cooler
- On unexpected disconnect (`ConnectionLost` event): start the reconnect timer (single-fire, backoff)
- **Backoff schedule:** 8s × 4, then 20s, 40s, then 5 min capped. Avoids hammering the BLE radio for hours when the cooler is absent (battery + airspace cost). Counter resets to 0 on any successful connection, on `StartReconnectTimer`, and on `StopReconnectTimer`.
- Timer callback calls `ConnectWithTimeoutAsync(savedAddress)`, marshals `OnConnected()` + `ApplyCurrentAsync()` to UI thread via `BeginInvoke`. On failure, reschedules the next tick with the next backoff delay.
- Manual connect (`BtnConnect_Click`) and Scan (`BtnScan_Click`) both stop the reconnect timer first so a pending tick doesn't flip UI state mid-interaction. Concurrent `ConnectAsync` calls are additionally serialized inside `BleDevice` via a SemaphoreSlim so timer/click races can't corrupt `_device`/`_service` state mid-resolve.
- Stops on successful connect or explicit disconnect
- `ConnectAsync` primes the BLE device cache with a 5s advertisement scan before resolving the device, then retries GATT resolution up to 3 times with 1.5s delay (GATT server may not be ready after restart)

### Disconnect

User clicks Disconnect (or app shuts down):
1. Stop reconnect timer
2. Send reset command (`0x19`) -- cooler returns to factory defaults
3. `BleDevice.Disconnect()` -- null chars, dispose service + device

### Shutdown order (MainForm.Dispose)

```
1. Task.Run(() => LiquidCoolerTab.ShutdownAsync()).Wait(3000)
     -- stop timer, send reset (0x19), cooler returns to factory defaults.
        MUST run before base.Dispose because base.Dispose disposes the
        LiquidCoolerTab (and its reconnect timer) -- if this ran after,
        StopReconnectTimer would throw ObjectDisposedException and
        SendResetAsync would never fire. Task.Run avoids deadlock by
        running async BLE work off the UI SynchronizationContext.
        3s timeout prevents hang if BLE is stuck.
2. Tray icon + menu disposed
3. base.Dispose() -- child controls:
     - FanTab restores EC curves via LinkedFanController.Dispose
     - LiquidCoolerTab.Dispose unsubscribes ConnectionLost and waits up
       to 3s (via Timer.Dispose(WaitHandle)) for any in-flight reconnect
       callback to return before the timer is actually disposed. This
       prevents the callback's ScheduleNextReconnect from throwing
       ObjectDisposedException on a disposed timer, and prevents
       BleDevice.Disconnect from racing with the callback's in-flight
       ConnectAsync.
4. BleDevice.Dispose() -- disconnect
5. HidDevice.Dispose()
6. EcDevice.Dispose() -- last (child controls may write during disposal)
```

---

## 6. Persistence

### liquid_cooler_state.json (next to exe)

Stores device-level state that persists across app restarts, independent of user profile slots.

```jsonc
{
  "savedAddress": 123456789012,  // ulong BLE address for auto-reconnect
  "savedDeviceName": "CoolingSystem LCT22002",  // for combo box display on restart
  "autoConnect": true,
  "fanOn": true,      "fanDuty": 50,
  "pumpOn": true,     "pumpDuty": 60,   "pumpVoltage": 3,  // PumpVoltage.V8
  "rgbOn": false,     "r": 0, "g": 128, "b": 0,  "rgbMode": 0
}
```

### User profiles (userprofile_N.json)

`LiquidCoolerProfileSettings` is saved/loaded as the `liquidCooler` field in user profile JSON. Nullable -- absent for profiles saved before LC support was added, or on machines without LC.

On profile load: `ApplySettings()` updates UI controls, then `ApplyCurrentAsync()` sends BLE commands (fire-and-forget, only if connected).

On profile save: `ReadSettings()` reads current UI state from the tab controls.

---

## 7. EC Registers (not used in v1)

The EC has two liquid cooling registers confirmed across 45 of 46 OEM profiles:

| Address | Name | Purpose |
|---------|------|---------|
| `0x7E6` | `ADDR_LC_FAN_VALUE` | Liquid cooling fan duty |
| `0x7E7` | `ADDR_LC_PUMP_VALUE` | Liquid cooling pump duty |

These are likely bookkeeping registers updated by the OEM app for EC-side telemetry or auto-mode. Since we send commands directly over BLE to the cooler's MCU, these registers are not needed. They may become useful if EC-based auto-mode (temp-reactive pump/fan curves) is implemented in the future.

---

## 8. What's NOT Implemented (v1 scope)

| Feature | Why deferred |
|---------|-------------|
| Auto-mode (temp-based curves) | Adds complexity: needs temp polling loop, curve UI, pump safety logic. Manual control is the useful first step. |
| Telemetry readback | Investigated thoroughly. Probe tool tested 8 candidate commands -- no response. ETW capture of OEM app showed no telemetry commands beyond what we already implement. The cooler does not appear to support readback. DSDT `BTLC` packet is likely host-to-cooler (sending temps), not cooler-to-host. |
| EC registers 0x7E6/0x7E7 | Bookkeeping only. BLE direct control doesn't need them. |
| Fan LED (separate zone) | OEM's `FanLEDControl` is a distinct command from `LEDControl`. Tuxedo doesn't implement it. Could be added if the cooler has separate fan LEDs. |
| Pump duty slider | Removed from UI. Pump is voltage-controlled only -- duty cycle byte has no hardware effect. Still sent in protocol (default 60) for compatibility. |

---

## 9. RE Sources

| Source | What it provided |
|--------|-----------------|
| Tuxedo Control Center (`tuxedo/tuxedo-control-center/src/e-app/LCT21001.ts`) | Complete BLE protocol: NUS UUIDs, 8-byte frame format, all command encodings, pump voltages, RGB modes, FW version read |
| OEM decompilation (`decompilation/defaulttool_decompile.txt:13315`) | Feature detection: `IsSupportLiquidCooling()` reads EC `0x7A4` bit 7 |
| OEM decompilation (`decompilation/gcuservice_full_decompile.txt:11505-17135`) | BLE class structure, state machine, LiquidCooling class signatures (bodies obfuscated). Note: BLE code is in GCUService decompilation but GCUService (SYSTEM service) cannot access Bluetooth. Actual BLE is handled by `GamingCenter3_Cross.exe` (UI app), brokered through svchost. |
| DSDT firmware (`acpi/dsdt.dsl:74309`) | BTLC telemetry packet format (11 bytes, header `0x12`), GNVS variable definitions, EC thermal sensor mapping. Likely host-to-cooler direction (not readback). |
| OEM profiles (`Profiles/MECHREVO/*.json`) | 45/46 profiles define `0x7E6`/`0x7E7` LC registers |
| Tuxedo feature gating (`TuxedoControlCenterDaemon.ts:548`) | DMI-based model whitelist (7 STELLARIS/STEPOL SKUs). Not used by us -- we use the OEM's EC bit 7 approach instead. |
| ETW BLE capture (`tools/ble_capture_start.bat`) | Live BLE traffic from OEM app on tester hardware (LCT22002, FW 2.0.0.4). Discovered CMD `0x33` (commit), response `0x31` (status), confirmed OEM enables CCCD before sending commands. Captured via `Microsoft-Windows-BTH-BTHPORT` and `Microsoft-Windows-USB-UCX` ETW providers. Frida hooks on GCUService and GamingCenter3_Cross both failed to capture BLE -- WinRT BLE APIs are brokered through svchost `bluetoothapis.dll`. |
