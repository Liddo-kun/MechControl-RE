using MechControl.Features;

namespace MechControl.UI;

/// <summary>
/// Live view of each GPU limit next to the clock: its setting, its reading, how close the reading
/// is to the setting, and the clock cap of the driver controller enforcing it (with how far that
/// cap is above the actual clock). No verdicts: several limits hold the clock back at once and
/// blend into each other, so the view shows the numbers only.
/// </summary>
sealed class GpuLimitsView : TableLayoutPanel
{
    private sealed record Row(Label Name, Label Setting, Label Now, Label Share, Label? Cap);

    private readonly ToolTip _tooltip;
    private readonly Row _clock, _power, _core, _rest, _nvvdd, _msvdd, _boardInput;
    private readonly Label _status;

    public GpuLimitsView(ToolTip tooltip)
    {
        _tooltip = tooltip;
        Dock = DockStyle.Top;
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        ColumnCount = 5;
        for (int i = 0; i < ColumnCount; i++) ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

        Controls.Add(new Label { AutoSize = true });
        foreach (var (text, tip) in new[]
        {
            ("Setting", "The active limit (for GPU power, the enforced limit: TGP + Dynamic Boost, or the driver's lower budget)."),
            ("Now", "The driver's latest reading (it updates every 20 ms); refreshed twice a second while this tab is shown."),
            ("Of limit", "The reading as a share of its setting."),
            ("Controller cap (above clock)", "The highest clock the driver controller enforcing this limit allows (power or current controller); the lower cap applies. " +
                "In brackets: how far the cap is above the actual clock. Board input has no controller: the GPU's inline power controller (IPC) " +
                "slows the clock below the caps."),
        })
            Controls.Add(Heading(text, tip));

        _clock = AddRow("GPU clock", "The graphics clock (NVML) against the driver's maximum.");
        _power = AddRow("GPU power", "Total GPU power (TGP) against the enforced limit.");
        _core = AddRow("Core rails", "The rails the power controller manages and predicts (NVVDD and MSVDD supply).", part: true);
        _rest = AddRow("Memory and other", "GPU power outside the core rails: memory, PCIe and the rest of the board. The core gets the power limit minus this, " +
            "so raising TGP raises core power by less than the full amount.", part: true);
        // One current controller caps both rails, so its cap spans the two rows.
        _nvvdd = AddRow("NVVDD current", RailTip, capRows: 2);
        _msvdd = AddRow("MSVDD current", RailTip, capRows: 0);
        _boardInput = AddRow("Board input current", "Board input has no controller or cap: the GPU's inline power controller (IPC) slows the clock " +
            "below every cap. IPC active: the share of time it acted since the last reading. It also clips power peaks, so it can act without board input.");

        _status = Note("", UiMetrics.RowGap);
        // Measured on an RTX 5090 Laptop (driver 617.14): current = rail power / a fixed voltage
        // (0.630 V NVVDD under load vs ~0.89 V real), so it reads high. Details in
        // notes/nvidia_gpu_limiters.md.
        Note("NVVDD and MSVDD currents are calculated by the driver from rail power, not measured, " +
            "and can read well above the real current. The limits use the same calculated values.", 0);
        Note("When software shunt modding, tune the NVVDD, MSVDD and board input current limits so that they are all " +
            "solid green 99% of the time, and not any higher.", UiMetrics.RowGap);
        ShowTelemetry(null, "not read yet");
    }

    private Label Note(string text, int top)
    {
        var note = new Label { Text = text, AutoSize = true, ForeColor = UiColors.Hint, MaximumSize = new Size(600, 0), Margin = new Padding(3, top, 3, 0) };
        Controls.Add(note);
        SetColumnSpan(note, ColumnCount);
        return note;
    }

    private const string RailTip = "The current controller lowers its clock cap until neither rail's current would exceed its limit at that cap.";

    private Label Heading(string text, string tip)
    {
        var heading = new Label { Text = text, AutoSize = true, ForeColor = UiColors.Hint, Margin = new Padding(12, UiMetrics.RowGap, 0, 0) };
        _tooltip.SetToolTip(heading, tip);
        return heading;
    }

    // capRows: how many rows this row's cap cell spans (0: covered by the row above).
    private Row AddRow(string name, string tip, bool part = false, int capRows = 1)
    {
        var label = new Label { Text = name, AutoSize = true, Font = part ? UiHelpers.FontNormal : UiHelpers.FontBold, Margin = new Padding(part ? 12 : 0, 6, 0, 0) };
        _tooltip.SetToolTip(label, tip);
        Controls.Add(label);
        static Label Cell() => new() { AutoSize = true, Margin = new Padding(12, 6, 0, 0) };
        var row = new Row(label, Cell(), Cell(), Cell(), capRows > 0 ? Cell() : null);
        Controls.Add(row.Setting); Controls.Add(row.Now); Controls.Add(row.Share);
        if (row.Cap != null)
        {
            Controls.Add(row.Cap);
            if (capRows > 1) { SetRowSpan(row.Cap, capRows); row.Cap.Anchor = AnchorStyles.Left; }
        }
        return row;
    }

