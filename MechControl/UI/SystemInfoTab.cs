using MechControl.Monitoring;

namespace MechControl.UI;

class SystemInfoTab : UserControl
{
    // Temperature labels
    private readonly Label _lblCpuTemp;
    private readonly Label _lblGpuTemp;

    // Power delivery labels
    private readonly Label _lblAdapterCurrent;
    private readonly Label _lblSystemPower;
    private readonly Label _lblGpuPowerAlloc;
    private readonly Label _lblThermalBudget;

    public SystemInfoTab(string? systemName, string? cpuName)
    {
        Dock = DockStyle.Fill;
        AutoScroll = true;

        int y = 16;
        int labelX = 16;
        int valueX = 275;

        // ── System Identity ─────────────────────────────
        if (systemName != null || cpuName != null)
        {
            if (systemName != null)
            {
                var lblSystemName = new Label
                {
                    Text = systemName,
                    Location = new System.Drawing.Point(16, y),
                    AutoSize = true,
                    Font = UiHelpers.FontHeader
                };
                Controls.Add(lblSystemName);
                y += 24;
            }
            if (cpuName != null)
            {
                var lblCpuName = new Label
                {
                    Text = cpuName,
                    Location = new System.Drawing.Point(16, y),
                    AutoSize = true,
                    Font = UiHelpers.FontNormal
                };
                Controls.Add(lblCpuName);
                y += 24;
            }
            y += 12;
        }

        // ── Temperatures ────────────────────────────────
        var lblTempTitle = new Label
        {
            Text = "Temperatures",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Font = UiHelpers.FontHeader
        };
        Controls.Add(lblTempTitle);
        y += 32;

        _lblCpuTemp = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "CPU Temperature [0x43E]:");
        _lblGpuTemp = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "GPU Temperature [0x44F]:");
        y += 12;

        // ── Power Delivery ──────────────────────────────
        var lblPowerTitle = new Label
        {
            Text = "Power Delivery",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Font = UiHelpers.FontHeader
        };
        Controls.Add(lblPowerTitle);
        y += 32;

        _lblAdapterCurrent = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "Adapter Current [0x449]:");
        _lblSystemPower    = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "System Power [0x60C-0x60D]:");
        _lblGpuPowerAlloc  = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "GPU Power Alloc / NPCF [0x7D5]:");
        _lblThermalBudget  = UiHelpers.AddLabeledRow(this, ref y, labelX, valueX, "Thermal Budget / CTWA [0x788]:");

        UiHelpers.FixZOrder(Controls);
    }

    public void OnMonitorUpdated(MonitorData data)
    {
        _lblCpuTemp.Text = data.CpuTemp > 0 ? $"{data.CpuTemp}\u00B0C" : "--";
        _lblGpuTemp.Text = data.GpuTemp > 0 ? $"{data.GpuTemp}\u00B0C" : "--";

        if (data.AdapterCurrentAmps > 0)
        {
            double estWatts = data.AdapterCurrentAmps * 19.5;
            _lblAdapterCurrent.Text = $"{data.AdapterCurrentAmps:F1} A ({estWatts:F0}W @ 19.5V)";
        }
        else
        {
            _lblAdapterCurrent.Text = "--";
        }

        _lblSystemPower.Text = data.SystemPowerWatts > 0
            ? $"{data.SystemPowerWatts} W"
            : "--";

        _lblGpuPowerAlloc.Text = data.GpuPowerAllocWatts > 0
            ? $"{data.GpuPowerAllocWatts} W"
            : "--";

        _lblThermalBudget.Text = data.ThermalBudgetWatts > 0
            ? $"{data.ThermalBudgetWatts} W"
            : "--";
    }

}
