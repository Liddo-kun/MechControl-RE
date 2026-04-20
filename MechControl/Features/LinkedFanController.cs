using MechControl.Driver;
using MechControl.Profile;

namespace MechControl.Features;

class LinkedFanController : IDisposable
{
    private readonly EcDevice _device;
    private readonly FanControl _fanControl;
    private readonly int _addrCpuTemp;
    private readonly int _addrGpuTemp;
    private readonly System.Windows.Forms.Timer _timer;
    private readonly Func<FanCurveModel?> _getCpuCurve;
    private readonly Func<FanCurveModel?> _getGpuCurve;

    private bool _enabled;
    private int _lastLinkedDuty = -1;

    private static readonly int[] FixedTemps = { 10, 16, 33, 42, 51, 60, 69, 78, 87, 96 };

    public int CpuTemp { get; private set; }
    public int GpuTemp { get; private set; }
    public int CpuDuty { get; private set; }
    public int GpuDuty { get; private set; }
    public int LinkedDuty { get; private set; }
    public int CpuRampTemp { get; private set; }
    public int GpuRampTemp { get; private set; }
    public bool IsEnabled => _enabled;
    public FanCurveModel? WrittenCpuCurve { get; private set; }
    public FanCurveModel? WrittenGpuCurve { get; private set; }

    public event Action? Updated;

    public LinkedFanController(EcDevice device, FanControl fanControl, MachineProfile profile,
        Func<FanCurveModel?> getCpuCurve, Func<FanCurveModel?> getGpuCurve)
    {
        _device = device;
        _fanControl = fanControl;
        _addrCpuTemp = profile.GetRegister("cpuTemp");
        _addrGpuTemp = profile.GetRegister("gpuTemp");
        _getCpuCurve = getCpuCurve;
        _getGpuCurve = getGpuCurve;

        _timer = new System.Windows.Forms.Timer { Interval = 1500 };
        _timer.Tick += OnTick;
    }

    public void Enable()
    {
        _enabled = true;
        _lastLinkedDuty = -1;
        OnTick(null, EventArgs.Empty);
        _timer.Start();
    }

    public void Disable()
    {
        _timer.Stop();
        _enabled = false;
        // Write the real panel curves back to EC
        WritePanelCurvesToEc();
        WrittenCpuCurve = null;
        WrittenGpuCurve = null;
        _lastLinkedDuty = -1;
        Updated?.Invoke();
    }

    /// <summary>
    /// Write the current panel curves to EC. Used on disable and when
    /// external code needs to sync panel state to hardware.
    /// </summary>
    public void WritePanelCurvesToEc()
    {
        var cpu = _getCpuCurve();
        var gpu = _getGpuCurve();
        if (cpu != null)
            _fanControl.ApplyCurves(cpu.Clone(), gpu?.Clone(), true);
    }

    private void OnTick(object? sender, EventArgs e)
    {
        if (!_enabled) return;

        var cpuCurve = _getCpuCurve();
        var gpuCurve = _getGpuCurve();
        if (cpuCurve == null || gpuCurve == null) return;

        try
        {
            CpuTemp = _device.ReadByte(_addrCpuTemp);
            GpuTemp = _device.ReadByte(_addrGpuTemp);

            CpuDuty = EvaluateCurve(cpuCurve, CpuTemp);
            GpuDuty = EvaluateCurve(gpuCurve, GpuTemp);
            LinkedDuty = Math.Max(CpuDuty, GpuDuty);

            // Safety ramp: each curve ramps based on its own sensor temperature
            int cpuRampStart = FindRampStart(CpuTemp);
            int gpuRampStart = FindRampStart(GpuTemp);
            CpuRampTemp = cpuRampStart < FixedTemps.Length ? FixedTemps[cpuRampStart] : 0;
            GpuRampTemp = gpuRampStart < FixedTemps.Length ? FixedTemps[gpuRampStart] : 0;

            // Skip if duty change is insignificant (< 2% diff, or both in low zone 1-7%)
            bool skip = _lastLinkedDuty >= 0
                && (Math.Abs(LinkedDuty - _lastLinkedDuty) < 2 || (LinkedDuty <= 7 && _lastLinkedDuty <= 7));

            if (!skip)
            {
                WrittenCpuCurve = BuildSafeCurve(LinkedDuty, cpuRampStart);
                WrittenGpuCurve = BuildSafeCurve(LinkedDuty, gpuRampStart);
                _fanControl.ApplyCurves(WrittenCpuCurve, WrittenGpuCurve, true);
                _lastLinkedDuty = LinkedDuty;
            }

            Updated?.Invoke();
        }
        catch (Exception ex)
        {
            Console.WriteLine($"LinkedFanController tick failed: {ex.Message}");
        }
    }

