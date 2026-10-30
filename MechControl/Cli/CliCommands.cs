using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization.Metadata;
using MechControl.Features;
using MechControl.Monitoring;
using MechControl.Profile;

namespace MechControl.Cli;

sealed record CliResult(int ExitCode, string Output)
{
    public const int Done = 0, Refused = 1, UsageError = 2;
    public static CliResult Error(string message) => new(Refused, message);
}

sealed class CliUsageException(string message) : Exception(message);

/// <summary>
/// mcctl commands. They run on the UI thread like the tabs and write through the same
/// controller entry points (model limits, WriteGuard, thermal failsafe), minus the dialogs:
/// nothing here restarts the GPU or turns the NVIDIA unlock on or off. Changes are live and
/// unsaved, like Apply without Save, so the next profile load (including the automatic one
/// after sleep or EC re-activation) replaces them. Options left out keep their current values.
/// </summary>
sealed class CliCommands
{
    private const string GpuUsage = "Usage: mcctl gpu [--tgp W] [--db W|on|off] [--nvvdd A] [--msvdd A] [--board-input A] [--json]";
    private const string CpuUsage = "Usage: mcctl cpu [--pl1 W] [--pl2 W] [--pl4 W] [--temp-limit C] [--json]";

    private readonly string _model;
    private readonly CpuPowerControl? _cpu;
    private readonly GpuPowerControl? _gpu;
    private readonly FanControl? _fan;
    private readonly GpuTemperatureFailsafe _failsafe;
    private readonly Func<MonitorData> _readMonitor;

    // Raised after a write so the matching tab shows the new values.
    public event Action? PowerApplied;
    public event Action? FanApplied;

    public CliCommands(string model, CpuPowerControl? cpu, GpuPowerControl? gpu, FanControl? fan,
        GpuTemperatureFailsafe failsafe, Func<MonitorData> readMonitor)
    {
        _model = model;
        _cpu = cpu;
        _gpu = gpu;
        _fan = fan;
        _failsafe = failsafe;
        _readMonitor = readMonitor;
    }

    public CliResult Execute(string[] args)
    {
        bool json = args.Contains("--json");
        string[] a = args.Where(arg => arg != "--json").ToArray();
        string command = a.FirstOrDefault() ?? "";
        try
        {
            var result = command switch
            {
                "status" when a.Length == 1 => Output(ReadStatus(), CliJsonContext.Default.CliStatus, StatusText, json),
                "status" => throw new CliUsageException("Usage: mcctl status [--json]"),
                "gpu" => SetGpu(Options(a, GpuUsage, "--tgp", "--db", "--nvvdd", "--msvdd", "--board-input"), json),
                "cpu" => SetCpu(Options(a, CpuUsage, "--pl1", "--pl2", "--pl4", "--temp-limit"), json),
                "fan" when a.Length == 3 && a[1] == "boost" && a[2] is ("on" or "off") => SetFanBoost(a[2] == "on", json),
                "fan" => throw new CliUsageException("Usage: mcctl fan boost on|off [--json]"),
                _ => throw new CliUsageException($"Unknown command \"{string.Join(' ', args)}\". Run mcctl help.")
            };
            if (command != "status") DiagnosticLog.Write($"[CLI] Applied: mcctl {string.Join(' ', args)}");
            return result;
        }
        catch (CliUsageException ex) { return new(CliResult.UsageError, ex.Message); }
        catch (Exception ex)
        {
            if (command != "status") DiagnosticLog.Warn($"[CLI] Not applied: mcctl {string.Join(' ', args)}: {ex.Message}");
            return CliResult.Error(ex.Message);
        }
    }

