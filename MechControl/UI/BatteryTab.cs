using MechControl.Features;
using MechControl.Profile;

namespace MechControl.UI;

class BatteryTab : UserControl
{
    private readonly BatteryControl _control;
    private readonly RadioButton _radioHighCapacity;
    private readonly RadioButton _radioBalanced;
    private readonly RadioButton _radioStationary;
    private readonly ToolTip _tooltip = new();
    private readonly int _addrBatteryMode;

    // Telemetry labels
    private readonly Label _lblPowerSource;
    private readonly Label _lblCharge;
    private readonly Label _lblTemperature;
    private readonly Label _lblCycleCount;
    private readonly Label _lblDesignCap;
    private readonly Label _lblFullChargeCap;
    private readonly Label _lblRemainingCap;
    private readonly Label _lblVoltage;
    private readonly Label _lblChargeRate;
    private readonly Label _lblHealth;
    private readonly System.Windows.Forms.Timer _telemetryTimer;

    public BatteryTab(BatteryControl control, MachineProfile profile)
    {
        _control = control;
        _addrBatteryMode = profile.GetRegister("batteryMode");

        Dock = DockStyle.Fill;

        var stack = UiHelpers.BuildSectionStack();

        // ── Charging Mode section ────────────────────────────────
        var grpMode = UiHelpers.BuildSection("Battery Charging Mode", fitContent: true);
        var modeLayout = new TableLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 1,
        };
        modeLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

        _radioHighCapacity = new RadioButton
        {
            Text = "High Capacity (Performance)",
            AutoSize = true,
            Checked = true,
            Margin = new Padding(0, 0, 0, 0),
        };
        modeLayout.Controls.Add(_radioHighCapacity);
        modeLayout.Controls.Add(MakeHint(
            "Charges to 100%. Best for maximum runtime per charge."));

        _radioBalanced = new RadioButton
        {
            Text = "Balanced",
            AutoSize = true,
            Margin = new Padding(0, UiMetrics.RowGap, 0, 0),
        };
        modeLayout.Controls.Add(_radioBalanced);
        modeLayout.Controls.Add(MakeHint(
            "Charges to ~80%. Good balance between longevity and capacity."));

        _radioStationary = new RadioButton
        {
            Text = "Stationary (Health)",
            AutoSize = true,
            Margin = new Padding(0, UiMetrics.RowGap, 0, 0),
        };
        modeLayout.Controls.Add(_radioStationary);
        modeLayout.Controls.Add(MakeHint(
            "Charges to ~60%. Best for battery longevity when always plugged in."));

