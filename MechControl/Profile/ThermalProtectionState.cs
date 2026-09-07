namespace MechControl.Profile;

[Flags]
enum ThermalAction { None = 0, Gpu = 1, Cpu = 2, Fan = 4, All = Gpu | Cpu | Fan }

// Numeric order is also the conservative interrupted-write recovery order.
enum ThermalStage { None, ResetPending, Tripped }

class ThermalTripRecord
{
    public DateTimeOffset? Time { get; set; }
    public string Reason { get; set; } = "";
    public int? TemperatureC { get; set; }
    public int? ThresholdC { get; set; }
    public ThermalAction Actions { get; set; }
    public string ActionDescription { get; set; } = "";
    public bool ShowMainWindow { get; set; } = true;
    public Dictionary<string, string> Results { get; set; } = new();

    public ThermalTripRecord Copy() => new()
    {
        Time = Time, Reason = Reason ?? "", TemperatureC = TemperatureC, ThresholdC = ThresholdC,
        Actions = Actions, ActionDescription = ActionDescription ?? "", ShowMainWindow = ShowMainWindow,
        Results = new(Results ?? new())
    };
}

class ThermalProtectionState
{
    public ThermalStage Stage { get; set; }
    public ThermalAction Held { get; set; }
    public int? ResetThresholdC { get; set; }
    public ThermalTripRecord? LastTrip { get; set; }
    public string RecoveryDetails { get; set; } = "";

    public ThermalProtectionState Copy() => new()
    {
        Stage = Stage, Held = Held, ResetThresholdC = ResetThresholdC,
        LastTrip = LastTrip?.Copy(), RecoveryDetails = RecoveryDetails ?? ""
    };

    // Never let an interrupted release/reset outrank an existing obligation.
    // History on an unheld record is diagnostic only, not hardware authority.
    public static ThermalProtectionState Recover(ThermalProtectionState? committed,
        ThermalProtectionState? pending, bool unreadable)
    {
        var result = (committed ?? pending ?? new()).Copy();
        var records = new[] { committed, pending }.Where(r => r != null).Cast<ThermalProtectionState>().ToArray();
        var held = records.Where(r => r.Stage != ThermalStage.None || r.Held != ThermalAction.None).ToArray();
        bool invalid = records.Any(r => !Enum.IsDefined(r.Stage) || (r.Held & ~ThermalAction.All) != 0 ||
            (r.Stage == ThermalStage.None) != (r.Held == ThermalAction.None) ||
            (r.Stage != ThermalStage.None && (r.ResetThresholdC is not (>= 30 and <= 95) ||
                r.LastTrip == null || string.IsNullOrWhiteSpace(r.LastTrip.Reason))));
        if (held.Length > 0)
        {
            result.Stage = held.Any(r => r.Stage == ThermalStage.Tripped) ? ThermalStage.Tripped : ThermalStage.ResetPending;
            result.Held = held.Aggregate(ThermalAction.None, (a, r) => a | (r.Held & ThermalAction.All));
            result.ResetThresholdC = held.Where(r => r.ResetThresholdC is >= 30 and <= 95)
                .Select(r => r.ResetThresholdC).Min();
            result.LastTrip = held.FirstOrDefault(r => r.LastTrip != null)?.LastTrip?.Copy();
        }
        bool mixed = held.Length > 1 && (held[0].LastTrip?.Time != held[1].LastTrip?.Time ||
            held[0].LastTrip?.Reason != held[1].LastTrip?.Reason);
        if (unreadable || invalid || mixed)
        {
            result.Stage = ThermalStage.Tripped;
            result.Held |= ThermalAction.Gpu;
            result.ResetThresholdC ??= GpuFailsafeSettings.MinimumThreshold;
            string details = "Saved protection state uncertain" + (mixed ? " - recovered different events" : "") +
                ". Held actions were conservatively combined.\n" +
                string.Join("\n", held.Select(r => $"{r.LastTrip?.Time}: {r.LastTrip?.Reason ?? "event unavailable"}; {r.Held}; reset threshold {r.ResetThresholdC}"));
            result.RecoveryDetails = details.Length > 8192 ? details[..8192] : details;
            result.LastTrip ??= new() { Reason = "Saved protection state uncertain; original event unavailable." };
        }
        return result;
    }
}
