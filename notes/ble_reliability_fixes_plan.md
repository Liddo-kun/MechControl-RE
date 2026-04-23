# MechControl -- BLE Reliability Fixes

## How to use this document

Self-contained plan. To execute after `/clear`:

1. Read `CLAUDE.md` (auto-loaded) for project conventions, build command, safety rules.
2. Read `Architecture_BLE.md` end-to-end for the BLE flow this plan modifies.
3. Read this file end-to-end. File paths below are absolute under `C:\Users\Jon\clean\`.
4. Use `TaskCreate`/`TaskUpdate` to track phases -- one task per phase, mark in-progress
   on entry, completed only after the phase's Validation passes.

## Goal

Fix three concurrency/lifecycle bugs in the BLE liquid cooler shutdown + retry path,
plus several smaller issues uncovered during the holistic review.

**Non-goal:** behavior changes to the protocol, UI layout, reconnect backoff schedule,
or feature gate. Model compatibility stays unchanged -- this is a reliability fix, not
a feature change.

## Bug catalog

### Bug 1 -- Shutdown reset command never fires (medium)

**Symptom:** On normal app exit, the cooler stays at its last fan/pump/RGB settings
instead of being reset to factory defaults via command `0x19`.

**Root cause (MechControl/UI/MainForm.cs:867-880):**

```
1. base.Dispose(disposing)        → LiquidCoolerTab.Dispose() runs first
                                    → _reconnectTimer.Dispose()          (tab.cs:787)
2. Task.Run(ShutdownAsync).Wait() → StopReconnectTimer()                 (tab.cs:774)
                                    → _reconnectTimer.Change(...)        (tab.cs:564)
                                    → ObjectDisposedException
                                    → exception unwinds, SendResetAsync never reached
