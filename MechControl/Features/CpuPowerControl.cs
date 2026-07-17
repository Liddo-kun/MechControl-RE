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
    }

    /// <summary>
    /// Write CPU power limits as a group (PL1 + PL2 + PL4 + temp limit).
    /// </summary>
    public void ApplyCpuPower(int pl1Watts, int pl2Watts, int pl4Watts, int tempLimitC)
    {
        byte pl1 = RegisterEncoding.EncodePL1(pl1Watts);
        byte pl2 = RegisterEncoding.EncodePL1(pl2Watts);
        byte pl4 = RegisterEncoding.EncodePL4(pl4Watts, _pl4Doubled);
        int tempClamped = Math.Clamp(tempLimitC, _tempMin, _tempMax);
        byte temp = _absoluteTemp
            ? RegisterEncoding.EncodeCpuTempLimitDirect(tempClamped)
            : RegisterEncoding.EncodeCpuTempLimit(tempClamped, _tjMax);

        DiagnosticLog.Write($"[CPU Power] PL1=0x{pl1:X2}({pl1Watts}W) PL2=0x{pl2:X2}({pl2Watts}W) " +
            $"PL4=0x{pl4:X2}({pl4Watts}W) Temp=0x{temp:X2}({tempClamped}C, {(_absoluteTemp ? "absolute" : $"offset, tjMax={_tjMax}")})");

        _guard.Write(_addrPL1, pl1);
        _guard.Write(_addrPL2, pl2);
        _guard.Write(_addrPL4, pl4);
        _guard.Write(_addrTempWrite, temp);
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
            TempLimitC = _absoluteTemp ? tempRaw : RegisterEncoding.DecodeCpuTempLimit(tempRaw, _tjMax)
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