    private CliResult SetGpu(Dictionary<string, string> o, bool json)
    {
        var gpu = _gpu ?? throw new InvalidOperationException("GPU power control is unavailable on this machine (see mechcontrol.log).");
        var now = gpu.ReadGpuPower();
        int tgp = o.TryGetValue("--tgp", out var text) ? Whole("--tgp", text, "watts") : now.TgpWatts;
        bool db = now.DynamicBoostEnabled;
        int dbWatts = now.DbWatts;
        if (o.TryGetValue("--db", out text))
        {
            db = text != "off";
            if (text is not ("on" or "off")) dbWatts = Whole("--db", text, "watts, on or off");
        }
        bool power = o.ContainsKey("--tgp") || o.ContainsKey("--db");
        if (power) gpu.ValidatePower(tgp, db, dbWatts);

        GpuProfilePlan? currents = null;
        if (o.ContainsKey("--nvvdd") || o.ContainsKey("--msvdd") || o.ContainsKey("--board-input"))
        {
            var driver = gpu.PowerOverride ?? throw new InvalidOperationException(NvidiaUnavailable(gpu));
            var live = driver.ReadCurrentLimits();
            bool unlocked = gpu.TgpUnlocked;
            int? Current(string option, string rail, GpuCurrentLimit? limit)
            {
                if (!o.TryGetValue(option, out var value)) return null;
                int milliamps = Milliamps(option, value);
                if (limit == null) throw new NotSupportedException($"This GPU has no {rail} current limit.");
                var (min, max) = CurrentRange(limit, unlocked);
                if (milliamps >= min && milliamps <= max) return milliamps;
                throw new InvalidOperationException($"{rail} current {Amps(milliamps)} A is outside the range {Amps(min)}-{Amps(max)} A." +
                    (milliamps <= max || milliamps > limit.AllowedMaximumMilliamps(true) ? ""
                        : unlocked ? " Raising the active ceiling needs a GPU restart: apply it once from the Power tab."
                        : gpu.CanUnlockTgp ? " Turn on the unlock in the Power tab to allow it." : ""));
            }
            int? nvvdd = Current("--nvvdd", "NVVDD", live.Nvvdd);
            int? msvdd = Current("--msvdd", "MSVDD", live.Msvdd);
            int? boardInput = Current("--board-input", "Board input", live.BoardInput);
            if (driver.RequiresRestart(unlocked, nvvdd, msvdd, boardInput))
                throw new InvalidOperationException("These current limits need a GPU restart: apply them once from the Power tab.");
            currents = new(new GpuProfileSettings { NvvddMilliamps = nvvdd, MsvddMilliamps = msvdd, BoardInputMilliamps = boardInput },
                unlocked, Restart: false, Notice: "");
        }

        // The Power tab's Apply order: driver currents, then the EC/driver power request.
        try
        {
            _failsafe.ApplyExplicit(() =>
            {
                if (currents != null) gpu.ConfigureProfile(currents);
                if (power) gpu.ApplyGpuPower(tgp, db, dbWatts);
            });
        }
        finally { PowerApplied?.Invoke(); } // A partial apply or a trip changed values too.
        return Output(ReadGpu(gpu), CliJsonContext.Default.CliGpu, GpuText, json);
    }

    private CliResult SetCpu(Dictionary<string, string> o, bool json)
    {
        var cpu = _cpu ?? throw new InvalidOperationException("CPU power control is unavailable on this machine (see mechcontrol.log).");
        var now = cpu.ReadCpuPower();
        int Value(string option, int current, string unit) => o.TryGetValue(option, out var text) ? Whole(option, text, unit) : current;
        int pl1 = Value("--pl1", now.PL1Watts, "watts"), pl2 = Value("--pl2", now.PL2Watts, "watts");
        int pl4 = Value("--pl4", now.PL4Watts, "watts"), tempLimit = Value("--temp-limit", now.TempLimitC, "degrees C");
        cpu.ValidatePower(pl1, pl2, pl4, tempLimit);
        try { _failsafe.ApplyExplicit(() => cpu.ApplyCpuPower(pl1, pl2, pl4, tempLimit)); }
        finally { PowerApplied?.Invoke(); }
        return Output(ReadCpu(cpu), CliJsonContext.Default.CliCpu, CpuText, json);
    }

    private CliResult SetFanBoost(bool on, bool json)
    {
        var fan = _fan ?? throw new InvalidOperationException("Fan control is unavailable on this machine (see mechcontrol.log).");
        // As on the Fan tab: boost on is always allowed; only turning off an emergency boost waits for a reset.
        if (!on && _failsafe.Holds(ThermalAction.Fan))
            throw new InvalidOperationException("Fan boost is held on by the tripped thermal failsafe. Reset it or load a profile first.");
        try { fan.SetFanBoost(on); }
        finally { FanApplied?.Invoke(); }
        return Output(ReadFans(), CliJsonContext.Default.CliFans, FansText, json);
    }

