namespace MechControl.Profile;

[Flags]
enum ThermalAction { None = 0, Gpu = 1, Cpu = 2, Fan = 4, All = Gpu | Cpu | Fan }

// 1 was the retired "reset, restore power explicitly" stage. Saved files still use 2.
enum ThermalStage { None = 0, Tripped = 2 }

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
    public ThermalTripRecord? LastTrip { get; set; }
    public string RecoveryDetails { get; set; } = "";

    public ThermalProtectionState Copy() => new()
    {
        Stage = Stage, Held = Held, LastTrip = LastTrip?.Copy(), RecoveryDetails = RecoveryDetails ?? ""
    };

    // Never let an interrupted reset outrank an existing trip.
    // History on an unheld record is diagnostic only, not hardware authority.
    public static ThermalProtectionState Recover(ThermalProtectionState? committed,
        ThermalProtectionState? pending, bool unreadable, bool gpuRecoverySupported = true)
    {
        // The retired stage 1 means the user had already reset the trip.
        var records = new[] { committed, pending }.Where(r => r != null).Cast<ThermalProtectionState>()
            .Select(r => (int)r.Stage == 1 ? new ThermalProtectionState { LastTrip = r.LastTrip?.Copy() } : r).ToArray();
        var result = (records.FirstOrDefault() ?? new()).Copy();
        var held = records.Where(r => r.Stage == ThermalStage.Tripped).ToArray();
        bool invalid = records.Any(r => !Enum.IsDefined(r.Stage) || (r.Held & ~ThermalAction.All) != 0 ||
            (r.Stage == ThermalStage.None) != (r.Held == ThermalAction.None));
        if (held.Length > 0)
        {
            result.Stage = ThermalStage.Tripped;
            result.Held = held.Aggregate(ThermalAction.None, (a, r) => a | (r.Held & ThermalAction.All));
            result.LastTrip = held.FirstOrDefault(r => r.LastTrip != null)?.LastTrip?.Copy();
        }
        if (unreadable || invalid)
        {
            result.Held &= ThermalAction.All;
            if (gpuRecoverySupported) result.Held |= ThermalAction.Gpu;
            result.Stage = result.Held == ThermalAction.None ? ThermalStage.None : ThermalStage.Tripped;
            result.RecoveryDetails += result.Held == ThermalAction.None
                ? "\nSaved protection state uncertain - no supported GPU fallback; review configuration."
                : "\nSaved protection state uncertain - configuration-recovery hold, not a measured thermal event. Reset clears it.";
        }
        if (result.RecoveryDetails.Length > 8192) result.RecoveryDetails = result.RecoveryDetails[..8192];
        return result;
    }
}