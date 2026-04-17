# Plan: System Tray Integration

## Goal
MechControl starts minimized to system tray. Left-click tray icon opens the window. Closing/minimizing hides to tray instead of exiting. Right-click tray icon shows context menu with "Exit" to actually quit. No console window.

---

## Files to Change

### 1. `MechControl.csproj` (line 4)

Change `OutputType` from `Exe` to `WinExe`. This eliminates the console window entirely — no P/Invoke hack needed.

```xml
<!-- BEFORE -->
<OutputType>Exe</OutputType>

<!-- AFTER -->
<OutputType>WinExe</OutputType>
```

That's the only change in this file.

---

### 2. `MechControl/UI/MainForm.cs`

This is the main file to modify. All tray logic lives here.

#### 2a. Add fields (after line 30, the `_profileBar` field)

```csharp
private readonly NotifyIcon _trayIcon;
private readonly ContextMenuStrip _trayMenu;
private bool _exitRequested;
```

#### 2b. Create tray icon in constructor (after the `FormClosing` handler at line 318-323, before the closing brace of the constructor)

```csharp
// System tray icon
_trayMenu = new ContextMenuStrip();
_trayMenu.Items.Add("Show", null, (s, e) => ShowFromTray());
_trayMenu.Items.Add("-");  // separator
_trayMenu.Items.Add("Exit", null, (s, e) => { _exitRequested = true; Close(); });

_trayIcon = new NotifyIcon
{
    Text = "MechControl",
    Icon = SystemIcons.Application,  // uses default app icon; replace with custom .ico later
    ContextMenuStrip = _trayMenu,
    Visible = true
};
_trayIcon.MouseClick += (s, e) =>
{
    if (e.Button == MouseButtons.Left)
        ShowFromTray();
};

// Start hidden in tray
WindowState = FormWindowState.Minimized;
ShowInTaskbar = false;
```

#### 2c. Modify the existing FormClosing handler (lines 318-323)

Replace the current `FormClosing` handler:

```csharp
// BEFORE:
FormClosing += (s, e) =>
{
    _batteryTab?.StopTelemetry();
    _monitor.Dispose();
    DiagnosticLog.Close();
};

// AFTER:
FormClosing += (s, e) =>
{
    if (!_exitRequested && e.CloseReason == CloseReason.UserClosing)
    {
        // Hide to tray instead of closing
        e.Cancel = true;
        Hide();
        ShowInTaskbar = false;
        return;
    }

    // Actually exiting — clean up
    _trayIcon.Visible = false;
    _batteryTab?.StopTelemetry();
    _monitor.Dispose();
    DiagnosticLog.Close();
};
```

#### 2d. Add ShowFromTray helper method (after the `RefreshAllTabs` method, around line 344)

```csharp
private void ShowFromTray()
{
    Show();
    ShowInTaskbar = true;
    WindowState = FormWindowState.Normal;
    Activate();
}
```

#### 2e. Override OnResize to hide on minimize (after ShowFromTray)

```csharp
protected override void OnResize(EventArgs e)
{
    base.OnResize(e);
    if (WindowState == FormWindowState.Minimized)
    {
        Hide();
        ShowInTaskbar = false;
    }
}
```

#### 2f. Update Dispose method (line 525-533) to clean up tray resources

```csharp
// BEFORE:
protected override void Dispose(bool disposing)
{
    if (disposing)
    {
        _hidDevice.Dispose();
        _device.Dispose();
    }
    base.Dispose(disposing);
}

// AFTER:
protected override void Dispose(bool disposing)
{
    if (disposing)
    {
        _trayIcon.Dispose();
        _trayMenu.Dispose();
        _hidDevice.Dispose();
        _device.Dispose();
    }
    base.Dispose(disposing);
}
```

---

### 3. Icon (optional improvement, not required for v1)

Currently uses `SystemIcons.Application` (generic Windows icon). To add a custom icon later:

1. Create/find a 16x16 + 32x32 .ico file
2. Place at `MechControl/Resources/app.ico`
3. Add to csproj: `<ApplicationIcon>Resources\app.ico</ApplicationIcon>`
4. Load in MainForm: `Icon = new Icon("Resources/app.ico"); _trayIcon.Icon = Icon;`

---

## Behavior Summary

| Action | Result |
|--------|--------|
| App starts | Window hidden, tray icon appears |
| Left-click tray icon | Window shows and activates |
| Right-click tray icon | Context menu: "Show" / "Exit" |
| Click X (close button) | Window hides to tray (app keeps running) |
| Click minimize | Window hides to tray |
| Right-click → Exit | App actually exits, cleans up EC/HID/tray |
| Alt+F4 | Same as clicking X (hides to tray) |

---

## Testing Checklist

1. Build: `cd MechControl && dotnet build -c Release`
2. No console window appears on launch
3. App starts with only tray icon visible (no taskbar entry, no window)
4. Left-click tray icon → window appears, taskbar entry appears
5. Click X → window disappears, taskbar entry gone, tray icon remains
6. Minimize → same as clicking X (hides to tray)
7. Right-click tray → context menu with "Show" and "Exit"
8. "Show" → same as left-click
9. "Exit" → app closes completely, tray icon disappears
10. EC monitor keeps running while hidden (verify via diagnostic log)
11. After "Exit", verify no orphaned processes (`tasklist | grep MechControl`)

---

## Notes

- The EcMonitor continues running while the window is hidden. This is intentional — it keeps the EC connection alive and monitors hardware.
- `_exitRequested` flag distinguishes user clicking X (hide) from the Exit menu item (actual close). `CloseReason.UserClosing` ensures we only intercept user-initiated closes, not system shutdown/logoff.
- No changes to Program.cs needed — `Application.Run(new MainForm(...))` still works because the form isn't closed, just hidden.
