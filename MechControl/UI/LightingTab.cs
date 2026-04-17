using MechControl.Features;
using MechControl.Profile;

namespace MechControl.UI;

class LightingTab : UserControl
{
    private readonly LightingControl _control;
    private readonly LightingState _state;
    private readonly LocalDimmingControl? _dimming;
    private readonly LightingIdleTimer _idleTimer;

    // Logo controls
    private readonly CheckBox _chkLogo;
    private readonly NumericUpDown _nudLogoR;
    private readonly NumericUpDown _nudLogoG;
    private readonly NumericUpDown _nudLogoB;
    private readonly Panel _logoPreview;
    private readonly TrackBar _trkLogoBrightness;
    private readonly Label _lblLogoBrightness;

    // Keyboard controls
    private readonly CheckBox _chkKeyboard;
    private readonly NumericUpDown _nudKbR;
    private readonly NumericUpDown _nudKbG;
    private readonly NumericUpDown _nudKbB;
    private readonly Panel _kbPreview;
    private readonly TrackBar _trkKbBrightness;
    private readonly Label _lblKbBrightness;

    // Light bar controls
    private readonly CheckBox _chkLightBar;
    private readonly NumericUpDown _nudBarR;
    private readonly NumericUpDown _nudBarG;
    private readonly NumericUpDown _nudBarB;
    private readonly Panel _barPreview;
    private readonly TrackBar _trkBarBrightness;
    private readonly Label _lblBarBrightness;

    // Idle timer
    private readonly ComboBox _cboIdleTimer;

    // Local dimming
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
        AutoScroll = true;

        int y = 8;

        // ===== Logo Section =====
        var grpLogo = new GroupBox
        {
            Text = "Logo LED (Rear Cover)",
            Location = new System.Drawing.Point(8, y),
            Size = new System.Drawing.Size(420, 170),
            Font = UiHelpers.FontBold
        };
        Controls.Add(grpLogo);

        int gy = 26;
        _chkLogo = new CheckBox
        {
            Text = "Enabled",
            Location = new System.Drawing.Point(12, gy),
            AutoSize = true,
            Font = UiHelpers.FontNormal
        };
        grpLogo.Controls.Add(_chkLogo);

        grpLogo.Controls.Add(new Label
        {
            Text = "Brightness:",
            Location = new System.Drawing.Point(110, gy + 2),
            AutoSize = true,
            Font = UiHelpers.FontNormal
        });
        _trkLogoBrightness = new TrackBar
        {
            Minimum = 0, Maximum = 100, Value = 50, TickFrequency = 25,
            Location = new System.Drawing.Point(205, gy - 4),
            Size = new System.Drawing.Size(160, 28)
        };
        grpLogo.Controls.Add(_trkLogoBrightness);
        _lblLogoBrightness = new Label
        {
            Text = "50%",
            Location = new System.Drawing.Point(370, gy + 2),
            AutoSize = true,
            Font = UiHelpers.FontNormal
        };
        grpLogo.Controls.Add(_lblLogoBrightness);
        _trkLogoBrightness.ValueChanged += (s, e) =>
            _lblLogoBrightness.Text = $"{_trkLogoBrightness.Value}%";

        gy += 46;
        (_nudLogoR, _nudLogoG, _nudLogoB, _logoPreview) = CreateColorRow(grpLogo, gy);

        gy += 52;
        var btnApplyLogo = new Button
        {
            Text = "Apply",
            Location = new System.Drawing.Point(12, gy),
            Size = new System.Drawing.Size(80, 32),
            Font = UiHelpers.FontNormal
        };
        btnApplyLogo.Click += BtnApplyLogo_Click;
        grpLogo.Controls.Add(btnApplyLogo);

        var btnPickLogo = new Button
        {
            Text = "Pick Color...",
            Location = new System.Drawing.Point(100, gy),
            Size = new System.Drawing.Size(100, 32),
            Font = UiHelpers.FontNormal
        };
        btnPickLogo.Click += (s, e) => PickColor(_nudLogoR, _nudLogoG, _nudLogoB, _logoPreview);
        grpLogo.Controls.Add(btnPickLogo);

