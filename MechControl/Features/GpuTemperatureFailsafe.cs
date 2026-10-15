using MechControl.Profile;

namespace MechControl.Features;

// UI-thread state machine. Samples and retry cadence belong to the shared EC monitor.
class GpuTemperatureFailsafe
{
    // Trip decisions use the mean of the last few one-second readings (at least
    // MinimumSamples), so a single noisy sample cannot trip while a real cooler
    // failure still trips within seconds.
    public const int AverageWindow = 5;
    public const int MinimumSamples = 3;

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
    private readonly Queue<int> _window = new();

    public bool Enabled => _settings.Enabled;
    public int ThresholdC => _settings.ThresholdC;
    public bool Available => (_supported & ThermalAction.Gpu) != 0;
    public bool Supports(ThermalAction action) => (_supported & action) == action;
    public ThermalAction HeldActions => _state.Held;
    public bool IsTripped => _state.Stage == ThermalStage.Tripped;
    public bool RequiresMonitoring => Enabled || IsTripped;
    private ThermalAction VerifiedActions => _verifiedEpoch == Volatile.Read(ref _hardwareEpoch) ? _verified : ThermalAction.None;
    public bool LimitsApplied => (VerifiedActions & _state.Held) == _state.Held && IsTripped;
    public bool ProtectionIncomplete => IsTripped && (!LimitsApplied || !_persisted);
    public bool ShowMainWindow => _settings.ShowMainWindow;
    public int InvalidSamples { get; private set; }
    public int? TemperatureC { get; private set; }
    public int? PeakTemperatureC { get; private set; }
    public double? AverageTemperatureC => _window.Count == 0 ? null : _window.Average();
    public string TripReason => IsTripped ? _state.LastTrip?.Reason ?? "Saved protection state uncertain" : "";
    public event Action? Changed;
    public event Action? TemperatureChanged;
    public event Action<string>? Alert;

    public string StateText => ProtectionIncomplete
        ? "PROTECTION INCOMPLETE - " + string.Join(", ", Actions(_state.Held & ~VerifiedActions).Select(a => ActionName(a) + " not verified")
            .Concat(_persisted ? Array.Empty<string>() : new[] { "persistence not verified" }))
        : IsTripped ? "TRIPPED - emergency settings written and verified"
        : !Available ? "Unavailable - GPU power/temperature support not validated"
        : !Enabled ? "Disabled"
        : TemperatureC == null ? $"Temperature unavailable - {InvalidSamples}/5 failed checks"
        : $"Armed - trips when the {AverageWindow} s average reaches {ThresholdC} C";

    public string Status => StateText + (IsTripped ? $"\nHeld actions: {string.Join(", ", Actions(_state.Held).Select(ActionName))}. {TripReason}" : "") +
        string.Concat(_errors.Select(p => $"\n{ActionName(p.Key)} not verified: {p.Value}")) +
        (_persistenceError.Length > 0 ? "\nPersistence not verified: " + _persistenceError : "") +
        (_state.RecoveryDetails.Length > 0 ? "\n" + _state.RecoveryDetails : "") +
        (IsTripped ? "\nStop workloads if protection is incomplete. To restore normal settings, Reset" +
            (Enabled ? $" (GPU average must be below the {ThresholdC} C trigger)" : "") + " or load a profile that matches your cooling." : "");

    public string LastTripText => _state.LastTrip is not { } trip ? "--" :
        $"{(trip.Time.HasValue ? trip.Time.Value.ToLocalTime().ToString("g") : "Time unavailable")}: {trip.Reason}" +
        (trip.TemperatureC.HasValue ? $"\nGPU {trip.TemperatureC} C; threshold {trip.ThresholdC} C." : "") +
        $"\n{trip.ActionDescription}\n" + string.Join("; ", trip.Results.Select(p => $"{p.Key}: {p.Value}")) +
        (_state.RecoveryDetails.Length > 0 ? "\n" + _state.RecoveryDetails : "");

