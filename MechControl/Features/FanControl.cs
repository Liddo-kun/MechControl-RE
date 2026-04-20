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
    }

    public void SetFanBoost(bool enabled)
    {
        _guard.Write(_addrFanControlMode, enabled ? (byte)0x40 : (byte)0x00);
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
        return raw == 0x40;
    }

    public FanCurveModel ReadCpuCurve() =>
        FanCurveModel.ReadFromEc(_device, _cpuTempUpBase, _cpuTempDownBase, _cpuDutyBase, _entriesPerArray, _activeEntries);

    public FanCurveModel ReadGpuCurve() =>
        FanCurveModel.ReadFromEc(_device, _gpuTempUpBase, _gpuTempDownBase, _gpuDutyBase, _entriesPerArray, _activeEntries);

    /// <summary>
    /// Set master enable + latch, then write CPU + GPU fan tables.
    /// Matches probe-verified order: 0x7C5 bit 7, 0x7C6 = 0x04, then table data.
    /// </summary>
    public void ApplyCurves(FanCurveModel cpuCurve, FanCurveModel? gpuCurve, bool enabled)
    {
        if (!_formatSupported) return;

        // Master enable + latch before table (probe-verified sequence)
        _guard.WriteBitfield(_addrFanCurveEnable, _fanCurveEnableMask,
            enabled ? _fanCurveEnableMask : (byte)0x00);
        _guard.Write(AddrFanCurveMode, 0x04);

        WriteCurve(cpuCurve, _cpuTempUpBase, _cpuTempDownBase, _cpuDutyBase);
        if (_hasGpuFan && gpuCurve != null)
            WriteCurve(gpuCurve, _gpuTempUpBase, _gpuTempDownBase, _gpuDutyBase);

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
        // Normalize TempDown before writing (cross-point: tempDown[i+1] = tempUp[i] - 4C).
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
