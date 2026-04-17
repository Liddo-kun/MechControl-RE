using MechControl.Driver;
using MechControl.Features;
using MechControl.Profile;

namespace MechControl.UI;

class FanTab : UserControl
{
    private readonly FanControl _control;
    private readonly FanSwitchSpeedControl? _switchSpeedControl;
    private readonly LinkedFanController? _linkedController;

    private readonly CheckBox _chkFanBoost;
    private readonly CheckBox _chkFanCurve;
    private readonly CheckBox? _chkLinkFans;
    private readonly FanCurvePanel _cpuCurvePanel;
    private readonly FanCurvePanel _gpuCurvePanel;
    private readonly Panel? _linkedStatusPanel;
    private readonly Label _lblValidation;
    private readonly CheckBox? _chkSwitchSpeed;
    private readonly TrackBar? _sliderSwitchSpeed;
    private readonly Label? _lblSwitchSpeedValue;
    private Button? _btnApplySwitchSpeed;
    private readonly Button _btnApply;
    private readonly Button _btnRead;
    private readonly bool _hasGpuFan;
    private readonly bool _curveEditingEnabled;

    // Panels own the curves — these properties are the source of truth
    public FanCurveModel? CpuCurve => _cpuCurvePanel.Curve;
    public FanCurveModel? GpuCurve => _gpuCurvePanel.Curve;
    public bool LinkedEnabled => _linkedController?.IsEnabled ?? false;