    private CliStatus ReadStatus() => new(_model, new(_failsafe.IsTripped, _failsafe.StateText),
        _cpu is { } cpu ? ReadCpu(cpu) : null, _gpu is { } gpu ? ReadGpu(gpu) : null, ReadFans());

    private static CliCpu ReadCpu(CpuPowerControl cpu)
    {
        var v = cpu.ReadCpuPower();
        static CliSetting Setting(int value, (int Min, int Max) range) => new(value, range.Min, range.Max);
        return new(Setting(v.PL1Watts, cpu.PL1Range), Setting(v.PL2Watts, cpu.PL2Range), Setting(v.PL4Watts, cpu.PL4Range),
            Setting(v.TempLimitC, cpu.TempLimitRange));
    }

    private static CliGpu ReadGpu(GpuPowerControl gpu)
    {
        var ec = gpu.ReadGpuPower();
        string? nvidia = null;
        GpuPowerLimits? power = null;
        GpuCurrentLimits? limits = null;
        GpuTelemetry? live = null;
        // As the Power tab's live readings: never opens NVIDIA or reads a GPU Windows reports asleep.
        if (gpu.PowerOverride is not { } driver) nvidia = NvidiaUnavailable(gpu);
        else if (!driver.IsGpuAwake()) nvidia = "GPU asleep, not read";
        else
        {
            try { power = driver.ReadPowerLimits(); limits = driver.ReadCurrentLimits(); live = driver.ReadTelemetry(); }
            catch (Exception ex) { nvidia = "NVIDIA readback failed: " + ex.Message; power = null; limits = null; }
        }
        CliCurrent? Current(GpuCurrentLimit? limit, int? measured, int? predicted)
        {
            if (limit == null) return null;
            var (min, max) = CurrentRange(limit, gpu.TgpUnlocked);
            return new(limit.CurrentMilliamps, min, max, limit.DefaultMilliamps, measured, predicted);
        }
        return new(new(ec.TgpWatts, gpu.MinTgpWatts, gpu.MaxTgpWatts), ec.DynamicBoostEnabled,
            new(ec.DbWatts, gpu.MinDbWatts, gpu.MaxDbWatts), gpu.TgpUnlocked, gpu.CanUnlockTgp,
            nvidia, power?.CeilingWatts, power?.EnforcedWatts, power?.RequestedWatts,
            Current(limits?.Nvvdd, live?.NvvddMilliamps, live?.NvvddPredictedMilliamps),
            Current(limits?.Msvdd, live?.MsvddMilliamps, live?.MsvddPredictedMilliamps),
            Current(limits?.BoardInput, live?.BoardInputMilliamps, null),
            live?.CurrentCapMhz, live?.PowerCapMhz, live?.MaxClockMhz);
    }

    private CliFans ReadFans()
    {
        var m = _readMonitor();
        return new(_fan?.ReadFanBoost(), m.CpuFanRpm, m.GpuFanRpm, m.CpuFanPercent, m.GpuFanPercent, m.CpuTemp, m.GpuTemp);
    }

    // The Power tab's slider range (never below 1 A), capped at the active ceiling: above it needs a GPU restart.
    private static (int Min, int Max) CurrentRange(GpuCurrentLimit limit, bool unlocked) =>
        (Math.Max(1000, limit.MinimumMilliamps), Math.Min(limit.AllowedMaximumMilliamps(unlocked), limit.MaximumMilliamps));

    private static string NvidiaUnavailable(GpuPowerControl gpu) =>
        gpu.OpenPowerOverride != null ? "NVIDIA not connected; turn on the unlock once in the Power tab to use current limits"
        : gpu.PowerOverrideUnavailableReason ?? "No NVIDIA GPU control on this machine";

    // "--name value" pairs, each at most once, at least one.
    private static Dictionary<string, string> Options(string[] a, string usage, params string[] names)
    {
        var options = new Dictionary<string, string>();
        for (int i = 1; i < a.Length; i += 2)
            if (!names.Contains(a[i]) || i + 1 == a.Length || !options.TryAdd(a[i], a[i + 1]))
                throw new CliUsageException(usage);
        return options.Count > 0 ? options : throw new CliUsageException(usage);
    }