    public static int EvaluateCurve(FanCurveModel curve, int temp)
    {
        if (curve.Points.Length == 0) return 0;

        if (temp <= curve.Points[0].TempUp)
            return curve.Points[0].DutyPercent;

        if (temp >= curve.Points[^1].TempUp)
            return curve.Points[^1].DutyPercent;

        for (int i = 0; i < curve.Points.Length - 1; i++)
        {
            if (temp <= curve.Points[i + 1].TempUp)
            {
                int t0 = curve.Points[i].TempUp, t1 = curve.Points[i + 1].TempUp;
                int d0 = curve.Points[i].DutyPercent, d1 = curve.Points[i + 1].DutyPercent;
                if (t1 == t0) return d0;
                return (int)Math.Round(d0 + (float)(temp - t0) / (t1 - t0) * (d1 - d0));
            }
        }

        return curve.Points[^1].DutyPercent;
    }

    /// <summary>
    /// Index of first FixedTemp above safeTemp, or FixedTemps.Length if none.
    /// </summary>
    private static int FindRampStart(int safeTemp)
    {
        for (int i = 0; i < FixedTemps.Length; i++)
            if (FixedTemps[i] > safeTemp)
                return i;
        return FixedTemps.Length;
    }

    /// <summary>
    /// Build curve: flat at dutyPercent below rampStart, then linear ramp to 100%.
    /// If the app crashes, EC keeps this curve — fans ramp up if temps rise.
    /// </summary>
    private static FanCurveModel BuildSafeCurve(int dutyPercent, int rampStart)
    {
        dutyPercent = Math.Clamp(dutyPercent, 1, 100);
        var curve = new FanCurveModel(FixedTemps.Length);
        int rampCount = FixedTemps.Length - rampStart;

        for (int i = 0; i < FixedTemps.Length; i++)
        {
            curve.Points[i].TempUp = FixedTemps[i];
            // Cross-point hysteresis (matches FanCurveModel.EnforceRange, OEM convention):
            // tempDown[i] = tempUp[i-1] - RangeCelsius. Point 0 pinned to 0 (not written to EC).
            curve.Points[i].TempDown = i > 0 ? Math.Max(0, FixedTemps[i - 1] - FanCurveModel.RangeCelsius) : 0;

            // Point 0 duty pinned to 0 (EC requires duty[0] = 0x00, force-written by WriteCurve).
            if (i == 0)
            {
                curve.Points[i].DutyPercent = 0;
            }
            else if (i < rampStart || rampCount <= 0)
            {
                curve.Points[i].DutyPercent = dutyPercent;
            }
            else if (rampCount == 1)
            {
                curve.Points[i].DutyPercent = 100;
            }
            else
            {
                float t = (float)(i - rampStart) / (rampCount - 1);
                curve.Points[i].DutyPercent = (int)Math.Round(dutyPercent + t * (100 - dutyPercent));
            }
        }
        return curve;
    }

    public void Dispose()
    {
        _timer.Stop();
        if (_enabled)
        {
            _enabled = false;
            try { WritePanelCurvesToEc(); } catch { }
        }
        _timer.Dispose();
    }
}
