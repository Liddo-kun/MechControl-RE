using MechControl.Driver;
using MechControl.Profile;

namespace MechControl.Features;

class FanSwitchSpeedControl
{
    private readonly EcDevice _device;
    private readonly WriteGuard _guard;
    private readonly int _addrFanSwitchSpeed;

    public FanSwitchSpeedControl(EcDevice device, WriteGuard guard, MachineProfile profile)
    {
        _device = device;
        _guard = guard;
        _addrFanSwitchSpeed = profile.GetRegister("fanSwitchSpeed");
    }

    public (bool enabled, int milliseconds) ReadState()
    {
        byte raw = _device.ReadByte(_addrFanSwitchSpeed);
        return RegisterEncoding.DecodeFanSwitchSpeed(raw);
    }

    public void Apply(bool enabled, int milliseconds)
    {
        _guard.Write(_addrFanSwitchSpeed, RegisterEncoding.EncodeFanSwitchSpeed(enabled, milliseconds));
    }
}
