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
        AutoScroll = true;

        int y = 16;

        var lblTitle = new Label
        {
            Text = "Battery Charging Mode",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Font = UiHelpers.FontHeader
        };
        Controls.Add(lblTitle);
        y += 32;

        // High Capacity
        _radioHighCapacity = new RadioButton
        {
            Text = "High Capacity (Performance)",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Checked = true
        };
        Controls.Add(_radioHighCapacity);
        y += 22;

        Controls.Add(new Label
        {
            Text = "Charges to 100%. Best for maximum runtime per charge.",
            Location = new System.Drawing.Point(36, y),
            AutoSize = true,
            ForeColor = System.Drawing.SystemColors.GrayText
        });
        y += 30;

        // Balanced
        _radioBalanced = new RadioButton
        {
            Text = "Balanced",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true
        };
        Controls.Add(_radioBalanced);
        y += 22;

        Controls.Add(new Label
        {
            Text = "Charges to ~80%. Good balance between longevity and capacity.",
            Location = new System.Drawing.Point(36, y),
            AutoSize = true,
            ForeColor = System.Drawing.SystemColors.GrayText
        });
        y += 30;

        // Stationary
        _radioStationary = new RadioButton
        {
            Text = "Stationary (Health)",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true
        };
        Controls.Add(_radioStationary);
        y += 22;

        Controls.Add(new Label
        {
            Text = "Charges to ~60%. Best for battery longevity when always plugged in.",
            Location = new System.Drawing.Point(36, y),
            AutoSize = true,
            ForeColor = System.Drawing.SystemColors.GrayText
        });
        y += 40;

        // Buttons
        var btnApply = new Button
        {
            Text = "Apply",
            Location = new System.Drawing.Point(16, y),
            Size = new System.Drawing.Size(100, 32)
        };
        btnApply.Click += BtnApply_Click;
        Controls.Add(btnApply);

        var btnRead = new Button
        {
            Text = "Read from EC",
            Location = new System.Drawing.Point(126, y),
            Size = new System.Drawing.Size(140, 32)
        };
        btnRead.Click += BtnRead_Click;
        Controls.Add(btnRead);
        y += 52;

        // ── Battery Telemetry Section ────────────────────────────
        var lblTelemetryTitle = new Label
        {
            Text = "Battery Telemetry",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Font = UiHelpers.FontHeader
        };
        Controls.Add(lblTelemetryTitle);
        y += 32;

        int labelX = 16;
        int valueX = 280;

        _lblPowerSource   = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "Power Source [0x490]:");
        _lblCharge        = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "State of Charge [0x4AB]:");
        _lblHealth        = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "Battery Health (SOH) [0x404/402]:");
        _lblCycleCount    = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "Cycle Count [0x4A6-0x4A7]:");
        _lblTemperature   = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "Temperature [0x4A2-0x4A3]:");
        _lblDesignCap     = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "Design Capacity [0x402-0x403]:");
        _lblFullChargeCap = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "Full Charge Cap [0x404-0x405]:");
        _lblRemainingCap  = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "Remaining Cap [0x436-0x437]:");
        _lblVoltage       = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "Voltage [0x438/0x408]:");
        _lblChargeRate    = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "Charge Rate [0x434-0x435]:");

        // Tooltips for mode radio buttons
        _tooltip.SetToolTip(_radioHighCapacity, $"Register 0x{_addrBatteryMode:X3} | Value: 0x{control.GetModeValue(0):X2}");
        _tooltip.SetToolTip(_radioBalanced, $"Register 0x{_addrBatteryMode:X3} | Value: 0x{control.GetModeValue(1):X2}");
        _tooltip.SetToolTip(_radioStationary, $"Register 0x{_addrBatteryMode:X3} | Value: 0x{control.GetModeValue(2):X2}");

        // Auto-refresh telemetry every 2 seconds
        _telemetryTimer = new System.Windows.Forms.Timer { Interval = 2000 };
        _telemetryTimer.Tick += (_, _) => RefreshTelemetry();

        UiHelpers.FixZOrder(Controls);
    }

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
        try
        {
            int mode = _control.ReadBatteryMode();
            SelectMode(mode);
        }
        catch (Exception ex)
        {
            MessageBox.Show(
                $"Failed to read battery mode:\n{ex.Message}",
                "EC Read Error", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }

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

    private void BtnApply_Click(object? sender, EventArgs e)
    {
        try
        {
            _control.SetBatteryMode(SelectedMode);
        }
        catch (Exception ex)
        {
            MessageBox.Show(
                $"Failed to set battery mode:\n{ex.Message}",
                "EC Write Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void BtnRead_Click(object? sender, EventArgs e)
    {
        RefreshFromEc();
    }
}
