using MechControl.Profile;

namespace MechControl.Features;

// UI-thread state machine. Samples and retry cadence belong to the shared EC monitor.
class GpuTemperatureFailsafe
{
    private readonly Func<int> _readTemperature;
    private readonly Action<ThermalAction> _applyEmergency;
    private readonly Func<ThermalAction, bool>? _checkEmergency;
    private readonly Action<ThermalProtectionState> _save;
    private readonly ThermalAction _supported;
    private readonly Func<ThermalAction, string> _describe;
    private ThermalProtectionState _state;
    private GpuFailsafeSettings _settings = new();
    private readonly Dictionary<ThermalAction, string> _errors = new();
    private ThermalAction _verified;
    private bool _persisted;
    private bool _enforcing;
    private int _retryTicks;
    private string _persistenceError = "";
    private string _subscriberError = "";
    private int _hardwareEpoch;
    private int _verifiedEpoch;
    private int _enforcementGeneration;

    public bool Enabled => _settings.Enabled;
    public int ThresholdC => _settings.ThresholdC;
    public bool Available => (_supported & ThermalAction.Gpu) != 0;
    public bool Supports(ThermalAction action) => (_supported & action) == action;
    public ThermalAction HeldActions => _state.Held;
    public bool IsTripped => _state.Stage == ThermalStage.Tripped;
    public bool NeedsPowerRestore => _state.Stage == ThermalStage.ResetPending;
    public bool HasHold => _state.Stage != ThermalStage.None;
    public bool RequiresMonitoring => Enabled || HasHold;
    private ThermalAction VerifiedActions => _verifiedEpoch == Volatile.Read(ref _hardwareEpoch) ? _verified : ThermalAction.None;
    public bool LimitsApplied => (VerifiedActions & _state.Held) == _state.Held && HasHold;
    public bool ProtectionIncomplete => HasHold && (!LimitsApplied || !_persisted);
    public bool NeedsAttention => HasHold;
    public bool ShowMainWindow => _settings.ShowMainWindow;
    public bool ConfigurationPartial { get; private set; }
    public int InvalidSamples { get; private set; }
    public int? TemperatureC { get; private set; }
    public int? PeakTemperatureC { get; private set; }
    public int ResetBelowC => Math.Min(_state.ResetThresholdC ?? ThresholdC, Enabled ? ThresholdC : 95) - 3;
    public string TripReason => IsTripped ? _state.LastTrip?.Reason ?? "Saved protection state uncertain" : "";
    public event Action? Changed;
    public event Action? TemperatureChanged;
    public event Action<string>? Alert;

    public string StateText => ProtectionIncomplete
        ? "PROTECTION INCOMPLETE - " + string.Join(", ", Actions(_state.Held & ~VerifiedActions).Select(a => ActionName(a) + " not verified")
            .Concat(_persisted ? Array.Empty<string>() : new[] { "persistence not verified" }))
        : IsTripped ? "TRIPPED - emergency settings written and verified"
        : NeedsPowerRestore ? "Reset - emergency settings retained; restore explicitly"
        : !Available ? "Unavailable - GPU power/temperature support not validated"
        : !Enabled ? "Disabled"
        : TemperatureC == null ? $"Temperature unavailable - {InvalidSamples}/5 failed checks"
        : $"Armed - trips at {ThresholdC} C";

    public string Status => StateText + (HasHold ? $"\nHeld actions: {string.Join(", ", Actions(_state.Held).Select(ActionName))}. {TripReason}" : "") +
        string.Concat(_errors.Select(p => $"\n{ActionName(p.Key)} not verified: {p.Value}")) +
        (_persistenceError.Length > 0 ? "\nPersistence not verified: " + _persistenceError : "") +
        (_state.RecoveryDetails.Length > 0 ? "\n" + _state.RecoveryDetails : "") +
        (IsTripped ? $"\nStop workloads if protection is incomplete. Reset requires a fresh GPU reading at or below {ResetBelowC} C." : "");