        y += 178;

        // ===== Keyboard Section =====
        var grpKeyboard = new GroupBox
        {
            Text = "Keyboard Backlight",
            Location = new System.Drawing.Point(8, y),
            Size = new System.Drawing.Size(420, 170),
            Font = UiHelpers.FontBold
        };
        Controls.Add(grpKeyboard);

        gy = 22;
        _chkKeyboard = new CheckBox
        {
            Text = "Enabled",
            Location = new System.Drawing.Point(12, gy),
            AutoSize = true,
            Font = UiHelpers.FontNormal
        };
        grpKeyboard.Controls.Add(_chkKeyboard);

        grpKeyboard.Controls.Add(new Label
        {
            Text = "Brightness:",
            Location = new System.Drawing.Point(110, gy + 2),
            AutoSize = true,
            Font = UiHelpers.FontNormal
        });
        _trkKbBrightness = new TrackBar
        {
            Minimum = 0,
            Maximum = 100,
            Value = 50,
            TickFrequency = 25,
            Location = new System.Drawing.Point(205, gy - 4),
            Size = new System.Drawing.Size(160, 28)
        };
        grpKeyboard.Controls.Add(_trkKbBrightness);
        _lblKbBrightness = new Label
        {
            Text = "50%",
            Location = new System.Drawing.Point(370, gy + 2),
            AutoSize = true,
            Font = UiHelpers.FontNormal
        };
        grpKeyboard.Controls.Add(_lblKbBrightness);
        _trkKbBrightness.ValueChanged += (s, e) =>
            _lblKbBrightness.Text = $"{_trkKbBrightness.Value}%";

        gy += 46;
        (_nudKbR, _nudKbG, _nudKbB, _kbPreview) = CreateColorRow(grpKeyboard, gy);

        gy += 52;
        var btnApplyKb = new Button
        {
            Text = "Apply",
            Location = new System.Drawing.Point(12, gy),
            Size = new System.Drawing.Size(80, 32),
            Font = UiHelpers.FontNormal
        };
        btnApplyKb.Click += BtnApplyKeyboard_Click;
        grpKeyboard.Controls.Add(btnApplyKb);

        var btnPickKb = new Button
        {
            Text = "Pick Color...",
            Location = new System.Drawing.Point(100, gy),
            Size = new System.Drawing.Size(100, 32),
            Font = UiHelpers.FontNormal
        };
        btnPickKb.Click += (s, e) => PickColor(_nudKbR, _nudKbG, _nudKbB, _kbPreview);
        grpKeyboard.Controls.Add(btnPickKb);

        var btnSaveKb = new Button
        {
            Text = "Save to EC",
            Location = new System.Drawing.Point(208, gy),
            Size = new System.Drawing.Size(110, 32),
            Font = UiHelpers.FontNormal
        };
        btnSaveKb.Click += BtnSaveKeyboard_Click;
        grpKeyboard.Controls.Add(btnSaveKb);

        y += 178;

        // ===== Light Bar Section =====
        var grpBar = new GroupBox
        {
            Text = "Light Bar (Front)",
            Location = new System.Drawing.Point(8, y),
            Size = new System.Drawing.Size(420, 170),
            Font = UiHelpers.FontBold
        };
        Controls.Add(grpBar);

        gy = 22;
        _chkLightBar = new CheckBox
        {
            Text = "Enabled",
            Location = new System.Drawing.Point(12, gy),
            AutoSize = true,
            Font = UiHelpers.FontNormal
        };
        grpBar.Controls.Add(_chkLightBar);

