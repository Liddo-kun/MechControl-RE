using MechControl.Features;
using MechControl.Monitoring;

namespace MechControl.UI;

class MonitorPanel : UserControl
{
    private readonly Label _lblCpuFan;
    private readonly Label _lblGpuFan;
    private readonly Label _lblCpuTemp;
    private readonly Label _lblBattery;
    private readonly Label _lblFanBoost;
    private readonly Label _lblFanCurve;

    public MonitorPanel()
    {
        Height = 64;
        Dock = DockStyle.Bottom;
        BorderStyle = BorderStyle.FixedSingle;
        BackColor = System.Drawing.SystemColors.ControlLight;

        var flow = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = true,
            AutoSize = false,
            Padding = new Padding(4, 4, 4, 0)
        };

        _lblCpuFan = MakeLabel("CPU Fan: --");
        _lblGpuFan = MakeLabel("GPU Fan: --");
        _lblCpuTemp = MakeLabel("CPU Temp Limit: --");
        _lblBattery = MakeLabel("Battery: --");
        _lblFanBoost = MakeLabel("Fan Boost: --");
        _lblFanCurve = MakeLabel("Fan Curve: --");

        flow.Controls.Add(_lblCpuFan);
        flow.Controls.Add(MakeSep());
        flow.Controls.Add(_lblGpuFan);
        flow.Controls.Add(MakeSep());
        flow.Controls.Add(_lblCpuTemp);
        flow.Controls.Add(MakeSep());
        flow.Controls.Add(_lblBattery);
        flow.Controls.Add(MakeSep());
        flow.Controls.Add(_lblFanBoost);
        flow.Controls.Add(MakeSep());
        flow.Controls.Add(_lblFanCurve);

        Controls.Add(flow);
    }

    public void OnMonitorUpdated(MonitorData data)
    {
        _lblCpuFan.Text = $"CPU Fan: {data.CpuFanPercent}% ({data.CpuFanRpm} RPM)";
        _lblGpuFan.Text = $"GPU Fan: {data.GpuFanPercent}% ({data.GpuFanRpm} RPM)";
        _lblCpuTemp.Text = $"CPU Temp Limit: {data.CpuTempLimit}\u00B0C";
        _lblBattery.Text = $"Battery: {data.BatterySoc}% ({data.BatteryModeName}) [0x7A6=0x{data.BatteryModeRaw:X2}]";
        _lblFanBoost.Text = $"Fan Boost: {(data.FanBoostActive ? "ON" : "OFF")}";
        _lblFanCurve.Text = $"Fan Curve: {(data.FanCurveEnabled ? "ON" : "OFF")}";
    }

    private static Label MakeLabel(string text) => new Label
    {
        Text = text,
        AutoSize = true,
        Font = UiHelpers.FontBold,
        Margin = new Padding(2, 2, 2, 0)
    };

    private static Label MakeSep() => new Label
    {
        Text = "|",
        AutoSize = true,
        ForeColor = UiColors.Hint,
        Font = UiHelpers.FontBold,
        Margin = new Padding(0, 2, 0, 0)
    };
}