    public string LastTripText => _state.LastTrip is not { } trip ? "--" :
        $"{(trip.Time.HasValue ? trip.Time.Value.ToLocalTime().ToString("g") : "Time unavailable")}: {trip.Reason}" +
        (trip.TemperatureC.HasValue ? $"\nGPU {trip.TemperatureC} C; threshold {trip.ThresholdC} C." : "") +
        $"\n{trip.ActionDescription}\n" + string.Join("; ", trip.Results.Select(p => $"{p.Key}: {p.Value}")) +
        (_state.RecoveryDetails.Length > 0 ? "\n" + _state.RecoveryDetails : "");

    public void EnsureCanSaveProfile()
    {
        if (HasHold) throw new InvalidOperationException("Reset the failsafe and explicitly restore all held power/fan actions before saving a profile.");
    }

    public GpuTemperatureFailsafe(Func<int> readTemperature, Action<ThermalAction> applyEmergency,
        Action<ThermalProtectionState> save, ThermalProtectionState? state = null,
        ThermalAction supported = ThermalAction.Gpu, Func<ThermalAction, string>? describe = null,
        Func<ThermalAction, bool>? checkEmergency = null)
    {
        _readTemperature = readTemperature;
        _applyEmergency = applyEmergency;
        _checkEmergency = checkEmergency;
        _save = save;
        _state = ThermalProtectionState.Recover(state, null, false);
        _settings.ShowMainWindow = _state.LastTrip?.ShowMainWindow ?? true;
        _supported = supported;
        _describe = describe ?? (a => "Emergency actions: " + a);
        // Historical verification is not current verification after restart.
    }

    public GpuFailsafeSettings GetSettings() => _settings.Copy();
    public ThermalProtectionState GetState() => _state.Copy();
    public ThermalAction EffectiveActions(GpuFailsafeSettings settings) =>
        (ThermalAction.Gpu | (settings.ReduceCpuPower ? ThermalAction.Cpu : 0) |
            (settings.FanBoost ? ThermalAction.Fan : 0)) & _supported;

    public string DescribeActions(GpuFailsafeSettings settings)
    {
        string text = "On trip: " + _describe(EffectiveActions(settings)) + ".";
        if (!Available) text += " GPU protection unavailable on this model.";
        if (settings.ReduceCpuPower && !Supports(ThermalAction.Cpu)) text += " CPU reduction unavailable on this model; GPU protection remains active.";
        if (settings.FanBoost && !Supports(ThermalAction.Fan)) text += " Fan boost unavailable on this model; GPU protection remains active.";
        return text;
    }

    public void ValidateSettings(GpuFailsafeSettings settings)
    {
        settings.Validate();
        if (settings.Enabled && !Available) throw new InvalidOperationException("This profile requires unavailable GPU protection; profile not applied.");
    }

    public void Configure(GpuFailsafeSettings settings)
    {
        ValidateSettings(settings);
        bool newArm = settings.Enabled && !Enabled && !HasHold;
        _settings = settings.Copy();
        if (newArm) { PeakTemperatureC = null; InvalidSamples = 0; }
        if (!RequiresMonitoring) TemperatureC = null;
        CheckBeforeApply();
        Notify();
    }

    public void ApplyProfile(GpuFailsafeSettings destination, ThermalAction actions,
        Action<Restoration?> applyAndSaveSlot, bool deliberate)
    {
        ValidateSettings(destination);
        Configure(GpuFailsafeSettings.Transition(_settings, destination));
        try
        {
            ApplyExplicit(actions, applyAndSaveSlot, deliberate);
            // No second fresh read or weaker policy until hardware + slot save + release succeeded.
            _settings = destination.Copy();
            if (!RequiresMonitoring) TemperatureC = null;
            ConfigurationPartial = false;
        }
        catch { ConfigurationPartial = true; throw; }
        finally { Notify(); }
    }