3. outer try { } catch { }        → swallows the exception silently     (form.cs:875)
```

### Bug 2A -- Disconnect races with in-flight ConnectAsync (high, shutdown-scenario)

**Symptom:** During app shutdown, if the reconnect timer callback is mid-`ConnectAsync`
when `_bleDevice.Dispose()` fires, `Disconnect()` nulls `_device`/`_service`/`_tx`
while `ConnectAsync` is still assigning them. Race window causes NRE on the thread
pool (async void) → process crash; or silently corrupted state (connected when
shutdown expected us disconnected).

**Root cause (MechControl/Driver/BleDevice.cs:147, 247, 329):**
- `_connectLock` guards `ConnectAsync` only.
- `Disconnect()` runs unguarded -- nulls `_device`, `_tx`, disposes `_service`.
- `Dispose()` calls `Disconnect()` then disposes `_connectLock`.
- Timer callback on thread pool is still inside `ConnectAsync`, about to assign `_device`.

**Note on runtime trigger:** the "user clicks Disconnect during reconnect" scenario
does **not** actually trigger this bug. Reasoning: while a reconnect is in flight the
tab is disconnected, so the button reads "Connect", not "Disconnect" -- the user
can't click Disconnect. `BtnConnect_Click`'s connect branch goes through
`_connectLock` and is properly serialized. `ConnectionStatusChanged` doesn't call
`Disconnect()`. The only real trigger is shutdown via `BleDevice.Dispose()`.

### Bug 2B -- Timer callback crashes on disposed timer (high)

**Symptom:** On shutdown with a reconnect callback in flight, the callback reaches
`ScheduleNextReconnect()` after `_reconnectTimer.Dispose()` has run. `Timer.Change()`
on a disposed timer throws `ObjectDisposedException`. Because
`ReconnectTimerCallback` is `async void`, the exception propagates to the thread
pool as unhandled → **process crash**.

**Root cause (MechControl/UI/LiquidCoolerTab.cs:569-604):**

```csharp
private async void ReconnectTimerCallback(object? state)
{
    ...
    bool succeeded = false;
    try { if (await ConnectWithTimeoutAsync(...)) succeeded = true; ... }
    catch { }
    finally { _reconnecting = false; }

    if (!succeeded)
    {
        _reconnectAttempt++;
        ScheduleNextReconnect();   // ← OUTSIDE try/catch, and:
                                   //   _reconnectTimer.Change(...) can throw
                                   //   ObjectDisposedException after Dispose
    }
}
```

Both the placement outside the try/catch AND the lack of a wait-for-in-flight on
timer disposal contribute.

### Smaller issues (batched as one phase)

- **S1: Scan doesn't stop reconnect timer.** `BtnScan_Click` (tab.cs:382) lets the
  timer fire mid-scan, flipping UI state behind the user's mental model.
- **S2: Reconnect callback doesn't probe BT availability.** Works indirectly via
  `watcher.Start()` throwing (BleDevice.cs:125-130), logs a spurious
  "Advertisement watcher start failed" every backoff tick while BT is off.
- **S3: No diagnostic logging for BLE state transitions.** Uses `Console.WriteLine`
  only. `DiagnosticLog.Write` is the standard elsewhere in the app. Hurts field
  debugging.
- **S4: InitAsync BT poll redundant with reconnect timer's fast tier.** `InitAsync`
  polls `IsBluetoothAvailableAsync` 4× with 3s delays (9s window, not 12s as the
  doc/comment claim) just to decide whether to call `TryConnectAsync` vs
  `StartReconnectTimer`. But `TryConnectAsync`'s failure path calls
  `StartReconnectTimer` anyway. Two retry loops doing similar work.
- **S5: Doc/code drift.** `Architecture_BLE.md:198` says "~12s window", actual is
  9s (or whatever we pick after S4). Fix after S4.

### Explicitly skipped

**Unguarded mutation of `_tx`/`_rx` in `OnConnectionStatusChanged`
(BleDevice.cs:318-327).** Races with in-flight `WriteAsync` / `WriteAndReceiveAsync`.
Theoretical NRE, but every current caller has a try/catch that swallows it (same
user-visible outcome as a legitimate BLE failure). Not worth a fix now; revisit if
a new caller lacks a catch.

## Fixes

All three bugs collapse to one discipline:
**"Wait for in-flight async work before tearing down resources."**

Applied in two places:

### Fix A -- LiquidCoolerTab.Dispose: wait for in-flight callback (fixes 2A + 2B)

**File:** `MechControl/UI/LiquidCoolerTab.cs:782-790`

Replace:

```csharp
protected override void Dispose(bool disposing)
{
    if (disposing)
    {
        _ble.ConnectionLost -= _onConnectionLostHandler;
        _reconnectTimer.Dispose();
    }
    base.Dispose(disposing);
}
```

With:

```csharp
protected override void Dispose(bool disposing)
{
    if (disposing)
    {
        _ble.ConnectionLost -= _onConnectionLostHandler;

        // Wait for any in-flight ReconnectTimerCallback to return before
        // the timer is actually disposed. Prevents:
        //   - ObjectDisposedException from ScheduleNextReconnect's
        //     _reconnectTimer.Change() on a disposed timer (bug 2B).
        //   - BleDevice.Disconnect() racing with the callback's in-flight
        //     ConnectAsync (bug 2A) -- callback finishes, ConnectAsync
        //     exits, _connectLock released, before MainForm.Dispose moves
        //     on to _bleDevice.Dispose().
        using var done = new ManualResetEvent(false);
        try
        {
            _reconnectTimer.Dispose(done);
            done.WaitOne(3000);   // cap on shutdown latency
        }
        catch { }
    }
    base.Dispose(disposing);
}
```

Also: harden `ScheduleNextReconnect` / `StopReconnectTimer` against the disposed-timer
case (defensive; the wait above already prevents it, but protects against any
`StopReconnectTimer` path we haven't traced):

**File:** `MechControl/UI/LiquidCoolerTab.cs:562-573`

```csharp
private void StopReconnectTimer()
{
    try { _reconnectTimer.Change(Timeout.Infinite, Timeout.Infinite); }
    catch (ObjectDisposedException) { }
    _reconnecting = false;
    _reconnectAttempt = 0;
}

