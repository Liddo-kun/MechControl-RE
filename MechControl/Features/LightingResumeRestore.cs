using MechControl.Driver;
using Microsoft.Win32;

namespace MechControl.Features;

/// <summary>
/// Restores keyboard / light bar lighting after standby.
///
/// The ITE HID controller can drop off the USB bus and re-enumerate across
/// sleep cycles (USB selective suspend / Modern Standby), which both
/// invalidates the app's cached HID handles (HidD_SetFeature error 1167) and
/// can reset the controller's lighting state. On resume: settle delay (USB
/// re-enumeration is slower than the EC), reopen the HID device paths, then
/// re-apply the user's saved zone state from lighting_state.json.
///
/// Only zones saved as Enabled are pushed — a zone the user turned off (or
/// never configured) is left to the controller's own flash-persisted state,
/// so this never overrides factory behavior on an untouched machine.
/// The logo zone is skipped entirely: it persists via EC registers, and
/// EC-side recovery is EcActivationWatchdog's job.
///
/// Model compatibility: HID enumeration is by ITE VID only; zones a model
/// doesn't expose report unavailable and are skipped.
/// </summary>
sealed class LightingResumeRestore : IDisposable
{
    private const int ResumeSettleDelayMs = 8_000;

    private readonly HidDevice _hid;
    private readonly LightingControl _control;
    private volatile bool _disposed;

    public LightingResumeRestore(HidDevice hid, LightingControl control)
    {
        _hid = hid;
        _control = control;
    }

    public void Start()
    {
        SystemEvents.PowerModeChanged += OnPowerModeChanged;
    }

    private void OnPowerModeChanged(object sender, PowerModeChangedEventArgs e)
    {
        if (e.Mode != PowerModes.Resume) return;
        // Raised on the UI message loop — move off it and give USB
        // re-enumeration time to finish before reopening.
        Task.Run(async () =>
        {
            await Task.Delay(ResumeSettleDelayMs);
            Restore();
        });
    }

    private void Restore()
    {
        if (_disposed) return;
        try
        {
            if (!_hid.Open())
            {
                DiagnosticLog.Warn("[Lighting] Resume restore: no HID lighting devices found on reopen");
                return;
            }

            var state = LightingState.Load();
            ReapplyIfEnabled(LightingControl.ZoneKeyboard, state.Keyboard, "keyboard");
            ReapplyIfEnabled(LightingControl.ZoneLightBar, state.LightBar, "light bar");
            DiagnosticLog.Write("[Lighting] Reopened HID devices after resume");
        }
        catch (Exception ex)
        {
            DiagnosticLog.Warn($"[Lighting] Resume restore failed: {ex.Message}");
        }
    }

    private void ReapplyIfEnabled(int zone, ZoneState zs, string name)
    {
        if (!zs.Enabled || !_control.IsZoneAvailable(zone)) return;
        try
        {
            _control.SetZoneOn(zone, zs.R, zs.G, zs.B, zs.Brightness);
            DiagnosticLog.Write($"[Lighting] Re-applied {name} state after resume");
        }
        catch (Exception ex)
        {
            DiagnosticLog.Warn($"[Lighting] Resume {name} re-apply failed: {ex.Message}");
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        SystemEvents.PowerModeChanged -= OnPowerModeChanged;
    }
}