    public void EnsureCanSaveProfile()
    {
        if (IsTripped) throw new InvalidOperationException("Reset the thermal failsafe before saving a profile; the current power/fan values are emergency settings.");
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
        _state = ThermalProtectionState.Recover(state, null, false, (supported & ThermalAction.Gpu) != 0);
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
        SetConfiguration(settings);
        CheckBeforeApply();
        Notify();
    }

    private void SetConfiguration(GpuFailsafeSettings settings)
    {
        bool newArm = settings.Enabled && !Enabled && !IsTripped;
        _settings = settings.Copy();
        if (newArm) { PeakTemperatureC = null; InvalidSamples = 0; _window.Clear(); }
        if (!RequiresMonitoring) ClearTemperature();
    }

    private void ClearTemperature() { TemperatureC = null; _window.Clear(); }

    private void AddSample(int temperature)
    {
        TemperatureC = temperature;
        PeakTemperatureC = Math.Max(PeakTemperatureC ?? temperature, temperature);
        InvalidSamples = 0;
        _window.Enqueue(temperature);
        if (_window.Count > AverageWindow) _window.Dequeue();
    }

    private bool OverThreshold => _window.Count >= MinimumSamples && AverageTemperatureC >= ThresholdC;
    private void TripOverThreshold() => Trip($"GPU averaged {AverageTemperatureC:0.#} C over the last {_window.Count} readings " +
        $"(threshold {ThresholdC} C, latest {TemperatureC} C).", (int)Math.Round(AverageTemperatureC!.Value));

    // Choosing a profile is the user's decision about the current cooling, so a
    // deliberate load clears an outstanding trip and arms that profile's own
    // trigger (which may trip again). Automatic startup/recovery loads keep it.
    public void ApplyProfile(GpuFailsafeSettings destination, Action apply, bool deliberate)
    {
        ValidateSettings(destination);
        if (deliberate) Release("Cleared by loading a profile");
        SetConfiguration(destination);
        try { ApplyExplicit(apply, deliberate); }
        finally { Notify(); }
    }

    public void CheckBeforeApply()
    {
        if (!Enabled || IsTripped || _enforcing) return;
        // A deliberate fresh check is not a recurring monitor/retry tick. One
        // failed read falls back to the recent average when there is one.
        if (ReadTemperature() is int temperature) AddSample(temperature);
        if (_window.Count == 0)
        {
            TemperatureC = null;
            Trip("GPU temperature unavailable before applying protected power settings.", null);
        }
        else if (OverThreshold) TripOverThreshold();
        else Notify();
    }

    private int? ReadTemperature()
    {
        try { int t = _readTemperature(); return t is > 0 and < 128 ? t : null; }
        catch { return null; }
    }

    public void OnTemperatureSample(int? temperature)
    {
        if (!RequiresMonitoring) return;
        if (temperature is > 0 and < 128) AddSample(temperature.Value);
        else { TemperatureC = null; InvalidSamples++; }
        if (!IsTripped && Enabled)
        {
            if (InvalidSamples >= 5) { Trip("GPU temperature unavailable for 5 consecutive checks.", null); return; }
            if (TemperatureC != null && OverThreshold) { TripOverThreshold(); return; }
        }
        if (IsTripped && ++_retryTicks >= 5) EnforceMinimum(checkExisting: true);
        Notify(TemperatureChanged);
    }

    public void TestTrip()
    {
        if (!Available || IsTripped) throw new InvalidOperationException("Test unavailable while protection is unsupported or already tripped.");
        Trip("Manual failsafe test (real emergency settings requested).", null);
    }

