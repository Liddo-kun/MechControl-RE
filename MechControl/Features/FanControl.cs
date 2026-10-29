using MechControl.Driver;
using MechControl.Profile;

namespace MechControl.Features;

class FanControl
{
    // Universal Uniwill fan curve mode register — complements 0x7C5 bit 7.
    // "Custom fan curve active" semantically means BOTH bits set:
    //   0x7C5 bit 7 == 1 AND 0x7C6 == 0x04
    private const int AddrFanCurveMode = 0x7C6;

    private readonly EcDevice _device;
    private readonly WriteGuard _guard;

    private readonly int _addrFanControlMode;
    private readonly byte _fanBoostMask;       // 0x40 (bit 6); other bits are EC mode bits
    private readonly int _addrFanCurveEnable;
    private readonly byte _fanCurveEnableMask; // 0x80 (bit 7)

    private readonly int _cpuTempUpBase;
    private readonly int _cpuTempDownBase;
    private readonly int _cpuDutyBase;
    private readonly int _gpuTempUpBase;
    private readonly int _gpuTempDownBase;
    private readonly int _gpuDutyBase;
    private readonly int _activeEntries;
    private readonly int _entriesPerArray;
    private readonly int _paddingTemp;
    private readonly bool _hasGpuFan;
    private readonly bool _formatSupported;

    public bool HasGpuFan => _hasGpuFan;
    public bool FormatSupported => _formatSupported;
    public GpuTemperatureFailsafe? Failsafe { get; set; }
    public bool CanApplyBoost { get; private set; }

    /// <summary>
    /// Fires after ApplyCurves completes. Passes the caller's curve references
    /// (with EnforceRange already applied in-place by WriteCurve). Subscribers
    /// must NOT mutate — Clone first if they need a persistent copy. Duty[0] in
    /// the object may be non-zero even though the EC received 0x00.
    /// </summary>
    public event Action<FanCurveModel, FanCurveModel?>? CurvesApplied;

    public FanControl(EcDevice device, WriteGuard guard, MachineProfile profile)
    {
        _device = device;
        _guard = guard;

        _addrFanControlMode = profile.GetRegister("fanControlMode");
        _fanBoostMask = profile.Bitfields["fanBoost"].GetMask();
        _addrFanCurveEnable = profile.GetRegister("fanCurveEnable");
        _fanCurveEnableMask = profile.Bitfields["fanCurveEnable"].GetMask();

        var ft = profile.FanTable;
        _cpuTempUpBase = ft.CpuTempUpAddr;
        _cpuTempDownBase = ft.CpuTempDownAddr;
        _cpuDutyBase = ft.CpuDutyAddr;
        _gpuTempUpBase = ft.GpuTempUpAddr;
        _gpuTempDownBase = ft.GpuTempDownAddr;
        _gpuDutyBase = ft.GpuDutyAddr;
        _activeEntries = ft.ActiveEntries;
        _entriesPerArray = ft.EntriesPerArray;
        _paddingTemp = ft.PaddingTemp;
        _hasGpuFan = ft.HasGpuFan;
        _formatSupported = ft.Format == "RamFan1";
        CanApplyBoost = profile.ExpandAllowlist().Contains(_addrFanControlMode);
    }

    public void ValidateFailsafeRegisters()
    {
        // Any value is plausible (bits 4 and 7 are EC mode bits); 0xFF reads as unprogrammed.
        try { CanApplyBoost &= _device.ReadByte(_addrFanControlMode) != 0xFF; }
        catch { CanApplyBoost = false; }
    }

    // Turning boost on only adds cooling, so it is never gated by the failsafe.
    public void SetFanBoost(bool enabled)
    {
        try
        {
            if (!enabled) Failsafe?.CheckBeforeApply();
            if (Failsafe?.Holds(ThermalAction.Fan) == true)
            {
                if (!CanApplyBoost) throw new InvalidOperationException("Held fan boost unavailable; fan boost changes blocked.");
                enabled = true;
            }
            _guard.WriteBitfield(_addrFanControlMode, _fanBoostMask, enabled ? _fanBoostMask : (byte)0x00);
        }
        catch (Exception ex) { Failsafe?.ReportWriteFailure(ThermalAction.Fan, ex); throw; }
    }

    /// <summary>
    /// Set custom fan curve master enable on/off. Only writes 0x7C5 bit 7.
    /// Prefer ApplyCurves(cpu, gpu, enabled) which handles both table + enable.
    /// This standalone method is only for toggling without rewriting tables.
    /// </summary>
    public void SetCustomFanCurveMode(bool enabled)
    {
        _guard.WriteBitfield(_addrFanCurveEnable, _fanCurveEnableMask,
            enabled ? _fanCurveEnableMask : (byte)0x00);
    }

