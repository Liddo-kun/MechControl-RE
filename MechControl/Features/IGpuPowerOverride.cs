namespace MechControl.Features;

// Driver units are retained in profiles/readback: no rounding of a detected rail limit.
// Default/StockMaximum come from the original BIOS; Maximum/Current are live.
record GpuCurrentLimit(int MinimumMilliamps, int DefaultMilliamps, int MaximumMilliamps, int CurrentMilliamps, int StockMaximumMilliamps)
{
    // UI policy, not a hardware rating: the unlock allows up to twice the stock default.
    public int AllowedMaximumMilliamps(bool unlocked) =>
        unlocked ? Math.Max(StockMaximumMilliamps, checked(DefaultMilliamps * 2)) : StockMaximumMilliamps;
}
record GpuCurrentLimits(GpuCurrentLimit Nvvdd, GpuCurrentLimit Msvdd);
// Live driver ceiling, enforced limit and request (null when none is set, e.g. after a GPU restart).
record GpuPowerLimits(int CeilingWatts, int EnforcedWatts, int? RequestedWatts);

interface IGpuPowerOverride
{
    bool Enabled { get; }
    int StockPowerLimitWatts { get; }
    int MinimumPowerLimitWatts { get; }
    GpuPowerLimits ReadPowerLimits();
    GpuCurrentLimits ReadCurrentLimits();
    // Null currents keep the live request. Only raising a ceiling or toggling the unlock restarts the GPU.
    bool RequiresRestart(bool enabled, int? nvvddMilliamps, int? msvddMilliamps);
    void Configure(bool enabled, int? nvvddMilliamps, int? msvddMilliamps, bool allowRestart);
    void ApplyPowerLimit(int watts);
    bool IsPowerLimitAtMost(int watts);
    // False when the driver dropped this request or firmware reset the laptop budget below it.
    bool IsPowerLimitApplied(int watts);
}
