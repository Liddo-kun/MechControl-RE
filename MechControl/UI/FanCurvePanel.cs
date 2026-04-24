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
    private static readonly System.Drawing.Pen s_hysteresisPen = new(
        System.Drawing.Color.FromArgb(130, 65, 105, 225), 1.5f)
    {
        DashStyle = System.Drawing.Drawing2D.DashStyle.Dash,
    };
    private static readonly System.Drawing.Pen s_currentTempPen = new(
        System.Drawing.Color.FromArgb(200, 220, 80, 80), 1.5f)
    {
        DashStyle = System.Drawing.Drawing2D.DashStyle.Dot,
    };
    private static readonly System.Drawing.SolidBrush s_fillBrush = new(System.Drawing.Color.FromArgb(30, 65, 105, 225));
    private static readonly System.Drawing.SolidBrush s_pointBrush = new(System.Drawing.Color.RoyalBlue);
    private static readonly System.Drawing.SolidBrush s_selectedPointBrush = new(System.Drawing.Color.OrangeRed);

    public int DutyBaseAddress { get; set; }
    public bool ReadOnly { get; set; }

    /// <summary>
    /// When true, draw straight line segments between points (implies linear
    /// interpolation) and anchor each drag handle at (tempUp[i], duty[i]).
    /// When false (default), draw the curve as a step function matching how
    /// the EC actually evaluates the table: duty[i] applies when
    /// tempUp[i-1] &lt; temp ≤ tempUp[i], with the vertical step at tempUp[i]
    /// jumping to duty[i+1] going up. In step mode the handle anchor shifts
    /// to the step-up corner (tempUp[i], duty[i+1]) for i &lt; n-1 so that
    /// "dragging a point to (50°C, 80%)" reads as "crossing 50°C jumps to
    /// 80%" — what users expect. Handle n-1 stays on the plateau's right
    /// edge, so it shares its duty value with handle n-2 (both edit
    /// duty[n-1]); moving either slides the whole top plateau.
    /// Interpolated mode is only honest when an external software loop
    /// samples the curve (Link Fans); in stock EC mode the step view
    /// reflects real fan behavior.
    /// </summary>
    public bool InterpolatedRendering { get; set; }

    private int? _currentTempC;

    /// <summary>
    /// Current measured temperature in °C. When set and in range, a dotted
    /// light-red vertical line is drawn at that temp. Set null to hide.
    /// Invalidate is skipped when the panel isn't Visible, so pushing updates
    /// while the fan tab is inactive or the form is in the tray is free.
    /// </summary>
    public int? CurrentTempC
    {
        get => _currentTempC;
        set
        {
            if (_currentTempC == value) return;
            _currentTempC = value;
            if (Visible) Invalidate();
        }
    }

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
        set
        {
            // Normalize tempDown on assignment so the dashed hysteresis line
            // is correct from the first paint, regardless of whether the
            // incoming curve came from a stale profile, EC read, or snapshot.
            value?.EnforceRange();
            _curve = value;
            Invalidate();
        }
    }

    public const int MinWidth = 400;
    public const int MinHeight = 250;

    public FanCurvePanel()
    {
        DoubleBuffered = true;
        MinimumSize = new System.Drawing.Size(MinWidth, MinHeight);
        BackColor = System.Drawing.Color.White;
        BorderStyle = BorderStyle.FixedSingle;
    }

    /// <summary>
    /// Index into Points[] of the duty value edited by handle <paramref name="handleIdx"/>.
    /// In interpolated mode it's the identity. In step mode, handle i sits at the
    /// step-up corner and its Y = duty[i+1]; the final handle has no "next zone",
    /// so it shares duty[n-1] with handle n-2.
    /// </summary>
    private int HandleDutyIndex(int handleIdx)
    {
        if (_curve == null || InterpolatedRendering) return handleIdx;
        return Math.Min(handleIdx + 1, _curve.Points.Length - 1);
    }

    private System.Drawing.PointF HandlePixel(int handleIdx)
    {
        var p = _curve!.Points[handleIdx];
        float duty = _curve.Points[HandleDutyIndex(handleIdx)].DutyPercent;
        return ToPixel(p.TempUp, duty);
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

        int n = _curve.Points.Length;

        if (InterpolatedRendering)
        {
            // Diagonal fill + lines (implies linear interpolation — only honest
            // under Link Fans, where a software loop really does interpolate).
            var fillPts = new System.Drawing.PointF[n + 2];
            for (int i = 0; i < n; i++)
                fillPts[i] = ToPixel(_curve.Points[i].TempUp, _curve.Points[i].DutyPercent);
            fillPts[n] = ToPixel(_curve.Points[n - 1].TempUp, 0);
            fillPts[n + 1] = ToPixel(_curve.Points[0].TempUp, 0);
            g.FillPolygon(s_fillBrush, fillPts);

            for (int i = 0; i < n - 1; i++)
            {
                var a = ToPixel(_curve.Points[i].TempUp, _curve.Points[i].DutyPercent);
                var b = ToPixel(_curve.Points[i + 1].TempUp, _curve.Points[i + 1].DutyPercent);
                g.DrawLine(s_linePen, a, b);
            }
        }
        else
        {
            // Step-function rendering — matches EC behavior under the right-
            // point rule: duty[i] applies when tempUp[i-1] < temp ≤ tempUp[i].
            // So the drag point (tempUp[i], duty[i]) sits at the TOP-RIGHT
            // corner of its duty zone. Going up, the vertical step at
            // tempUp[i] jumps from duty[i] to duty[i+1] (entering the next
            // zone). Horizontal at duty[i+1] spans (tempUp[i], tempUp[i+1]].
            var fillPath = new List<System.Drawing.PointF>
            {
                ToPixel(_curve.Points[0].TempUp, 0),
                ToPixel(_curve.Points[0].TempUp, _curve.Points[0].DutyPercent),
            };
            for (int i = 0; i < n - 1; i++)
            {
                fillPath.Add(ToPixel(_curve.Points[i].TempUp, _curve.Points[i + 1].DutyPercent));
                fillPath.Add(ToPixel(_curve.Points[i + 1].TempUp, _curve.Points[i + 1].DutyPercent));
            }
            fillPath.Add(ToPixel(_curve.Points[n - 1].TempUp, 0));
            g.FillPolygon(s_fillBrush, fillPath.ToArray());

            for (int i = 0; i < n - 1; i++)
            {
                var a = ToPixel(_curve.Points[i].TempUp, _curve.Points[i].DutyPercent);
                var corner = ToPixel(_curve.Points[i].TempUp, _curve.Points[i + 1].DutyPercent);
                var b = ToPixel(_curve.Points[i + 1].TempUp, _curve.Points[i + 1].DutyPercent);
                g.DrawLine(s_linePen, a, corner);
                g.DrawLine(s_linePen, corner, b);
            }

            // Hysteresis / going-down staircase using tempDown thresholds.
            // Traced right-to-left: at temps above tempDown[i] the EC stays at
            // duty[i]; when temp drops below tempDown[i] it falls to duty[i-1].
            // Dashed and lighter so it reads as "the return path" rather than
            // competing with the primary curve. TempDown is kept current by
            // OnMouseMove calling EnforceRange after each drag.
            if (n >= 2)
            {
                var down = ToPixel(_curve.Points[n - 1].TempUp, _curve.Points[n - 1].DutyPercent);
                for (int i = n - 1; i >= 1; i--)
                {
                    var corner = ToPixel(_curve.Points[i].TempDown, _curve.Points[i].DutyPercent);
                    g.DrawLine(s_hysteresisPen, down, corner);
                    var next = ToPixel(_curve.Points[i].TempDown, _curve.Points[i - 1].DutyPercent);
                    g.DrawLine(s_hysteresisPen, corner, next);
                    down = next;
                }
                // Close to the solid line's left endpoint at 0%
                g.DrawLine(s_hysteresisPen, down, ToPixel(_curve.Points[0].TempUp, 0));
            }
        }

        // Current-temperature indicator (dotted vertical line, painted under
        // the point markers so markers remain legible when they overlap).
        if (_currentTempC.HasValue && _currentTempC.Value > TempMin && _currentTempC.Value <= TempMax)
        {
            var top = ToPixel(_currentTempC.Value, DutyMax);
            var bot = ToPixel(_currentTempC.Value, DutyMin);
            g.DrawLine(s_currentTempPen, top.X, top.Y, bot.X, bot.Y);
        }

        // Points (anchor depends on render mode — see HandleDutyIndex doc).
        for (int i = 0; i < _curve.Points.Length; i++)
        {
            var px = HandlePixel(i);
            var brush = i == _selectedIndex ? s_selectedPointBrush : s_pointBrush;
            g.FillEllipse(brush, px.X - PointRadius, px.Y - PointRadius, PointRadius * 2, PointRadius * 2);
            g.DrawEllipse(s_axisPen, px.X - PointRadius, px.Y - PointRadius, PointRadius * 2, PointRadius * 2);
        }
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (ReadOnly || _curve == null || e.Button != MouseButtons.Left) return;

        // Hit test — iterate in reverse so the last (rightmost) handle wins
        // when handle n-2 and n-1 overlap at the top-right corner of the
        // plateau (they share duty[n-1] and differ only in X).
        for (int i = _curve.Points.Length - 1; i >= 0; i--)
        {
            var px = HandlePixel(i);
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
        int dutyIdx = HandleDutyIndex(i);
        int newTemp = (int)Math.Round(temp);
        int newDuty = (int)Math.Round(duty);

        // Clamp temp between neighbors. Adjacent points must be at least
        // FanCurveModel.RangeCelsius apart so each zone gets a well-defined
        // hysteresis band. If an imported curve has neighbors closer than
        // 2*RangeCelsius, tempMin > tempMax and this point can't move
        // horizontally until a neighbor does; pin TempUp so duty is still
        // editable and no crash occurs.
        int tempMin = i > 0 ? _curve.Points[i - 1].TempUp + FanCurveModel.RangeCelsius : 0;
        int tempMax = i < _curve.Points.Length - 1 ? _curve.Points[i + 1].TempUp - FanCurveModel.RangeCelsius : TempMax;
        newTemp = tempMin <= tempMax
            ? Math.Clamp(newTemp, tempMin, tempMax)
            : _curve.Points[i].TempUp;

        // Clamp duty — duty[0] locked to 0% (EC requires duty[0] = 0x00);
        // all others minimum 1% (raw 0 triggers EC auto-override to ~25%).
        // In step mode handle 0 edits duty[1], so dutyIdx == 0 only occurs
        // in interpolated mode when i == 0.
        newDuty = dutyIdx == 0 ? 0 : Math.Clamp(newDuty, 1, DutyMax);

        _curve.Points[i].TempUp = newTemp;
        _curve.Points[dutyIdx].DutyPercent = newDuty;

        // Monotonicity: keep duty non-decreasing by index. Pushes fan out
        // from dutyIdx (the value we just wrote), not from the handle index.
        for (int j = dutyIdx + 1; j < _curve.Points.Length; j++)
        {
            if (_curve.Points[j].DutyPercent < _curve.Points[j - 1].DutyPercent)
                _curve.Points[j].DutyPercent = _curve.Points[j - 1].DutyPercent;
            else
                break;
        }
        for (int j = dutyIdx - 1; j >= 0; j--)
        {
            if (_curve.Points[j].DutyPercent > _curve.Points[j + 1].DutyPercent)
            {
                _curve.Points[j].DutyPercent = Math.Max(j == 0 ? 0 : 1, _curve.Points[j + 1].DutyPercent);
            }
            else
                break;
        }

        // Re-derive TempDown so the hysteresis line tracks the drag in real
        // time. EnforceRange is the sole owner of the tempDown formula.
        _curve.EnforceRange();

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
        for (int i = _curve.Points.Length - 1; i >= 0; i--)
        {
            var px = HandlePixel(i);
            float dx = mx - px.X;
            float dy = my - px.Y;
            if (dx * dx + dy * dy <= HitRadius * HitRadius)
            {
                if (_tooltipIndex != i)
                {
                    _tooltipIndex = i;
                    int dutyIdx = HandleDutyIndex(i);
                    int dutyPct = _curve.Points[dutyIdx].DutyPercent;
                    int tempC = _curve.Points[i].TempUp;
                    int rawDuty = RegisterEncoding.EncodeFanDuty(dutyPct);
                    string tip = DutyBaseAddress > 0
                        ? $"{tempC}°C → {dutyPct}% | Register 0x{DutyBaseAddress + dutyIdx:X3} (Raw {rawDuty})"
                        : $"{tempC}°C → {dutyPct}% (Raw {rawDuty})";
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

    protected override void Dispose(bool disposing)
    {
        if (disposing)
            _tooltip.Dispose();
        base.Dispose(disposing);
    }
}