    public FanTab(FanControl control, MachineProfile profile, EcDevice device,
        FanSwitchSpeedControl? switchSpeedControl = null)
    {
        _control = control;
        _switchSpeedControl = switchSpeedControl;
        _hasGpuFan = profile.FanTable.HasGpuFan;
        _curveEditingEnabled = profile.FanTable.Format == "RamFan1";
        Dock = DockStyle.Fill;
        AutoScroll = true;

        int y = 12;

        // --- Fan Boost toggle ---
        _chkFanBoost = new CheckBox
        {
            Text = "Fan Boost",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Font = UiHelpers.FontBold
        };
        Controls.Add(_chkFanBoost);

        var btnApplyBoost = new Button
        {
            Text = "Apply Boost",
            Location = new System.Drawing.Point(160, y),
            Size = new System.Drawing.Size(110, 32)
        };
        btnApplyBoost.Click += BtnApplyBoost_Click;
        Controls.Add(btnApplyBoost);

        y += 32;

        // --- Fan Curve Enable toggle ---
        _chkFanCurve = new CheckBox
        {
            Text = "Custom Fan Curve",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Font = UiHelpers.FontBold
        };
        Controls.Add(_chkFanCurve);

        int curveEnableY = y; // capture for Link Fans checkbox placement

        // --- Curve editors side by side ---
        _cpuCurvePanel = new FanCurvePanel
        {
            Title = "CPU Fan Curve",
            DutyBaseAddress = profile.FanTable.CpuDutyAddr
        };
        _cpuCurvePanel.CurveChanged += OnCurveChanged;
        Controls.Add(_cpuCurvePanel);

        _gpuCurvePanel = new FanCurvePanel
        {
            Title = "GPU Fan Curve",
            DutyBaseAddress = profile.FanTable.GpuDutyAddr,
            Visible = _hasGpuFan && _curveEditingEnabled
        };
        _gpuCurvePanel.CurveChanged += OnCurveChanged;
        Controls.Add(_gpuCurvePanel);

        // Format warning when curve editing is not supported
        if (!_curveEditingEnabled)
        {
            _cpuCurvePanel.Visible = false;
            var lblFormatWarn = new Label
            {
                Text = $"Fan curve editing is not available (format: {profile.FanTable.Format}). Only fan boost and curve enable are supported.",
                Location = new System.Drawing.Point(16, y + 20),
                AutoSize = true,
                ForeColor = System.Drawing.Color.OrangeRed,
                Font = UiHelpers.FontBold
            };
            Controls.Add(lblFormatWarn);
        }

        // --- Link Fans toggle (only if GPU fan present and temp registers available) ---
        if (_hasGpuFan && _curveEditingEnabled
            && profile.Registers.ContainsKey("cpuTemp") && profile.Registers.ContainsKey("gpuTemp"))
        {
            _linkedController = new LinkedFanController(device, control, profile,
                () => _cpuCurvePanel.Curve, () => _gpuCurvePanel.Curve);

            _chkLinkFans = new CheckBox
            {
                Text = "Link Fans",
                Location = new System.Drawing.Point(340, curveEnableY),
                AutoSize = true,
                Font = UiHelpers.FontBold
            };
            _chkLinkFans.CheckedChanged += OnLinkFansChanged;
            Controls.Add(_chkLinkFans);

            _linkedStatusPanel = new Panel
            {
                Location = new System.Drawing.Point(450, curveEnableY),
                Size = new System.Drawing.Size(420, 26),
                Visible = false,
                BorderStyle = BorderStyle.FixedSingle
            };
            _linkedStatusPanel.Paint += PaintLinkedStatus;
            Controls.Add(_linkedStatusPanel);

            _linkedController.Updated += () => { if (_linkedStatusPanel?.Visible == true) _linkedStatusPanel.Invalidate(); };
        }

        y += 36;

        int btnY = y + 260;

        _btnApply = new Button
        {
            Text = "Apply",
            Location = new System.Drawing.Point(8, btnY),
            Size = new System.Drawing.Size(130, 32)
        };
        _btnApply.Click += BtnApply_Click;
        Controls.Add(_btnApply);

        _btnRead = new Button
        {
            Text = "Read from EC",
            Location = new System.Drawing.Point(8, btnY + 36),
            Size = new System.Drawing.Size(140, 32),
            Visible = _curveEditingEnabled
        };
        _btnRead.Click += BtnRead_Click;
        Controls.Add(_btnRead);

        _btnApply.Visible = _curveEditingEnabled;

        _lblValidation = new Label
        {
            Location = new System.Drawing.Point(140, btnY + 38),
            AutoSize = true,
            Font = UiHelpers.FontSmall,
            ForeColor = System.Drawing.Color.Green,
            Text = "",
            Visible = _curveEditingEnabled
        };
        Controls.Add(_lblValidation);

        // --- Fan Switch Speed ---
        if (_switchSpeedControl != null)
        {
            int ssY = btnY + 72;

            _chkSwitchSpeed = new CheckBox
            {
                Text = "Fan Switch Speed",
                Location = new System.Drawing.Point(16, ssY),
                AutoSize = true,
                Font = UiHelpers.FontBold
            };
            _chkSwitchSpeed.CheckedChanged += (s, e) =>
            {
                if (_sliderSwitchSpeed != null)
                    _sliderSwitchSpeed.Enabled = _chkSwitchSpeed.Checked;
            };
            Controls.Add(_chkSwitchSpeed);

            ssY += 28;

            _sliderSwitchSpeed = new TrackBar
            {
                Minimum = 1,
                Maximum = 127,
                Value = 3,
                TickFrequency = 10,
                LargeChange = 10,
                SmallChange = 1,
                AutoSize = false,
                Location = new System.Drawing.Point(16, ssY),
                Size = new System.Drawing.Size(400, 45),
                Enabled = false
            };
            Controls.Add(_sliderSwitchSpeed);

            _lblSwitchSpeedValue = new Label
            {
                Text = "300 ms",
                Location = new System.Drawing.Point(426, ssY + 8),
                AutoSize = true,
                Font = UiHelpers.FontMedium
            };
            Controls.Add(_lblSwitchSpeedValue);

            _sliderSwitchSpeed.ValueChanged += (s, e) =>
                _lblSwitchSpeedValue!.Text = $"{_sliderSwitchSpeed.Value * 100} ms";

            ssY += 48;

            _btnApplySwitchSpeed = new Button
            {
                Text = "Apply",
                Location = new System.Drawing.Point(16, ssY),
                Size = new System.Drawing.Size(110, 32)
            };
            _btnApplySwitchSpeed.Click += BtnApplySwitchSpeed_Click;
            Controls.Add(_btnApplySwitchSpeed);
        }

        // Single resize handler for all layout
        int curveStartY = y;
        Resize += (s, e) =>
        {
            if (!_curveEditingEnabled) return; // no curve panels to layout

            int panels = _hasGpuFan ? 2 : 1;
            int panelW = Math.Max(200, (ClientSize.Width - 8 * (panels + 1)) / panels);
            int bottomH = _switchSpeedControl != null ? 200 : 80;
            int curveH = Math.Max(250, ClientSize.Height - curveStartY - bottomH);

            _cpuCurvePanel.SetBounds(8, curveStartY, panelW, curveH);
            if (_hasGpuFan)
                _gpuCurvePanel.SetBounds(8 + (panelW + 4), curveStartY, panelW, curveH);
            if (_linkedStatusPanel != null)
                _linkedStatusPanel.Width = Math.Max(200, ClientSize.Width - 450 - 8);

            int dynBtnY = curveStartY + curveH + 8;
            _btnApply.Location = new System.Drawing.Point(8, dynBtnY);
            _btnRead.Location = new System.Drawing.Point(8, dynBtnY + 36);
            _lblValidation.Location = new System.Drawing.Point(140, dynBtnY + 38);

            if (_chkSwitchSpeed != null)
            {
                int ssY = dynBtnY + 72;
                _chkSwitchSpeed.Location = new System.Drawing.Point(16, ssY);
                if (_sliderSwitchSpeed != null)
                    _sliderSwitchSpeed.Location = new System.Drawing.Point(16, ssY + 28);
                if (_lblSwitchSpeedValue != null)
                    _lblSwitchSpeedValue.Location = new System.Drawing.Point(426, ssY + 36);
                if (_btnApplySwitchSpeed != null)
                    _btnApplySwitchSpeed.Location = new System.Drawing.Point(16, ssY + 76);
            }
        };
        // Trigger initial layout
        OnResize(EventArgs.Empty);

        UiHelpers.FixZOrder(Controls);
    }

