namespace MechControl.Features;

// Driver units are retained in profiles/readback: no rounding of a detected rail limit.
// Default/StockMaximum come from the original BIOS; Maximum/Current are live.
record GpuCurrentLimit(int MinimumMilliamps, int DefaultMilliamps, int MaximumMilliamps, int CurrentMilliamps, int StockMaximumMilliamps)
{
    // UI policy, not a hardware rating: all three currents share the same unlock headroom.
    public int AllowedMaximumMilliamps(bool unlocked) =>
        unlocked ? Math.Max(StockMaximumMilliamps, checked((int)((long)DefaultMilliamps * Profile.UniversalDefaults.GpuCurrentUnlockPercent / 100))) : StockMaximumMilliamps;
}
record GpuCurrentLimits(GpuCurrentLimit Nvvdd, GpuCurrentLimit Msvdd, GpuCurrentLimit? BoardInput = null);
// Live driver ceiling, enforced limit and request (null when none is set, e.g. after a GPU restart).
record GpuPowerLimits(int CeilingWatts, int EnforcedWatts, int? RequestedWatts);
// The laptop GPU budget (F7) and the combined platform budget (TPP) firmware and Dynamic Boost publish to the driver.
record GpuPowerBudget(int LaptopWatts, int PlatformWatts);

interface IGpuPowerOverride
{
    bool Enabled { get; }
    int StockPowerLimitWatts { get; }
    int MinimumPowerLimitWatts { get; }
    GpuPowerLimits ReadPowerLimits();
    GpuCurrentLimits ReadCurrentLimits();
    // Null currents keep the live request. Only toggling the unlock (or exceeding a live ceiling
    // below the unlocked range, staged by an older build) restarts the GPU.
    bool RequiresRestart(bool enabled, int? nvvddMilliamps, int? msvddMilliamps, int? boardInputMilliamps);
    void Configure(bool enabled, int? nvvddMilliamps, int? msvddMilliamps, int? boardInputMilliamps, bool allowRestart);
    void ApplyPowerLimit(int watts);
    bool IsPowerLimitAtMost(int watts);
    // False when the driver dropped this request or firmware reset the laptop budget below it.
    bool IsPowerLimitApplied(int watts);
    GpuPowerBudget ReadPowerBudget();
    // Windows' last device power state, without contacting the GPU. A driver read while it
    // sleeps would wake it.
    bool IsGpuAwake();
}