    private void Trip(string reason, int? temperature)
    {
        if (IsTripped) return;
        _state.Stage = ThermalStage.Tripped;
        _state.Held |= EffectiveActions(_settings) | ThermalAction.Gpu;
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
        if (!IsTripped || _enforcing) return;
        _enforcing = true;
        bool wasComplete = !ProtectionIncomplete;
        int epoch = Volatile.Read(ref _hardwareEpoch);
        var previousVerified = _verifiedEpoch == epoch ? _verified : ThermalAction.None;
        bool changed = !_persisted || previousVerified != _state.Held;
        _verifiedEpoch = epoch;
        _verified = ThermalAction.None;
        try
        {
            _retryTicks = 0;
            foreach (var action in Actions(_state.Held))
            {
                string result;
                try
                {
                    // Never substitute readback for initial/recovery writes, or
                    // trust an earlier hardware epoch's verification.
                    bool matches = false;
                    if (checkExisting && (previousVerified & action) != 0 && epoch == Volatile.Read(ref _hardwareEpoch))
                        try { matches = _checkEmergency?.Invoke(action) == true; }
                        catch { /* Failed readback loses confidence; the full guarded apply may recover. */ }
                    if (!matches)
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
            // Lower power first. A crash before this save can lose the new trip
            // record; hardware and durable state are not one atomic operation.
            if (!_persisted) Persist();
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

    // Clears the trip. Emergency values stay in hardware until something is
    // applied; the UI reapplies the active profile straight after a reset.
    // While enabled, a reading at or above the current trigger would trip again
    // immediately, so the user is told how to get out instead.
    public void Reset()
    {
        if (!IsTripped) return;
        if (Enabled)
        {
            if (ReadTemperature() is int temperature) AddSample(temperature);
            else TemperatureC = null;
            if (TemperatureC == null || OverThreshold)
            {
                Notify();
                throw new InvalidOperationException((TemperatureC == null ? "Reset refused: GPU temperature unavailable"
                    : $"Reset refused: GPU averages {AverageTemperatureC:0.#} C, at or above the {ThresholdC} C trigger") +
                    ", so the failsafe would trip again. Load a profile that matches your cooling, or raise the trigger / disable the failsafe and Apply, then Reset.");
            }
        }
        Release("Manually reset");
        if (!RequiresMonitoring) ClearTemperature();
        Notify();
    }

    private void Release(string reason)
    {
        if (!IsTripped) return;
        var next = _state.Copy();
        next.Stage = ThermalStage.None;
        next.Held = ThermalAction.None;
        next.RecoveryDetails = "";
        try { _save(next); }
        catch (Exception ex) { _persistenceError = ex.Message; _persisted = false; Notify(); throw; }
        _state = next;
        _persisted = true;
        _persistenceError = "";
        _errors.Clear();
        InvalidSamples = 0;
        PeakTemperatureC = TemperatureC;
        DiagnosticLog.Write($"[GPU Failsafe] {reason}; emergency hold released.");
    }

    public bool Holds(ThermalAction action) => IsTripped && (_state.Held & action) != 0;

    public void ReportWriteFailure(ThermalAction action, Exception error)
    {
        if (_enforcing || !Holds(action)) return;
        bool wasComplete = !ProtectionIncomplete;
        _verified &= ~action;
        _errors[action] = error.Message;
        if (_state.LastTrip is { } trip) trip.Results[ActionName(action)] = "NOT VERIFIED: " + error.Message;
        _persisted = false;
        Notify();
        if (wasComplete) NotifyAlert();
    }

    // Deliberate user writes are refused while tripped. Automatic startup/recovery
    // writes proceed; the controllers substitute the held emergency values.
    public void ApplyExplicit(Action apply, bool deliberate = true)
    {
        bool wasTripped = IsTripped;
        CheckBeforeApply();
        if (deliberate && IsTripped)
            throw new InvalidOperationException(wasTripped ? "Thermal failsafe is tripped. Reset it or load a profile first."
                : "Not applied: the thermal failsafe tripped. " + TripReason);
        int epoch = Volatile.Read(ref _hardwareEpoch);
        try
        {
            apply();
            if (deliberate && IsTripped) throw new InvalidOperationException("Failsafe tripped during application; emergency settings retained.");
            if (epoch != Volatile.Read(ref _hardwareEpoch))
                throw new InvalidOperationException("EC reactivated during application; the complete settings must be reapplied.");
        }
        catch
        {
            EnforceMinimum(); // Reassert any hold an interrupted write sequence left behind.
            throw;
        }
        finally { Notify(); }
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
