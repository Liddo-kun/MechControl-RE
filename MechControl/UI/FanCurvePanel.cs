using MechControl.Features;
using MechControl.Profile;

namespace MechControl.UI;

class FanCurvePanel : UserControl
{
    private FanCurveModel? _curve;
    private int _selectedIndex = -1;
    private bool _dragging;
    private string _title = "Fan Curve";
    private readonly ToolTip _tooltip = new();
    private int _tooltipIndex = -1;

    private static readonly System.Drawing.Pen s_gridPen = new(System.Drawing.Color.FromArgb(190, 195, 200), 1);
    private static readonly System.Drawing.Pen s_axisPen = new(System.Drawing.Color.Black, 1);
    private static readonly System.Drawing.Pen s_linePen = new(System.Drawing.Color.RoyalBlue, 2);
    private static readonly System.Drawing.SolidBrush s_fillBrush = new(System.Drawing.Color.FromArgb(30, 65, 105, 225));
    private static readonly System.Drawing.SolidBrush s_pointBrush = new(System.Drawing.Color.RoyalBlue);
    private static readonly System.Drawing.SolidBrush s_selectedPointBrush = new(System.Drawing.Color.OrangeRed);

    public int DutyBaseAddress { get; set; }
    public bool ReadOnly { get; set; }

    // Graph margins
    private const int MarginLeft = 44;
    private const int MarginBottom = 28;
    private const int MarginTop = 24;
    private const int MarginRight = 12;
    private const int PointRadius = 5;
    private const int HitRadius = 10;

    // Axis ranges
    private const int TempMin = 0;
    private const int TempMax = 105;
    private const int DutyMin = 0;
    private const int DutyMax = 100;

    public event Action? CurveChanged;

    public string Title
    {
        get => _title;
        set { _title = value; Invalidate(); }
    }

    public FanCurveModel? Curve
    {
        get => _curve;
        set { _curve = value; Invalidate(); }
    }

    public FanCurvePanel()
    {
        DoubleBuffered = true;
        MinimumSize = new System.Drawing.Size(400, 250);
        BackColor = System.Drawing.Color.White;
        BorderStyle = BorderStyle.FixedSingle;
    }

    // Convert temp,duty to pixel coords
    private System.Drawing.PointF ToPixel(float temp, float duty)
    {
        float graphW = Width - MarginLeft - MarginRight;
        float graphH = Height - MarginTop - MarginBottom;
        float x = MarginLeft + (temp - TempMin) / (TempMax - TempMin) * graphW;
        float y = MarginTop + (1f - (duty - DutyMin) / (DutyMax - DutyMin)) * graphH;
        return new System.Drawing.PointF(x, y);
    }

