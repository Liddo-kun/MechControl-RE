using System.Runtime.InteropServices;

namespace MechControl.Features;

/// <summary>
/// Monitors system idle time (no keyboard/mouse input) and turns off
/// keyboard backlight and light bar after a configurable timeout.
/// Restores them when input is detected. Does not affect the logo.
/// </summary>
class LightingIdleTimer : IDisposable
{
    [StructLayout(LayoutKind.Sequential)]
    private struct LASTINPUTINFO
    {
        public uint cbSize;
        public uint dwTime;
    }

    [DllImport("user32.dll")]
    private static extern bool GetLastInputInfo(ref LASTINPUTINFO plii);

    private readonly LightingControl _control;
    private readonly LightingState _state;
    private readonly System.Windows.Forms.Timer _pollTimer;

    private int _timeoutSeconds;
    private bool _isSleeping;

    public LightingIdleTimer(LightingControl control, LightingState state)
    {
        _control = control;
        _state = state;
        _pollTimer = new System.Windows.Forms.Timer { Interval = 1000 };
        _pollTimer.Tick += PollTick;
    }

    public int TimeoutSeconds
    {
        get => _timeoutSeconds;
        set
        {
            _timeoutSeconds = value;
            if (value <= 0)
            {
                _pollTimer.Stop();
                if (_isSleeping) Wake();
            }
            else
            {
                _pollTimer.Start();
            }
        }
    }

    private static uint GetIdleTimeMs()
    {
        var info = new LASTINPUTINFO { cbSize = (uint)Marshal.SizeOf<LASTINPUTINFO>() };
        if (!GetLastInputInfo(ref info)) return 0;
        return unchecked((uint)Environment.TickCount - info.dwTime);
    }

    private void PollTick(object? sender, EventArgs e)
    {
        if (_timeoutSeconds <= 0) return;

        uint idleMs = GetIdleTimeMs();
        uint thresholdMs = (uint)_timeoutSeconds * 1000;

        if (!_isSleeping && idleMs >= thresholdMs)
            Sleep();
        else if (_isSleeping && idleMs < 2000)
            Wake();
    }

    private void Sleep()
    {
        _isSleeping = true;

        try
        {
            if (_state.Keyboard.Enabled && _control.IsZoneAvailable(LightingControl.ZoneKeyboard))
                _control.SetZoneOff(LightingControl.ZoneKeyboard);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[IdleTimer] KB off failed: {ex.Message}");
        }

        try
        {
            if (_state.LightBar.Enabled && _control.IsZoneAvailable(LightingControl.ZoneLightBar))
                _control.SetZoneOff(LightingControl.ZoneLightBar);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[IdleTimer] LB off failed: {ex.Message}");
        }
    }

    private void Wake()
    {
        _isSleeping = false;

        try
        {
            if (_state.Keyboard.Enabled && _control.IsZoneAvailable(LightingControl.ZoneKeyboard))
            {
                var kb = _state.Keyboard;
                _control.SetZoneOn(LightingControl.ZoneKeyboard, kb.R, kb.G, kb.B, kb.Brightness);
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[IdleTimer] KB restore failed: {ex.Message}");
        }

        try
        {
            if (_state.LightBar.Enabled && _control.IsZoneAvailable(LightingControl.ZoneLightBar))
            {
                var bar = _state.LightBar;
                _control.SetZoneOn(LightingControl.ZoneLightBar, bar.R, bar.G, bar.B, bar.Brightness);
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[IdleTimer] LB restore failed: {ex.Message}");
        }
    }

    public void Dispose()
    {
        _pollTimer.Stop();
        _pollTimer.Dispose();
    }
}
