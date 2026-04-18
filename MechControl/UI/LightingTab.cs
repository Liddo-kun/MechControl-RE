using MechControl.Features;
using MechControl.Profile;

namespace MechControl.UI;

class LightingTab : UserControl
{
    private readonly LightingControl _control;
    private readonly LightingState _state;
    private readonly LocalDimmingControl? _dimming;
    private readonly LightingIdleTimer _idleTimer;

    private readonly LightingZonePanel _zoneLogo;
    private readonly LightingZonePanel _zoneKeyboard;
    private readonly LightingZonePanel _zoneLightBar;

    private readonly ComboBox _cboIdleTimer;
    private readonly CheckBox? _chkDimming;

    private static readonly (string Text, int Seconds)[] IdleTimerOptions =
    [
        ("Off", 0),
        ("10 seconds", 10),
        ("30 seconds", 30),
        ("1 minute", 60),
        ("2 minutes", 120),
        ("5 minutes", 300),
        ("10 minutes", 600),
    ];

    public LightingTab(LightingControl control, MachineProfile profile,
        LocalDimmingControl? dimming = null)
    {
        _control = control;
        _dimming = dimming;
        _state = LightingState.Load();
        Dock = DockStyle.Fill;

        var stack = UiHelpers.BuildSectionStack();

        // ── Zone panels ─────────────────────────────────────────
        _zoneLogo = new LightingZonePanel("Logo LED (Rear Cover)", hasSaveButton: false);
        _zoneLogo.ApplyClicked += (s, e) => ApplyZone(LightingControl.ZoneLogo, _zoneLogo, _state.Logo, "logo");
        stack.Controls.Add(_zoneLogo);

        _zoneKeyboard = new LightingZonePanel("Keyboard Backlight", hasSaveButton: true);
        _zoneKeyboard.ApplyClicked += (s, e) => ApplyZone(LightingControl.ZoneKeyboard, _zoneKeyboard, _state.Keyboard, "keyboard");
        _zoneKeyboard.SaveClicked += (s, e) => SaveZoneToBios(LightingControl.ZoneKeyboard, _zoneKeyboard, _state.Keyboard, "keyboard");
        stack.Controls.Add(_zoneKeyboard);

        _zoneLightBar = new LightingZonePanel("Light Bar (Front)", hasSaveButton: true);
        _zoneLightBar.ApplyClicked += (s, e) => ApplyZone(LightingControl.ZoneLightBar, _zoneLightBar, _state.LightBar, "light bar");
        _zoneLightBar.SaveClicked += (s, e) => SaveZoneToBios(LightingControl.ZoneLightBar, _zoneLightBar, _state.LightBar, "light bar");
        stack.Controls.Add(_zoneLightBar);

        // ── Idle Timer section ──────────────────────────────────
        var grpIdle = UiHelpers.BuildSection("Idle Timer (Keyboard + Light Bar)", fitContent: true);
        var idleLayout = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 3,
        };
        idleLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        idleLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        idleLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));

        idleLayout.Controls.Add(new Label
        {
            Text = "Turn off after:",
            AutoSize = true,
            Font = UiHelpers.FontNormal,
            Margin = new Padding(0, 6, 12, 0),
        });
        _cboIdleTimer = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList,
            Width = 140,
            Font = UiHelpers.FontNormal,
            Margin = new Padding(0, 2, 0, 0),
        };
        foreach (var opt in IdleTimerOptions)
            _cboIdleTimer.Items.Add(opt.Text);
        _cboIdleTimer.SelectedIndex = 0;
        _cboIdleTimer.SelectedIndexChanged += CboIdleTimer_Changed;
        idleLayout.Controls.Add(_cboIdleTimer);
        idleLayout.Controls.Add(new Label { Text = "", AutoSize = true }); // spacer
        grpIdle.Controls.Add(idleLayout);
        stack.Controls.Add(grpIdle);

        _idleTimer = new LightingIdleTimer(control, _state);

        // Restore saved idle timer setting (unrecognized values fall back to "Off")
        int savedSeconds = _state.IdleTimerSeconds;
        for (int i = 0; i < IdleTimerOptions.Length; i++)
        {
            if (IdleTimerOptions[i].Seconds == savedSeconds)
            {
                _cboIdleTimer.SelectedIndex = i;
                break;
            }
        }

        // ── Local Dimming section ───────────────────────────────
        if (_dimming != null)
        {
            var grpDimming = UiHelpers.BuildSection("Local Dimming (Mini-LED)", fitContent: true);
            _chkDimming = new CheckBox
            {
                Text = "Enable Local Dimming",
                AutoSize = true,
                Font = UiHelpers.FontNormal,
                Margin = new Padding(0, 4, 0, 0),
            };
            _chkDimming.CheckedChanged += ChkDimming_CheckedChanged;
            grpDimming.Controls.Add(_chkDimming);
            stack.Controls.Add(grpDimming);
        }

        // ── Read button row ─────────────────────────────────────
        var btnRead = new Button
        {
            Text = "Read from EC",
            Width = UiMetrics.ButtonReadWidth,
            Height = UiMetrics.ButtonHeight,
            Margin = new Padding(0, 0, 0, UiMetrics.SectionGap),
        };
        btnRead.Click += (s, e) => RefreshFromEc();
        stack.Controls.Add(btnRead);

        Controls.Add(stack);

        // Disable zone sections if HID handles aren't available
        _zoneKeyboard.Enabled = _control.IsZoneAvailable(LightingControl.ZoneKeyboard);
        _zoneLightBar.Enabled = _control.IsZoneAvailable(LightingControl.ZoneLightBar);
        // Logo falls back to EC-only if HID not available, so always enabled
    }

    private void CboIdleTimer_Changed(object? sender, EventArgs e)
    {
        int idx = _cboIdleTimer.SelectedIndex;
        if (idx < 0 || idx >= IdleTimerOptions.Length) return;
        int seconds = IdleTimerOptions[idx].Seconds;
        _idleTimer.TimeoutSeconds = seconds;
        _state.IdleTimerSeconds = seconds;
        _state.Save();
    }

    private bool _suppressDimmingEvent;

    private void ChkDimming_CheckedChanged(object? sender, EventArgs e)
    {
        if (_suppressDimmingEvent || _dimming == null || _chkDimming == null) return;
        try
        {
            _dimming.SetState(_chkDimming.Checked);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to toggle local dimming:\n{ex.Message}",
                "Write Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            // Revert checkbox to actual state
            _suppressDimmingEvent = true;
            try { _chkDimming.Checked = _dimming.ReadState(); }
            catch { }
            _suppressDimmingEvent = false;
        }
    }

    public void RefreshFromEc()
    {
        // Reload lighting state from file (may have been updated by profile load)
        var fresh = LightingState.Load();
        _state.Keyboard = fresh.Keyboard;
        _state.LightBar = fresh.LightBar;
        _state.Logo = fresh.Logo;
        _state.IdleTimerSeconds = fresh.IdleTimerSeconds;

        // Update idle timer combobox + timer
        int savedIdx = 0;
        for (int i = 0; i < IdleTimerOptions.Length; i++)
        {
            if (IdleTimerOptions[i].Seconds == _state.IdleTimerSeconds)
            { savedIdx = i; break; }
        }
        _cboIdleTimer.SelectedIndex = savedIdx;

        // Local dimming: read from EC
        if (_dimming != null && _chkDimming != null)
        {
            try
            {
                _suppressDimmingEvent = true;
                _chkDimming.Checked = _dimming.ReadState();
                _suppressDimmingEvent = false;
            }
            catch (Exception ex)
            {
                _suppressDimmingEvent = false;
                Console.WriteLine($"[LightingTab] Failed to read dimming state: {ex.Message}");
            }
        }

        // Logo: read from EC (has registers)
        try
        {
            var (enabled, r, g, b, brightness) = _control.ReadLogoState();
            _zoneLogo.ZoneEnabled = enabled;
            _zoneLogo.ZoneColor = System.Drawing.Color.FromArgb(r, g, b);
            _zoneLogo.ZoneBrightness = brightness;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[LightingTab] Failed to read logo state: {ex.Message}");
        }

        // Keyboard + Light bar: restore from saved state (HID-only, no EC readback)
        LoadZoneFromState(_zoneKeyboard, _state.Keyboard);
        LoadZoneFromState(_zoneLightBar, _state.LightBar);
    }

    private static void LoadZoneFromState(LightingZonePanel zone, ZoneState state)
    {
        zone.ZoneEnabled = state.Enabled;
        zone.ZoneColor = System.Drawing.Color.FromArgb(state.R, state.G, state.B);
        zone.ZoneBrightness = state.Brightness;
    }

    private static void SaveZoneToState(LightingZonePanel zone, ZoneState state)
    {
        state.Enabled = zone.ZoneEnabled;
        state.R = zone.ZoneColor.R;
        state.G = zone.ZoneColor.G;
        state.B = zone.ZoneColor.B;
        state.Brightness = zone.ZoneBrightness;
    }

    private void ApplyZone(int zoneId, LightingZonePanel zone, ZoneState zoneState, string label) =>
        UiHelpers.TryUi(() =>
        {
            if (!_control.IsZoneAvailable(zoneId)) return;
            var c = zone.ZoneColor;
            if (!zone.ZoneEnabled || zone.ZoneBrightness == 0)
                _control.SetZoneOff(zoneId);
            else
                _control.SetZoneOn(zoneId, c.R, c.G, c.B, zone.ZoneBrightness);
            SaveZoneToState(zone, zoneState);
            _state.Save();
        }, $"apply {label} settings", "Write Error");

    private void SaveZoneToBios(int zoneId, LightingZonePanel zone, ZoneState zoneState, string label) =>
        UiHelpers.TryUi(() =>
        {
            var c = zone.ZoneColor;
            _control.SaveToBios(zoneId, c.R, c.G, c.B, zone.ZoneBrightness, zone.ZoneEnabled);
            SaveZoneToState(zone, zoneState);
            _state.Save();
        }, $"save {label} to BIOS", "Write Error");

    protected override void Dispose(bool disposing)
    {
        if (disposing)
            _idleTimer.Dispose();
        base.Dispose(disposing);
    }
}