    private static int Whole(string option, string value, string unit) =>
        int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out int result) ? result
            : throw new CliUsageException($"{option} expects whole {unit}, not \"{value}\".");

    // Amps ("240", "10.25", "10.25A") or milliamps ("10250mA"), exact to the milliamp.
    private static int Milliamps(string option, string value)
    {
        bool milli = value.EndsWith("mA", StringComparison.OrdinalIgnoreCase);
        string number = milli ? value[..^2] : value.EndsWith("A", StringComparison.OrdinalIgnoreCase) ? value[..^1] : value;
        if (decimal.TryParse(number, NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture, out decimal amount))
        {
            decimal milliamps = milli ? amount : amount * 1000;
            if (milliamps == decimal.Truncate(milliamps) && milliamps > 0 && milliamps <= int.MaxValue) return (int)milliamps;
        }
        throw new CliUsageException($"{option} expects amps (e.g. 240 or 10.25) or milliamps (e.g. 10250mA), exact to 1 mA, not \"{value}\".");
    }

    private static CliResult Output<T>(T value, JsonTypeInfo<T> type, Func<T, string> text, bool json) =>
        new(CliResult.Done, json ? JsonSerializer.Serialize(value, type) : text(value));

    // Plain ASCII: agents and consoles read this through whatever code page is active.
    private static string StatusText(CliStatus s) => string.Join(Environment.NewLine,
        $"Model {s.Model}; thermal failsafe: {s.Failsafe.State}",
        s.Gpu is { } gpu ? GpuText(gpu) : "GPU   power control unavailable",
        s.Cpu is { } cpu ? CpuText(cpu) : "CPU   power control unavailable",
        FansText(s.Fans));

    private static string GpuText(CliGpu g)
    {
        var lines = new List<string>
        {
            $"GPU   TGP {Setting(g.Tgp, "W")}; Dynamic Boost {OnOff(g.DynamicBoost)}, {Setting(g.DynamicBoostWatts, "W")}; " +
            $"unlock {(g.Unlocked ? "on" : g.UnlockAvailable ? "off" : "unavailable")}"
        };
        if (g.Nvidia != null)
        {
            lines.Add($"      NVIDIA: {g.Nvidia}");
            return string.Join(Environment.NewLine, lines);
        }
        lines.Add($"      NVIDIA ceiling {g.CeilingWatts} W; enforced {g.EnforcedWatts} W; requested {(g.RequestedWatts is int w ? $"{w} W" : "none")}");
        void Current(string name, CliCurrent? c, bool predicted)
        {
            lines.Add(c == null ? $"      {name} current limit: not available on this GPU"
                : $"      {name} limit {Amps(c.LimitMa)} A ({Amps(c.MinMa)}-{Amps(c.MaxMa)} A, stock {Amps(c.StockMa)} A); measured {Reading(c.MeasuredMa)}" +
                  (predicted ? $"; at clock cap {Reading(c.AtClockCapMa)}" : ""));
        }
        Current("NVVDD", g.Nvvdd, true);
        Current("MSVDD", g.Msvdd, true);
        Current("Board input", g.BoardInput, false);
        lines.Add($"      Clock caps: current controller {Mhz(g.CurrentCapMhz)}; power controller {Mhz(g.PowerCapMhz)}; max {Mhz(g.MaxClockMhz)}");
        return string.Join(Environment.NewLine, lines);
    }

    private static string CpuText(CliCpu c) =>
        $"CPU   PL1 {Setting(c.Pl1, "W")}; PL2 {Setting(c.Pl2, "W")}; PL4 {Setting(c.Pl4, "W")}; temp limit {Setting(c.TempLimit, "C")}";

    private static string FansText(CliFans f) =>
        $"Fans  boost {(f.Boost is bool boost ? OnOff(boost) : "unavailable")}; " +
        $"CPU {f.CpuRpm} RPM, {f.CpuDutyPercent}%, {f.CpuTempC} C; GPU {f.GpuRpm} RPM, {f.GpuDutyPercent}%, {f.GpuTempC} C";

    private static string Setting(CliSetting s, string unit) => $"{s.Value} {unit} ({s.Min}-{s.Max} {unit})";
    private static string OnOff(bool on) => on ? "on" : "off";
    private static string Amps(int milliamps) => (milliamps / 1000.0).ToString("0.###", CultureInfo.InvariantCulture);
    private static string Reading(int? milliamps) => milliamps is int ma ? Amps(ma) + " A" : "--";
    private static string Mhz(int? mhz) => mhz is int value ? $"{value} MHz" : "--";
}