    public void ShowTelemetry(GpuTelemetry? t, string reason)
    {
        // Every cell but the controller cap; board input's cap cell by IPC activity.
        Tint(Ratio(t?.GpuPowerMilliwatts, t?.EnforcedMilliwatts), PowerNormal, PowerLimiting, _power.Name, _power.Setting, _power.Now, _power.Share);
        Tint(Ratio(t?.NvvddMilliamps, t?.NvvddLimitMilliamps), NvvddNormal, NvvddLimiting, _nvvdd.Name, _nvvdd.Setting, _nvvdd.Now, _nvvdd.Share);
        Tint(Ratio(t?.MsvddMilliamps, t?.MsvddLimitMilliamps), MsvddNormal, MsvddLimiting, _msvdd.Name, _msvdd.Setting, _msvdd.Now, _msvdd.Share);
        Tint(Ratio(t?.BoardInputMilliamps, t?.BoardInputLimitMilliamps), BoardInputNormal, BoardInputLimiting,
            _boardInput.Name, _boardInput.Setting, _boardInput.Now, _boardInput.Share);
        Tint(t?.InlinePowerActivity, IpcNormal, IpcLimiting, _boardInput.Cap!);
        if (t == null)
        {
            foreach (var row in new[] { _clock, _power, _core, _rest, _nvvdd, _msvdd, _boardInput })
                Show(row, "--", "--", "--", "--");
            _status.Text = $"-- ({reason})";
            _status.Visible = true;
            return;
        }
        _status.Visible = false;
        int? clock = t.ClockMhz;
        string Cap(string controller, int? cap) => cap is int c
            ? $"{controller} {c} MHz" + (clock is int k ? $" ({c - k:+0;-0;0})" : "") : "--";

        Show(_clock, t.MaxClockMhz is int max ? $"max {max} MHz" : "--", Mhz(clock), Share(clock, t.MaxClockMhz), "");
        Show(_power, Watts(t.EnforcedMilliwatts), Watts(t.GpuPowerMilliwatts), Share(t.GpuPowerMilliwatts, t.EnforcedMilliwatts), Cap("power", t.PowerCapMhz));
        int? rest = t.GpuPowerMilliwatts - t.CorePowerMilliwatts;
        Show(_core, "", Watts(t.CorePowerMilliwatts), "", "");
        Show(_rest, "", rest >= 0 ? Watts(rest) : "--", "", "");
        Show(_nvvdd, Amps(t.NvvddLimitMilliamps, "0.###"), Amps(t.NvvddMilliamps, "0.0"), Share(t.NvvddMilliamps, t.NvvddLimitMilliamps), Cap("current", t.CurrentCapMhz));
        Show(_msvdd, Amps(t.MsvddLimitMilliamps, "0.###"), Amps(t.MsvddMilliamps, "0.0"), Share(t.MsvddMilliamps, t.MsvddLimitMilliamps), "");
        Show(_boardInput, Amps(t.BoardInputLimitMilliamps, "0.###"), Amps(t.BoardInputMilliamps, "0.00"), Share(t.BoardInputMilliamps, t.BoardInputLimitMilliamps),
            "IPC active " + (t.InlinePowerActivity is double ipc ? $"{Math.Min(ipc, 1):0%}" : "--"));
    }

    // Green up to the first value, amber midway, red from the second. IPC: on the tested RTX 5090
    // Laptop it reached at most 37% from power peaks alone (board input maxed, 195 W), 67-73% with
    // board input partly limiting (11.053 A, 175 W) and 93.5% with it clearly limiting. GPU power,
    // NVVDD and MSVDD: the reading against its limit, from what Jon sees while each limits. Board
    // input is enforced on fast peaks, so its average stays well below the limit while limiting
    // (71-74% at 11.053 A, 175 W, where raising it gained clock; 60-63% at 15.474 A, not limiting).
    private const double IpcNormal = 0.52, IpcLimiting = 0.80, PowerNormal = 0.94, PowerLimiting = 0.99,
        NvvddNormal = 0.89, NvvddLimiting = 0.99, MsvddNormal = 0.90, MsvddLimiting = 0.99, BoardInputNormal = 0.68, BoardInputLimiting = 0.81;

    // Unknown: the normal text color.
    private static void Tint(double? value, double normal, double limiting, params Label[] cells)
    {
        static Color Mix(Color a, Color b, double x) =>
            Color.FromArgb((int)(a.R + (b.R - a.R) * x), (int)(a.G + (b.G - a.G) * x), (int)(a.B + (b.B - a.B) * x));
        double f = value is double v ? Math.Clamp((v - normal) / (limiting - normal), 0, 1) : 0;
        Color color = value == null ? SystemColors.ControlText
            : f < 0.5 ? Mix(UiColors.StatusOk, UiColors.StatusWarn, f * 2) : Mix(UiColors.StatusWarn, UiColors.StatusError, f * 2 - 1);
        foreach (var cell in cells) cell.ForeColor = color;
    }

    private static void Show(Row row, string setting, string now, string share, string cap)
    {
        row.Setting.Text = setting; row.Now.Text = now; row.Share.Text = share;
        if (row.Cap != null) row.Cap.Text = cap;
    }

    private static double? Ratio(int? value, int? limit) => value is int v && limit is int l and > 0 ? (double)v / l : null;
    private static string Share(int? value, int? limit) => Ratio(value, limit) is double r ? $"{r:0%}" : "--";
    private static string Mhz(int? mhz) => mhz is int value ? $"{value} MHz" : "--";
    private static string Watts(int? milliwatts) => milliwatts is int mw ? $"{mw / 1000.0:0.0} W" : "--";
    private static string Amps(int? milliamps, string format) => milliamps is int ma ? (ma / 1000.0).ToString(format) + " A" : "--";
}
