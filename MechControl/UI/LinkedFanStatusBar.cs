using MechControl.Features;

namespace MechControl.UI;

class LinkedFanStatusBar : Panel
{
    private readonly LinkedFanController _controller;
    private static readonly System.Drawing.Color s_bgColor = System.Drawing.Color.FromArgb(245, 245, 245);
    private static readonly System.Drawing.SolidBrush s_fillBrush = new(System.Drawing.Color.FromArgb(80, 65, 105, 225));

    // Font.Height is cached by the runtime and DPI-correct; avoids a per-paint MeasureString.
    private static readonly int s_mainFontHeight = UiHelpers.FontNormal.Height;
    private static readonly int s_rampFontHeight = UiHelpers.FontTiny.Height;

    public LinkedFanStatusBar(LinkedFanController controller)
    {
        _controller = controller;
        Height = 26;
        Visible = false;
        BorderStyle = BorderStyle.FixedSingle;
        DoubleBuffered = true;
        _controller.Updated += OnControllerUpdated;
    }

    private void OnControllerUpdated()
    {
        if (!IsHandleCreated || !Visible) return;
        BeginInvoke(Invalidate);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        if (!_controller.IsEnabled) return;

        var g = e.Graphics;
        int w = ClientSize.Width;
        int h = ClientSize.Height;

        g.Clear(s_bgColor);
        float fillW = w * _controller.LinkedDuty / 100f;
        g.FillRectangle(s_fillBrush, 0, 0, fillW, h);

        var font = UiHelpers.FontNormal;
        var fontRamp = UiHelpers.FontTiny;
        float mainY = (h - s_mainFontHeight) / 2f;
        float rampY = (h - s_rampFontHeight) / 2f;
        float x = 4;

        void DrawMain(string s)
        {
            g.DrawString(s, font, System.Drawing.Brushes.Black, x, mainY);
            x += g.MeasureString(s, font).Width - 4;
        }
        void DrawRamp(string s)
        {
            g.DrawString(s, fontRamp, System.Drawing.Brushes.DimGray, x, rampY);
            x += g.MeasureString(s, fontRamp).Width - 2;
        }

        DrawMain($"{_controller.LinkedDuty}%  \u2502  CPU {_controller.CpuTemp}\u00B0\u2192{_controller.CpuDuty}% ");
        if (_controller.CpuRampTemp > 0)
            DrawRamp($"(safety ramp {_controller.CpuRampTemp}\u00B0C)");
        DrawMain($"  \u2502  GPU {_controller.GpuTemp}\u00B0\u2192{_controller.GpuDuty}% ");
        if (_controller.GpuRampTemp > 0)
            DrawRamp($"(safety ramp {_controller.GpuRampTemp}\u00B0C)");
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
            _controller.Updated -= OnControllerUpdated;
        base.Dispose(disposing);
    }
}
