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

    public GpuPowerControl(EcDevice device, WriteGuard guard, MachineProfile profile)
    {
        _device = device;
        _guard = guard;
        _addrTgpDbCtrl = profile.GetRegister("tgpDbCtrl");
        _addrTgp = profile.GetRegister("tgp");
        _addrDbTotalPower = profile.GetRegister("dbTotalPower");
        _addrDbMaxTgp = profile.GetRegister("dbMaxTgp");
        _dbMask = profile.Bitfields["dynamicBoostEn"].GetMask(); // 0x02
        _tgpOffset = profile.TgpOffset;
    }

    public void ApplyGpuPower(int tgpWatts, bool dynamicBoostEnabled, int dbWatts)
    {
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
            DynamicBoostEnabled = (ctrlRaw & _dbMask) != 0,
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