private void ScheduleNextReconnect()
{
    int delayMs = ReconnectBackoffMs[Math.Min(_reconnectAttempt, ReconnectBackoffMs.Length - 1)];
    try { _reconnectTimer.Change(delayMs, Timeout.Infinite); }
    catch (ObjectDisposedException) { }
}
```

### Fix B -- MainForm.Dispose: run ShutdownAsync before base.Dispose (fixes 1)

**File:** `MechControl/UI/MainForm.cs:844-880`

Move the `ShutdownAsync` call to **before** `base.Dispose(disposing)` so the
`_reconnectTimer` is still live when `StopReconnectTimer` / `SendResetAsync` run.

Replace the current ordering (simplified):

```csharp
protected override void Dispose(bool disposing)
{
    if (disposing) { /* tray cleanup etc. */ }
    base.Dispose(disposing);      // ← disposes LiquidCoolerTab (timer gone)
    if (disposing)
    {
        if (_liquidCoolerTab != null)
        {
            try { Task.Run(() => _liquidCoolerTab.ShutdownAsync()).Wait(3000); }
            catch { }
        }
        _bleDevice?.Dispose();
        _hidDevice.Dispose();
        _device.Dispose();
    }
}
```

With:

```csharp
protected override void Dispose(bool disposing)
{
    if (disposing)
    {
        // Send reset + stop timer BEFORE base.Dispose disposes the
        // LiquidCoolerTab (which disposes the timer). Otherwise
        // ShutdownAsync's StopReconnectTimer throws ObjectDisposedException
        // and SendResetAsync never runs (bug 1).
        // Task.Run avoids deadlock by running async BLE work off the UI
        // SynchronizationContext. 3s timeout prevents hang if BLE is stuck.
        if (_liquidCoolerTab != null)
        {
            try { Task.Run(() => _liquidCoolerTab.ShutdownAsync()).Wait(3000); }
            catch { }
        }

        /* tray cleanup etc. -- whatever was in the first `if (disposing)` */
    }
    base.Dispose(disposing);      // FanTab restores curves; LiquidCoolerTab
                                  // waits for in-flight callback (Fix A)
    if (disposing)
    {
        _bleDevice?.Dispose();
        _hidDevice.Dispose();
        _device.Dispose();        // EC last -- child controls may write during disposal
    }
}
```

Note: Fix A's wait-for-in-flight in `LiquidCoolerTab.Dispose` still runs during
`base.Dispose` and still protects 2A/2B even after this reordering -- Fix A and
Fix B are independent.

### Fix C -- Smaller issues

**S1 (tab.cs:382)** -- Add `StopReconnectTimer()` at the top of `BtnScan_Click`:

```csharp
private async void BtnScan_Click(object? sender, EventArgs e)
{
    StopReconnectTimer();
    // ... existing body unchanged
}
```

**S2 (tab.cs:575-604)** -- Check BT availability at the top of
`ReconnectTimerCallback` before attempting connect:

```csharp
private async void ReconnectTimerCallback(object? state)
{
    if (_reconnecting || _ble.IsConnected || _state.SavedAddress == 0) return;

    // Skip the connect attempt if BT isn't available -- saves logging a
    // spurious "watcher start failed" every backoff tick.
    if (!await BleDevice.IsBluetoothAvailableAsync())
    {
        _reconnectAttempt++;
        ScheduleNextReconnect();
        return;
    }

    _reconnecting = true;
    // ... rest unchanged
}
```

**S3** -- Route BLE state transitions through `DiagnosticLog.Write`. Replace
`Console.WriteLine` in:
- `BleDevice.cs:89`  -- "Scan start failed"
- `BleDevice.cs:128` -- "Advertisement watcher start failed"
- `BleDevice.cs:185`, `201` -- NUS / TX retry messages
- `BleDevice.cs:234` -- "Connected to ..."
- `BleDevice.cs:262` -- "Disconnected"
- `BleDevice.cs:322` -- "Connection lost"
- `LiquidCoolerTab.cs:747` -- "[LC] Apply failed"
- (leave `Console.WriteLine` in `LiquidCoolerState.cs` load/save catches -- persistence
  errors are rare and not BLE-lifecycle events)

Also add new log lines at reconnect-attempt boundaries:
- Start of `ReconnectTimerCallback`: `$"[BLE] Reconnect attempt {_reconnectAttempt + 1}"`.
- Successful reconnect: `"[BLE] Reconnect succeeded"`.

**S4 (tab.cs:752-769)** -- Collapse `InitAsync`'s BT poll into the reconnect timer.
Replace:

```csharp
public async Task InitAsync()
{
    for (int i = 0; i < 4; i++)
    {
        await CheckBluetoothAsync();
        if (!_btUnavailablePanel.Visible) break;
        if (i < 3) await Task.Delay(3000);
    }

    if (!_state.AutoConnect || _state.SavedAddress == 0) return;

    if (!_btUnavailablePanel.Visible)
        await TryConnectAsync(_state.SavedAddress);
    else
        StartReconnectTimer();
}
```

With:

```csharp
public async Task InitAsync()
{
    // One BT availability check. If BT isn't up yet, rely on the reconnect
    // timer's fast tier (8s × 4) instead of a second retry loop. The first
    // tick is scheduled immediately below.
    await CheckBluetoothAsync();

    if (!_state.AutoConnect || _state.SavedAddress == 0) return;

    if (!_btUnavailablePanel.Visible)
        await TryConnectAsync(_state.SavedAddress);
    else
        StartReconnectTimer();   // will retry once BT comes up (S2 check)
}
```

The reconnect timer's S2 gating (check BT before connect) + S3 logging together
cover the slow-BT-on-cold-boot case without the InitAsync loop.

**S5** -- After S4 lands, update `Architecture_BLE.md:198` to match: remove
"retries BT availability check up to 4 times with 3s delays" and the "~12s window"
language. Describe the new behavior: "InitAsync does a single BT check; if BT is
down it starts the reconnect timer, which gates its own connect attempts on BT
availability and retries with backoff."

## Execution phases

Each phase leaves the app building and running. One commit per phase so a crash
or rollback has a clean boundary.

### Phase 0 -- baseline
- Build + run elevated on the test machine.
- Verify current behavior: BLE tab loads, can scan, can connect, settings apply.
- Note: baseline screenshot is not needed (this is behavior, not UI layout).

### Phase 1 -- Fix A (LiquidCoolerTab.Dispose wait-for-in-flight + defensive catches)
- Edit `MechControl/UI/LiquidCoolerTab.cs`:
  - `Dispose(bool)` as shown in **Fix A**.
  - `StopReconnectTimer` + `ScheduleNextReconnect` add `try/catch(ObjectDisposedException)`.
- Build.
- **Validation:**
  - Run app, connect to cooler, let it sit.
  - Close app via File → Exit (triggers `MainForm.Dispose`).
  - App exits cleanly (no crash dialog, no stuck process in Task Manager).
  - Launch again -- auto-reconnect works on next start.
  - (Optional stress: power off the cooler while connected, let reconnect timer
    enter its retry loop, then close the app mid-retry. Should exit cleanly.)

### Phase 2 -- Fix B (MainForm.Dispose reorder)
- Edit `MechControl/UI/MainForm.cs:844-880`: move `ShutdownAsync` call before
  `base.Dispose(disposing)`.
- Build.
- **Validation:**
  - Enable DiagnosticLog.
  - Connect to cooler, set fan/pump to non-default values, verify they take effect.
  - Close app via Exit.
  - Observe: cooler returns to reset/defaults state (fan spins down, LEDs change).
  - DiagnosticLog shows a successful reset write before shutdown.
  - No crash.

### Phase 3 -- Fix C: smaller issues (S1, S2, S3 batched)
- Edit `MechControl/UI/LiquidCoolerTab.cs` for S1 + S2.
- Edit `MechControl/Driver/BleDevice.cs` + `MechControl/UI/LiquidCoolerTab.cs` for S3.
- Build.
- **Validation:**
  - Turn off Bluetooth in Windows Settings with auto-connect enabled.
  - Launch app. DiagnosticLog shows reconnect attempts gated on BT availability
    (no spurious "watcher start failed" entries every tick).
  - Turn BT on. Next reconnect tick connects cleanly.
  - Click Scan during an active reconnect cycle -- timer stops, scan runs, no
    silent connect behind the UI.

### Phase 4 -- S4 (collapse InitAsync BT poll)
- Edit `MechControl/UI/LiquidCoolerTab.cs:752-769` per **Fix C / S4**.
- Build.
- **Validation:**
  - With auto-connect enabled and cooler powered on:
    - Cold boot via Task Scheduler (the original motivating scenario).
      Alternative: reboot, let app start via Task Scheduler, verify auto-connect
      succeeds within the first few reconnect ticks.
  - With BT turned off in Windows:
    - Launch app. Tab shows BT-unavailable overlay.
    - Turn BT on. Within ~8s, reconnect timer fires, overlay clears on success.
  - With cooler powered off:
    - Launch app. Reconnect timer runs backoff schedule indefinitely (capped 5min).

### Phase 5 -- S5 (doc update)
- Edit `Architecture_BLE.md:198` per **Fix C / S5**.
- No code change, no build.
- **Validation:** doc matches code.

## Risk register

| Risk | Phase | Mitigation |
|------|-------|------------|
| `Timer.Dispose(WaitHandle)` wait hangs past 3s (callback genuinely stuck on BLE op) | 1 | 3s WaitOne cap; callback is itself bounded by `ConnectWithTimeoutAsync`'s 15s internal timeout -- worst case shutdown latency is ~3s, then the orphaned callback finishes in the background. |
| Reordering `ShutdownAsync` before `base.Dispose` changes other child-control disposal order | 2 | Only `LiquidCoolerTab` talks to BLE. Fan/Lighting/Power tabs don't use `_bleDevice`. EC/HID disposal still happens last. Verify by running the app and exercising other tabs' disposal paths (exit from each tab). |
| `IsBluetoothAvailableAsync` inside `ReconnectTimerCallback` adds latency per tick | 3 (S2) | The call is a single `GetDefaultAsync` + `GetRadioAsync` -- both complete in <100ms when BT is on. Cheap compared to the 5s advertisement prime already in `ConnectAsync`. |
| Removing InitAsync BT poll regresses Task Scheduler cold-boot case | 4 (S4) | S2's gating in the reconnect callback covers this: callback probes BT each tick, skips the connect attempt until BT is up, then tries. The first reconnect tick fires 8s after `StartReconnectTimer`, so worst-case cold-boot wait is 8s longer than today's 9s poll -- effectively equivalent. If Phase 4 validation fails on the test machine, revert this phase only (others stand). |

## Model-compatibility note

Nothing in this plan reads or writes EC registers, touches profile data, or changes
hardware detection. `IsSupportLiquidCooling` (EC 0x7A4 bit 7) still gates the tab.
All fixes are in the BLE driver / tab / form lifecycle -- model-agnostic by
construction.

## File pointers (quick reference)

| Change | File | Lines |
|--------|------|-------|
| Fix A -- Dispose wait | `MechControl/UI/LiquidCoolerTab.cs` | 782-790 |
| Fix A -- defensive catch | `MechControl/UI/LiquidCoolerTab.cs` | 562-573 |
| Fix B -- reorder shutdown | `MechControl/UI/MainForm.cs` | 844-880 |
| S1 -- Scan stops timer | `MechControl/UI/LiquidCoolerTab.cs` | 382 |
| S2 -- BT probe in callback | `MechControl/UI/LiquidCoolerTab.cs` | 575-604 |
| S3 -- DiagnosticLog | `MechControl/Driver/BleDevice.cs` | 89, 128, 185, 201, 234, 262, 322 |
| S3 -- DiagnosticLog | `MechControl/UI/LiquidCoolerTab.cs` | 575 (start), 587 (success), 747 (apply) |
| S4 -- Collapse InitAsync | `MechControl/UI/LiquidCoolerTab.cs` | 752-769 |
| S5 -- Doc update | `Architecture_BLE.md` | 198 |

## Build

```powershell
cd C:\Users\Jon\clean\MechControl
pwsh -File build-scripts\build-release.ps1
# Run elevated:
#   bin\Release\net8.0-windows10.0.19041.0\MechControl.exe
```
