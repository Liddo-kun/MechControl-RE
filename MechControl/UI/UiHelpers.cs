namespace MechControl.UI;

static class UiHelpers
{
    public static readonly System.Drawing.Font FontTiny = new("Segoe UI", 7f);
    public static readonly System.Drawing.Font FontNormal = new("Segoe UI", 9f);
    public static readonly System.Drawing.Font FontBold = new("Segoe UI", 9f, System.Drawing.FontStyle.Bold);
    public static readonly System.Drawing.Font FontHeader = new("Segoe UI", 12f, System.Drawing.FontStyle.Bold);

    // Records returned from builders
    public sealed record SliderRow(
        System.Windows.Forms.Label Label,
        System.Windows.Forms.TrackBar Slider,
        System.Windows.Forms.Label Value);

    public sealed record RgbRow(
        System.Windows.Forms.NumericUpDown R,
        System.Windows.Forms.NumericUpDown G,
        System.Windows.Forms.NumericUpDown B,
        System.Windows.Forms.Panel Preview);

    public sealed record RgbFlow(
        System.Windows.Forms.FlowLayoutPanel Flow,
        System.Windows.Forms.NumericUpDown R,
        System.Windows.Forms.NumericUpDown G,
        System.Windows.Forms.NumericUpDown B,
        System.Windows.Forms.Panel Preview);

    // Outer top-down section stack. TableLayoutPanel instead of FlowLayoutPanel
    // because we need each section to fill the tab width (a FlowLayoutPanel
    // TopDown does not stretch children horizontally, only stacks them).
    public static System.Windows.Forms.TableLayoutPanel BuildSectionStack()
    {
        var t = new System.Windows.Forms.TableLayoutPanel
        {
            Dock = System.Windows.Forms.DockStyle.Fill,
            ColumnCount = 1,
            AutoScroll = true,
            Padding = new System.Windows.Forms.Padding(
                UiMetrics.MarginLeft, UiMetrics.RowGap,
                UiMetrics.MarginLeft, UiMetrics.RowGap),
        };
        t.ColumnStyles.Add(new System.Windows.Forms.ColumnStyle(
            System.Windows.Forms.SizeType.Percent, 100f));
        return t;
    }

    // Standardized section container. Returns a custom Section panel that
    // draws its own titled border (GroupBox's AutoSize has title-clipping
    // issues we don't want to fight).
    // Default: Dock.Top — stretches to fill the stack column width.
    // fitContent: true — AutoSize shrinks to wrap content horizontally.
    public static Section BuildSection(string title, bool fitContent = false)
        => new Section(title, fitContent);

    // 3-column table for slider rows: AutoSize label | Percent 100 slider | AutoSize value
    public static System.Windows.Forms.TableLayoutPanel BuildSliderTable()
    {
        var t = new System.Windows.Forms.TableLayoutPanel
        {
            Dock = System.Windows.Forms.DockStyle.Top,
            AutoSize = true,
            AutoSizeMode = System.Windows.Forms.AutoSizeMode.GrowAndShrink,
            ColumnCount = 3,
        };
        t.ColumnStyles.Add(new System.Windows.Forms.ColumnStyle(System.Windows.Forms.SizeType.AutoSize));
        t.ColumnStyles.Add(new System.Windows.Forms.ColumnStyle(System.Windows.Forms.SizeType.Percent, 100f));
        t.ColumnStyles.Add(new System.Windows.Forms.ColumnStyle(System.Windows.Forms.SizeType.AutoSize));
        return t;
    }