        grpBar.Controls.Add(new Label
        {
            Text = "Brightness:",
            Location = new System.Drawing.Point(110, gy + 2),
            AutoSize = true,
            Font = UiHelpers.FontNormal
        });
        _trkBarBrightness = new TrackBar
        {
            Minimum = 0, Maximum = 100, Value = 50, TickFrequency = 25,
            Location = new System.Drawing.Point(205, gy - 4),
            Size = new System.Drawing.Size(160, 28)
        };
        grpBar.Controls.Add(_trkBarBrightness);
        _lblBarBrightness = new Label
        {
            Text = "50%",
            Location = new System.Drawing.Point(370, gy + 2),
            AutoSize = true,
            Font = UiHelpers.FontNormal
        };
        grpBar.Controls.Add(_lblBarBrightness);
        _trkBarBrightness.ValueChanged += (s, e) =>
            _lblBarBrightness.Text = $"{_trkBarBrightness.Value}%";

        gy += 46;
        (_nudBarR, _nudBarG, _nudBarB, _barPreview) = CreateColorRow(grpBar, gy);

        gy += 52;
        var btnApplyBar = new Button
        {
            Text = "Apply",
            Location = new System.Drawing.Point(12, gy),
            Size = new System.Drawing.Size(80, 32),
            Font = UiHelpers.FontNormal
        };
        btnApplyBar.Click += BtnApplyLightBar_Click;
        grpBar.Controls.Add(btnApplyBar);

        var btnPickBar = new Button
        {
            Text = "Pick Color...",
            Location = new System.Drawing.Point(100, gy),
            Size = new System.Drawing.Size(100, 32),
            Font = UiHelpers.FontNormal
        };
        btnPickBar.Click += (s, e) => PickColor(_nudBarR, _nudBarG, _nudBarB, _barPreview);
        grpBar.Controls.Add(btnPickBar);

        var btnSaveBar = new Button
        {
            Text = "Save to EC",
            Location = new System.Drawing.Point(208, gy),
            Size = new System.Drawing.Size(110, 32),
            Font = UiHelpers.FontNormal
        };
        btnSaveBar.Click += BtnSaveLightBar_Click;
        grpBar.Controls.Add(btnSaveBar);

        y += 178;

        // ===== Idle Timer Section =====
        var grpIdle = new GroupBox
        {
            Text = "Idle Timer (Keyboard + Light Bar)",
            Location = new System.Drawing.Point(8, y),
            Size = new System.Drawing.Size(420, 59),
            Font = UiHelpers.FontBold
        };
        Controls.Add(grpIdle);

