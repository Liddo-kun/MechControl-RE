using MechControl.Driver;
using MechControl.Profile;

namespace MechControl.Features;

class GpuPowerControl
{
    private readonly EcDevice _device;
    private readonly WriteGuard _guard;

    private readonly int _addrTgpDbCtrl;
    private readonly int _addrTgp;
    private readonly int _addrDbTotalPower;
    private readonly int _addrDbMaxTgp;
    private readonly byte _dbMask;
    private readonly int _tgpOffset;
    private readonly LimitInfo _tgpLimit, _dbLimit;

    public GpuTemperatureFailsafe? Failsafe { get; set; }
    public int MinimumTgpWatts { get; }
    public bool CanApplyMinimumTgp { get; private set; }

    // Within the 0x07 control mask, bit 1 is the actual Dynamic Boost enable.
    // The OEM app writes 0x07 (on) or 0x05 (off) — bits 0,2 stay set in both states.
    private const byte DbEnableBit = 0x02;

    public GpuPowerControl(EcDevice device, WriteGuard guard, MachineProfile profile)
    {
        _device = device;
        _guard = guard;
        _addrTgpDbCtrl = profile.GetRegister("tgpDbCtrl");
        _addrTgp = profile.GetRegister("tgp");
        _addrDbTotalPower = profile.GetRegister("dbTotalPower");
        _addrDbMaxTgp = profile.GetRegister("dbMaxTgp");
        _dbMask = profile.Bitfields["dynamicBoostEn"].GetMask(); // 0x07 (bits 0,1,2)
        _tgpOffset = profile.TgpOffset;
        var limit = profile.Limits.TryGetValue("TGP", out var tgp) ? tgp : UniversalDefaults.ConservativeLimits["TGP"];
        _tgpLimit = limit;
        _dbLimit = profile.Limits.GetValueOrDefault("dynamicBoost") ?? UniversalDefaults.ConservativeLimits["dynamicBoost"];
        MinimumTgpWatts = RegisterEncoding.DecodeTGP(RegisterEncoding.EncodeTGP(limit.Min, _tgpOffset), _tgpOffset);
        CanApplyMinimumTgp = !profile.TgpLimitIsFallback && profile.Limits.ContainsKey("TGP") &&
            limit.Encoding == "offset" && limit.Offset.HasValue &&
            limit.Min > 0 && limit.Min <= limit.Max && MinimumTgpWatts == limit.Min;
    }

    public void ApplyGpuPower(int tgpWatts, bool dynamicBoostEnabled, int dbWatts)
    {
        try
        {
            Failsafe?.CheckBeforeApply();
            if (Failsafe?.Holds(ThermalAction.Gpu) == true)
            {
                if (!CanApplyMinimumTgp)
                    throw new InvalidOperationException("GPU failsafe latched, but a model-specific minimum TGP is unavailable. GPU power changes blocked.");
                tgpWatts = MinimumTgpWatts;
                dynamicBoostEnabled = false;
                dbWatts = 0;
            }
            ValidatePower(tgpWatts, dynamicBoostEnabled, dbWatts);
            byte tgp = RegisterEncoding.EncodeTGP(tgpWatts, _tgpOffset);
            byte db = dynamicBoostEnabled ? RegisterEncoding.EncodeDB(dbWatts) : (byte)0;
            _guard.WriteBitfield(_addrTgpDbCtrl, _dbMask, dynamicBoostEnabled ? (byte)0x07 : (byte)0x05);
            _guard.Write(_addrTgp, tgp);
            _guard.Write(_addrDbTotalPower, dynamicBoostEnabled ? (byte)255 : (byte)0);
            _guard.Write(_addrDbMaxTgp, db);
        }
        catch (Exception ex) { Failsafe?.ReportWriteFailure(ThermalAction.Gpu, ex); throw; }
    }

    public void ValidatePower(int tgpWatts, bool dynamicBoostEnabled, int dbWatts)
    {
        if (_tgpLimit.Encoding != "offset" || tgpWatts < _tgpLimit.Min || tgpWatts > _tgpLimit.Max ||
            RegisterEncoding.DecodeTGP(RegisterEncoding.EncodeTGP(tgpWatts, _tgpOffset), _tgpOffset) != tgpWatts)
            throw new ArgumentOutOfRangeException(nameof(tgpWatts), $"GPU TGP {tgpWatts} W exceeds model range {_tgpLimit.Min}-{_tgpLimit.Max} W or supported register encoding.");
        if (dynamicBoostEnabled && (_dbLimit.Encoding != "direct" || dbWatts < _dbLimit.Min || dbWatts > _dbLimit.Max || RegisterEncoding.EncodeDB(dbWatts) != dbWatts))
            throw new ArgumentOutOfRangeException(nameof(dbWatts), $"Dynamic Boost {dbWatts} W exceeds model range {_dbLimit.Min}-{_dbLimit.Max} W or supported register encoding.");
    }

    public bool IsMinimumPowerRequested() => CanApplyMinimumTgp &&
        _guard.VerifiesSameAddress(_addrTgpDbCtrl) && _guard.VerifiesSameAddress(_addrTgp) &&
        _guard.VerifiesSameAddress(_addrDbTotalPower) && _guard.VerifiesSameAddress(_addrDbMaxTgp) &&
        (_device.ReadByte(_addrTgpDbCtrl) & _dbMask) == (0x05 & _dbMask) &&
        _device.ReadByte(_addrTgp) == RegisterEncoding.EncodeTGP(MinimumTgpWatts, _tgpOffset) &&
        _device.ReadByte(_addrDbTotalPower) == 0 && _device.ReadByte(_addrDbMaxTgp) == 0;

    public void ValidateFailsafeRegisters(bool gpuPresent)
    {
        CanApplyMinimumTgp &= gpuPresent;
        if (!CanApplyMinimumTgp) return;
        try
        {
            var values = ReadGpuPower(); // All four GPU registers must be readable.
            CanApplyMinimumTgp = values.TgpWatts >= _tgpOffset && values.TgpWatts <= _tgpLimit.Max &&
                values.DbWatts >= 0 && values.DbWatts <= _dbLimit.Max && _device.ReadByte(_addrTgpDbCtrl) != 0xFF;
        }
        catch { CanApplyMinimumTgp = false; }
    }

    /// <summary>
    /// Read current GPU power values from EC.
    /// </summary>
    public GpuPowerValues ReadGpuPower()
    {
        byte ctrlRaw = _device.ReadByte(_addrTgpDbCtrl);
        byte tgpRaw = _device.ReadByte(_addrTgp);
        byte dbTotalRaw = _device.ReadByte(_addrDbTotalPower);
        byte dbMaxRaw = _device.ReadByte(_addrDbMaxTgp);

        return new GpuPowerValues
        {
            TgpWatts = RegisterEncoding.DecodeTGP(tgpRaw, _tgpOffset),
            DynamicBoostEnabled = (ctrlRaw & DbEnableBit) != 0,
            DbWatts = RegisterEncoding.DecodeDB(dbMaxRaw)
        };
    }
}

struct GpuPowerValues
{
    public int TgpWatts;
    public bool DynamicBoostEnabled;
    public int DbWatts;
}