    // 2-column table for caption/value rows: AutoSize caption | AutoSize value.
    // No Dock so the table width = sum of content widths — lets a fit-content
    // GroupBox wrap tightly around the rows instead of stretching to window.
    public static System.Windows.Forms.TableLayoutPanel BuildKeyValueTable()
    {
        var t = new System.Windows.Forms.TableLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = System.Windows.Forms.AutoSizeMode.GrowAndShrink,
            ColumnCount = 2,
        };
        t.ColumnStyles.Add(new System.Windows.Forms.ColumnStyle(System.Windows.Forms.SizeType.AutoSize));
        t.ColumnStyles.Add(new System.Windows.Forms.ColumnStyle(System.Windows.Forms.SizeType.AutoSize));
        return t;
    }

    // Slider row: (Label AutoSize | Slider Fill | Value AutoSize). Formatter
    // overrides default "{v} {unit}" formatting.
    public static SliderRow BuildSliderRow(
        string label, int min, int max, string unit,
        int? initial = null, Func<int, string>? formatter = null)
    {
        var lbl = new System.Windows.Forms.Label
        {
            Text = label,
            AutoSize = true,
            Font = FontBold,
            Anchor = System.Windows.Forms.AnchorStyles.Left,
            Margin = new System.Windows.Forms.Padding(0, 8, 8, 0),
        };
        int initValue = initial ?? min;
        if (initValue < min) initValue = min;
        if (initValue > max) initValue = max;
        var trk = new System.Windows.Forms.TrackBar
        {
            Minimum = min,
            Maximum = max,
            Value = initValue,
            TickFrequency = Math.Max(1, (max - min) / 10),
            LargeChange = Math.Max(1, (max - min) / 10),
            SmallChange = 1,
            AutoSize = false,
            Height = UiMetrics.SliderHeight,
            Dock = System.Windows.Forms.DockStyle.Fill,
            Margin = new System.Windows.Forms.Padding(0, 0, 0, 0),
        };
        var fmt = formatter ?? (v => string.IsNullOrEmpty(unit) ? v.ToString() : $"{v} {unit}");
        var val = new System.Windows.Forms.Label
        {
            Text = fmt(trk.Value),
            AutoSize = true,
            Font = FontNormal,
            Anchor = System.Windows.Forms.AnchorStyles.Left,
            Margin = new System.Windows.Forms.Padding(8, 8, 0, 0),
        };
        trk.ValueChanged += (_, _) => val.Text = fmt(trk.Value);
        return new SliderRow(lbl, trk, val);
    }

    // Add a SliderRow's three controls to a 3-col slider table in order.
    public static void AddSliderRow(
        System.Windows.Forms.TableLayoutPanel layout, SliderRow row)
    {
        layout.Controls.Add(row.Label);
        layout.Controls.Add(row.Slider);
        layout.Controls.Add(row.Value);
    }

    // Add a bold caption + "--" value label pair to a 2-col key/value table.
    // Returns the value label so the caller can update it later.
    public static System.Windows.Forms.Label AddKeyValueRow(
        System.Windows.Forms.TableLayoutPanel layout, string caption, string initial = "--")
    {
        var lblCaption = new System.Windows.Forms.Label
        {
            Text = caption,
            AutoSize = true,
            Font = FontBold,
            Margin = new System.Windows.Forms.Padding(0, 3, 16, 3),
        };
        var lblValue = new System.Windows.Forms.Label
        {
            Text = initial,
            AutoSize = true,
            Font = FontNormal,
            Margin = new System.Windows.Forms.Padding(0, 3, 0, 3),
        };
        layout.Controls.Add(lblCaption);
        layout.Controls.Add(lblValue);
        return lblValue;
    }

    // Small inline label for "R:", "G:", "B:", etc. inside flow layouts.
    public static System.Windows.Forms.Label MakeInlineLabel(string text) => new()
    {
        Text = text,
        AutoSize = true,
        Font = FontNormal,
        Margin = new System.Windows.Forms.Padding(4, 8, 2, 0),
    };

    // RGB triplet + live preview panel
    public static RgbRow BuildRgbRow(byte r, byte g, byte b)
    {
        System.Windows.Forms.NumericUpDown Make(int initial) => new()
        {
            Minimum = 0,
            Maximum = 255,
            Value = initial,
            Width = UiMetrics.NumericWidth,
            Font = FontNormal,
            TextAlign = System.Windows.Forms.HorizontalAlignment.Right,
            Margin = new System.Windows.Forms.Padding(0, 4, 4, 4),
        };
        var nudR = Make(r);
        var nudG = Make(g);
        var nudB = Make(b);
        var preview = new System.Windows.Forms.Panel
        {
            Width = UiMetrics.RgbPreviewSize,
            Height = UiMetrics.RgbPreviewSize,
            BorderStyle = System.Windows.Forms.BorderStyle.FixedSingle,
            BackColor = System.Drawing.Color.FromArgb(r, g, b),
            Margin = new System.Windows.Forms.Padding(8, 2, 0, 2),
        };
        void Update(object? s, EventArgs e) =>
            preview.BackColor = System.Drawing.Color.FromArgb(
                (int)nudR.Value, (int)nudG.Value, (int)nudB.Value);
        nudR.ValueChanged += Update;
        nudG.ValueChanged += Update;
        nudB.ValueChanged += Update;
        return new RgbRow(nudR, nudG, nudB, preview);
    }

    // Full RGB flow: [R:] [nud] [G:] [nud] [B:] [nud] [preview]. Caller can
    // append extra controls (e.g. a Mode combo) to Flow after this returns.
    public static RgbFlow BuildRgbFlow(byte r, byte g, byte b)
    {
        var row = BuildRgbRow(r, g, b);
        var flow = new System.Windows.Forms.FlowLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = System.Windows.Forms.AutoSizeMode.GrowAndShrink,
            FlowDirection = System.Windows.Forms.FlowDirection.LeftToRight,
            Margin = new System.Windows.Forms.Padding(0, UiMetrics.RowGap, 0, UiMetrics.RowGap),
        };
        flow.Controls.Add(MakeInlineLabel("R:"));
        flow.Controls.Add(row.R);
        flow.Controls.Add(MakeInlineLabel("G:"));
        flow.Controls.Add(row.G);
        flow.Controls.Add(MakeInlineLabel("B:"));
        flow.Controls.Add(row.B);
        flow.Controls.Add(row.Preview);
        return new RgbFlow(flow, row.R, row.G, row.B, row.Preview);
    }

    // Standard apply/action button. Defaults to ButtonApplyWidth; pass width for
    // wider labels ("Pick Color...", "Save to EC"). Margin defaults to RowGap-top.
    public static System.Windows.Forms.Button BuildApplyButton(
        string text, EventHandler onClick,
        int width = 0, System.Windows.Forms.Padding? margin = null)
    {
        var b = new System.Windows.Forms.Button
        {
            Text = text,
            Width = width > 0 ? width : UiMetrics.ButtonApplyWidth,
            Height = UiMetrics.ButtonHeight,
            Font = FontNormal,
            Margin = margin ?? new System.Windows.Forms.Padding(0, UiMetrics.RowGap, 0, 0),
        };
        b.Click += onClick;
        return b;
    }

    // Apply + optional Read button pair
    public static (System.Windows.Forms.Button Apply, System.Windows.Forms.Button? Read) BuildApplyReadRow(
        EventHandler onApply, EventHandler? onRead = null)
    {
        var btnApply = BuildApplyButton("Apply", onApply,
            margin: new System.Windows.Forms.Padding(0, UiMetrics.RowGap, UiMetrics.ButtonGap, 0));
        var btnRead = onRead != null
            ? BuildApplyButton("Read from EC", onRead, width: UiMetrics.ButtonReadWidth)
            : null;
        return (btnApply, btnRead);
    }

    // Unified try/catch + MessageBox for UI write paths
    public static void TryUi(Action action, string operation, string? title = null)
    {
        try { action(); }
        catch (Exception ex)
        {
            DiagnosticLog.Error($"UI operation '{operation}' failed: {ex.Message}");
            System.Windows.Forms.MessageBox.Show(
                $"Failed to {operation}:\n{ex.Message}",
                title ?? "Error",
                System.Windows.Forms.MessageBoxButtons.OK,
                System.Windows.Forms.MessageBoxIcon.Error);
        }
    }

    // Async variant of TryUi for handlers that await BLE/HID operations.
    public static async Task TryUiAsync(Func<Task> action, string operation, string? title = null)
    {
        try { await action(); }
        catch (Exception ex)
        {
            DiagnosticLog.Error($"UI operation '{operation}' failed: {ex.Message}");
            System.Windows.Forms.MessageBox.Show(
                $"Failed to {operation}:\n{ex.Message}",
                title ?? "Error",
                System.Windows.Forms.MessageBoxButtons.OK,
                System.Windows.Forms.MessageBoxIcon.Error);
        }
    }
}
