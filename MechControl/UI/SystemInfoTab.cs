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
    private readonly Label _lblDbMaxLive;
    private readonly Label _lblTgpLive;

    // NVIDIA driver budgets: read only while this tab is on screen, at most every 5 s.
    private readonly Features.GpuPowerControl? _nvidiaBudget;
    private readonly Label? _lblLaptopBudget, _lblPlatformBudget;
    private long _nextBudgetRead;

    public SystemInfoTab(
        string? systemName,
        string? cpuName,
        Features.GpuPowerControl? nvidiaBudget = null)
    {
        Dock = DockStyle.Fill;

        var stack = UiHelpers.BuildSectionStack();

        // ── System Identity ─────────────────────────────
        if (systemName != null || cpuName != null)
        {
            var grpIdentity = UiHelpers.BuildSection("System Identity", fitContent: true);
            var identityLayout = new TableLayoutPanel
            {
                AutoSize = true,
                AutoSizeMode = AutoSizeMode.GrowAndShrink,
                ColumnCount = 1,
            };
            identityLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

            if (systemName != null)
            {
                identityLayout.Controls.Add(new Label
                {
                    Text = systemName,
                    AutoSize = true,
                    Font = UiHelpers.FontHeader,
                    Margin = new Padding(0, 0, 0, 4),
                });
            }
            if (cpuName != null)
            {
                identityLayout.Controls.Add(new Label
                {
                    Text = cpuName,
                    AutoSize = true,
                    Font = UiHelpers.FontNormal,
                });
            }
            grpIdentity.Controls.Add(identityLayout);
            stack.Controls.Add(grpIdentity);
        }

        // ── Temperatures ────────────────────────────────
        var grpTemp = UiHelpers.BuildSection("Temperatures", fitContent: true);
        var tempLayout = UiHelpers.BuildKeyValueTable();
        _lblCpuTemp = UiHelpers.AddKeyValueRow(tempLayout, "CPU Temperature [0x43E]:");
        _lblGpuTemp = UiHelpers.AddKeyValueRow(tempLayout, "GPU Temperature [0x44F]:");
        grpTemp.Controls.Add(tempLayout);
        stack.Controls.Add(grpTemp);

        // ── Power Delivery ──────────────────────────────
        var grpPower = UiHelpers.BuildSection("Power Delivery", fitContent: true);
        var powerLayout = UiHelpers.BuildKeyValueTable();
        // Source headings only when a second source (the NVIDIA driver) is shown.
        void Heading(string text)
        {
            var heading = new Label { Text = text, AutoSize = true, Font = new Font(UiHelpers.FontBold, FontStyle.Bold | FontStyle.Underline),
                Margin = new Padding(0, powerLayout.Controls.Count == 0 ? 0 : 8, 0, 0) };
            powerLayout.Controls.Add(heading);
            powerLayout.SetColumnSpan(heading, 2);
        }
        if (nvidiaBudget != null) Heading("EC");
        _lblAdapterCurrent = UiHelpers.AddKeyValueRow(powerLayout, "Adapter Current [0x449]:");
        _lblSystemPower    = UiHelpers.AddKeyValueRow(powerLayout, "System Power [0x60C-0x60D]:");
        _lblTgpLive        = UiHelpers.AddKeyValueRow(powerLayout, "TGP Live / NPCF ACBT [0x788]:");
        _lblDbMaxLive      = UiHelpers.AddKeyValueRow(powerLayout, "DB Max Live / NPCF AMAT [0x7D5]:");
        if (nvidiaBudget != null)
        {
            _nvidiaBudget = nvidiaBudget;
            Heading("NVIDIA Driver");
            _lblLaptopBudget   = UiHelpers.AddKeyValueRow(powerLayout, "GPU Budget F7:");
            _lblPlatformBudget = UiHelpers.AddKeyValueRow(powerLayout, "CPU+GPU Budget TPP:");
            VisibleChanged += (_, _) => _nextBudgetRead = 0; // Fresh values as soon as the tab is shown.
        }
        grpPower.Controls.Add(powerLayout);
        stack.Controls.Add(grpPower);

        Controls.Add(stack);
    }

    public void OnMonitorUpdated(MonitorData data)
    {
        // Skip label updates while the tab isn't shown — the monitor still ticks
        // for the tray/other consumers, but the labels don't need refreshing.
        if (!Visible) return;

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

        _lblTgpLive.Text = $"{data.TgpLiveWatts} W";

        _lblDbMaxLive.Text = data.DbMaxLiveWatts > 0
            ? $"{data.DbMaxLiveWatts} W"
            : "off";

        if (_nvidiaBudget != null && FindForm()?.WindowState != FormWindowState.Minimized &&
            Environment.TickCount64 >= _nextBudgetRead)
        {
            _nextBudgetRead = Environment.TickCount64 + 5000;
            var budget = _nvidiaBudget.ReadDisplayBudget(out string reason);
            _lblLaptopBudget!.Text = budget != null ? $"{budget.LaptopWatts} W" : $"-- ({reason})";
            _lblPlatformBudget!.Text = budget != null ? $"{budget.PlatformWatts} W" : $"-- ({reason})";
        }
    }
}