    /// <summary>
    /// Set linked fan mode. No-op if controller unavailable.
    /// </summary>
    public void SetLinkedEnabled(bool enabled)
    {
        if (_chkLinkFans == null || _linkedController == null) return;
        if (_chkLinkFans.Checked != enabled)
            _chkLinkFans.Checked = enabled; // triggers OnLinkFansChanged
    }

    /// <summary>
    /// Load curves into panels and write to EC. Called by profile load.
    /// Panels become the source of truth; the EC gets a copy.
    /// </summary>
    public void LoadCurves(FanCurveModel cpuCurve, FanCurveModel gpuCurve, bool enabled)
    {
        if (!_curveEditingEnabled) return;

        _cpuCurvePanel.Curve = cpuCurve;
        if (_hasGpuFan)
            _gpuCurvePanel.Curve = gpuCurve;
        UpdateValidation();

        // Write to EC
        if (_linkedController == null || !_linkedController.IsEnabled)
        {
            _control.ApplyCurves(cpuCurve, gpuCurve, enabled);
        }
        else
        {
            // Linked mode manages its own table writes; just set enable state
            _control.SetCustomFanCurveMode(enabled);
        }
    }

    /// <summary>
    /// Refresh non-curve state from EC (boost, curve enable, switch speed).
    /// Curves are NOT read from EC — panels are the source of truth.
    /// </summary>
    public void RefreshFromEc()
    {
        try
        {
            _chkFanBoost.Checked = _control.ReadFanBoost();
            _chkFanCurve.Checked = _control.ReadCustomFanCurveMode();

            if (_switchSpeedControl != null && _chkSwitchSpeed != null && _sliderSwitchSpeed != null)
            {
                var (enabled, ms) = _switchSpeedControl.ReadState();
                _chkSwitchSpeed.Checked = enabled;
                _sliderSwitchSpeed.Enabled = enabled;
                int ticks = Math.Clamp(ms / 100, 1, 127);
                _sliderSwitchSpeed.Value = ticks;
            }
        }
        catch (Exception ex)
        {
            MessageBox.Show(
                $"Failed to read fan settings:\n{ex.Message}",
                "EC Read Error", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    /// <summary>
    /// Seed curve panels directly from provided models without writing to the
    /// EC. Sibling of <see cref="LoadCurves"/> used for cold-boot fallback
    /// seeding (profile default or generic starter curve) where there is no
    /// valid EC state to read from.
    /// </summary>
    public void SeedCurves(FanCurveModel cpuCurve, FanCurveModel? gpuCurve)
    {
        if (!_curveEditingEnabled) return;
        _cpuCurvePanel.Curve = cpuCurve;
        if (_hasGpuFan && gpuCurve != null)
            _gpuCurvePanel.Curve = gpuCurve;
        UpdateValidation();
    }

    /// <summary>
    /// Seed curve panels from a pre-captured fan snapshot. Used at startup so
    /// initial panel state comes from the single snapshot read instead of
    /// re-reading the EC.
    /// </summary>
    public void SeedCurvesFromSnapshot(FanSnapshot snapshot)
    {
        if (!_curveEditingEnabled) return;
        var cpu = snapshot.BuildCpuCurve();
        if (cpu.Points.Length == 0)
            cpu = FanCurveModel.CreateGenericDefault();
        FanCurveModel? gpu = _hasGpuFan ? snapshot.BuildGpuCurve() : null;
        if (gpu != null && gpu.Points.Length == 0)
            gpu = FanCurveModel.CreateGenericDefault();
        SeedCurves(cpu, gpu);
    }

    /// <summary>Set the custom fan curve checkbox without triggering an EC write.</summary>
    public void SetCurveEnableCheckbox(bool enabled)
    {
        _chkFanCurve.Checked = enabled;
    }

    /// <summary>
    /// Read curves from EC into panels. Only called by "Read from EC" button
    /// and on initial startup when no profile is auto-loaded.
    /// </summary>
    public void ReadCurvesFromEc()
    {
        if (!_curveEditingEnabled) return;
        try
        {
            _cpuCurvePanel.Curve = _control.ReadCpuCurve();
            if (_hasGpuFan)
                _gpuCurvePanel.Curve = _control.ReadGpuCurve();
            UpdateValidation();
        }
        catch (Exception ex)
        {
            MessageBox.Show(
                $"Failed to read fan curves:\n{ex.Message}",
                "EC Read Error", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private void OnCurveChanged()
    {
        UpdateValidation();
    }

    private void UpdateValidation()
    {
        var errors = new List<string>();
        if (_cpuCurvePanel.Curve != null)
        {
            var cpuErrors = _cpuCurvePanel.Curve.Validate();
            foreach (var err in cpuErrors)
                errors.Add($"CPU: {err}");
        }
        if (_gpuCurvePanel.Curve != null)
        {
            var gpuErrors = _gpuCurvePanel.Curve.Validate();
            foreach (var err in gpuErrors)
                errors.Add($"GPU: {err}");
        }

        if (errors.Count == 0)
        {
            _lblValidation.ForeColor = System.Drawing.Color.Green;
            _lblValidation.Text = "Valid";
        }
        else
        {
            _lblValidation.ForeColor = System.Drawing.Color.Red;
            _lblValidation.Text = string.Join("\n", errors);
        }
    }

    private void SetLinkedControlsEnabled(bool linked)
    {
        _btnApply.Enabled = !linked;
        _btnRead.Enabled = !linked;
        _chkFanCurve.Enabled = !linked;
    }

    private void OnLinkFansChanged(object? sender, EventArgs e)
    {
        if (_linkedController == null || _chkLinkFans == null) return;

        if (_chkLinkFans.Checked)
        {
            if (_cpuCurvePanel.Curve == null || _gpuCurvePanel.Curve == null)
            {
                MessageBox.Show("Load fan curves first (Read from EC).",
                    "Link Fans", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                _chkLinkFans.Checked = false;
                return;
            }

            _chkFanCurve.Checked = true;
            SetLinkedControlsEnabled(true);
            if (_linkedStatusPanel != null) _linkedStatusPanel.Visible = true;

            try
            {
                _linkedController.Enable();
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to enable linked fans:\n{ex.Message}",
                    "EC Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                _chkLinkFans.Checked = false;
                SetLinkedControlsEnabled(false);
                if (_linkedStatusPanel != null) _linkedStatusPanel.Visible = false;
                return;
            }
        }
        else
        {
            try
            {
                _linkedController.Disable();
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to restore fan curves:\n{ex.Message}",
                    "EC Error", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
            SetLinkedControlsEnabled(false);
            if (_linkedStatusPanel != null) _linkedStatusPanel.Visible = false;
        }
    }

    private void PaintLinkedStatus(object? sender, PaintEventArgs e)
    {
        if (_linkedController == null || !_linkedController.IsEnabled) return;
        var g = e.Graphics;
        var panel = (Panel)sender!;
        int w = panel.ClientSize.Width;
        int h = panel.ClientSize.Height;

        // Duty fill bar
        g.Clear(System.Drawing.Color.FromArgb(245, 245, 245));
        float fillW = w * _linkedController.LinkedDuty / 100f;
        using var fillBrush = new System.Drawing.SolidBrush(
            System.Drawing.Color.FromArgb(80, 65, 105, 225));
        g.FillRectangle(fillBrush, 0, 0, fillW, h);

        // Status text — draw segments to mix font sizes
        var font = UiHelpers.FontSmall;
        var fontRamp = UiHelpers.FontTiny;
        var black = System.Drawing.Brushes.Black;
        var gray = System.Drawing.Brushes.DimGray;
        float mainY = (h - g.MeasureString("X", font).Height) / 2f;
        float rampY = (h - g.MeasureString("X", fontRamp).Height) / 2f;
        float x = 4;

        string cpuRamp = _linkedController.CpuRampTemp > 0 ? $"(safety ramp {_linkedController.CpuRampTemp}\u00B0C)" : "";
        string gpuRamp = _linkedController.GpuRampTemp > 0 ? $"(safety ramp {_linkedController.GpuRampTemp}\u00B0C)" : "";

        void DrawMain(string s) { g.DrawString(s, font, black, x, mainY); x += g.MeasureString(s, font).Width - 4; }
        void DrawRamp(string s) { g.DrawString(s, fontRamp, gray, x, rampY); x += g.MeasureString(s, fontRamp).Width - 2; }

        DrawMain($"{_linkedController.LinkedDuty}%  \u2502  CPU {_linkedController.CpuTemp}\u00B0\u2192{_linkedController.CpuDuty}% ");
        if (cpuRamp.Length > 0) DrawRamp(cpuRamp);
        DrawMain($"  \u2502  GPU {_linkedController.GpuTemp}\u00B0\u2192{_linkedController.GpuDuty}% ");
        if (gpuRamp.Length > 0) DrawRamp(gpuRamp);
    }

    private void BtnApplyBoost_Click(object? sender, EventArgs e)
    {
        try
        {
            _control.SetFanBoost(_chkFanBoost.Checked);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to set fan boost:\n{ex.Message}",
                "EC Write Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void BtnApply_Click(object? sender, EventArgs e)
    {
        if (_cpuCurvePanel.Curve == null) return;

        try
        {
            // Validate both curves before writing anything
            var errors = _cpuCurvePanel.Curve.Validate();
            if (errors.Count > 0)
            {
                MessageBox.Show("CPU curve has validation errors:\n" + string.Join("\n", errors),
                    "Validation Error", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (_gpuCurvePanel.Curve != null)
            {
                var gpuErrors = _gpuCurvePanel.Curve.Validate();
                if (gpuErrors.Count > 0)
                {
                    MessageBox.Show("GPU curve has validation errors:\n" + string.Join("\n", gpuErrors),
                        "Validation Error", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return;
                }
            }

            // Write both fan tables, then set latch + master enable
            _control.ApplyCurves(_cpuCurvePanel.Curve, _gpuCurvePanel.Curve, _chkFanCurve.Checked);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to apply fan settings:\n{ex.Message}",
                "EC Write Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void BtnApplySwitchSpeed_Click(object? sender, EventArgs e)
    {
        if (_switchSpeedControl == null || _chkSwitchSpeed == null || _sliderSwitchSpeed == null) return;
        try
        {
            _switchSpeedControl.Apply(_chkSwitchSpeed.Checked, _sliderSwitchSpeed.Value * 100);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to set fan switch speed:\n{ex.Message}",
                "EC Write Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void BtnRead_Click(object? sender, EventArgs e)
    {
        ReadCurvesFromEc();
        RefreshFromEc();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
            _linkedController?.Dispose();
        base.Dispose(disposing);
    }
}
