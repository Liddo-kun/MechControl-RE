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
    private readonly int _maximumTgpWatts;
    private readonly int _maximumDbWatts;

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
        _maximumTgpWatts = limit.Max;
        _maximumDbWatts = profile.Limits.TryGetValue("dynamicBoost", out var db) ? db.Max : UniversalDefaults.ConservativeLimits["dynamicBoost"].Max;
        MinimumTgpWatts = RegisterEncoding.DecodeTGP(RegisterEncoding.EncodeTGP(limit.Min, _tgpOffset), _tgpOffset);
        CanApplyMinimumTgp = !profile.TgpLimitIsFallback && profile.Limits.ContainsKey("TGP") &&
            limit.Encoding == "offset" && limit.Offset.HasValue &&
            limit.Min > 0 && limit.Min <= limit.Max && MinimumTgpWatts == limit.Min;
    }

    public void ApplyGpuPower(int tgpWatts, bool dynamicBoostEnabled, int dbWatts,
        GpuTemperatureFailsafe.Restoration? restoration = null)
    {
        try
        {
            Failsafe?.CheckBeforeApply();
            if (Failsafe?.Holds(ThermalAction.Gpu, restoration) == true)
            {
                if (!CanApplyMinimumTgp)
                    throw new InvalidOperationException("GPU failsafe latched, but a model-specific minimum TGP is unavailable. GPU power changes blocked.");
                tgpWatts = MinimumTgpWatts;
                dynamicBoostEnabled = false;
                dbWatts = 0;
            }
            // OEM writes full byte: 0x07 (DB on) or 0x05 (DB off).
            // Bits 0,2 = TGP enable (always set), bit 1 = Dynamic Boost.
            if (dynamicBoostEnabled)
            {
                _guard.WriteBitfield(_addrTgpDbCtrl, _dbMask, 0x07); // bits 0,1,2
                _guard.Write(_addrTgp, RegisterEncoding.EncodeTGP(tgpWatts, _tgpOffset));
                _guard.Write(_addrDbTotalPower, 255); // DB total power flag
                _guard.Write(_addrDbMaxTgp, RegisterEncoding.EncodeDB(dbWatts));
            }
            else
            {
                _guard.WriteBitfield(_addrTgpDbCtrl, _dbMask, 0x05); // bits 0,2 only
                _guard.Write(_addrTgp, RegisterEncoding.EncodeTGP(tgpWatts, _tgpOffset));
                _guard.Write(_addrDbTotalPower, 0);
                _guard.Write(_addrDbMaxTgp, 0);
            }
            Failsafe?.ConfirmWrite(ThermalAction.Gpu, restoration);
        }
        catch (Exception ex) { Failsafe?.ReportWriteFailure(ThermalAction.Gpu, ex); throw; }
    }

    public void ValidateFailsafeRegisters(bool gpuPresent)
    {
        CanApplyMinimumTgp &= gpuPresent;
        if (!CanApplyMinimumTgp) return;
        try
        {
            var values = ReadGpuPower(); // All four GPU registers must be readable.
            CanApplyMinimumTgp = values.TgpWatts >= _tgpOffset && values.TgpWatts <= _maximumTgpWatts &&
                values.DbWatts >= 0 && values.DbWatts <= _maximumDbWatts && _device.ReadByte(_addrTgpDbCtrl) != 0xFF;
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
