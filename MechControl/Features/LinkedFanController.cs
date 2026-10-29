using MechControl.Monitoring;

namespace MechControl.Features;

class LinkedFanController : IDisposable
{
    private readonly FanControl _fanControl;
    private readonly EcMonitor _monitor;
    private readonly Func<FanCurveModel?> _getCpuCurve;
    private readonly Func<FanCurveModel?> _getGpuCurve;

    private bool _enabled;
    private bool _tickFailed;
    private int _lastLinkedDuty = -1;
    private int _lastCpuRampStart = -1;
    private int _lastGpuRampStart = -1;

    // The EC applies duty[i] above tempUp[i-1], so the always-100% last point takes over
    // above the second-to-last temp. Keep that above sustained load temperatures: at 89°C,
    // a CPU held at 90°C by its temp limit ran its fan at 100% while the GPU fan stayed linked.
    private static readonly int[] FixedTemps = { 19, 29, 39, 49, 57, 65, 73, 81, 97, 101 };

    // Ramp start must be at least this many degrees above the current sensor temp.
    // Without it, a sensor just below a grid point (e.g. 50°C vs the 51°C point)
    // leaves only 1°C of flat headroom before the curve starts ramping toward 100%.
    private const int MinHeadroomCelsius = 4;

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

    public LinkedFanController(FanControl fanControl, EcMonitor monitor,
        Func<FanCurveModel?> getCpuCurve, Func<FanCurveModel?> getGpuCurve)
    {
        _fanControl = fanControl;
        _getCpuCurve = getCpuCurve;
        _getGpuCurve = getGpuCurve;

        _monitor = monitor;
    }

    public void Enable()
    {
        _monitor.FanTemperatureSample -= OnTemperatureSample;
        _enabled = false;
        _lastLinkedDuty = -1;
        // Enable only after fresh curves are written and verified; preserve failures for the caller.
        ApplyTemperatures(_monitor.ReadFanTemperatures());
        _enabled = true;
        _monitor.FanTemperatureSample += OnTemperatureSample;
        _monitor.Start();
    }

    public void Disable()
    {
        _monitor.FanTemperatureSample -= OnTemperatureSample;
        _enabled = false;
        // Write the real panel curves back to EC
        WritePanelCurvesToEc();
        WrittenCpuCurve = null;
        WrittenGpuCurve = null;
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

    private void OnTemperatureSample((int? Cpu, int? Gpu) temperatures)
    {
        if (!_enabled) return;
        try { ApplyTemperatures(temperatures); }
        catch (Exception ex)
        {
            // Log only the first failure of a streak to avoid spam.
            if (!_tickFailed)
                DiagnosticLog.Warn($"LinkedFanController tick failed: {ex.Message} (suppressing repeats)");
            _tickFailed = true;
        }
    }

    private void ApplyTemperatures((int? Cpu, int? Gpu) temperatures)
    {
        var cpuCurve = _getCpuCurve();
        var gpuCurve = _getGpuCurve();
        if (cpuCurve == null || gpuCurve == null) throw new IOException("Linked fan curves unavailable.");
        if (temperatures.Cpu is not (> 0 and < 128) || temperatures.Gpu is not (> 0 and < 128))
            throw new IOException("Linked fan temperatures unavailable; retaining EC safety-ramp curves.");
        CpuTemp = temperatures.Cpu.Value;
        GpuTemp = temperatures.Gpu.Value;

        CpuDuty = EvaluateCurve(cpuCurve, CpuTemp);
        GpuDuty = EvaluateCurve(gpuCurve, GpuTemp);
        LinkedDuty = Math.Max(CpuDuty, GpuDuty);

        // Safety ramp: each curve ramps based on its own sensor temperature,
        // with MinHeadroomCelsius of flat zone before the ramp kicks in.
        int cpuRampStart = FindRampStart(CpuTemp + MinHeadroomCelsius);
        int gpuRampStart = FindRampStart(GpuTemp + MinHeadroomCelsius);
        CpuRampTemp = cpuRampStart < FixedTemps.Length ? FixedTemps[cpuRampStart] : 0;
        GpuRampTemp = gpuRampStart < FixedTemps.Length ? FixedTemps[gpuRampStart] : 0;

        // Skip if duty change is insignificant (< 2% diff, or both in low zone 1-7%)
        // AND neither ramp start has shifted upward (sensor crossed a grid point),
        // which would shrink headroom below the safety floor. Cooling drift
        // (rampStart lower than last-written) is safe to skip.
        bool skip = _lastLinkedDuty >= 0
            && cpuRampStart <= _lastCpuRampStart
            && gpuRampStart <= _lastGpuRampStart
            && (Math.Abs(LinkedDuty - _lastLinkedDuty) < 2 || (LinkedDuty <= 7 && _lastLinkedDuty <= 7));

        if (!skip)
        {
            var cpu = BuildSafeCurve(LinkedDuty, cpuRampStart);
            var gpu = BuildSafeCurve(LinkedDuty, gpuRampStart);
            _lastLinkedDuty = -1; // A failed group must be retried even if temperatures revert.
            _fanControl.ApplyCurves(cpu, gpu, true);
            WrittenCpuCurve = cpu;
            WrittenGpuCurve = gpu;
            _lastLinkedDuty = LinkedDuty;
            _lastCpuRampStart = cpuRampStart;
            _lastGpuRampStart = gpuRampStart;
        }

        Updated?.Invoke();
        _tickFailed = false;
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
            // tempDown[i] = tempUp[i-1] - RangeCelsius (see FanCurveModel.EnforceRange).
            curve.Points[i].TempDown = i > 0 ? Math.Max(0, FixedTemps[i - 1] - FanCurveModel.RangeCelsius) : 0;

            // Point 0 duty pinned to 0 (EC requires duty[0] = 0x00, force-written by WriteCurve).
            if (i == 0)
            {
                curve.Points[i].DutyPercent = 0;
            }
            // Last point always 100% (in effect above FixedTemps[^2]) — preserves the
            // crash-safety tail even when the sensor is hot enough that FindRampStart
            // left no ramp (rampCount <= 1).
            else if (i == FixedTemps.Length - 1)
            {
                curve.Points[i].DutyPercent = 100;
            }
            else if (i < rampStart || rampCount <= 0)
            {
                curve.Points[i].DutyPercent = dutyPercent;
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
        // Leave the EC state as-is on app exit. If linked mode was on, the last-written
        // safety curve (flat at linkedDuty with ramp-to-100 tail) stays in effect —
        // exactly what it was designed for. Writing panel curves here caused a GPU fan
        // cliff-drop while CPU was under load, which the EC interprets as inadequate
        // cooling (tach=0 on GPU fan + high CPU power) and latches CPU at 600 MHz
        // after ~6s. The OEM app has the same bug; we avoid it by not rewriting.
        _monitor.FanTemperatureSample -= OnTemperatureSample;
        _enabled = false;
    }
}
