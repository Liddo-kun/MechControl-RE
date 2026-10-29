using MechControl.Features;
using MechControl.Profile;

namespace MechControl.UI;

class PowerTab : UserControl
{
    private readonly CpuPowerControl? _cpuControl;
    private readonly GpuPowerControl? _gpuControl;

    private readonly TrackBar? _sliderPL1;
    private readonly TrackBar? _sliderPL2;
    private readonly TrackBar? _sliderPL4;
    private readonly TrackBar? _sliderTemp;
    private readonly Label? _lblPL1;
    private readonly Label? _lblPL2;
    private readonly Label? _lblPL4;
    private readonly Label? _lblTemp;

    private readonly TrackBar? _sliderTgp;
    private readonly Label? _lblTgp;
    private readonly CheckBox? _chkTgpUnlock;
    private readonly UiHelpers.SliderRow? _rowNvvdd;
    private readonly UiHelpers.SliderRow? _rowMsvdd;
    private readonly UiHelpers.SliderRow? _rowBoardInput;
    private readonly Label? _lblDriverLimits;
    private bool _refreshing;
    private readonly CheckBox? _chkDynamicBoost;
    private readonly TrackBar? _sliderDb;
    private readonly Label? _lblDb;

    private readonly ToolTip _tooltip = new();
    private readonly int _addrPL1;
    private readonly int _addrPL2;
    private readonly int _addrPL4;
    private readonly bool _pl4Doubled;
    private readonly int _addrTempWrite;
    private readonly int _addrTgp;
    private readonly int _addrDbMaxTgp;
    private readonly int _tjMax;
    private readonly bool _absoluteTemp;
    private readonly int _tgpOffset;

    private readonly bool _cpuEnabled;
    private readonly bool _gpuEnabled;