    /// <summary>
    /// Read whether custom fan curve mode is currently active.
    /// Requires BOTH 0x7C5 bit 7 = 1 AND 0x7C6 = 0x04.
    /// </summary>
    public bool ReadCustomFanCurveMode()
    {
        byte enable = _device.ReadByte(_addrFanCurveEnable);
        byte mode = _device.ReadByte(AddrFanCurveMode);
        return (enable & _fanCurveEnableMask) != 0 && mode == 0x04;
    }

    public bool ReadFanBoost()
    {
        byte raw = _device.ReadByte(_addrFanControlMode);
        return (raw & _fanBoostMask) != 0;
    }

    public bool IsBoostRequested() => CanApplyBoost && _guard.VerifiesSameAddress(_addrFanControlMode) && ReadFanBoost();

    public FanCurveModel ReadCpuCurve() =>
        FanCurveModel.ReadFromEc(_device, _cpuTempUpBase, _cpuTempDownBase, _cpuDutyBase, _entriesPerArray, _activeEntries);

    public FanCurveModel ReadGpuCurve() =>
        FanCurveModel.ReadFromEc(_device, _gpuTempUpBase, _gpuTempDownBase, _gpuDutyBase, _entriesPerArray, _activeEntries);

    /// <summary>
    /// Set master enable, then write CPU + GPU fan tables bracketed by the
    /// 0x7C6 latch. The 0x04 → 0x00 → 0x04 transition forces the EC to drop
    /// any cached fan-curve state and re-evaluate current temp against the
    /// fresh table; without it the EC is lazy and holds the previously-
    /// computed duty until the next threshold crossing, which makes rapid
    /// curve swaps look "stuck" at the old value.
    /// </summary>
    public void ApplyCurves(FanCurveModel cpuCurve, FanCurveModel? gpuCurve, bool enabled)
    {
        if (!_formatSupported) return;

        // Master enable — separate concern from the table/latch.
        _guard.WriteBitfield(_addrFanCurveEnable, _fanCurveEnableMask,
            enabled ? _fanCurveEnableMask : (byte)0x00);

        // Deactivate latch so the EC won't evaluate a half-written table.
        _guard.Write(AddrFanCurveMode, 0x00);

        WriteCurve(cpuCurve, _cpuTempUpBase, _cpuTempDownBase, _cpuDutyBase);
        if (_hasGpuFan && gpuCurve != null)
            WriteCurve(gpuCurve, _gpuTempUpBase, _gpuTempDownBase, _gpuDutyBase);

        // Re-activate latch — transition from 0x00 forces EC re-evaluation.
        _guard.Write(AddrFanCurveMode, 0x04);

        CurvesApplied?.Invoke(cpuCurve, _hasGpuFan ? gpuCurve : null);
    }

    private void WriteCurve(FanCurveModel curve, int tempUpBase, int tempDownBase, int dutyBase)
    {
        if (!_formatSupported)
        {
            DiagnosticLog.Warn("Fan curve write blocked: fan table format is not RamFan1");
            return;
        }
        if (curve.ActiveEntries == 0)
        {
            DiagnosticLog.Warn("Fan curve write blocked: curve has 0 active entries");
            return;
        }
        // Recompute TempDown from TempUp so writes always reflect the current
        // curve (tempDown[i] = tempUp[i-1] - RangeCelsius).
        curve.EnforceRange();

        byte paddingRawDuty = RegisterEncoding.EncodeFanDuty(curve.Points[curve.ActiveEntries - 1].DutyPercent);
        if (paddingRawDuty == 0) paddingRawDuty = 2;

        int active = curve.ActiveEntries;

        // Sequential array writes matching probe-verified order:
        //   all tempUp, all tempDown (skip [0]), all duty

        // --- tempUp ---
        for (int i = 0; i < active; i++)
            _guard.Write(tempUpBase + i, (byte)curve.Points[i].TempUp);
        for (int i = active; i < _entriesPerArray; i++)
            _guard.Write(tempUpBase + i, (byte)_paddingTemp);

        // --- tempDown (skip index 0 — OEM never writes it) ---
        for (int i = 1; i < active; i++)
            _guard.Write(tempDownBase + i, (byte)curve.Points[i].TempDown);
        // First padding tempDown: last active tempDown + 4 (OEM bridge value)
        if (active < _entriesPerArray)
            _guard.Write(tempDownBase + active,
                (byte)Math.Min(255, curve.Points[active - 1].TempDown + 4));
        for (int i = active + 1; i < _entriesPerArray; i++)
            _guard.Write(tempDownBase + i, (byte)_paddingTemp);

        // --- duty (index 0 = 0x00, EC requires it) ---
        _guard.Write(dutyBase, 0x00);
        for (int i = 1; i < active; i++)
        {
            byte rawDuty = RegisterEncoding.EncodeFanDuty(curve.Points[i].DutyPercent);
            if (rawDuty == 0) rawDuty = 2;
            _guard.Write(dutyBase + i, rawDuty);
        }
        for (int i = active; i < _entriesPerArray; i++)
            _guard.Write(dutyBase + i, paddingRawDuty);
    }
}
