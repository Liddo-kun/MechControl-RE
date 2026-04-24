using MechControl.Driver;
using MechControl.Features;
using MechControl.Monitoring;
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
    private readonly LinkedFanStatusBar? _linkedStatusBar;
    private readonly FanCurveDebugSection? _debugSection;

    // Fixed height for all curve graphs (edit panels + debug panels).
    private const int CurvePanelHeight = 320;
    private readonly Label _lblValidation;
    private readonly CheckBox? _chkSwitchSpeed;
    private readonly TrackBar? _sliderSwitchSpeed;
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

        var stack = UiHelpers.BuildSectionStack();

        // ── Fan Boost section ────────────────────────────────────
        var grpBoost = UiHelpers.BuildSection("Fan Boost", fitContent: true);
        _chkFanBoost = new CheckBox
        {
            Text = "Enable Fan Boost",
            AutoSize = true,
            Font = UiHelpers.FontBold,
            Anchor = AnchorStyles.Left,
            Margin = new Padding(0, 6, 16, 0),
        };
        var btnApplyBoost = UiHelpers.BuildApplyButton(
            "Apply Boost", BtnApplyBoost_Click, margin: new Padding(0));
        btnApplyBoost.Anchor = AnchorStyles.Left;

        var boostLayout = new FlowLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            FlowDirection = FlowDirection.LeftToRight,
        };
        boostLayout.Controls.Add(_chkFanBoost);
        boostLayout.Controls.Add(btnApplyBoost);
        grpBoost.Controls.Add(boostLayout);
        stack.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        stack.Controls.Add(grpBoost);

        // ── Fan Switch Speed section (conditional) ───────────────
        if (_switchSpeedControl != null)
        {
            var grpSwitch = UiHelpers.BuildSection("Fan Switch Speed");
            var switchStack = new TableLayoutPanel
            {
                Dock = DockStyle.Top,
                AutoSize = true,
                AutoSizeMode = AutoSizeMode.GrowAndShrink,
                ColumnCount = 1,
            };
            switchStack.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));

            _chkSwitchSpeed = new CheckBox
            {
                Text = "Enable Fan Switch Speed",
                AutoSize = true,
                Font = UiHelpers.FontBold,
                Margin = new Padding(0, 4, 0, 0),
            };
            switchStack.Controls.Add(_chkSwitchSpeed);

            var ssSliderRow = UiHelpers.BuildSliderTable();
            var ssRow = UiHelpers.BuildSliderRow(
                "Switch Speed", 1, 127, "", initial: 3,
                formatter: v => $"{v * 100} ms");
            _sliderSwitchSpeed = ssRow.Slider;
            _sliderSwitchSpeed.Enabled = false;
            UiHelpers.AddSliderRow(ssSliderRow, ssRow);
            switchStack.Controls.Add(ssSliderRow);

            _chkSwitchSpeed.CheckedChanged += (s, e) =>
                _sliderSwitchSpeed.Enabled = _chkSwitchSpeed.Checked;

            _btnApplySwitchSpeed = UiHelpers.BuildApplyButton("Apply", BtnApplySwitchSpeed_Click);
            switchStack.Controls.Add(_btnApplySwitchSpeed);

            grpSwitch.Controls.Add(switchStack);
            stack.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            stack.Controls.Add(grpSwitch);
        }

        // ── Custom Fan Curve section ─────────────────────────────
        // Section AutoSizes to fit: header (AutoSize) + curves (fixed 320px) + footer (AutoSize).
        var grpCurve = UiHelpers.BuildSection("Custom Fan Curve");

        int curvesRowHeight = _curveEditingEnabled
            ? CurvePanelHeight + UiMetrics.RowGap * 2
            : 100;

        var curveInner = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 1,
            RowCount = 3,
        };
        curveInner.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));
        curveInner.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        curveInner.RowStyles.Add(new RowStyle(SizeType.Absolute, curvesRowHeight));
        curveInner.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        // Header row: [Custom Fan Curve chk] [Link Fans chk] [status panel fill]
        var headerLayout = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 3,
            Margin = new Padding(0),
        };
        headerLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        headerLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        headerLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));

        _chkFanCurve = new CheckBox
        {
            Text = "Custom Fan Curve",
            AutoSize = true,
            Font = UiHelpers.FontBold,
            Anchor = AnchorStyles.Left,
            Margin = new Padding(0, 4, 24, 0),
        };
        headerLayout.Controls.Add(_chkFanCurve);

        // Curves row
        var curvesLayout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = _hasGpuFan && _curveEditingEnabled ? 2 : 1,
            RowCount = 1,
            Margin = new Padding(0, UiMetrics.RowGap, 0, UiMetrics.RowGap),
        };
        if (_hasGpuFan && _curveEditingEnabled)
        {
            curvesLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50f));
            curvesLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50f));
        }
        else
        {
            curvesLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));
        }
        curvesLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100f));

        _cpuCurvePanel = new FanCurvePanel
        {
            Title = "CPU Fan Curve",
            DutyBaseAddress = profile.FanTable.CpuDutyAddr,
            Dock = DockStyle.Fill,
            Margin = new Padding(0, 0, 4, 0),
            Visible = _curveEditingEnabled,
        };
        _cpuCurvePanel.CurveChanged += OnCurveChanged;

        _gpuCurvePanel = new FanCurvePanel
        {
            Title = "GPU Fan Curve",
            DutyBaseAddress = profile.FanTable.GpuDutyAddr,
            Dock = DockStyle.Fill,
            Margin = new Padding(4, 0, 0, 0),
            Visible = _hasGpuFan && _curveEditingEnabled,
        };
        _gpuCurvePanel.CurveChanged += OnCurveChanged;

        curvesLayout.Controls.Add(_cpuCurvePanel);
        if (_hasGpuFan && _curveEditingEnabled)
            curvesLayout.Controls.Add(_gpuCurvePanel);

        // Footer row: [Apply] [Read] [Validation]
        var footerLayout = new FlowLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            FlowDirection = FlowDirection.LeftToRight,
        };

        var (applyBtn, readBtn) = UiHelpers.BuildApplyReadRow(BtnApply_Click, BtnRead_Click);
        _btnApply = applyBtn;
        _btnRead = readBtn!;
        _btnApply.Visible = _curveEditingEnabled;
        _btnRead.Visible = _curveEditingEnabled;
        _btnRead.Margin = new Padding(0, UiMetrics.RowGap, UiMetrics.ButtonGap, 0);

        _lblValidation = new Label
        {
            AutoSize = true,
            Font = UiHelpers.FontNormal,
            ForeColor = UiColors.StatusOk,
            Text = "",
            Visible = _curveEditingEnabled,
            Margin = new Padding(0, 14, 0, 0),
        };

        footerLayout.Controls.Add(_btnApply);
        footerLayout.Controls.Add(_btnRead);
        footerLayout.Controls.Add(_lblValidation);

        curveInner.Controls.Add(headerLayout, 0, 0);
        curveInner.Controls.Add(curvesLayout, 0, 1);
        curveInner.Controls.Add(footerLayout, 0, 2);

        if (!_curveEditingEnabled)
        {
            // Replace curves row with a format-unsupported warning
            var lblFormatWarn = new Label
            {
                Text = $"Fan curve editing is not available (format: {profile.FanTable.Format}). Only fan boost and curve enable are supported.",
                AutoSize = true,
                ForeColor = UiColors.StatusWarn,
                Font = UiHelpers.FontBold,
                Dock = DockStyle.Fill,
                Margin = new Padding(0, UiMetrics.RowGap, 0, 0),
            };
            curvesLayout.Controls.Clear();
            curvesLayout.Controls.Add(lblFormatWarn);
        }

        // Link Fans (conditional)
        if (_hasGpuFan && _curveEditingEnabled
            && profile.Registers.ContainsKey("cpuTemp") && profile.Registers.ContainsKey("gpuTemp"))
        {
            _linkedController = new LinkedFanController(device, control, profile,
                () => _cpuCurvePanel.Curve, () => _gpuCurvePanel.Curve);

            _chkLinkFans = new CheckBox
            {
                Text = "Link Fans",
                AutoSize = true,
                Font = UiHelpers.FontBold,
                Anchor = AnchorStyles.Left,
                Margin = new Padding(0, 4, 24, 0),
            };
            _chkLinkFans.CheckedChanged += OnLinkFansChanged;
            headerLayout.Controls.Add(_chkLinkFans);

            _linkedStatusBar = new LinkedFanStatusBar(_linkedController)
            {
                Dock = DockStyle.Fill,
                Margin = new Padding(0, 2, 0, 2),
            };
            headerLayout.Controls.Add(_linkedStatusBar);
        }

        grpCurve.Controls.Add(curveInner);
        stack.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        stack.Controls.Add(grpCurve);

        // ── Debug section: curves actually written to EC ─────────
        // Always mounted, starts hidden. Toggled by Link Fans.
        if (_curveEditingEnabled)
        {
            _debugSection = new FanCurveDebugSection(control, profile, _hasGpuFan, CurvePanelHeight)
            {
                Visible = false,
            };
            stack.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            stack.Controls.Add(_debugSection);
        }

        Controls.Add(stack);
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
        UiHelpers.TryUi(() =>
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
        }, "read fan settings", "EC Read Error");
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
    /// Route live EC telemetry into the curve panels so they can draw a
    /// current-temperature indicator. Cheap when the tab isn't active — each
    /// panel's setter skips Invalidate when it isn't Visible.
    /// </summary>
    public void OnMonitorUpdated(MonitorData data)
    {
        int? cpu = data.CpuTemp > 0 ? data.CpuTemp : null;
        int? gpu = data.GpuTemp > 0 ? data.GpuTemp : null;
        _cpuCurvePanel.CurrentTempC = cpu;
        if (_hasGpuFan)
            _gpuCurvePanel.CurrentTempC = gpu;
        _debugSection?.OnMonitorUpdated(cpu, gpu);
    }

    /// <summary>
    /// Read curves from EC into panels. Only called by "Read from EC" button
    /// and on initial startup when no profile is auto-loaded.
    /// </summary>
    public void ReadCurvesFromEc()
    {
        if (!_curveEditingEnabled) return;
        UiHelpers.TryUi(() =>
        {
            _cpuCurvePanel.Curve = _control.ReadCpuCurve();
            if (_hasGpuFan)
                _gpuCurvePanel.Curve = _control.ReadGpuCurve();
            UpdateValidation();
        }, "read fan curves", "EC Read Error");
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
            _lblValidation.ForeColor = UiColors.StatusOk;
            _lblValidation.Text = "Valid";
        }
        else
        {
            _lblValidation.ForeColor = UiColors.StatusError;
            _lblValidation.Text = string.Join("\n", errors);
        }
    }

    private void SetLinkedControlsEnabled(bool linked)
    {
        _btnApply.Enabled = !linked;
        _btnRead.Enabled = !linked;
        _chkFanCurve.Enabled = !linked;

        // In linked mode LinkedFanController samples the panel curves with
        // linear interpolation to compute a per-tick duty, so diagonal lines
        // are accurate. In stock mode the EC evaluates the table as a step
        // function, so show the stepwise rendering instead.
        _cpuCurvePanel.InterpolatedRendering = linked;
        _gpuCurvePanel.InterpolatedRendering = linked;
        _cpuCurvePanel.Invalidate();
        _gpuCurvePanel.Invalidate();
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
            if (_linkedStatusBar != null) _linkedStatusBar.Visible = true;
            if (_debugSection != null) _debugSection.Visible = true;

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
                if (_linkedStatusBar != null) _linkedStatusBar.Visible = false;
                if (_debugSection != null) _debugSection.Visible = false;
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
            if (_linkedStatusBar != null) _linkedStatusBar.Visible = false;
            if (_debugSection != null) _debugSection.Visible = false;
        }
    }

    private void BtnApplyBoost_Click(object? sender, EventArgs e) =>
        UiHelpers.TryUi(() => _control.SetFanBoost(_chkFanBoost.Checked),
            "set fan boost", "EC Write Error");

    private void BtnApply_Click(object? sender, EventArgs e)
    {
        if (_cpuCurvePanel.Curve == null) return;

        UiHelpers.TryUi(() =>
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
        }, "apply fan settings", "EC Write Error");
    }

    private void BtnApplySwitchSpeed_Click(object? sender, EventArgs e)
    {
        if (_switchSpeedControl == null || _chkSwitchSpeed == null || _sliderSwitchSpeed == null) return;
        UiHelpers.TryUi(() =>
            _switchSpeedControl.Apply(_chkSwitchSpeed.Checked, _sliderSwitchSpeed.Value * 100),
            "set fan switch speed", "EC Write Error");
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