    public PowerTab(CpuPowerControl? cpuControl, GpuPowerControl? gpuControl,
        MachineProfile profile, StartupValidator? validator = null)
    {
        _cpuControl = cpuControl;
        _gpuControl = gpuControl;
        _cpuEnabled = validator?.IsFeatureEnabled("cpuPower") ?? true;
        _gpuEnabled = validator?.IsFeatureEnabled("gpuPower") ?? true;

        _addrPL1 = profile.GetRegister("PL1");
        _addrPL2 = profile.GetRegister("PL2");
        _addrPL4 = profile.GetRegister("PL4");
        _pl4Doubled = profile.Pl4Doubled;
        _addrTempWrite = profile.GetRegister("cpuTempLimitWrite");
        _addrTgp = profile.GetRegister("tgp");
        _addrDbMaxTgp = profile.GetRegister("dbMaxTgp");

        _tjMax = profile.TjMax;
        _absoluteTemp = profile.CpuTempEncoding == "direct";
        _tgpOffset = profile.TgpOffset;

        var tempLimit = profile.Limits.TryGetValue("cpuTempLimit", out var tl) ? tl : UniversalDefaults.ConservativeLimits["cpuTempLimit"];
        var tgpLimit = profile.Limits.TryGetValue("TGP", out var tgl) ? tgl : UniversalDefaults.ConservativeLimits["TGP"];

        Dock = DockStyle.Fill;

        var stack = UiHelpers.BuildSectionStack();

        // === CPU Power ===
        var grpCpu = UiHelpers.BuildSection("CPU Power");
        if (_cpuEnabled)
        {
            var pl1Limit = profile.Limits.TryGetValue("PL1", out var p1) ? p1 : UniversalDefaults.ConservativeLimits["PL1"];
            var pl2Limit = profile.Limits.TryGetValue("PL2", out var p2) ? p2 : UniversalDefaults.ConservativeLimits["PL2"];
            var pl4Limit = profile.Limits.TryGetValue("PL4", out var p4) ? p4 : UniversalDefaults.ConservativeLimits["PL4"];

            var cpuLayout = UiHelpers.BuildSliderTable();

            var pl1 = UiHelpers.BuildSliderRow("PL1 (Sustained)", pl1Limit.Min, pl1Limit.Max, "W");
            var pl2 = UiHelpers.BuildSliderRow("PL2 (Burst)", pl2Limit.Min, pl2Limit.Max, "W");
            var pl4 = UiHelpers.BuildSliderRow("PL4 (Peak)", pl4Limit.Min, pl4Limit.Max, "W");
            var tmp = UiHelpers.BuildSliderRow("CPU Temp Limit", tempLimit.Min, tempLimit.Max, "\u00B0C");

            UiHelpers.AddSliderRow(cpuLayout, pl1);
            UiHelpers.AddSliderRow(cpuLayout, pl2);
            UiHelpers.AddSliderRow(cpuLayout, pl4);
            UiHelpers.AddSliderRow(cpuLayout, tmp);

            _sliderPL1 = pl1.Slider; _lblPL1 = pl1.Value;
            _sliderPL2 = pl2.Slider; _lblPL2 = pl2.Value;
            _sliderPL4 = pl4.Slider; _lblPL4 = pl4.Value;
            _sliderTemp = tmp.Slider; _lblTemp = tmp.Value;

            grpCpu.Controls.Add(cpuLayout);
        }
        else
        {
            grpCpu.Controls.Add(MakeDisabledLabel(
                "CPU power controls disabled — register validation failed. See log for details."));
        }
        stack.Controls.Add(grpCpu);

        // === GPU Power ===
        var grpGpu = UiHelpers.BuildSection("GPU Power");
        if (_gpuEnabled)
        {
            var dbLimit = profile.Limits.TryGetValue("dynamicBoost", out var db) ? db : UniversalDefaults.ConservativeLimits["dynamicBoost"];

            var gpuLayout = UiHelpers.BuildSliderTable();

            var tgp = UiHelpers.BuildSliderRow("GPU TGP (before Dynamic Boost)", _gpuControl?.MinTgpWatts ?? tgpLimit.Min, _gpuControl?.MaxTgpWatts ?? tgpLimit.Max, "W");
            UiHelpers.AddSliderRow(gpuLayout, tgp);
            _sliderTgp = tgp.Slider; _lblTgp = tgp.Value;

            if (_gpuControl?.CanUnlockTgp == true)
            {
                _chkTgpUnlock = new CheckBox
                {
                    Text = $"Unlock GPU power / current limits (TGP {_gpuControl.UnlockedMinTgpWatts}-{_gpuControl.UnlockedMaxTgpWatts} W)",
                    AutoSize = true,
                    Checked = _gpuControl.TgpUnlocked,
                    Margin = new Padding(0, UiMetrics.RowGap, 0, 0),
                };
                _chkTgpUnlock.CheckedChanged += TgpUnlock_CheckedChanged;
                gpuLayout.Controls.Add(_chkTgpUnlock);
                gpuLayout.SetColumnSpan(_chkTgpUnlock, 3);
            }
            // Nothing to explain on models without an NVIDIA GPU.
            else if (_gpuControl != null && (_gpuControl.PowerOverride != null || _gpuControl.OpenPowerOverride != null
                || _gpuControl.PowerOverrideUnavailableReason != null))
            {
                var unavailable = MakeDisabledLabel(_gpuControl.PowerOverride == null && _gpuControl.OpenPowerOverride == null
                    ? _gpuControl.PowerOverrideUnavailableReason!
                    : "TGP unlock unavailable: this model's EC range cannot be extended.");
                unavailable.ForeColor = UiColors.Hint;
                unavailable.MaximumSize = new Size(600, 0);
                gpuLayout.Controls.Add(unavailable);
                gpuLayout.SetColumnSpan(unavailable, 3);
            }

            _chkDynamicBoost = new CheckBox
            {
                Text = "Enable Dynamic Boost",
                AutoSize = true,
                Font = UiHelpers.FontBold,
                Margin = new Padding(0, UiMetrics.RowGap, 0, 0),
            };
            gpuLayout.Controls.Add(_chkDynamicBoost);
            gpuLayout.SetColumnSpan(_chkDynamicBoost, 3);

            var dbRow = UiHelpers.BuildSliderRow("Dynamic Boost Max", dbLimit.Min, dbLimit.Max, "W");
            UiHelpers.AddSliderRow(gpuLayout, dbRow);
            _sliderDb = dbRow.Slider; _lblDb = dbRow.Value;
            _sliderDb.Enabled = false;

            _chkDynamicBoost.CheckedChanged += (s, e) =>
            {
                if (_sliderDb != null)
                    _sliderDb.Enabled = _chkDynamicBoost.Checked;
            };

            if (_gpuControl != null && (_gpuControl.CanUnlockTgp || _gpuControl.PowerOverride != null))
            {
                _rowNvvdd = AddCurrentSlider(gpuLayout, "NVVDD / Core current limit", 1000);
                _rowMsvdd = AddCurrentSlider(gpuLayout, "MSVDD / Fabric current limit", 1000);
                // Board input is ~10 A, so a whole amp would be a ~10% (~20 W) step.
                _rowBoardInput = AddCurrentSlider(gpuLayout, "Board input current limit", 100);
                _lblDriverLimits = new Label { AutoSize = true, ForeColor = UiColors.Hint, MaximumSize = new Size(600, 0) };
                gpuLayout.Controls.Add(_lblDriverLimits);
                gpuLayout.SetColumnSpan(_lblDriverLimits, 3);
                RefreshDriverControls();
            }

            grpGpu.Controls.Add(gpuLayout);
        }
        else
        {
            grpGpu.Controls.Add(MakeDisabledLabel(
                "GPU power controls disabled — register validation failed. See log for details."));
        }
        stack.Controls.Add(grpGpu);

        // === Global Apply / Read (atomic commit semantics) ===
        var (btnApply, btnRead) = UiHelpers.BuildApplyReadRow(BtnApply_Click, BtnRead_Click);
        btnApply.Enabled = _cpuEnabled || _gpuEnabled;
        btnRead!.Enabled = _cpuEnabled || _gpuEnabled;

        var buttonRow = new FlowLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            FlowDirection = FlowDirection.LeftToRight,
            Margin = new Padding(0, 0, 0, UiMetrics.SectionGap),
        };
        buttonRow.Controls.Add(btnApply);
        buttonRow.Controls.Add(btnRead);
        stack.Controls.Add(buttonRow);