    public void CheckBeforeApply()
    {
        if (!Enabled || IsTripped || _enforcing) return;
        var temperature = ReadTemperature();
        if (temperature == null)
        {
            TemperatureC = null;
            Trip("GPU temperature unavailable before applying protected power settings.", null);
        }
        else
        {
            // A deliberate fresh check is not a recurring monitor/retry tick.
            TemperatureC = temperature;
            PeakTemperatureC = Math.Max(PeakTemperatureC ?? temperature.Value, temperature.Value);
            InvalidSamples = 0;
            if (temperature >= ThresholdC) Trip($"GPU reached {temperature} C (threshold {ThresholdC} C).", temperature);
            else Notify();
        }
    }

    private int? ReadTemperature()
    {
        try { int t = _readTemperature(); return t is > 0 and < 128 ? t : null; }
        catch { return null; }
    }

    public void OnTemperatureSample(int? temperature)
    {
        if (!RequiresMonitoring) return;
        TemperatureC = temperature is > 0 and < 128 ? temperature : null;
        if (TemperatureC.HasValue) PeakTemperatureC = Math.Max(PeakTemperatureC ?? TemperatureC.Value, TemperatureC.Value);
        if (!IsTripped && Enabled)
        {
            if (TemperatureC == null)
            {
                if (++InvalidSamples >= 5) { Trip("GPU temperature unavailable for 5 consecutive checks.", null); return; }
            }
            else
            {
                InvalidSamples = 0;
                if (TemperatureC >= ThresholdC) { Trip($"GPU reached {TemperatureC} C (threshold {ThresholdC} C).", TemperatureC); return; }
            }
        }
        if (HasHold && ++_retryTicks >= 5) EnforceMinimum(checkExisting: true);
        Notify(TemperatureChanged);
    }

    public void TestTrip()
    {
        if (!Available || HasHold) throw new InvalidOperationException("Test unavailable until protection is supported and all outstanding actions are restored.");
        Trip("Manual failsafe test (real emergency settings requested).", null);
    }

    private void Trip(string reason, int? temperature)
    {
        if (IsTripped) return;
        _state.Stage = ThermalStage.Tripped;
        _state.Held |= EffectiveActions(_settings) | ThermalAction.Gpu;
        _state.ResetThresholdC = Math.Min(_state.ResetThresholdC ?? ThresholdC, ThresholdC);
        _state.LastTrip = new()
        {
            Time = DateTimeOffset.Now, Reason = reason, TemperatureC = temperature, ThresholdC = ThresholdC,
            Actions = _state.Held, ActionDescription = DescribeActions(_settings) +
                (_state.Held != EffectiveActions(_settings) ? " Retained obligations: " + _describe(_state.Held) + "." : ""),
            ShowMainWindow = _settings.ShowMainWindow
        };
        _persisted = false;
        DiagnosticLog.Warn("[GPU Failsafe] " + reason);
        EnforceMinimum();
        NotifyAlert();
    }

    public static IEnumerable<ThermalAction> Actions(ThermalAction actions)
    {
        // Fan action is prompt even if grouped CPU verification blocks for a while.
        foreach (var a in new[] { ThermalAction.Gpu, ThermalAction.Fan, ThermalAction.Cpu })
            if ((actions & a) != 0) yield return a;
    }

    private static string ActionName(ThermalAction action) => action switch
    {
        ThermalAction.Gpu => "GPU", ThermalAction.Cpu => "CPU", ThermalAction.Fan => "fan boost", _ => action.ToString()
    };

    public void EnforceMinimum() => EnforceMinimum(checkExisting: false);

