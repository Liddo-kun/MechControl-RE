using MechControl.Driver;
using MechControl.Profile;

namespace MechControl.Monitoring;

struct MonitorData
{
    public int CpuFanPercent;
    public int GpuFanPercent;
    public int CpuFanRpm;
    public int GpuFanRpm;
    public int CpuTempLimit;
    public int BatterySoc;
    public int BatteryModeIndex;
    public string BatteryModeName;
    public byte BatteryModeRaw;
    public bool FanBoostActive;
    public bool FanCurveEnabled;

    // System info (universal EC addresses, always populated)
    public int CpuTemp;               // degrees C
    public int GpuTemp;               // degrees C
    public double AdapterCurrentAmps; // 0x449 / 10.0
    public int SystemPowerWatts;      // 0x60C | (0x60D << 8)

    // NPCF mirrors: 0x7D5 shadows 0x746 (DB max, direct watts); 0x788 shadows 0x744
    // (TGP offset, watts - TgpOffset). DSDT multiplies each by 8 before stuffing into
    // NPCF.AMAT / NPCF.ACBT — that ×8 is the NPCF unit (1/8 W), NOT a watt conversion.
    // We display the physical watts here, not the NPCF integers.
    public int DbMaxLiveWatts;        // 0x7D5 raw, direct watts (0 = DB off)
    public int TgpLiveWatts;          // 0x788 raw + TgpOffset (total TGP)
}

class EcMonitor : IDisposable
{
    private readonly EcDevice _device;
    private readonly MachineProfile _profile;
    private readonly System.Windows.Forms.Timer _timer;

    private readonly int _addrCpuFanDuty;
    private readonly int _addrGpuFanDuty;
    private readonly int _addrCpuFanRpmLo;
    private readonly int _addrCpuFanRpmHi;
    private readonly int _addrGpuFanRpmLo;
    private readonly int _addrGpuFanRpmHi;
    private readonly int _addrCpuTempLimitRead;
    private readonly int _addrBatterySoc;
    private readonly int _addrBatteryMode;
    private readonly int _addrStatusByte;
    private readonly int _addrFanCurveEnable;
    private readonly int _tjMax;
    private readonly bool _absoluteTemp;

    // System info — universal EC addresses (DSDT-defined, same across models)
    private readonly int _addrCpuTemp;
    private readonly int _addrGpuTemp;
    private const int AddrAdapterCurrent = 0x449;
    private const int AddrSystemPowerLo  = 0x60C;
    private const int AddrSystemPowerHi  = 0x60D;
    private const int AddrDbMaxMirror    = 0x7D5;  // DBAP (shadows 0x746, direct watts)
    private const int AddrTgpMirror      = 0x788;  // CTWA (shadows 0x744, watts - TgpOffset)

    private int _consecutiveFailures;

    public event Action<MonitorData>? Updated;
    public event Action<string>? Stopped;

    public EcMonitor(EcDevice device, MachineProfile profile)
    {
        _device = device;
        _profile = profile;

        _addrCpuFanDuty = profile.GetRegister("cpuFanDuty");
        _addrGpuFanDuty = profile.GetRegister("gpuFanDuty");
        _addrCpuFanRpmLo = profile.GetRegister("cpuFanRpmLo");
        _addrCpuFanRpmHi = profile.GetRegister("cpuFanRpmHi");
        _addrGpuFanRpmLo = profile.GetRegister("gpuFanRpmLo");
        _addrGpuFanRpmHi = profile.GetRegister("gpuFanRpmHi");
        _addrCpuTempLimitRead = profile.GetRegister("cpuTempLimitRead");
        _addrBatterySoc = profile.GetRegister("batterySoc");
        _addrBatteryMode = profile.GetRegister("batteryMode");
        _addrStatusByte = profile.GetRegister("statusByte");
        _addrFanCurveEnable = profile.GetRegister("fanCurveEnable");

        _tjMax = profile.TjMax;
        _absoluteTemp = profile.CpuTempEncoding == "direct";

        // CPU/GPU temp addresses from profile (these are already defined)
        _addrCpuTemp = TryGetRegister(profile, "cpuTemp");
        _addrGpuTemp = TryGetRegister(profile, "gpuTemp");

        _timer = new System.Windows.Forms.Timer { Interval = 1000 };
        _timer.Tick += OnTick;
    }

