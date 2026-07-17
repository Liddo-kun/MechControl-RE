using System.Diagnostics;

namespace MechControl.Monitoring;

/// <summary>
/// Publishes fan RPM as Windows Performance Counters so external tools
/// (MSI Afterburner via PerfCounter.dll, perfmon, etc.) can read them.
/// Passive sink: consumes <see cref="EcMonitor.Updated"/> data instead of
/// running its own timer, so the fan RPM bytes are read from the EC once per
/// second total instead of twice.
/// </summary>
class PerfCounterPublisher : IDisposable
{
    private const string CategoryName = "MechControl";
    private const string CategoryHelp = "MechControl EC sensor data";
    private const int SchemaVersion = 2;

    private PerformanceCounter? _cpuFanRpm;
    private PerformanceCounter? _gpuFanRpm;
    private bool _available;

    public void Start()
    {
        try
        {
            EnsureCategory();

            _cpuFanRpm = new PerformanceCounter(CategoryName, "CPU Fan RPM", false);
            _gpuFanRpm = new PerformanceCounter(CategoryName, "GPU Fan RPM", false);
            _available = true;
        }
        catch (Exception ex)
        {
            DiagnosticLog.Warn($"PerfCounter init failed: {ex.Message}");
            _available = false;
        }
    }

    /// <summary>
    /// Subscriber for <see cref="EcMonitor.Updated"/> (fires on the UI thread).
    /// </summary>
    public void OnMonitorUpdated(MonitorData data)
    {
        if (!_available) return;

        try
        {
            _cpuFanRpm!.RawValue = data.CpuFanRpm;
            _gpuFanRpm!.RawValue = data.GpuFanRpm;
        }
        catch
        {
            // Counter write failed — skip, try again on the next update
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
        _cpuFanRpm?.Dispose();
        _gpuFanRpm?.Dispose();
    }
}