    private void EnforceMinimum(bool checkExisting)
    {
        if (!HasHold || _enforcing) return;
        _enforcing = true;
        bool wasComplete = !ProtectionIncomplete;
        _enforcementGeneration++;
        int epoch = Volatile.Read(ref _hardwareEpoch);
        var previousVerified = _verifiedEpoch == epoch ? _verified : ThermalAction.None;
        bool changed = !_persisted || previousVerified != _state.Held;
        _verifiedEpoch = epoch;
        _verified = ThermalAction.None;
        try
        {
            _retryTicks = 0;
            bool intentSaved = _persisted || Persist(); // Disk failure never skips actions.
            foreach (var action in Actions(_state.Held))
            {
                string result;
                try
                {
                    // Never substitute readback for initial/recovery/restoration
                    // writes, or trust an earlier hardware epoch's verification.
                    if (!checkExisting || (previousVerified & action) == 0 ||
                        epoch != Volatile.Read(ref _hardwareEpoch) || _checkEmergency?.Invoke(action) != true)
                        _applyEmergency(action);
                    if (epoch != Volatile.Read(ref _hardwareEpoch))
                        throw new IOException("EC reactivated during emergency writes; the complete group must be reverified.");
                    _verified |= action;
                    _errors.Remove(action);
                    result = "written and verified (EC request, not physical enforcement)";
                }
                catch (Exception ex)
                {
                    _verified &= ~action;
                    if (!_errors.TryGetValue(action, out var previous) || previous != ex.Message)
                        DiagnosticLog.Error($"[GPU Failsafe] {action} not verified: {ex.Message}");
                    _errors[action] = ex.Message;
                    result = "NOT VERIFIED: " + ex.Message;
                }
                if (_state.LastTrip is { } trip && (!trip.Results.TryGetValue(ActionName(action), out var old) || old != result))
                {
                    trip.Results[ActionName(action)] = result;
                    _persisted = false;
                    changed = true;
                }
            }
            if (!_persisted && intentSaved) Persist();
        }
        finally { _enforcing = false; }
        if (changed || ProtectionIncomplete) Notify();
        if (wasComplete && ProtectionIncomplete) NotifyAlert();
    }

    private bool Persist()
    {
        try { _save(_state.Copy()); _persisted = true; _persistenceError = ""; return true; }
        catch (Exception ex)
        {
            if (_persistenceError != ex.Message) DiagnosticLog.Error("[GPU Failsafe] Persistence not verified: " + ex.Message);
            _persistenceError = ex.Message;
            _persisted = false;
            return false;
        }
    }

    // Called immediately by the watchdog, possibly off the UI thread. Only an
    // epoch changes here; UI/enforcement still run on the existing UI thread.
    public void InvalidateVerification() => Interlocked.Increment(ref _hardwareEpoch);

    public void Reset()
    {
        if (!IsTripped) return;
        TemperatureC = ReadTemperature();
        if (TemperatureC == null || TemperatureC > ResetBelowC)
        {
            Notify();
            throw new InvalidOperationException($"Reset refused: GPU must report a valid temperature at or below {ResetBelowC} C.");
        }
        EnforceMinimum();
        if (ProtectionIncomplete) throw new InvalidOperationException("Reset refused. " + Status);
        var next = _state.Copy();
        next.Stage = ThermalStage.ResetPending;
        try { _save(next); }
        catch (Exception ex) { _persistenceError = ex.Message; _persisted = false; Notify(); throw; }
        _state = next;
        InvalidSamples = 0;
        PeakTemperatureC = TemperatureC;
        DiagnosticLog.Write("[GPU Failsafe] Manually reset; emergency settings held until explicit restoration.");
        Notify();
    }

    // A short-lived, per-operation permit. Not a global bypass flag; ordinary
    // startup/recovery/emergency writes can neither obtain nor complete a release.
    public sealed class Restoration
    {
        internal GpuTemperatureFailsafe Owner { get; }
        internal ThermalAction Allowed { get; }
        internal ThermalAction Verified { get; set; }
        internal bool Active { get; set; } = true;
        internal Restoration(GpuTemperatureFailsafe owner, ThermalAction allowed) { Owner = owner; Allowed = allowed; }
    }

    public bool Holds(ThermalAction action, Restoration? permit = null) => (_state.Held & action) != 0 &&
        !(NeedsPowerRestore && permit is { Active: true } && permit.Owner == this && (permit.Allowed & action) == action);