        Controls.Add(stack);

        // Tooltips (PL4 doubled-tooltip logic kept here, not in formatter)
        if (_sliderPL1 != null)
            _sliderPL1.ValueChanged += (s, e) =>
                _tooltip.SetToolTip(_sliderPL1, $"Register 0x{_addrPL1:X3} | Encoding: direct | Raw: {_sliderPL1.Value}");
        if (_sliderPL2 != null)
            _sliderPL2.ValueChanged += (s, e) =>
                _tooltip.SetToolTip(_sliderPL2, $"Register 0x{_addrPL2:X3} | Encoding: direct | Raw: {_sliderPL2.Value}");
        if (_sliderPL4 != null)
            _sliderPL4.ValueChanged += (s, e) =>
            {
                int raw = _pl4Doubled ? _sliderPL4.Value / 2 : _sliderPL4.Value;
                string encoding = _pl4Doubled ? "double" : "direct";
                _tooltip.SetToolTip(_sliderPL4, $"Register 0x{_addrPL4:X3} | Encoding: {encoding} | Raw: {raw}");
            };
        if (_sliderTemp != null)
            _sliderTemp.ValueChanged += (s, e) =>
                _tooltip.SetToolTip(_sliderTemp, _absoluteTemp
                    ? $"Register 0x{_addrTempWrite:X3} | Encoding: direct | Raw: 0x{RegisterEncoding.EncodeCpuTempLimitDirect(_sliderTemp.Value):X2}"
                    : $"Register 0x{_addrTempWrite:X3} | Encoding: inverted_offset | Raw: 0x{RegisterEncoding.EncodeCpuTempLimit(_sliderTemp.Value, _tjMax):X2}");
        if (_sliderTgp != null)
            _sliderTgp.ValueChanged += (s, e) =>
                _tooltip.SetToolTip(_sliderTgp, _sliderTgp.Value < tgpLimit.Min
                    ? $"Below the EC minimum: NVIDIA driver request only (EC stays at {tgpLimit.Min} W)"
                    : $"Register 0x{_addrTgp:X3} | Encoding: offset +{_tgpOffset} | Raw: {_sliderTgp.Value - _tgpOffset}");
        if (_sliderDb != null)
            _sliderDb.ValueChanged += (s, e) =>
                _tooltip.SetToolTip(_sliderDb, $"Register 0x{_addrDbMaxTgp:X3} | Encoding: direct | Raw: {_sliderDb.Value}");
    }

    private static Label MakeDisabledLabel(string text) => new()
    {
        Text = text,
        AutoSize = true,
        ForeColor = UiColors.StatusError,
        Dock = DockStyle.Top,
    };

    public void RefreshFromEc()
    {
        if (_cpuEnabled && _cpuControl != null && _sliderPL1 != null)
        {
            UiHelpers.TryUi(() =>
            {
                var cpu = _cpuControl.ReadCpuPower();
                _sliderPL1.Value = Math.Clamp(cpu.PL1Watts, _sliderPL1.Minimum, _sliderPL1.Maximum);
                _sliderPL2!.Value = Math.Clamp(cpu.PL2Watts, _sliderPL2.Minimum, _sliderPL2.Maximum);
                _sliderPL4!.Value = Math.Clamp(cpu.PL4Watts, _sliderPL4.Minimum, _sliderPL4.Maximum);
                _sliderTemp!.Value = Math.Clamp(cpu.TempLimitC, _sliderTemp.Minimum, _sliderTemp.Maximum);
            }, "read CPU power values", "EC Read Error");
        }

        RefreshGpuFromHardware();
    }

    private void RefreshGpuFromHardware()
    {
        if (_gpuEnabled && _gpuControl != null && _sliderTgp != null)
        {
            UiHelpers.TryUi(() =>
            {
                var gpu = _gpuControl.ReadGpuPower();
                RefreshDriverControls();
                _sliderTgp.Value = Math.Clamp(gpu.TgpWatts, _sliderTgp.Minimum, _sliderTgp.Maximum);
                _chkDynamicBoost!.Checked = gpu.DynamicBoostEnabled;
                _sliderDb!.Value = Math.Clamp(gpu.DbWatts, _sliderDb.Minimum, _sliderDb.Maximum);
                _sliderDb.Enabled = gpu.DynamicBoostEnabled;
            }, "read GPU power values", "EC Read Error");
        }
    }

    private void BtnApply_Click(object? sender, EventArgs e)
    {
        var failsafe = _gpuControl?.Failsafe ?? _cpuControl?.Failsafe;
        bool attempted = false;
        UiHelpers.TryUi(() =>
        {
            var plan = _gpuEnabled && _sliderTgp != null ? _gpuControl?.PlanProfile(ReadGpuSettings(), automatic: false) : null;
            if (plan != null && !UiHelpers.ConfirmGpuPlan(_gpuControl!, plan)) return;
            attempted = true;
            void Apply()
            {
                if (plan != null) _gpuControl!.ConfigureProfile(plan);
                if (_cpuEnabled && _cpuControl != null && _sliderPL1 != null)
                    _cpuControl.ApplyCpuPower(_sliderPL1.Value, _sliderPL2!.Value,
                        _sliderPL4!.Value, _sliderTemp!.Value);

                if (plan != null)
                    _gpuControl!.ApplyGpuPower(plan.Settings.Tgp,
                        plan.Settings.DynamicBoostEnabled, plan.Settings.DynamicBoostWatts);
            }
            if (failsafe != null) failsafe.ApplyExplicit(Apply);
            else Apply();
        }, "apply power settings", "EC Write Error");
        if (attempted || failsafe?.IsTripped == true) RefreshFromEc();
    }

    private void TgpUnlock_CheckedChanged(object? sender, EventArgs e)
    {
        if (_refreshing) return;
        var gpu = _gpuControl!;
        bool unlocked = _chkTgpUnlock!.Checked;
        if (unlocked == gpu.TgpUnlocked) return;
        UiHelpers.TryUi(() =>
        {
            var driver = gpu.EnsurePowerOverride();
            if (UiHelpers.ConfirmGpuUnlock(unlocked, driver.StockPowerLimitWatts)) gpu.SetUnlock(unlocked);
        }, "change NVIDIA BIOS limits");
        RefreshGpuFromHardware();
    }

    // Positions are steps of stepMilliamps. An unmoved slider keeps the exact detected request,
    // which some BIOSes define in fractional amps, and writes nothing.
    private UiHelpers.SliderRow AddCurrentSlider(TableLayoutPanel layout, string name, int stepMilliamps)
    {
        UiHelpers.SliderRow? row = null;
        row = UiHelpers.BuildSliderRow(name, 1, 2, "A", formatter: v => FormatCurrent(row?.Slider, v));
        row.Slider.Tag = new CurrentSlider(stepMilliamps);
        UiHelpers.AddSliderRow(layout, row);
        _tooltip.SetToolTip(row.Slider, $"Applies live within the active ceiling. The unlock allows up to {UniversalDefaults.GpuCurrentUnlockPercent}% of the stock current; raising an older ceiling restarts the GPU.");
        return row;
    }

    // A current slider's Tag. Unmoved is the position SetCurrent chose for the detected request.
    private sealed class CurrentSlider(int stepMilliamps)
    {
        public int StepMilliamps { get; } = stepMilliamps;
        public GpuCurrentLimit? Limit { get; set; }
        public int Unmoved { get; set; }
    }

    private static string FormatCurrent(TrackBar? slider, int position)
    {
        if (slider?.Tag is not CurrentSlider { Limit: { } limit } current) return "--";
        int milliamps = position == current.Unmoved ? limit.CurrentMilliamps : position * current.StepMilliamps;
        return $"{milliamps / 1000.0:0.###} A";
    }

    private static int? SelectedMilliamps(UiHelpers.SliderRow? row) =>
        row?.Slider.Tag is CurrentSlider { Limit: not null } current && row.Slider.Value != current.Unmoved
            ? row.Slider.Value * current.StepMilliamps : null;

    private static void SetCurrent(UiHelpers.SliderRow row, GpuCurrentLimit? limit, bool unlocked)
    {
        var current = (CurrentSlider)row.Slider.Tag!;
        int step = current.StepMilliamps;
        current.Limit = limit;
        row.Slider.Enabled = limit != null;
        if (limit != null)
        {
            // Never below 1 A, whatever the step.
            int min = (Math.Max(1000, limit.MinimumMilliamps) + step - 1) / step;
            int max = Math.Max(min, limit.AllowedMaximumMilliamps(unlocked) / step);
            row.Slider.SetRange(min, max);
            // Clamped, the detected request can sit one step off its rounded position
            // (11.053 A at an 11.0 A maximum); that position still means "unmoved".
            row.Slider.Value = current.Unmoved = Math.Clamp((int)Math.Round(limit.CurrentMilliamps / (double)step), min, max);
            row.Slider.TickFrequency = row.Slider.LargeChange = Math.Max(1, (max - min) / 10);
        }
        row.Value.Text = FormatCurrent(row.Slider, row.Slider.Value);
    }

    private void RefreshDriverControls()
    {
        var gpu = _gpuControl!;
        _refreshing = true;
        try
        {
            if (_chkTgpUnlock != null) { _chkTgpUnlock.Checked = gpu.TgpUnlocked; _chkTgpUnlock.Enabled = gpu.CanUnlockTgp; }
            _sliderTgp!.SetRange(gpu.MinTgpWatts, gpu.MaxTgpWatts);
            _sliderTgp.TickFrequency = Math.Max(1, (_sliderTgp.Maximum - _sliderTgp.Minimum) / 10);
            if (_rowNvvdd == null) return;
            GpuCurrentLimits? limits = null;
            string status = "Turn on the unlock to adjust GPU current limits.";
            if (gpu.PowerOverride is { } driver)
            {
                // Never let a transient driver error break the tab; show it instead.
                try
                {
                    limits = driver.ReadCurrentLimits();
                    var power = driver.ReadPowerLimits();
                    status = $"Driver ceiling {power.CeilingWatts} W · enforced {power.EnforcedWatts} W · stock currents " +
                        $"{limits.Nvvdd.DefaultMilliamps / 1000.0:0.###} / {limits.Msvdd.DefaultMilliamps / 1000.0:0.###}" +
                        (limits.BoardInput is { } input ? $" / {input.DefaultMilliamps / 1000.0:0.###} A" : " A");
                }
                catch (Exception ex) { limits = null; status = "NVIDIA readback failed: " + ex.Message; }
            }
            SetCurrent(_rowNvvdd, limits?.Nvvdd, gpu.TgpUnlocked);
            SetCurrent(_rowMsvdd!, limits?.Msvdd, gpu.TgpUnlocked);
            SetCurrent(_rowBoardInput!, limits?.BoardInput, gpu.TgpUnlocked);
            _lblDriverLimits!.Text = gpu.ProfileNotice.Length > 0 ? status + "\n" + gpu.ProfileNotice : status;
        }
        finally { _refreshing = false; }
    }

    private GpuProfileSettings ReadGpuSettings() => new()
    {
        Tgp = _sliderTgp?.Value ?? 0,
        DynamicBoostEnabled = _chkDynamicBoost?.Checked == true,
        DynamicBoostWatts = _sliderDb?.Value ?? 0,
        // Not a profile: works within the active unlock, so unmoved sliders keep their values.
        BiosOverrideEnabled = _gpuControl?.TgpUnlocked == true ? true : null,
        NvvddMilliamps = SelectedMilliamps(_rowNvvdd),
        MsvddMilliamps = SelectedMilliamps(_rowMsvdd),
        BoardInputMilliamps = SelectedMilliamps(_rowBoardInput)
    };

    private void BtnRead_Click(object? sender, EventArgs e)
    {
        RefreshFromEc();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
            _tooltip.Dispose();
        base.Dispose(disposing);
    }
}
