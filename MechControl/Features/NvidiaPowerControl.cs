using MechControl.Hardware.Nvidia;

namespace MechControl.Features;

/// <summary>Coordinates persistent BIOS ceilings with live driver requests.</summary>
sealed class NvidiaPowerControl : IGpuPowerOverride, IDisposable
{
    private static string RecoveryPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "MechControl", "NvidiaOverrides", "originals.txt");
    private readonly NvidiaApi api;
    private readonly NvidiaOverrideStore store;
    private readonly NvidiaBios original;
    private readonly object sync = new();
    // Firmware sets both laptop budgets at boot, power-source changes and EC changes. The values
    // before our first raise are returned when lowering; if firmware moved the platform budget
    // since our last write, the current values become the originals.
    private NvidiaBudgetSnapshot? budgetOriginal, budgetApplied;
    // Board input is offered only while a driver policy matches the BIOS field (VerifyActive).
    private bool boardInputVerified;
    private uint UnlockedCeilingMw => checked(original.Power.Maximum + (uint)Profile.UniversalDefaults.TgpUnlockHeadroomWatts * 1000);
    public bool Enabled { get; private set; }
    public int StockPowerLimitWatts => checked((int)(original.Power.Maximum / 1000));
    public int MinimumPowerLimitWatts => checked((int)((original.Power.Minimum + 999) / 1000));

    // Neither check initializes NVAPI/NVML, which could wake a sleeping discrete GPU.
    public static bool HasNvidiaAdapter() => NvidiaOverrideStore.HasNvidiaAdapter();
    public static bool HasSavedState() => File.Exists(RecoveryPath);

    private NvidiaPowerControl(NvidiaApi api, NvidiaOverrideStore store, NvidiaBios original, bool staged, byte[] active)
    {
        this.api = api; this.store = store; this.original = original;
        bool patched = !active.AsSpan().SequenceEqual(original.GetImage());
        VerifyActive(active);
        if (staged != patched)
            throw new IOException(staged
                ? "NVIDIA BIOS limits are staged but not active. Restart Windows to activate them, then reopen MechControl."
                : "Removed NVIDIA BIOS limits are still active. Restart Windows, then reopen MechControl.");
        Enabled = staged;
    }

    public static NvidiaPowerControl Open(int minimumTgpWatts)
    {
        var api = new NvidiaApi();
        NvidiaOverrideStore? store = null;
        try
        {
            var id = api.Identity;
            if (id.Domain != 0) throw new NotSupportedException("NVIDIA overrides do not support this PCI domain.");
            store = new NvidiaOverrideStore(id.DeviceId, id.SubsystemId, id.Bus, id.Device, id.Function, RecoveryPath);
            byte[] active = api.ReadBios();
            var bios = store.Open(active, out bool staged, out bool discarded);
            if (discarded)
                DiagnosticLog.Warn("[NVIDIA Power] Removed MechControl GPU overrides that no longer apply (GPU BIOS replaced or driver reinstalled).");
            if (bios.Nvvdd.Default > int.MaxValue / 2 || bios.Msvdd.Default > int.MaxValue / 2 || bios.BoardInput?.Default > int.MaxValue / 2)
                throw new NotSupportedException("The detected NVIDIA current ranges exceed supported slider units.");
            if (bios.DeviceId != id.DeviceId || !bios.Version.Equals(id.VbiosVersion, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("NVIDIA BIOS and GPU identity differ.");
            var controller = new NvidiaPowerControl(api, store, bios, staged, active);
            _ = api.ReadPowerBudget(); // Establish support before offering an unlock that cannot raise the laptop budget.
            var limits = api.ReadPowerLimits();
            if (minimumTgpWatts * 1000L < limits.MinimumMw || minimumTgpWatts * 1000L > bios.Power.Maximum)
                throw new NotSupportedException("The NVIDIA driver cannot accept this model's minimum TGP.");
            DiagnosticLog.Write($"[NVIDIA Power] {id.Name}, driver {id.DriverVersion}, BIOS {id.VbiosVersion}: " +
                $"stock ceiling {controller.StockPowerLimitWatts} W, active {limits.MaximumMw / 1000} W; " +
                $"stock currents {bios.Nvvdd.Default / 1000.0:0.###}/{bios.Msvdd.Default / 1000.0:0.###} A; " +
                $"board input {(controller.boardInputVerified && bios.BoardInput is { } input ? $"{input.Default / 1000.0:0.###} A" : "unavailable")}. Recovery: {store.RecoveryPath}");
            return controller;
        }
        catch { store?.Dispose(); api.Dispose(); throw; }
    }

    // A GPU restart holds the lock for several seconds on a worker thread. The UI
    // thread (including the thermal failsafe) fails fast and retries instead of hanging.
    private Held Enter()
    {
        if (!Monitor.TryEnter(sync, 250)) throw new InvalidOperationException("The NVIDIA GPU is restarting; try again in a few seconds.");
        return new Held(sync);
    }
    private readonly struct Held(object sync) : IDisposable { public void Dispose() => Monitor.Exit(sync); }

    public GpuPowerLimits ReadPowerLimits()
    {
        using (Enter())
        {
            var power = api.ReadPowerLimits();
            return new(checked((int)(power.MaximumMw / 1000)), checked((int)(power.EnforcedMw / 1000)),
                power.RequestedMw is uint mw ? checked((int)(mw / 1000)) : null);
        }
    }

    public GpuCurrentLimits ReadCurrentLimits() { using (Enter()) return Limits(); }

    private GpuCurrentLimits Limits()
    {
        var rails = api.ReadCurrentLimits();
        GpuCurrentLimit Convert(NvidiaCurrentLimit rail, NvidiaBiosField stock) => new(
            checked((int)Math.Max(rail.MinimumMa, stock.Minimum)), checked((int)stock.Default), checked((int)rail.MaximumMa),
            checked((int)rail.CurrentMa), checked((int)stock.Maximum));
        return new(Convert(rails.Nvvdd, original.Nvvdd), Convert(rails.Msvdd, original.Msvdd),
            boardInputVerified && original.BoardInput is { } input && rails.BoardInput is { } liveInput ? Convert(liveInput, input) : null);
    }

    public bool RequiresRestart(bool enabled, int? nvvddMilliamps, int? msvddMilliamps, int? boardInputMilliamps)
    {
        using (Enter()) return NeedsRestart(enabled, nvvddMilliamps, msvddMilliamps, boardInputMilliamps, Limits());
    }

    private bool NeedsRestart(bool enabled, int? nv, int? ms, int? input, GpuCurrentLimits live) =>
        enabled != Enabled || (enabled && (api.ReadPowerLimits().MaximumMw < UnlockedCeilingMw ||
            nv > live.Nvvdd.MaximumMilliamps || ms > live.Msvdd.MaximumMilliamps || input > live.BoardInput?.MaximumMilliamps));

    public void Configure(bool enabled, int? nvvddMilliamps, int? msvddMilliamps, int? boardInputMilliamps, bool allowRestart)
    {
        lock (sync)
        {
            var live = Limits();
            void Validate(int? value, GpuCurrentLimit? rail)
            {
                if (value.HasValue && rail == null) throw new NotSupportedException("Board input current control is unavailable.");
                if (rail == null) return;
                if (value is int v && (v < rail.MinimumMilliamps || v > rail.AllowedMaximumMilliamps(enabled)))
                    throw new ArgumentOutOfRangeException(nameof(value), $"Current must be within the detected range " +
                        $"{rail.MinimumMilliamps / 1000.0:0.###}–{rail.AllowedMaximumMilliamps(enabled) / 1000.0:0.###} A.");
            }
            Validate(nvvddMilliamps, live.Nvvdd); Validate(msvddMilliamps, live.Msvdd); Validate(boardInputMilliamps, live.BoardInput);
            bool restart = NeedsRestart(enabled, nvvddMilliamps, msvddMilliamps, boardInputMilliamps, live);
            if (restart && !allowRestart) throw new InvalidOperationException("These NVIDIA limits require a GPU restart. Apply them manually first.");
            // Unlocked, the registry holds each rail's maximum at the top of the unlocked range,
            // so every slider value applies live, and its default at the chosen current, so the
            // GPU starts there after a reboot or driver reset. Besides toggling the unlock, only
            // a value above a lower live maximum (staged by an older build) needs a restart.
            NvidiaRailOverride Rail(int? value, GpuCurrentLimit rail)
            {
                int max = rail.AllowedMaximumMilliamps(true);
                return new((uint)(value ?? Math.Min(rail.CurrentMilliamps, max)), (uint)max);
            }
            var request = enabled
                ? new NvidiaOverrideRequest(UnlockedCeilingMw, Rail(nvvddMilliamps, live.Nvvdd), Rail(msvddMilliamps, live.Msvdd),
                    live.BoardInput is { } input ? Rail(boardInputMilliamps, input) : null)
                : new();
            var records = original.BuildOverrides(request);
            if (restart)
            {
                RestoreBudget();
                byte[] expected = original.GetImage();
                foreach (var patch in records)
                    patch.Data.AsSpan(10).CopyTo(expected.AsSpan(checked((int)BitConverter.ToUInt32(patch.Data, 5))));
                store.Stage(original, records);
                NvidiaDeviceRestart.Restart(store.InstanceId);
                Reconnect();
                byte[] active = api.ReadBios();
                if (!active.AsSpan().SequenceEqual(expected)) throw new IOException("NVIDIA did not consume the staged BIOS limits. Restart Windows to activate them.");
                VerifyActive(active);
                Enabled = enabled;
                if (!boardInputVerified) boardInputMilliamps = null; // The driver didn't take its override; the rest still applies.
                if (!enabled) store.DeleteRecovery();
                // A restart clears the F8 request; the caller reapplies power through the
                // EC/failsafe path so a hold that started meanwhile is respected.
            }
            else if (enabled && !store.Matches(records)) store.Stage(original, records);
            if (nvvddMilliamps.HasValue || msvddMilliamps.HasValue || boardInputMilliamps.HasValue)
                api.SetCurrentLimits((uint?)nvvddMilliamps, (uint?)msvddMilliamps, (uint?)boardInputMilliamps);
            var now = Limits();
            DiagnosticLog.Write($"[NVIDIA Power] Override {(Enabled ? "on" : "off")}; currents {now.Nvvdd.CurrentMilliamps / 1000.0:0.###}/" +
                $"{now.Msvdd.CurrentMilliamps / 1000.0:0.###} A (ceilings {now.Nvvdd.MaximumMilliamps / 1000.0:0.###}/{now.Msvdd.MaximumMilliamps / 1000.0:0.###} A); " +
                $"board input {(now.BoardInput is { } board ? $"{board.CurrentMilliamps / 1000.0:0.###} A (ceiling {board.MaximumMilliamps / 1000.0:0.###} A)" : "unavailable")}; GPU restart {restart}.");
        }
    }

    // Above stock the laptop budget is raised too. Firmware may reset it a second later
    // (EC changes); the app checks IsPowerLimitApplied a few seconds after a raise.
    public void ApplyPowerLimit(int watts)
    {
        using (Enter())
        {
            uint target = checked((uint)watts * 1000);
            bool raise = Enabled && target > original.Power.Maximum;
            if (raise) RaiseBudget(target);
            api.SetPowerLimit(target);
            // Lowering never depends on the budget interfaces: F8 alone is enforced.
            if (!raise) RestoreBudget();
            else DiagnosticLog.Write($"[NVIDIA Power] Requested {watts} W.");
        }
    }

    public GpuPowerBudget ReadPowerBudget()
    {
        using (Enter())
        {
            var budget = api.ReadPowerBudget();
            return new(checked((int)(budget.F7Mw / 1000)), checked((int)(budget.PlatformMw / 1000)));
        }
    }

    // Polled twice a second for display: never block the UI thread behind a GPU restart.
    public GpuTelemetry ReadTelemetry()
    {
        if (!Monitor.TryEnter(sync)) throw new InvalidOperationException("The NVIDIA GPU is restarting.");
        try
        {
            var t = api.ReadTelemetry();
            static int? Milliamps(uint? value) => value is uint v ? checked((int)v) : null;
            static int? Mhz(uint? khz) => khz is uint v ? (int)(v / 1000) : null;
            return new(checked((int)t.NvvddMa), checked((int)t.MsvddMa), boardInputVerified ? Milliamps(t.BoardInputMa) : null,
                Milliamps(t.NvvddPredictedMa), Milliamps(t.MsvddPredictedMa), Mhz(t.CurrentCapKhz), Mhz(t.PowerCapKhz), Mhz(t.CeilingKhz));
        }
        finally { Monitor.Exit(sync); }
    }

    public bool IsGpuAwake() => store.IsAwake();

    public bool IsPowerLimitAtMost(int watts)
    {
        using (Enter())
        {
            var power = api.ReadPowerLimits();
            uint maximum = checked((uint)watts * 1000);
            return power.RequestedMw.HasValue && power.RequestedMw.Value <= maximum && power.EnforcedMw <= maximum;
        }
    }

    public bool IsPowerLimitApplied(int watts)
    {
        using (Enter())
        {
            uint target = checked((uint)watts * 1000);
            return api.ReadPowerLimits().RequestedMw == target &&
                (target <= original.Power.Maximum || api.ReadPowerBudget().F7Mw >= target);
        }
    }

    private void RaiseBudget(uint target)
    {
        var current = api.ReadPowerBudget();
        if (budgetApplied?.PlatformMw != current.PlatformMw) budgetOriginal = current;
        var desired = current.RaiseTo(target, budgetOriginal!);
        if (desired == current) return;
        // Recorded first: a partly applied write still restores to the originals.
        budgetApplied = desired;
        api.WritePowerBudget(desired);
    }

    // Best effort and never blocking: returns only values still ours.
    private void RestoreBudget()
    {
        if (budgetOriginal is not { } saved || budgetApplied is not { } applied) return;
        budgetOriginal = budgetApplied = null;
        try
        {
            var current = api.ReadPowerBudget();
            var desired = current with
            {
                F7Mw = current.F7Mw == applied.F7Mw ? saved.F7Mw : current.F7Mw,
                PlatformMw = current.PlatformMw == applied.PlatformMw ? saved.PlatformMw : current.PlatformMw
            };
            if (desired != current) api.WritePowerBudget(desired);
        }
        catch (Exception ex) { DiagnosticLog.Warn($"[NVIDIA Power] Laptop budget not restored: {ex.Message}"); }
    }

    private void Reconnect()
    {
        for (int attempt = 0; ; attempt++)
        {
            try
            {
                api.Reinitialize();
                // Dynamic Boost publishes its stock F7 request after device start,
                // later than NVAPI becomes callable. Let that initialization finish
                // before capturing or raising budgets (otherwise it overwrites us).
                Thread.Sleep(5000);
                return;
            }
            catch (Exception) when (attempt < 19) { Thread.Sleep(250); }
        }
    }

    private void VerifyActive(byte[] rom)
    {
        var bios = NvidiaBios.Parse(rom);
        var power = api.ReadPowerLimits();
        var rails = api.ReadCurrentLimits();
        if (power.MinimumMw != bios.Power.Minimum || power.DefaultMw != bios.Power.Default || power.MaximumMw != bios.Power.Maximum ||
            rails.Nvvdd.MinimumMa != bios.Nvvdd.Minimum || rails.Nvvdd.DefaultMa != bios.Nvvdd.Default || rails.Nvvdd.MaximumMa != bios.Nvvdd.Maximum ||
            rails.Msvdd.MinimumMa != bios.Msvdd.Minimum || rails.Msvdd.DefaultMa != bios.Msvdd.Default || rails.Msvdd.MaximumMa != bios.Msvdd.Maximum)
            throw new NotSupportedException("NVIDIA driver limits do not match the parsed BIOS policies.");
        // Optional: a driver that exposes this policy differently (or not at all) loses only
        // board input. Without it no slot-9 record is staged and the policy is never SET.
        boardInputVerified = bios.BoardInput is { } input && rails.BoardInput is { } live &&
            live.MinimumMa == input.Minimum && live.DefaultMa == input.Default && live.MaximumMa == input.Maximum;
        if (bios.BoardInput != null && !boardInputVerified)
            DiagnosticLog.Warn("[NVIDIA Power] Board input current limit unavailable: no driver policy matches the GPU BIOS.");
    }

    public void Dispose() { lock (sync) { store.Dispose(); api.Dispose(); } }
}