    public void ConfirmWrite(ThermalAction action, Restoration? permit)
    {
        if (permit is { Active: true } && permit.Owner == this && !IsTripped) permit.Verified |= action & permit.Allowed;
    }

    public void ReportWriteFailure(ThermalAction action, Exception error)
    {
        if (_enforcing || (_state.Held & action) == 0) return;
        bool wasComplete = !ProtectionIncomplete;
        _verified &= ~action;
        _errors[action] = error.Message;
        if (_state.LastTrip is { } trip) trip.Results[ActionName(action)] = "NOT VERIFIED: " + error.Message;
        _persisted = false;
        Notify();
        if (wasComplete) NotifyAlert();
    }

    public void ApplyExplicit(ThermalAction actions, Action<Restoration?> apply, bool deliberate = true)
    {
        CheckBeforeApply();
        if (deliberate && IsTripped) throw new InvalidOperationException("Reset the thermal failsafe before restoring settings. " + Status);
        var permit = deliberate && NeedsPowerRestore ? new Restoration(this, actions & _state.Held) : null;
        int epoch = Volatile.Read(ref _hardwareEpoch);
        int generation = _enforcementGeneration;
        try
        {
            apply(permit);
            if (deliberate && IsTripped) throw new InvalidOperationException("Failsafe tripped during application; emergency settings retained.");
            if (epoch != Volatile.Read(ref _hardwareEpoch))
                throw new InvalidOperationException("EC reactivated during application; the complete settings must be reapplied.");
            if (permit == null || permit.Allowed == ThermalAction.None) return;
            if (IsTripped || (permit.Verified & permit.Allowed) != permit.Allowed)
                throw new InvalidOperationException("Restoration incomplete: not all requested actions were verified.");
            if (generation != _enforcementGeneration)
                throw new InvalidOperationException("Emergency enforcement or EC reactivation interrupted restoration; reapply explicitly.");
            var next = _state.Copy();
            next.Held &= ~permit.Allowed;
            if (next.Held == ThermalAction.None) { next.Stage = ThermalStage.None; next.ResetThresholdC = null; }
            // Hardware first, durable release second. Failed persistence retains
            // the old hold and reasserts emergency requests in the catch path.
            _save(next);
            if (epoch != Volatile.Read(ref _hardwareEpoch))
                throw new IOException("EC reactivated while saving restoration; retaining the emergency hold.");
            _state = next;
            _persisted = true;
            _persistenceError = "";
            foreach (var action in Actions(permit.Allowed)) _errors.Remove(action);
            if (!HasHold)
            {
                if (Enabled) { PeakTemperatureC = TemperatureC; InvalidSamples = 0; }
                else TemperatureC = null;
            }
        }
        catch
        {
            if (permit != null) permit.Active = false;
            _persisted = false;
            EnforceMinimum();
            throw;
        }
        finally { if (permit != null) permit.Active = false; Notify(); }
    }

    public void ApplyFirmwareDefaults(Action<Restoration?> apply)
    {
        if (Enabled || IsTripped) throw new InvalidOperationException("Reset the trip and disable temperature protection before restoring firmware defaults.");
        ApplyExplicit(ThermalAction.All, apply);
    }

    private void Notify() => Notify(Changed);

    private void Notify(Action? handlers)
    {
        if (handlers == null) return;
        foreach (Action handler in handlers.GetInvocationList())
            try { handler(); } catch (Exception ex) { ReportSubscriber(ex); }
    }

    private void NotifyAlert()
    {
        if (Alert == null) return;
        foreach (Action<string> handler in Alert.GetInvocationList())
            try { handler(Status); } catch (Exception ex) { ReportSubscriber(ex); }
    }

    private void ReportSubscriber(Exception ex)
    {
        if (_subscriberError != ex.Message) DiagnosticLog.Error("[GPU Failsafe] UI/notification subscriber failed: " + ex.Message);
        _subscriberError = ex.Message;
    }
}