    private static int TryGetRegister(MachineProfile profile, string name) =>
        profile.Registers.ContainsKey(name) ? profile.GetRegister(name) : -1;

    public void Start() => _timer.Start();

    public void Stop() => _timer.Stop();

    public void Dispose()
    {
        _timer.Stop();
        _timer.Dispose();
    }

    private void OnTick(object? sender, EventArgs e)
    {
        try
        {
            var data = new MonitorData();

            data.CpuFanPercent = RegisterEncoding.DecodeFanDuty(_device.ReadByte(_addrCpuFanDuty));
            data.GpuFanPercent = RegisterEncoding.DecodeFanDuty(_device.ReadByte(_addrGpuFanDuty));

            // RPM byte order is inconsistent in the EC firmware:
            // CPU RPM: register "Lo" (0x464) is actually the high byte
            // GPU RPM: register "Lo" (0x46B) is actually the low byte
            byte cpuRpmA = _device.ReadByte(_addrCpuFanRpmLo);
            byte cpuRpmB = _device.ReadByte(_addrCpuFanRpmHi);
            data.CpuFanRpm = cpuRpmA << 8 | cpuRpmB;

            byte gpuRpmA = _device.ReadByte(_addrGpuFanRpmLo);
            byte gpuRpmB = _device.ReadByte(_addrGpuFanRpmHi);
            data.GpuFanRpm = gpuRpmB << 8 | gpuRpmA;

            byte tempRaw = _device.ReadByte(_addrCpuTempLimitRead);
            data.CpuTempLimit = _absoluteTemp ? tempRaw : RegisterEncoding.DecodeCpuTempLimit(tempRaw, _tjMax);
            data.BatterySoc = _device.ReadByte(_addrBatterySoc);

            byte batteryRaw = _device.ReadByte(_addrBatteryMode);
            data.BatteryModeRaw = batteryRaw;
            data.BatteryModeIndex = RegisterEncoding.DecodeBatteryMode(batteryRaw);
            data.BatteryModeName = data.BatteryModeIndex < _profile.BatteryModes.Count
                ? _profile.BatteryModes[data.BatteryModeIndex].Name
                : $"Unknown ({data.BatteryModeIndex})";

            byte statusRaw = _device.ReadByte(_addrStatusByte);
            data.FanBoostActive = (statusRaw & 0x04) != 0;

            byte fanCurveRaw = _device.ReadByte(_addrFanCurveEnable);
            data.FanCurveEnabled = (fanCurveRaw & 0x80) != 0;

            // System info (universal EC addresses)
            if (_addrCpuTemp >= 0)
                data.CpuTemp = _device.ReadByte(_addrCpuTemp);
            if (_addrGpuTemp >= 0)
                data.GpuTemp = _device.ReadByte(_addrGpuTemp);
            data.AdapterCurrentAmps = _device.ReadByte(AddrAdapterCurrent) / 10.0;
            data.SystemPowerWatts = _device.ReadByte(AddrSystemPowerLo)
                                  | (_device.ReadByte(AddrSystemPowerHi) << 8);
            data.DbMaxLiveWatts = _device.ReadByte(AddrDbMaxMirror);
            data.TgpLiveWatts = _device.ReadByte(AddrTgpMirror) + _profile.TgpOffset;

            _consecutiveFailures = 0;
            Updated?.Invoke(data);
        }
        catch (Exception ex)
        {
            _consecutiveFailures++;
            Console.WriteLine($"EcMonitor tick failed ({_consecutiveFailures}/5): {ex.Message}");

            if (_consecutiveFailures >= 5)
            {
                _timer.Stop();
                Stopped?.Invoke("Monitor stopped after 5 consecutive read failures.");
            }
        }
    }
}
