using MechControl.Features;
using MechControl.Profile;

namespace MechControl.UI;

/// <summary>
/// Read-only live view of the fan curves actually landing in the EC. Owns its
/// own subscription to FanControl.CurvesApplied and unsubscribes on Dispose.
/// Mount it in the stack and toggle Visible; no layout coordination required
/// from the parent.
/// </summary>
class FanCurveDebugSection : Section
{
    private readonly FanControl _control;
    private readonly FanCurvePanel _cpuPanel;
    private readonly FanCurvePanel? _gpuPanel;

    public FanCurveDebugSection(FanControl control, MachineProfile profile, bool hasGpuFan, int panelHeight)
        : base("Fan Curves Written to EC (Live)", fitContent: false)
    {
        _control = control;

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            Height = panelHeight,
            ColumnCount = hasGpuFan ? 2 : 1,
            RowCount = 1,
            Margin = new Padding(0, UiMetrics.RowGap, 0, UiMetrics.RowGap),
        };
        if (hasGpuFan)
        {
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50f));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50f));
        }
        else
        {
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));
        }
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100f));

        _cpuPanel = new FanCurvePanel
        {
            Title = "CPU (Sent to EC)",
            DutyBaseAddress = profile.FanTable.CpuDutyAddr,
            Dock = DockStyle.Fill,
            Margin = new Padding(0, 0, 4, 0),
            ReadOnly = true,
        };
        layout.Controls.Add(_cpuPanel);

        if (hasGpuFan)
        {
            _gpuPanel = new FanCurvePanel
            {
                Title = "GPU (Sent to EC)",
                DutyBaseAddress = profile.FanTable.GpuDutyAddr,
                Dock = DockStyle.Fill,
                Margin = new Padding(4, 0, 0, 0),
                ReadOnly = true,
            };
            layout.Controls.Add(_gpuPanel);
        }

        Controls.Add(layout);
        _control.CurvesApplied += OnCurvesApplied;
    }

    private void OnCurvesApplied(FanCurveModel cpu, FanCurveModel? gpu)
    {
        // Cascade-up Visible returns false when form is hidden to tray, when
        // the Fan tab isn't active, or when this section itself is collapsed
        // (Link Fans off). Skip all work in those cases — no Clone, no Invalidate.
        if (IsDisposed || !IsHandleCreated || !Visible) return;

        // Clone to avoid sharing references with the edit panel / linked controller.
        // Pin duty[0] = 0 to reflect EC state (WriteCurve force-writes 0x00).
        var cpuCopy = cpu.Clone();
        if (cpuCopy.Points.Length > 0) cpuCopy.Points[0].DutyPercent = 0;
        _cpuPanel.Curve = cpuCopy;

        if (_gpuPanel != null && gpu != null)
        {
            var gpuCopy = gpu.Clone();
            if (gpuCopy.Points.Length > 0) gpuCopy.Points[0].DutyPercent = 0;
            _gpuPanel.Curve = gpuCopy;
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
            _control.CurvesApplied -= OnCurvesApplied;
        base.Dispose(disposing);
    }
}