        grpIdle.Controls.Add(new Label
        {
            Text = "Turn off after:",
            Location = new System.Drawing.Point(12, 27),
            AutoSize = true,
            Font = UiHelpers.FontNormal
        });
        _cboIdleTimer = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList,
            Location = new System.Drawing.Point(120, 24),
            Size = new System.Drawing.Size(130, 24),
            Font = UiHelpers.FontNormal
        };
        foreach (var opt in IdleTimerOptions)
            _cboIdleTimer.Items.Add(opt.Text);
        _cboIdleTimer.SelectedIndex = 0;
        _cboIdleTimer.SelectedIndexChanged += CboIdleTimer_Changed;
        grpIdle.Controls.Add(_cboIdleTimer);

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

        y += 64;

        // ===== Local Dimming Section =====
        if (_dimming != null)
        {
            var grpDimming = new GroupBox
            {
                Text = "Local Dimming (Mini-LED)",
                Location = new System.Drawing.Point(8, y),
                Size = new System.Drawing.Size(420, 60),
                Font = UiHelpers.FontBold
            };
            Controls.Add(grpDimming);

            _chkDimming = new CheckBox
            {
                Text = "Enable Local Dimming",
                Location = new System.Drawing.Point(12, 24),
                AutoSize = true,
                Font = UiHelpers.FontNormal
            };
            _chkDimming.CheckedChanged += ChkDimming_CheckedChanged;
            grpDimming.Controls.Add(_chkDimming);

            y += 68;
        }

        // ===== Read button =====
        var btnRead = new Button
        {
            Text = "Read from EC",
            Location = new System.Drawing.Point(8, y),
            Size = new System.Drawing.Size(140, 32)
        };
        btnRead.Click += (s, e) => RefreshFromEc();
        Controls.Add(btnRead);

        // Disable zone sections if HID handles aren't available
        grpKeyboard.Enabled = _control.IsZoneAvailable(LightingControl.ZoneKeyboard);
        grpBar.Enabled = _control.IsZoneAvailable(LightingControl.ZoneLightBar);
        // Logo falls back to EC-only if HID not available, so always enabled

        foreach (Control grp in new Control[] { grpLogo, grpKeyboard, grpBar })
            UiHelpers.FixZOrder(grp.Controls);
    }

    private static (NumericUpDown r, NumericUpDown g, NumericUpDown b, Panel preview) CreateColorRow(Control parent, int y)
    {
        parent.Controls.Add(new Label { Text = "R:", Location = new System.Drawing.Point(12, y + 3), AutoSize = true, Font = UiHelpers.FontNormal });
        var nudR = new NumericUpDown { Minimum = 0, Maximum = 255, Location = new System.Drawing.Point(30, y), Size = new System.Drawing.Size(80, 24), Font = UiHelpers.FontNormal, TextAlign = HorizontalAlignment.Right };
        parent.Controls.Add(nudR);

        parent.Controls.Add(new Label { Text = "G:", Location = new System.Drawing.Point(116, y + 3), AutoSize = true, Font = UiHelpers.FontNormal });
        var nudG = new NumericUpDown { Minimum = 0, Maximum = 255, Location = new System.Drawing.Point(134, y), Size = new System.Drawing.Size(80, 24), Font = UiHelpers.FontNormal, TextAlign = HorizontalAlignment.Right };
        parent.Controls.Add(nudG);

        parent.Controls.Add(new Label { Text = "B:", Location = new System.Drawing.Point(220, y + 3), AutoSize = true, Font = UiHelpers.FontNormal });
        var nudB = new NumericUpDown { Minimum = 0, Maximum = 255, Location = new System.Drawing.Point(238, y), Size = new System.Drawing.Size(80, 24), Font = UiHelpers.FontNormal, TextAlign = HorizontalAlignment.Right };
        parent.Controls.Add(nudB);

        var preview = new Panel
        {
            Location = new System.Drawing.Point(326, y - 4),
            Size = new System.Drawing.Size(36, 36),
            BorderStyle = BorderStyle.FixedSingle,
            BackColor = System.Drawing.Color.Black
        };
        parent.Controls.Add(preview);

        Panel p = preview;
        EventHandler updatePreview = (s, e) =>
            p.BackColor = System.Drawing.Color.FromArgb((int)nudR.Value, (int)nudG.Value, (int)nudB.Value);
        nudR.ValueChanged += updatePreview;
        nudG.ValueChanged += updatePreview;
        nudB.ValueChanged += updatePreview;

        return (nudR, nudG, nudB, preview);
    }

    private static void PickColor(NumericUpDown nudR, NumericUpDown nudG, NumericUpDown nudB, Panel preview)
    {
        using var dlg = new ColorDialog
        {
            Color = preview.BackColor,
            FullOpen = true
        };
        if (dlg.ShowDialog() == DialogResult.OK)
        {
            nudR.Value = dlg.Color.R;
            nudG.Value = dlg.Color.G;
            nudB.Value = dlg.Color.B;
        }
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
            _chkLogo.Checked = enabled;
            _nudLogoR.Value = r;
            _nudLogoG.Value = g;
            _nudLogoB.Value = b;
            _trkLogoBrightness.Value = Math.Clamp(brightness, 0, 100);
            _lblLogoBrightness.Text = $"{_trkLogoBrightness.Value}%";
            _logoPreview.BackColor = System.Drawing.Color.FromArgb(r, g, b);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[LightingTab] Failed to read logo state: {ex.Message}");
        }

        // Keyboard: restore from saved state (HID-only, no EC readback)
        LoadSliderZone(_state.Keyboard, _chkKeyboard, _nudKbR, _nudKbG, _nudKbB, _kbPreview, _trkKbBrightness, _lblKbBrightness);

        // Light bar: restore from saved state (HID-only, no EC readback)
        LoadSliderZone(_state.LightBar, _chkLightBar, _nudBarR, _nudBarG, _nudBarB, _barPreview, _trkBarBrightness, _lblBarBrightness);
    }

    private static void LoadSliderZone(ZoneState zone, CheckBox chk,
        NumericUpDown nudR, NumericUpDown nudG, NumericUpDown nudB,
        Panel preview, TrackBar trk, Label lbl)
    {
        chk.Checked = zone.Enabled;
        nudR.Value = zone.R;
        nudG.Value = zone.G;
        nudB.Value = zone.B;
        trk.Value = Math.Clamp(zone.Brightness, 0, 100);
        lbl.Text = $"{trk.Value}%";
        preview.BackColor = System.Drawing.Color.FromArgb(zone.R, zone.G, zone.B);
    }

    private static void SaveSliderZone(ZoneState zone, CheckBox chk,
        NumericUpDown nudR, NumericUpDown nudG, NumericUpDown nudB, TrackBar trk)
    {
        zone.Enabled = chk.Checked;
        zone.R = (byte)nudR.Value;
        zone.G = (byte)nudG.Value;
        zone.B = (byte)nudB.Value;
        zone.Brightness = trk.Value;
    }

    private void BtnApplyLogo_Click(object? sender, EventArgs e) =>
        ApplyZone(LightingControl.ZoneLogo, _chkLogo, _nudLogoR, _nudLogoG, _nudLogoB,
            _trkLogoBrightness, _state.Logo, "logo");

    private void BtnApplyKeyboard_Click(object? sender, EventArgs e) =>
        ApplyZone(LightingControl.ZoneKeyboard, _chkKeyboard, _nudKbR, _nudKbG, _nudKbB,
            _trkKbBrightness, _state.Keyboard, "keyboard");

    private void BtnApplyLightBar_Click(object? sender, EventArgs e) =>
        ApplyZone(LightingControl.ZoneLightBar, _chkLightBar, _nudBarR, _nudBarG, _nudBarB,
            _trkBarBrightness, _state.LightBar, "light bar");

    private void BtnSaveKeyboard_Click(object? sender, EventArgs e) =>
        SaveZoneToBios(LightingControl.ZoneKeyboard, _nudKbR, _nudKbG, _nudKbB,
            _trkKbBrightness, _chkKeyboard, _state.Keyboard, "keyboard");

    private void BtnSaveLightBar_Click(object? sender, EventArgs e) =>
        SaveZoneToBios(LightingControl.ZoneLightBar, _nudBarR, _nudBarG, _nudBarB,
            _trkBarBrightness, _chkLightBar, _state.LightBar, "light bar");

    private void ApplyZone(int zone, CheckBox chk, NumericUpDown nudR, NumericUpDown nudG,
        NumericUpDown nudB, TrackBar trk, ZoneState zoneState, string label)
    {
        try
        {
            if (!_control.IsZoneAvailable(zone)) return;
            if (!chk.Checked || trk.Value == 0)
                _control.SetZoneOff(zone);
            else
                _control.SetZoneOn(zone, (byte)nudR.Value, (byte)nudG.Value, (byte)nudB.Value, trk.Value);
            SaveSliderZone(zoneState, chk, nudR, nudG, nudB, trk);
            _state.Save();
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to apply {label} settings:\n{ex.Message}",
                "Write Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void SaveZoneToBios(int zone, NumericUpDown nudR, NumericUpDown nudG,
        NumericUpDown nudB, TrackBar trk, CheckBox chk, ZoneState zoneState, string label)
    {
        try
        {
            _control.SaveToBios(zone, (byte)nudR.Value, (byte)nudG.Value, (byte)nudB.Value, trk.Value, chk.Checked);
            SaveSliderZone(zoneState, chk, nudR, nudG, nudB, trk);
            _state.Save();
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to save {label} to BIOS:\n{ex.Message}",
                "Write Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
            _idleTimer.Dispose();
        base.Dispose(disposing);
    }
}
