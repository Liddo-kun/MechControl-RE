using MechControl.Driver;
using MechControl.Profile;

namespace MechControl.Features;

class CpuPowerControl
{
    private readonly EcDevice _device;
    private readonly WriteGuard _guard;

    private readonly int _addrPL1;
    private readonly int _addrPL2;
    private readonly int _addrPL4;
    private readonly int _addrTempWrite;
    private readonly int _addrTempRead;
    private readonly bool _absoluteTemp;
    private readonly bool _pl4Doubled;
    private readonly int _tjMax;
    private readonly int _tempMin;
    private readonly int _tempMax;
    private readonly LimitInfo _pl1Limit, _pl2Limit, _pl4Limit;
    public GpuTemperatureFailsafe? Failsafe { get; set; }
    public bool CanApplyMinimum { get; private set; }
    public CpuPowerValues Minimum { get; }

    public CpuPowerControl(EcDevice device, WriteGuard guard, MachineProfile profile)
    {
        _device = device;
        _guard = guard;
        _addrPL1 = profile.GetRegister("PL1");
        _addrPL2 = profile.GetRegister("PL2");
        _addrPL4 = profile.GetRegister("PL4");
        _addrTempWrite = profile.GetRegister("cpuTempLimitWrite");
        _addrTempRead = profile.GetRegister("cpuTempLimitRead");
        _absoluteTemp = profile.CpuTempEncoding == "direct";
        _pl4Doubled = profile.Pl4Doubled;
        _pl1Limit = profile.Limits.GetValueOrDefault("PL1") ?? UniversalDefaults.ConservativeLimits["PL1"];
        _pl2Limit = profile.Limits.GetValueOrDefault("PL2") ?? UniversalDefaults.ConservativeLimits["PL2"];
        _pl4Limit = profile.Limits.GetValueOrDefault("PL4") ?? UniversalDefaults.ConservativeLimits["PL4"];

        // Temp limit bounds come from the profile, not encoding defaults —
        // TjMax and the allowed range differ per model.
        _tjMax = profile.TjMax;
        if (profile.Limits.TryGetValue("cpuTempLimit", out var tempLimit))
        {
            _tempMin = tempLimit.Min;
            _tempMax = tempLimit.Max;
        }
        else
        {
            _tempMin = 80;
            _tempMax = _absoluteTemp ? 120 : 95;
        }
        if (!profile.CpuLimitsAreFallback && profile.Limits.TryGetValue("PL1", out var p1) &&
            profile.Limits.TryGetValue("PL2", out var p2) && profile.Limits.TryGetValue("PL4", out var p4))
        {
            Minimum = new() { PL1Watts = p1.Min, PL2Watts = p2.Min, PL4Watts = p4.Min };
            CanApplyMinimum = new[] { p1, p2, p4 }.All(p => p.Min > 0 && p.Min <= p.Max) &&
                p1.Encoding == "direct" && p2.Encoding == "direct" && p4.Encoding is "direct" or "double" &&
                RegisterEncoding.DecodePL1(RegisterEncoding.EncodePL1(p1.Min)) == p1.Min &&
                RegisterEncoding.DecodePL1(RegisterEncoding.EncodePL1(p2.Min)) == p2.Min &&
                RegisterEncoding.DecodePL4(RegisterEncoding.EncodePL4(p4.Min, _pl4Doubled), _pl4Doubled) == p4.Min &&
                new[] { _addrPL1, _addrPL2, _addrPL4, _addrTempWrite }.All(profile.ExpandAllowlist().Contains);
        }
    }

    public void ValidateFailsafeRegisters()
    {
        if (!CanApplyMinimum) return;
        try
        {
            CanApplyMinimum = new[] { _addrPL1, _addrPL2, _addrPL4 }.All(a => _device.ReadByte(a) != 255);
            ReadEmergencyTemperatureLimit();
        }
        catch { CanApplyMinimum = false; }
    }

    private int ReadEmergencyTemperatureLimit()
    {
        byte raw = _device.ReadByte(_addrTempRead);
        int value = RegisterEncoding.DecodeCpuTempLimit(raw, _absoluteTemp, _tjMax);
        byte encoded = _absoluteTemp ? RegisterEncoding.EncodeCpuTempLimitDirect(value) : RegisterEncoding.EncodeCpuTempLimit(value, _tjMax);
        if (raw == 255 || value < _tempMin || value > _tempMax || (encoded & 0x7F) != (raw & 0x7F))
            throw new IOException("Current CPU temperature limit is invalid; refusing to replace it with a fallback.");
        return value;
    }

    public void ApplyMinimumPower()
    {
        if (!CanApplyMinimum) throw new InvalidOperationException("Model-specific CPU minimum/temperature-limit capability unavailable.");
        ApplyCpuPower(Minimum.PL1Watts, Minimum.PL2Watts, Minimum.PL4Watts, null);
    }

    // Only a previously verified hold may use this read-only maintenance check.
    // A fresh valid temperature limit is preserved, including firmware changes.
    public bool IsMinimumPowerRequested()
    {
        if (!CanApplyMinimum || !_guard.VerifiesSameAddress(_addrPL1) ||
            !_guard.VerifiesSameAddress(_addrPL2) || !_guard.VerifiesSameAddress(_addrPL4)) return false;
        ReadEmergencyTemperatureLimit();
        return _device.ReadByte(_addrPL1) == RegisterEncoding.EncodePL1(Minimum.PL1Watts) &&
            _device.ReadByte(_addrPL2) == RegisterEncoding.EncodePL1(Minimum.PL2Watts) &&
            _device.ReadByte(_addrPL4) == RegisterEncoding.EncodePL4(Minimum.PL4Watts, _pl4Doubled);
    }

