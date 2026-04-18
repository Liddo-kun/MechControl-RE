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

            var tgp = UiHelpers.BuildSliderRow("GPU TGP (Total Graphics Power)", tgpLimit.Min, tgpLimit.Max, "W");
            UiHelpers.AddSliderRow(gpuLayout, tgp);
            _sliderTgp = tgp.Slider; _lblTgp = tgp.Value;

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
                _tooltip.SetToolTip(_sliderTgp, $"Register 0x{_addrTgp:X3} | Encoding: offset +{_tgpOffset} | Raw: {_sliderTgp.Value - _tgpOffset}");
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

        if (_gpuEnabled && _gpuControl != null && _sliderTgp != null)
        {
            UiHelpers.TryUi(() =>
            {
                var gpu = _gpuControl.ReadGpuPower();
                _sliderTgp.Value = Math.Clamp(gpu.TgpWatts, _sliderTgp.Minimum, _sliderTgp.Maximum);
                _chkDynamicBoost!.Checked = gpu.DynamicBoostEnabled;
                _sliderDb!.Value = Math.Clamp(gpu.DbWatts, _sliderDb.Minimum, _sliderDb.Maximum);
                _sliderDb.Enabled = gpu.DynamicBoostEnabled;
            }, "read GPU power values", "EC Read Error");
        }
    }

    private void BtnApply_Click(object? sender, EventArgs e)
    {
        if (_cpuEnabled && _cpuControl != null && _sliderPL1 != null)
        {
            UiHelpers.TryUi(() => _cpuControl.ApplyCpuPower(
                _sliderPL1.Value,
                _sliderPL2!.Value,
                _sliderPL4!.Value,
                _sliderTemp!.Value),
                "apply CPU power settings", "EC Write Error");
        }

        if (_gpuEnabled && _gpuControl != null && _sliderTgp != null)
        {
            UiHelpers.TryUi(() => _gpuControl.ApplyGpuPower(
                _sliderTgp.Value,
                _chkDynamicBoost!.Checked,
                _sliderDb!.Value),
                "apply GPU power settings", "EC Write Error");
        }
    }

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