        var (btnApply, btnRead) = UiHelpers.BuildApplyReadRow(BtnApply_Click, BtnRead_Click);
        var buttonRow = new FlowLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            FlowDirection = FlowDirection.LeftToRight,
            Margin = new Padding(0, UiMetrics.RowGap, 0, 0),
        };
        buttonRow.Controls.Add(btnApply);
        buttonRow.Controls.Add(btnRead!);
        modeLayout.Controls.Add(buttonRow);

        grpMode.Controls.Add(modeLayout);
        stack.Controls.Add(grpMode);

        // ── Battery Telemetry section ───────────────────────────
        var grpTelemetry = UiHelpers.BuildSection("Battery Telemetry", fitContent: true);
        var telemetryLayout = UiHelpers.BuildKeyValueTable();

        _lblPowerSource   = UiHelpers.AddKeyValueRow(telemetryLayout, "Power Source [0x490]:");
        _lblCharge        = UiHelpers.AddKeyValueRow(telemetryLayout, "State of Charge [0x4AB]:");
        _lblHealth        = UiHelpers.AddKeyValueRow(telemetryLayout, "Battery Health (SOH) [0x404/402]:");
        _lblCycleCount    = UiHelpers.AddKeyValueRow(telemetryLayout, "Cycle Count [0x4A6-0x4A7]:");
        _lblTemperature   = UiHelpers.AddKeyValueRow(telemetryLayout, "Temperature [0x4A2-0x4A3]:");
        _lblDesignCap     = UiHelpers.AddKeyValueRow(telemetryLayout, "Design Capacity [0x402-0x403]:");
        _lblFullChargeCap = UiHelpers.AddKeyValueRow(telemetryLayout, "Full Charge Cap [0x404-0x405]:");
        _lblRemainingCap  = UiHelpers.AddKeyValueRow(telemetryLayout, "Remaining Cap [0x436-0x437]:");
        _lblVoltage       = UiHelpers.AddKeyValueRow(telemetryLayout, "Voltage [0x438/0x408]:");
        _lblChargeRate    = UiHelpers.AddKeyValueRow(telemetryLayout, "Charge Rate [0x434-0x435]:");

        grpTelemetry.Controls.Add(telemetryLayout);
        stack.Controls.Add(grpTelemetry);

        Controls.Add(stack);

        // Tooltips for mode radio buttons
        _tooltip.SetToolTip(_radioHighCapacity, $"Register 0x{_addrBatteryMode:X3} | Value: 0x{control.GetModeValue(0):X2}");
        _tooltip.SetToolTip(_radioBalanced, $"Register 0x{_addrBatteryMode:X3} | Value: 0x{control.GetModeValue(1):X2}");
        _tooltip.SetToolTip(_radioStationary, $"Register 0x{_addrBatteryMode:X3} | Value: 0x{control.GetModeValue(2):X2}");

        // Auto-refresh telemetry every 2 seconds
        _telemetryTimer = new System.Windows.Forms.Timer { Interval = 2000 };
        _telemetryTimer.Tick += (_, _) => RefreshTelemetry();
    }

    private static Label MakeHint(string text) => new()
    {
        Text = text,
        AutoSize = true,
        ForeColor = UiColors.Hint,
        Margin = new Padding(20, 0, 0, UiMetrics.RowGap),
    };

    private int SelectedMode =>
        _radioHighCapacity.Checked ? 0 :
        _radioBalanced.Checked ? 1 : 2;

    private void SelectMode(int mode)
    {
        _radioHighCapacity.Checked = mode == 0;
        _radioBalanced.Checked = mode == 1;
        _radioStationary.Checked = mode == 2;
    }

    public void StartTelemetry() => _telemetryTimer.Start();

    public void StopTelemetry() => _telemetryTimer.Stop();

    public void RefreshFromEc()
    {
        UiHelpers.TryUi(() => SelectMode(_control.ReadBatteryMode()),
            "read battery mode", "EC Read Error");
        RefreshTelemetry();
    }

    private void RefreshTelemetry()
    {
        try
        {
            var t = _control.ReadBatteryTelemetry();

            _lblPowerSource.Text = t.OnAC ? "AC (plugged in)" : "Battery";
            _lblCharge.Text = $"{t.ChargePercent}%";
            _lblTemperature.Text = $"{t.TemperatureCelsius:F1}\u00B0C";
            _lblCycleCount.Text = t.CycleCount.ToString();

            // Capacities are mAh from EC; show Wh estimate using design voltage
            double designWh = t.DesignCapacityMah * (t.DesignVoltageMv / 1_000_000.0);
            double fullChargeWh = t.FullChargeCapacityMah * (t.DesignVoltageMv / 1_000_000.0);
            double remainingWh = t.RemainingCapacityMah * (t.PresentVoltageMv / 1_000_000.0);

            _lblDesignCap.Text = $"{t.DesignCapacityMah} mAh (~{designWh:F1} Wh)";
            _lblFullChargeCap.Text = $"{t.FullChargeCapacityMah} mAh (~{fullChargeWh:F1} Wh)";
            _lblRemainingCap.Text = $"{t.RemainingCapacityMah} mAh (~{remainingWh:F1} Wh)";
            _lblVoltage.Text = $"{t.PresentVoltageMv} mV ({t.PresentVoltageMv / 1000.0:F2} V)  [design: {t.DesignVoltageMv} mV]";
            _lblChargeRate.Text = $"{t.ChargeRateMa} mA ({t.ChargeRateMa * t.PresentVoltageMv / 1_000_000.0:F1} W)";

            string healthTag = t.HealthPercent >= 80 ? "" : t.HealthPercent >= 50 ? " (degraded)" : " (poor)";
            _lblHealth.Text = t.DesignCapacityMah > 0
                ? $"{t.HealthPercent}%{healthTag}"
                : "N/A";
        }
        catch
        {
            // Silently ignore telemetry read failures — don't interrupt the user
        }
    }

    private void BtnApply_Click(object? sender, EventArgs e) =>
        UiHelpers.TryUi(() => _control.SetBatteryMode(SelectedMode),
            "set battery mode", "EC Write Error");

    private void BtnRead_Click(object? sender, EventArgs e)
    {
        RefreshFromEc();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _telemetryTimer.Dispose();
            _tooltip.Dispose();
        }
        base.Dispose(disposing);
    }
}
