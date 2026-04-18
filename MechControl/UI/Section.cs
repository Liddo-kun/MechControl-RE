namespace MechControl.UI;

/// <summary>
/// Labeled section panel: a thin border with the bold title text intersecting
/// the top edge (GroupBox look), but built as a plain TableLayoutPanel so the
/// AutoSize + title-rendering just works. Content added via Controls.Add goes
/// into row 1 (title occupies row 0, but is painted, not placed).
/// </summary>
class Section : TableLayoutPanel
{
    private readonly string _title;
    private readonly System.Drawing.Size _titleSize;

    private const int TitleX = 10;
    private const int TitleTopMargin = 2;
    private static readonly System.Drawing.Pen s_borderPen = new(System.Drawing.Color.FromArgb(180, 180, 180));
    private static readonly System.Drawing.Font s_titleFont = new("Segoe UI", 10f, System.Drawing.FontStyle.Bold);

    public Section(string title, bool fitContent)
    {
        _title = title;
        _titleSize = TextRenderer.MeasureText(title, s_titleFont);

        SetStyle(ControlStyles.ResizeRedraw | ControlStyles.UserPaint
            | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);

        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        ColumnCount = 1;
        ColumnStyles.Add(new ColumnStyle(
            fitContent ? SizeType.AutoSize : SizeType.Percent, 100f));

        // Leave vertical space at top for the painted title; row 0 is a
        // transparent spacer so added content (row 1) starts below the title.
        RowStyles.Add(new RowStyle(SizeType.Absolute, _titleSize.Height + TitleTopMargin + 6));

        Padding = new Padding(12, 0, 12, 10);
        Margin = new Padding(0, 0, 0, UiMetrics.SectionGap);
        MinimumSize = new System.Drawing.Size(_titleSize.Width + TitleX * 2 + 8, 0);

        // Row 0 spacer (empty panel so content added by caller lands in row 1).
        Controls.Add(new Panel { Width = 0, Height = 0, Margin = new Padding(0) }, 0, 0);

        if (!fitContent)
            Dock = DockStyle.Top;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics;
        int midY = TitleTopMargin + _titleSize.Height / 2;
        int tL = TitleX - 2;
        int tR = TitleX + _titleSize.Width + 2;

        g.DrawLine(s_borderPen, 0, midY, tL, midY);
        g.DrawLine(s_borderPen, tR, midY, Width - 1, midY);
        g.DrawLine(s_borderPen, 0, midY, 0, Height - 1);
        g.DrawLine(s_borderPen, Width - 1, midY, Width - 1, Height - 1);
        g.DrawLine(s_borderPen, 0, Height - 1, Width - 1, Height - 1);

        // TextRenderer (GDI) matches Label's ClearType rendering. DrawString
        // (GDI+) would give grayscale/aliased text that looks out of place.
        TextRenderer.DrawText(g, _title, s_titleFont,
            new System.Drawing.Point(TitleX, TitleTopMargin),
            System.Drawing.Color.Black,
            TextFormatFlags.NoPadding | TextFormatFlags.Left | TextFormatFlags.Top);
    }
}