    // Convert pixel to temp,duty
    private (float temp, float duty) FromPixel(float px, float py)
    {
        float graphW = Width - MarginLeft - MarginRight;
        float graphH = Height - MarginTop - MarginBottom;
        float temp = TempMin + (px - MarginLeft) / graphW * (TempMax - TempMin);
        float duty = DutyMax - (py - MarginTop) / graphH * (DutyMax - DutyMin);
        return (temp, duty);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics;
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;

        float graphW = Width - MarginLeft - MarginRight;
        float graphH = Height - MarginTop - MarginBottom;

        // Title
        g.DrawString(_title, UiHelpers.FontBold, System.Drawing.Brushes.Black, MarginLeft, 4);

        // Graph background
        g.FillRectangle(System.Drawing.Brushes.WhiteSmoke,
            MarginLeft, MarginTop, graphW, graphH);

        var labelBrush = System.Drawing.Brushes.DimGray;

        // Grid + axis labels
        for (int t = 0; t <= TempMax; t += 10)
        {
            var p = ToPixel(t, 0);
            var p2 = ToPixel(t, DutyMax);
            g.DrawLine(s_gridPen, p.X, p2.Y, p.X, p.Y);
            var sz = g.MeasureString(t.ToString(), UiHelpers.FontTiny);
            g.DrawString(t.ToString(), UiHelpers.FontTiny, labelBrush, p.X - sz.Width / 2, p.Y + 2);
        }
        for (int d = 0; d <= DutyMax; d += 10)
        {
            var p = ToPixel(0, d);
            var p2 = ToPixel(TempMax, d);
            g.DrawLine(s_gridPen, p.X, p.Y, p2.X, p.Y);
            var txt = $"{d}%";
            var sz = g.MeasureString(txt, UiHelpers.FontTiny);
            g.DrawString(txt, UiHelpers.FontTiny, labelBrush, MarginLeft - sz.Width - 3, p.Y - sz.Height / 2);
        }

        // Axes
        g.DrawLine(s_axisPen, MarginLeft, MarginTop, MarginLeft, Height - MarginBottom);
        g.DrawLine(s_axisPen, MarginLeft, Height - MarginBottom, Width - MarginRight, Height - MarginBottom);

        // Axis titles
        g.DrawString("\u00B0C", UiHelpers.FontTiny, labelBrush, Width - MarginRight - 12, Height - MarginBottom + 10);

        if (_curve == null || _curve.Points.Length == 0) return;

        // Fill below curve
        var fillPts = new System.Drawing.PointF[_curve.Points.Length + 2];
        for (int i = 0; i < _curve.Points.Length; i++)
            fillPts[i] = ToPixel(_curve.Points[i].TempUp, _curve.Points[i].DutyPercent);
        fillPts[_curve.Points.Length] = ToPixel(_curve.Points[^1].TempUp, 0);
        fillPts[_curve.Points.Length + 1] = ToPixel(_curve.Points[0].TempUp, 0);
        g.FillPolygon(s_fillBrush, fillPts);

        // Lines between points
        for (int i = 0; i < _curve.Points.Length - 1; i++)
        {
            var a = ToPixel(_curve.Points[i].TempUp, _curve.Points[i].DutyPercent);
            var b = ToPixel(_curve.Points[i + 1].TempUp, _curve.Points[i + 1].DutyPercent);
            g.DrawLine(s_linePen, a, b);
        }

        // Points
        for (int i = 0; i < _curve.Points.Length; i++)
        {
            var pt = _curve.Points[i];
            var px = ToPixel(pt.TempUp, pt.DutyPercent);
            var brush = i == _selectedIndex ? s_selectedPointBrush : s_pointBrush;
            g.FillEllipse(brush, px.X - PointRadius, px.Y - PointRadius, PointRadius * 2, PointRadius * 2);
            g.DrawEllipse(s_axisPen, px.X - PointRadius, px.Y - PointRadius, PointRadius * 2, PointRadius * 2);
        }
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (ReadOnly || _curve == null || e.Button != MouseButtons.Left) return;

        // Hit test
        for (int i = 0; i < _curve.Points.Length; i++)
        {
            var px = ToPixel(_curve.Points[i].TempUp, _curve.Points[i].DutyPercent);
            float dx = e.X - px.X;
            float dy = e.Y - px.Y;
            if (dx * dx + dy * dy <= HitRadius * HitRadius)
            {
                _selectedIndex = i;
                _dragging = true;
                Invalidate();
                return;
            }
        }
        _selectedIndex = -1;
        Invalidate();
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (!_dragging || _curve == null || _selectedIndex < 0)
        {
            UpdateHoverTooltip(e.X, e.Y);
            return;
        }

        var (temp, duty) = FromPixel(e.X, e.Y);
        int i = _selectedIndex;
        int newTemp = (int)Math.Round(temp);
        int newDuty = (int)Math.Round(duty);

        // Clamp temp between neighbors. Adjacent points must be at least
        // FanCurveModel.RangeCelsius apart so their hysteresis bands don't overlap.
        // If the loaded curve has neighbors already closer than 2*RangeCelsius
        // (e.g. legacy 6C curve imported from OEM), tempMin > tempMax and this
        // point cannot be moved horizontally without first moving a neighbor.
        // Pin TempUp in that case so duty is still editable and no crash occurs.
        int tempMin = i > 0 ? _curve.Points[i - 1].TempUp + FanCurveModel.RangeCelsius : 0;
        int tempMax = i < _curve.Points.Length - 1 ? _curve.Points[i + 1].TempUp - FanCurveModel.RangeCelsius : TempMax;
        newTemp = tempMin <= tempMax
            ? Math.Clamp(newTemp, tempMin, tempMax)
            : _curve.Points[i].TempUp;

        // Clamp duty — point 0 locked to 0% (EC requires duty[0] = 0x00),
        // all others minimum 1% (EC treats raw 0 as "auto", overrides to ~25%)
        newDuty = i == 0 ? 0 : Math.Clamp(newDuty, 1, DutyMax);

        _curve.Points[i].TempUp = newTemp;
        _curve.Points[i].DutyPercent = newDuty;

        // Push points to the right up if they're below us
        for (int j = i + 1; j < _curve.Points.Length; j++)
        {
            if (_curve.Points[j].DutyPercent < _curve.Points[j - 1].DutyPercent)
                _curve.Points[j].DutyPercent = _curve.Points[j - 1].DutyPercent;
            else
                break;
        }

        // Push points to the left down if they're above us
        for (int j = i - 1; j >= 0; j--)
        {
            if (_curve.Points[j].DutyPercent > _curve.Points[j + 1].DutyPercent)
            {
                _curve.Points[j].DutyPercent = Math.Max(j == 0 ? 0 : 1, _curve.Points[j + 1].DutyPercent);
            }
            else
                break;
        }
        Invalidate();
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        if (_dragging)
        {
            _dragging = false;
            CurveChanged?.Invoke();
        }
    }

    private void UpdateHoverTooltip(int mx, int my)
    {
        if (_curve == null) return;
        for (int i = 0; i < _curve.Points.Length; i++)
        {
            var px = ToPixel(_curve.Points[i].TempUp, _curve.Points[i].DutyPercent);
            float dx = mx - px.X;
            float dy = my - px.Y;
            if (dx * dx + dy * dy <= HitRadius * HitRadius)
            {
                if (_tooltipIndex != i)
                {
                    _tooltipIndex = i;
                    int rawDuty = RegisterEncoding.EncodeFanDuty(_curve.Points[i].DutyPercent);
                    string tip = DutyBaseAddress > 0
                        ? $"Point {i} | Register 0x{DutyBaseAddress + i:X3} | Raw: {rawDuty} | Duty: {_curve.Points[i].DutyPercent}%"
                        : $"Point {i} | Raw: {rawDuty} | Duty: {_curve.Points[i].DutyPercent}%";
                    _tooltip.SetToolTip(this, tip);
                }
                return;
            }
        }
        if (_tooltipIndex >= 0) { _tooltipIndex = -1; _tooltip.SetToolTip(this, ""); }
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        Invalidate();
    }
}
