using System.Diagnostics;
using MechControl.Driver;
using MechControl.Profile;

namespace MechControl.Monitoring;

/// <summary>
/// Publishes fan RPM/duty as Windows Performance Counters so external tools
/// (MSI Afterburner via PerfCounter.dll, perfmon, etc.) can read them.
/// Runs its own timer, reads only the 6 fan bytes from EC — no dependency on EcMonitor.
/// </summary>
class PerfCounterPublisher : IDisposable
{
    private const string CategoryName = "MechControl";
    private const string CategoryHelp = "MechControl EC sensor data";
    private const int SchemaVersion = 2;

    private readonly EcDevice _device;
    private readonly int _addrCpuFanRpmLo;
    private readonly int _addrCpuFanRpmHi;
    private readonly int _addrGpuFanRpmLo;
    private readonly int _addrGpuFanRpmHi;

    private PerformanceCounter? _cpuFanRpm;
    private PerformanceCounter? _gpuFanRpm;
    private System.Threading.Timer? _timer;
    private bool _available;

    public PerfCounterPublisher(EcDevice device, MachineProfile profile)
    {
        _device = device;
        _addrCpuFanRpmLo = profile.GetRegister("cpuFanRpmLo");
        _addrCpuFanRpmHi = profile.GetRegister("cpuFanRpmHi");
        _addrGpuFanRpmLo = profile.GetRegister("gpuFanRpmLo");
        _addrGpuFanRpmHi = profile.GetRegister("gpuFanRpmHi");
    }

    public void Start()
    {
        try
        {
            EnsureCategory();

            _cpuFanRpm = new PerformanceCounter(CategoryName, "CPU Fan RPM", false);
            _gpuFanRpm = new PerformanceCounter(CategoryName, "GPU Fan RPM", false);
            _available = true;

            _timer = new System.Threading.Timer(OnTick, null, 0, 1000);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"PerfCounter init failed: {ex.Message}");
            _available = false;
        }
    }

    private void OnTick(object? state)
    {
        if (!_available) return;

        try
        {
            // 4 EC byte reads — fan RPM only
            byte cpuRpmA = _device.ReadByte(_addrCpuFanRpmLo);
            byte cpuRpmB = _device.ReadByte(_addrCpuFanRpmHi);
            _cpuFanRpm!.RawValue = cpuRpmA << 8 | cpuRpmB;

            byte gpuRpmA = _device.ReadByte(_addrGpuFanRpmLo);
            byte gpuRpmB = _device.ReadByte(_addrGpuFanRpmHi);
            _gpuFanRpm!.RawValue = gpuRpmB << 8 | gpuRpmA;
        }
        catch
        {
            // EC read failed — skip this tick, try again next second
        }
    }

    private static void EnsureCategory()
    {
        string versionedHelp = $"{CategoryHelp} v{SchemaVersion}";

        if (PerformanceCounterCategory.Exists(CategoryName))
        {
            var cat = new PerformanceCounterCategory(CategoryName);
            if (cat.CategoryHelp == versionedHelp)
                return;
            PerformanceCounterCategory.Delete(CategoryName);
        }

        var counters = new CounterCreationDataCollection
        {
            new("CPU Fan RPM", "CPU fan speed in RPM", PerformanceCounterType.NumberOfItems64),
            new("GPU Fan RPM", "GPU fan speed in RPM", PerformanceCounterType.NumberOfItems64),
        };
        PerformanceCounterCategory.Create(CategoryName, versionedHelp,
            PerformanceCounterCategoryType.SingleInstance, counters);
    }

    public void Dispose()
    {
        _timer?.Dispose();
        _cpuFanRpm?.Dispose();
        _gpuFanRpm?.Dispose();
    }
}
