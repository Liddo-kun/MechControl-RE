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

    // The NVIDIA driver is opened only on demand (unlock, or saved MechControl state at
    // startup): NVAPI/NVML can wake a sleeping discrete GPU, and stock users never need it.
    public Func<IGpuPowerOverride>? OpenPowerOverride { get; set; }
    public IGpuPowerOverride? PowerOverride { get; set; }
    // Why opening NVIDIA failed (null when it never did, e.g. no NVIDIA GPU).
    public string? PowerOverrideUnavailableReason { get; private set; }
    // Runs a GPU restart; the app shows progress and keeps its UI thread (and failsafe) alive.
    public Action<string, Action> RunBlocking { get; set; } = (_, work) => work();
    // Firmware resets its laptop budget about a second after the EC registers change. While the
    // NVIDIA driver is open, the app calls ReassertDriverRequest a few seconds after every apply.
    public Action? ScheduleDriverCheck { get; set; }
    // Why the last profile load could not apply all of its NVIDIA settings ("" when it did).
    public string ProfileNotice { get; private set; } = "";
    public bool CanUnlockTgp => _canExtendEcLimit && (PowerOverride != null || OpenPowerOverride != null);
    public bool TgpUnlocked => PowerOverride?.Enabled == true;
    public int NormalMinTgpWatts => _tgpLimit.Min;
    public int NormalMaxTgpWatts => _tgpLimit.Max;
    // Below the EC minimum the EC stays at its minimum and only the driver request is lower.
    public int UnlockedMinTgpWatts => Math.Max(PowerOverride?.MinimumPowerLimitWatts ?? 1,
        (_tgpLimit.Min * UniversalDefaults.TgpUnlockMinimumPercent + 99) / 100);
    public int UnlockedMaxTgpWatts { get; }
    public int MinTgpWatts => TgpUnlocked && _canExtendEcLimit ? UnlockedMinTgpWatts : NormalMinTgpWatts;
    public int MaxTgpWatts => TgpUnlocked && _canExtendEcLimit ? UnlockedMaxTgpWatts : NormalMaxTgpWatts;
    private readonly bool _canExtendEcLimit;
    // The last driver request MechControl set, and the values it came from (for resume).
    private int? _driverWatts;
    private (int Tgp, bool DynamicBoost, int DbWatts)? _lastRequest;

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
        // Only a real model limit (ECSpec or profile) is extended, never the fallback.
        UnlockedMaxTgpWatts = Math.Min(limit.Max + UniversalDefaults.TgpUnlockHeadroomWatts,
            RegisterEncoding.DecodeTGP(RegisterEncoding.EncodeTGP(int.MaxValue, _tgpOffset), _tgpOffset));
        _canExtendEcLimit = !profile.TgpLimitIsFallback && profile.Limits.ContainsKey("TGP") &&
            limit.Encoding == "offset" && limit.Offset.HasValue && UnlockedMaxTgpWatts > limit.Max;
    }

    public void ApplyGpuPower(int tgpWatts, bool dynamicBoostEnabled, int dbWatts)
    {
        try
        {
            var requested = (tgpWatts, dynamicBoostEnabled, dbWatts);
            var driver = PowerOverride is { Enabled: true } unlocked ? unlocked : null;
            Failsafe?.CheckBeforeApply();
            if (Failsafe?.Holds(ThermalAction.Gpu) == true)
            {
                if (!CanApplyMinimumTgp)
                    throw new InvalidOperationException("GPU failsafe latched, but a model-specific minimum TGP is unavailable. GPU power changes blocked.");
                // The hold never raises a lower unlocked request.
                tgpWatts = driver == null ? MinimumTgpWatts
                    : Math.Clamp(Math.Min(tgpWatts, _driverWatts ?? tgpWatts), UnlockedMinTgpWatts, MinimumTgpWatts);
                dynamicBoostEnabled = false;
                dbWatts = 0;
            }
            ValidatePower(tgpWatts, dynamicBoostEnabled, dbWatts);
            byte ctrl = dynamicBoostEnabled ? (byte)0x07 : (byte)0x05;
            byte tgp = RegisterEncoding.EncodeTGP(Math.Max(tgpWatts, _tgpLimit.Min), _tgpOffset);
            byte dbTotal = dynamicBoostEnabled ? (byte)255 : (byte)0;
            byte db = dynamicBoostEnabled ? RegisterEncoding.EncodeDB(dbWatts) : (byte)0;
            _guard.WriteBitfield(_addrTgpDbCtrl, _dbMask, ctrl);
            _guard.Write(_addrTgp, tgp);
            _guard.Write(_addrDbTotalPower, dbTotal);
            _guard.Write(_addrDbMaxTgp, db);
            // While unlocked, the raised driver request (F8) would otherwise stay above the
            // EC request. Every EC request, including the thermal hold, is mirrored.
            int? request = driver == null ? null : Math.Min(driver.ReadPowerLimits().CeilingWatts, checked(tgpWatts + (dynamicBoostEnabled ? dbWatts : 0)));
            if (request is int watts) driver!.ApplyPowerLimit(watts);
            _driverWatts = request;
            _lastRequest = requested;
            if (PowerOverride != null) ScheduleDriverCheck?.Invoke();
        }
        catch (Exception ex) { Failsafe?.ReportWriteFailure(ThermalAction.Gpu, ex); throw; }
    }

    /// <summary>
    /// Reapply the last unlocked request if the driver dropped it (sleep) or firmware reset
    /// the laptop budget below it (EC changes, charger plugged in or out). On AC, log when the
    /// laptop budget stays below the EC request: Dynamic Boost has been seen to stop adding
    /// its boost until its driver (NVIDIA Platform Controllers and Framework) restarts.
    /// </summary>
    public void ReassertDriverRequest(bool onAc)
    {
        if (PowerOverride is not { } driver) return;
        if (_lastRequest is { } last && _driverWatts is int watts && driver.Enabled && !driver.IsPowerLimitApplied(watts))
        {
            DiagnosticLog.Write($"[NVIDIA Power] Driver request or laptop budget below {watts} W; reapplying.");
            ApplyGpuPower(last.Tgp, last.DynamicBoost, last.DbWatts);
            return; // The reapply schedules another check.
        }
        if (!onAc) return;
        var ec = ReadEcPower();
        int expected = ec.TgpWatts + (ec.DynamicBoostEnabled ? ec.DbWatts : 0), budget = driver.ReadPowerBudget().LaptopWatts;
        if (budget < expected)
            DiagnosticLog.Warn($"[NVIDIA Power] Laptop budget {budget} W is below the EC request {expected} W on AC; Dynamic Boost may be stuck. " +
                "Restarting the \"NVIDIA Platform Controllers and Framework\" device restores it.");
    }

    /// <summary>
    /// System Info display: the NVIDIA budgets, or why they weren't read. Never opens NVIDIA
    /// (stock users) and never reads a GPU that Windows reports asleep.
    /// </summary>
    public GpuPowerBudget? ReadDisplayBudget(out string reason)
    {
        reason = "";
        if (PowerOverride is not { } driver) { reason = "NVIDIA not connected"; return null; }
        if (!driver.IsGpuAwake()) { reason = "GPU asleep, not read"; return null; }
        try { return driver.ReadPowerBudget(); }
        catch (Exception) { reason = "unavailable"; return null; }
    }

    public void ValidatePower(int tgpWatts, bool dynamicBoostEnabled, int dbWatts, bool? unlocked = null)
    {
        bool unlock = (unlocked ?? TgpUnlocked) && _canExtendEcLimit;
        int minimum = unlock ? UnlockedMinTgpWatts : _tgpLimit.Min, maximum = unlock ? UnlockedMaxTgpWatts : NormalMaxTgpWatts;
        int ecTgp = Math.Max(tgpWatts, _tgpLimit.Min);
        if (_tgpLimit.Encoding != "offset" || tgpWatts < minimum || tgpWatts > maximum ||
            RegisterEncoding.DecodeTGP(RegisterEncoding.EncodeTGP(ecTgp, _tgpOffset), _tgpOffset) != ecTgp)
            throw new ArgumentOutOfRangeException(nameof(tgpWatts), $"GPU TGP {tgpWatts} W exceeds model range {minimum}-{maximum} W or supported register encoding." +
                (CanUnlockTgp && !TgpUnlocked && (tgpWatts > _tgpLimit.Max && tgpWatts <= UnlockedMaxTgpWatts || tgpWatts < _tgpLimit.Min && tgpWatts >= UnlockedMinTgpWatts)
                    ? " Enable the TGP unlock on the Power tab to allow it." : ""));
        if (dynamicBoostEnabled && (_dbLimit.Encoding != "direct" || dbWatts < _dbLimit.Min || dbWatts > _dbLimit.Max || RegisterEncoding.EncodeDB(dbWatts) != dbWatts))
            throw new ArgumentOutOfRangeException(nameof(dbWatts), $"Dynamic Boost {dbWatts} W exceeds model range {_dbLimit.Min}-{_dbLimit.Max} W or supported register encoding.");
    }

    public IGpuPowerOverride EnsurePowerOverride()
    {
        if (PowerOverride != null) return PowerOverride;
        var open = OpenPowerOverride ?? throw new InvalidOperationException(PowerOverrideUnavailableReason ?? "No NVIDIA GPU found.");
        try { return PowerOverride = open(); }
        catch (Exception ex)
        {
            OpenPowerOverride = null; // Unsupported GPU/driver/BIOS: stop offering the unlock this session.
            PowerOverrideUnavailableReason = ex.Message;
            DiagnosticLog.Warn($"[NVIDIA Power] {ex.Message}");
            throw;
        }
    }

    /// <summary>
    /// Decide what a profile can apply now. Loading a profile never removes the unlock.
    /// Automatic (startup/recovery) loads never restart the GPU: settings needing the
    /// unlock or higher current ceilings are limited to the active range, with a notice,
    /// and everything else in the profile still applies.
    /// </summary>
    public GpuProfilePlan PlanProfile(GpuProfileSettings profile, bool automatic)
    {
        var s = new GpuProfileSettings
        {
            Tgp = profile.Tgp, DynamicBoostEnabled = profile.DynamicBoostEnabled, DynamicBoostWatts = profile.DynamicBoostWatts,
            BiosOverrideEnabled = profile.BiosOverrideEnabled, NvvddMilliamps = profile.NvvddMilliamps, MsvddMilliamps = profile.MsvddMilliamps
        };
        string notice = "";
        bool unlock = TgpUnlocked;
        // Only values the unlock could have produced are limited; anything else is refused below.
        if ((s.BiosOverrideEnabled == true || s.Tgp > NormalMaxTgpWatts || s.Tgp < NormalMinTgpWatts) && !unlock && _canExtendEcLimit &&
            s.Tgp >= UnlockedMinTgpWatts && s.Tgp <= UnlockedMaxTgpWatts)
        {
            if (!automatic && CanUnlockTgp && TryOpen()) unlock = true;
            else
            {
                s.Tgp = Math.Clamp(s.Tgp, NormalMinTgpWatts, NormalMaxTgpWatts);
                s.NvvddMilliamps = s.MsvddMilliamps = null;
                notice = automatic && CanUnlockTgp
                    ? "This profile uses the GPU unlock, which is off, so its GPU power was limited to the normal range. Load it manually to turn the unlock on."
                    : $"This profile uses the GPU unlock, which is unavailable ({PowerOverrideUnavailableReason ?? "no NVIDIA GPU found"}). Its GPU power was limited to the normal range.";
            }
        }
        ValidatePower(s.Tgp, s.DynamicBoostEnabled, s.DynamicBoostWatts, unlock);
        bool restart = unlock && !TgpUnlocked;
        var driver = PowerOverride;
        if (driver == null) s.NvvddMilliamps = s.MsvddMilliamps = null; // Never open NVIDIA for stock-range currents.
        else if (s.NvvddMilliamps != null || s.MsvddMilliamps != null || (unlock && s.BiosOverrideEnabled != true))
        {
            var live = driver.ReadCurrentLimits();
            if (unlock && s.BiosOverrideEnabled != true)
            {
                // A profile that doesn't need the unlock runs at stock currents, even while unlocked.
                // Legacy/stock-saved profiles have no currents; never leave raised ones in place.
                s.NvvddMilliamps ??= live.Nvvdd.DefaultMilliamps;
                s.MsvddMilliamps ??= live.Msvdd.DefaultMilliamps;
            }
            int? Clamp(int? value, GpuCurrentLimit rail, int maximum) => value is int v ? Math.Clamp(v, rail.MinimumMilliamps, maximum) : null;
            s.NvvddMilliamps = Clamp(s.NvvddMilliamps, live.Nvvdd, live.Nvvdd.AllowedMaximumMilliamps(unlock));
            s.MsvddMilliamps = Clamp(s.MsvddMilliamps, live.Msvdd, live.Msvdd.AllowedMaximumMilliamps(unlock));
            if (!restart && driver.RequiresRestart(unlock, s.NvvddMilliamps, s.MsvddMilliamps))
            {
                if (!automatic) restart = true;
                else
                {
                    s.NvvddMilliamps = Clamp(s.NvvddMilliamps, live.Nvvdd, live.Nvvdd.MaximumMilliamps);
                    s.MsvddMilliamps = Clamp(s.MsvddMilliamps, live.Msvdd, live.Msvdd.MaximumMilliamps);
                    if (driver.RequiresRestart(unlock, s.NvvddMilliamps, s.MsvddMilliamps)) s.NvvddMilliamps = s.MsvddMilliamps = null;
                    notice = "This profile's GPU current limits need a GPU restart, so they were limited to the active ceilings. Load it manually to apply them.";
                }
            }
        }
        return new(s, unlock, restart, notice);
    }

    private bool TryOpen()
    {
        try { EnsurePowerOverride(); return true; }
        catch { return false; }
    }

    public void ConfigureProfile(GpuProfilePlan plan)
    {
        ProfileNotice = plan.Notice;
        var s = plan.Settings;
        if (!plan.Restart && s.NvvddMilliamps == null && s.MsvddMilliamps == null) return;
        if (Failsafe?.Holds(ThermalAction.Gpu) == true)
            return; // Automatic applies retain the existing hold; ApplyGpuPower writes the minimum.
        var driver = PowerOverride!;
        if (plan.Restart)
            RunBlocking("Restarting the GPU to apply its BIOS limits…",
                () => driver.Configure(plan.Unlock, s.NvvddMilliamps, s.MsvddMilliamps, true));
        else driver.Configure(plan.Unlock, s.NvvddMilliamps, s.MsvddMilliamps, false);
    }

    /// <summary>Turn the unlock on/off from the Power tab. Restarts the GPU.</summary>
    public void SetUnlock(bool unlocked)
    {
        if (Failsafe?.Holds(ThermalAction.Gpu) == true)
            throw new InvalidOperationException("Reset the thermal failsafe before changing NVIDIA BIOS limits.");
        var driver = EnsurePowerOverride();
        if (driver.Enabled == unlocked) return;
        RunBlocking(unlocked ? "Restarting the GPU to raise its limits…" : "Restarting the GPU to restore its original limits…",
            () => driver.Configure(unlocked, null, null, true));
        // The restart cleared the driver request; reassert the EC request within the new range.
        var ec = ReadGpuPower();
        ApplyGpuPower(Math.Clamp(ec.TgpWatts, MinTgpWatts, MaxTgpWatts), ec.DynamicBoostEnabled, ec.DbWatts);
    }

    /// <summary>Firmware Defaults: remove the unlock (GPU restart) or reset live currents.</summary>
    public void RestoreDriverDefaults()
    {
        var driver = PowerOverride;
        if (driver == null) return;
        if (driver.Enabled)
            RunBlocking("Restarting the GPU to restore its original limits…", () => driver.Configure(false, null, null, true));
        else
        {
            var limits = driver.ReadCurrentLimits();
            driver.Configure(false, limits.Nvvdd.DefaultMilliamps, limits.Msvdd.DefaultMilliamps, false);
        }
    }

    public void CaptureDriverSettings(GpuProfileSettings settings)
    {
        if (PowerOverride == null) return; // Never opens NVIDIA just to save a profile.
        var limits = PowerOverride.ReadCurrentLimits();
        settings.NvvddMilliamps = limits.Nvvdd.CurrentMilliamps;
        settings.MsvddMilliamps = limits.Msvdd.CurrentMilliamps;
        // Only a profile whose values need the unlock asks for it.
        settings.BiosOverrideEnabled = TgpUnlocked && (settings.Tgp > NormalMaxTgpWatts || settings.Tgp < NormalMinTgpWatts ||
            settings.Tgp + (settings.DynamicBoostEnabled ? settings.DynamicBoostWatts : 0) > PowerOverride.StockPowerLimitWatts ||
            limits.Nvvdd.CurrentMilliamps > limits.Nvvdd.StockMaximumMilliamps || limits.Msvdd.CurrentMilliamps > limits.Msvdd.StockMaximumMilliamps);
    }

    public bool IsMinimumPowerRequested() => CanApplyMinimumTgp &&
        _guard.VerifiesSameAddress(_addrTgpDbCtrl) && _guard.VerifiesSameAddress(_addrTgp) &&
        _guard.VerifiesSameAddress(_addrDbTotalPower) && _guard.VerifiesSameAddress(_addrDbMaxTgp) &&
        (_device.ReadByte(_addrTgpDbCtrl) & _dbMask) == (0x05 & _dbMask) &&
        _device.ReadByte(_addrTgp) == RegisterEncoding.EncodeTGP(MinimumTgpWatts, _tgpOffset) &&
        _device.ReadByte(_addrDbTotalPower) == 0 && _device.ReadByte(_addrDbMaxTgp) == 0 &&
        (PowerOverride is not { Enabled: true } driver || driver.IsPowerLimitAtMost(MinimumTgpWatts));

    public void ValidateFailsafeRegisters(bool gpuPresent)
    {
        CanApplyMinimumTgp &= gpuPresent;
        if (!CanApplyMinimumTgp) return;
        try
        {
            var values = ReadEcPower(); // All four GPU registers must be readable.
            // An unlocked request legitimately sits above the normal range.
            int maximum = _canExtendEcLimit ? UnlockedMaxTgpWatts : _tgpLimit.Max;
            CanApplyMinimumTgp = values.TgpWatts >= _tgpOffset && values.TgpWatts <= maximum &&
                values.DbWatts >= 0 && values.DbWatts <= _dbLimit.Max && _device.ReadByte(_addrTgpDbCtrl) != 0xFF;
        }
        catch { CanApplyMinimumTgp = false; }
    }

    /// <summary>
    /// Read current GPU power values: EC, or the lower unlocked driver request when the
    /// EC sits at its minimum.
    /// </summary>
    public GpuPowerValues ReadGpuPower()
    {
        var values = ReadEcPower();
        if (values.TgpWatts == _tgpLimit.Min && PowerOverride is { Enabled: true } driver &&
            driver.ReadPowerLimits().RequestedWatts - (values.DynamicBoostEnabled ? values.DbWatts : 0) is int below && below < values.TgpWatts)
            values.TgpWatts = Math.Max(below, UnlockedMinTgpWatts);
        return values;
    }

    private GpuPowerValues ReadEcPower()
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

// What a profile load applies now, after limiting it to what this session allows.
sealed record GpuProfilePlan(GpuProfileSettings Settings, bool Unlock, bool Restart, string Notice);

struct GpuPowerValues
{
    public int TgpWatts;
    public bool DynamicBoostEnabled;
    public int DbWatts;
}
