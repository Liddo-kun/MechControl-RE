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
        AutoScroll = true;

        int y = 12;

        // === CPU Power ===
        if (_cpuEnabled)
        {
            var pl1Limit = profile.Limits.TryGetValue("PL1", out var p1) ? p1 : UniversalDefaults.ConservativeLimits["PL1"];
            var pl2Limit = profile.Limits.TryGetValue("PL2", out var p2) ? p2 : UniversalDefaults.ConservativeLimits["PL2"];
            var pl4Limit = profile.Limits.TryGetValue("PL4", out var p4) ? p4 : UniversalDefaults.ConservativeLimits["PL4"];

            AddSectionHeader(ref y, "CPU Power");
            AddLabel(ref y, "PL1 (Sustained)");
            (_sliderPL1, _lblPL1) = AddSlider(ref y, pl1Limit.Min, pl1Limit.Max, "W");
            AddLabel(ref y, "PL2 (Burst)");
            (_sliderPL2, _lblPL2) = AddSlider(ref y, pl2Limit.Min, pl2Limit.Max, "W");
            AddLabel(ref y, "PL4 (Peak)");
            (_sliderPL4, _lblPL4) = AddSlider(ref y, pl4Limit.Min, pl4Limit.Max, "W");
            AddLabel(ref y, "CPU Temp Limit");
            (_sliderTemp, _lblTemp) = AddSlider(ref y, tempLimit.Min, tempLimit.Max, "\u00B0C");

            y += 8;
        }
        else
        {
            AddSectionHeader(ref y, "CPU Power");
            AddDisabledLabel(ref y, "CPU power controls disabled — register validation failed. See log for details.");
            y += 8;
        }

        // === GPU Power ===
        if (_gpuEnabled)
        {
            var dbLimit = profile.Limits.TryGetValue("dynamicBoost", out var db) ? db : UniversalDefaults.ConservativeLimits["dynamicBoost"];

            AddSectionHeader(ref y, "GPU Power");
            AddLabel(ref y, "GPU TGP (Total Graphics Power)");
            (_sliderTgp, _lblTgp) = AddSlider(ref y, tgpLimit.Min, tgpLimit.Max, "W");

            _chkDynamicBoost = new CheckBox
            {
                Text = "Enable Dynamic Boost",
                Location = new System.Drawing.Point(16, y),
                AutoSize = true,
                Font = UiHelpers.FontBold
            };
            _chkDynamicBoost.CheckedChanged += (s, e) =>
            {
                if (_sliderDb != null)
                    _sliderDb.Enabled = _chkDynamicBoost.Checked;
            };
            Controls.Add(_chkDynamicBoost);
            y += _chkDynamicBoost.PreferredSize.Height + 6;

            AddLabel(ref y, "Dynamic Boost Max");
            (_sliderDb, _lblDb) = AddSlider(ref y, dbLimit.Min, dbLimit.Max, "W");
            _sliderDb.Enabled = false;
        }
        else
        {
            AddSectionHeader(ref y, "GPU Power");
            AddDisabledLabel(ref y, "GPU power controls disabled — register validation failed. See log for details.");
        }

        // === Buttons ===
        y += 8;
        var btnApply = new Button
        {
            Text = "Apply",
            Location = new System.Drawing.Point(16, y),
            Size = new System.Drawing.Size(100, 32),
            Enabled = _cpuEnabled || _gpuEnabled
        };
        btnApply.Click += BtnApply_Click;
        Controls.Add(btnApply);

        var btnRead = new Button
        {
            Text = "Read from EC",
            Location = new System.Drawing.Point(126, y),
            Size = new System.Drawing.Size(140, 32),
            Enabled = _cpuEnabled || _gpuEnabled
        };
        btnRead.Click += BtnRead_Click;
        Controls.Add(btnRead);

        // Tooltips
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

        UiHelpers.FixZOrder(Controls);
    }

    private void AddSectionHeader(ref int y, string text)
    {
        var lbl = new Label
        {
            Text = text,
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Font = UiHelpers.FontHeader
        };
        Controls.Add(lbl);
        y += lbl.PreferredHeight + 6;
    }

    private void AddLabel(ref int y, string text)
    {
        var lbl = new Label
        {
            Text = text,
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Font = UiHelpers.FontBold
        };
        Controls.Add(lbl);
        y += lbl.PreferredHeight + 1;
    }

    private void AddDisabledLabel(ref int y, string text)
    {
        var lbl = new Label
        {
            Text = text,
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            ForeColor = System.Drawing.Color.DarkRed
        };
        Controls.Add(lbl);
        y += lbl.PreferredHeight + 8;
    }

    private (TrackBar slider, Label valueLabel) AddSlider(ref int y, int min, int max, string unit)
    {
        var slider = new TrackBar
        {
            Minimum = min,
            Maximum = max,
            Value = min,
            TickFrequency = Math.Max(1, (max - min) / 10),
            LargeChange = Math.Max(1, (max - min) / 10),
            SmallChange = 1,
            AutoSize = false,
            Location = new System.Drawing.Point(16, y),
            Size = new System.Drawing.Size(400, 30)
        };

        var valueLabel = new Label
        {
            Text = $"{min} {unit}",
            Location = new System.Drawing.Point(426, y + 8),
            AutoSize = true,
            Font = UiHelpers.FontMedium
        };

        string u = unit;
        slider.ValueChanged += (s, e) => valueLabel.Text = $"{slider.Value} {u}";

        Controls.Add(slider);
        Controls.Add(valueLabel);
        y += slider.Height + 2;
        return (slider, valueLabel);
    }

    public void RefreshFromEc()
    {
        if (_cpuEnabled && _cpuControl != null && _sliderPL1 != null)
        {
            try
            {
                var cpu = _cpuControl.ReadCpuPower();
                _sliderPL1.Value = Math.Clamp(cpu.PL1Watts, _sliderPL1.Minimum, _sliderPL1.Maximum);
                _sliderPL2!.Value = Math.Clamp(cpu.PL2Watts, _sliderPL2.Minimum, _sliderPL2.Maximum);
                _sliderPL4!.Value = Math.Clamp(cpu.PL4Watts, _sliderPL4.Minimum, _sliderPL4.Maximum);
                _sliderTemp!.Value = Math.Clamp(cpu.TempLimitC, _sliderTemp.Minimum, _sliderTemp.Maximum);
            }
            catch (Exception ex)
            {
                MessageBox.Show(
                    $"Failed to read CPU power values:\n{ex.Message}",
                    "EC Read Error", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        if (_gpuEnabled && _gpuControl != null && _sliderTgp != null)
        {
            try
            {
                var gpu = _gpuControl.ReadGpuPower();
                _sliderTgp.Value = Math.Clamp(gpu.TgpWatts, _sliderTgp.Minimum, _sliderTgp.Maximum);
                _chkDynamicBoost!.Checked = gpu.DynamicBoostEnabled;
                _sliderDb!.Value = Math.Clamp(gpu.DbWatts, _sliderDb.Minimum, _sliderDb.Maximum);
                _sliderDb.Enabled = gpu.DynamicBoostEnabled;
            }
            catch (Exception ex)
            {
                MessageBox.Show(
                    $"Failed to read GPU power values:\n{ex.Message}",
                    "EC Read Error", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }
    }

    private void BtnApply_Click(object? sender, EventArgs e)
    {
        if (_cpuEnabled && _cpuControl != null && _sliderPL1 != null)
        {
            try
            {
                _cpuControl.ApplyCpuPower(
                    _sliderPL1.Value,
                    _sliderPL2!.Value,
                    _sliderPL4!.Value,
                    _sliderTemp!.Value);
            }
            catch (Exception ex)
            {
                MessageBox.Show(
                    $"Failed to apply CPU power settings:\n{ex.Message}",
                    "EC Write Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        if (_gpuEnabled && _gpuControl != null && _sliderTgp != null)
        {
            try
            {
                _gpuControl.ApplyGpuPower(
                    _sliderTgp.Value,
                    _chkDynamicBoost!.Checked,
                    _sliderDb!.Value);
            }
            catch (Exception ex)
            {
                MessageBox.Show(
                    $"Failed to apply GPU power settings:\n{ex.Message}",
                    "EC Write Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }
    }

    private void BtnRead_Click(object? sender, EventArgs e)
    {
        RefreshFromEc();
    }
}
