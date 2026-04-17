namespace MechControl.UI;

static class UiHelpers
{
    public static readonly System.Drawing.Font FontTiny = new("Segoe UI", 7f);
    public static readonly System.Drawing.Font FontSmall = new("Segoe UI", 8.5f);
    public static readonly System.Drawing.Font FontSmallBold = new("Segoe UI", 8.5f, System.Drawing.FontStyle.Bold);
    public static readonly System.Drawing.Font FontNormal = new("Segoe UI", 9f);
    public static readonly System.Drawing.Font FontBold = new("Segoe UI", 9f, System.Drawing.FontStyle.Bold);
    public static readonly System.Drawing.Font FontMedium = new("Segoe UI", 9.5f);
    public static readonly System.Drawing.Font FontHeader = new("Segoe UI", 12f, System.Drawing.FontStyle.Bold);

    /// <summary>
    /// Fix WinForms z-order so controls lower on screen draw above
    /// overflow from upper controls (e.g., TrackBar covering labels below it).
    /// </summary>
    public static void FixZOrder(System.Windows.Forms.Control.ControlCollection controls)
    {
        foreach (var c in controls.Cast<System.Windows.Forms.Control>().OrderBy(c => c.Top).ToList())
            c.BringToFront();
    }

    public static System.Windows.Forms.Label AddLabeledRow(
        System.Windows.Forms.Control parent, ref int y, int labelX, int valueX, string caption)
    {
        parent.Controls.Add(new System.Windows.Forms.Label
        {
            Text = caption,
            Location = new System.Drawing.Point(labelX, y),
            AutoSize = true,
            Font = FontBold
        });

        var valueLabel = new System.Windows.Forms.Label
        {
            Text = "--",
            Location = new System.Drawing.Point(valueX, y),
            AutoSize = true,
            Font = FontNormal
        };
        parent.Controls.Add(valueLabel);

        y += 22;
        return valueLabel;
    }
}