    /// <summary>
    /// Preflight the complete CPU group before any of its writes.
    /// </summary>
    public void ValidatePower(int pl1Watts, int pl2Watts, int pl4Watts, int tempLimitC)
    {
        int effectivePl4 = RegisterEncoding.DecodePL4(RegisterEncoding.EncodePL4(pl4Watts, _pl4Doubled), _pl4Doubled);
        if (_pl1Limit.Encoding != "direct" || _pl2Limit.Encoding != "direct" || _pl4Limit.Encoding is not ("direct" or "double") ||
            pl1Watts < _pl1Limit.Min || pl1Watts > _pl1Limit.Max || RegisterEncoding.EncodePL1(pl1Watts) != pl1Watts ||
            pl2Watts < _pl2Limit.Min || pl2Watts > _pl2Limit.Max || RegisterEncoding.EncodePL1(pl2Watts) != pl2Watts ||
            pl4Watts < _pl4Limit.Min || pl4Watts > _pl4Limit.Max || effectivePl4 < _pl4Limit.Min || effectivePl4 > _pl4Limit.Max ||
            effectivePl4 != (_pl4Doubled ? pl4Watts / 2 * 2 : pl4Watts))
            throw new ArgumentOutOfRangeException("power", $"CPU request {pl1Watts}/{pl2Watts}/{pl4Watts} W exceeds model ranges " +
                $"{_pl1Limit.Min}-{_pl1Limit.Max}/{_pl2Limit.Min}-{_pl2Limit.Max}/{_pl4Limit.Min}-{_pl4Limit.Max} W or supported encoding. Doubled PL4 rounds down to an even wattage within model limits.");
        byte temp = _absoluteTemp ? RegisterEncoding.EncodeCpuTempLimitDirect(tempLimitC) : RegisterEncoding.EncodeCpuTempLimit(tempLimitC, _tjMax);
        int effectiveTemp = RegisterEncoding.DecodeCpuTempLimit(temp, _absoluteTemp, _tjMax);
        if (tempLimitC < _tempMin || tempLimitC > _tempMax || effectiveTemp != tempLimitC)
            throw new ArgumentOutOfRangeException(nameof(tempLimitC), "CPU temperature limit exceeds model limits or supported encoding.");
    }

    // Null explicitly requests emergency minima with a fresh preserved temperature limit.
    public void ApplyCpuPower(int pl1Watts, int pl2Watts, int pl4Watts, int? tempLimitC)
    {
        try
        {
            Failsafe?.CheckBeforeApply();
            bool emergency = tempLimitC == null || Failsafe?.Holds(ThermalAction.Cpu) == true;
            if (emergency)
            {
                if (!CanApplyMinimum) throw new InvalidOperationException("Held CPU reduction unavailable; CPU changes blocked.");
                pl1Watts = Minimum.PL1Watts;
                pl2Watts = Minimum.PL2Watts;
                pl4Watts = Minimum.PL4Watts;
                tempLimitC = ReadEmergencyTemperatureLimit();
            }
            ValidatePower(pl1Watts, pl2Watts, pl4Watts, tempLimitC!.Value);
            byte pl1 = RegisterEncoding.EncodePL1(pl1Watts);
            byte pl2 = RegisterEncoding.EncodePL1(pl2Watts);
            byte pl4 = RegisterEncoding.EncodePL4(pl4Watts, _pl4Doubled);
            byte temp = _absoluteTemp
                ? RegisterEncoding.EncodeCpuTempLimitDirect(tempLimitC.Value)
                : RegisterEncoding.EncodeCpuTempLimit(tempLimitC.Value, _tjMax);

            if (!emergency) DiagnosticLog.Write($"[CPU Power] PL1=0x{pl1:X2}({pl1Watts}W) PL2=0x{pl2:X2}({pl2Watts}W) " +
                $"PL4=0x{pl4:X2}({RegisterEncoding.DecodePL4(pl4, _pl4Doubled)}W effective) Temp=0x{temp:X2}({tempLimitC}C, {(_absoluteTemp ? "absolute" : $"offset, tjMax={_tjMax}")})");

            _guard.Write(_addrPL1, pl1);
            _guard.Write(_addrPL2, pl2);
            _guard.Write(_addrPL4, pl4);
            _guard.Write(_addrTempWrite, temp);
        }
        catch (Exception ex) { Failsafe?.ReportWriteFailure(ThermalAction.Cpu, ex); throw; }
    }

    /// <summary>
    /// Read current CPU power values from EC.
    /// </summary>
    public CpuPowerValues ReadCpuPower()
    {
        byte pl1Raw = _device.ReadByte(_addrPL1);
        byte pl2Raw = _device.ReadByte(_addrPL2);
        byte pl4Raw = _device.ReadByte(_addrPL4);
        byte tempRaw = _device.ReadByte(_addrTempRead);

        return new CpuPowerValues
        {
            PL1Watts = RegisterEncoding.DecodePL1(pl1Raw),
            PL2Watts = RegisterEncoding.DecodePL1(pl2Raw),
            PL4Watts = RegisterEncoding.DecodePL4(pl4Raw, _pl4Doubled),
            TempLimitC = RegisterEncoding.DecodeCpuTempLimit(tempRaw, _absoluteTemp, _tjMax)
        };
    }
}

struct CpuPowerValues
{
    public int PL1Watts;
    public int PL2Watts;
    public int PL4Watts;
    public int TempLimitC;
}
