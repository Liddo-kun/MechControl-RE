using MechControl;
using MechControl.Features;
using MechControl.Profile;
using System.Text.Json;

static partial class Program
{
    private static int Main()
    {
        (string Name, Action Test)[] tests =
        [
            ("settings defaults and threshold bounds", SettingsDefaultsAndThresholdBounds),
            ("settings JSON uses profile field names", SettingsJsonUsesProfileFieldNames),
            ("disabled performs no hardware or persistence work", DisabledPerformsNoWork),
            ("enabled configuration takes an initial fresh sample", ConfigureEnabledChecksImmediately),
            ("threshold is inclusive", ThresholdIsInclusive),
            ("invalid samples trip after five and valid resets counter", InvalidSamplesTripAndCounterResets),
            ("unavailable during Configure trips immediately", ConfigureUnavailableTripsImmediately),
            ("unavailable during CheckBeforeApply trips immediately", CheckBeforeApplyUnavailableTripsImmediately),
            ("latch survives disabled and changed profile settings", LatchSurvivesReconfigure),
            ("minimum enforcement retries every five samples", EnforcementRetriesEveryFiveSamples),
            ("persistence failure does not block hardware lowering", PersistenceFailureDoesNotBlockHardware),
            ("reset persistence failure retains latch", ResetPersistenceFailureRetainsLatch),
            ("reset requires a fresh valid cool sample and does not restore power", ResetRequiresFreshCoolSample),
            ("constructor restores persisted latch", ConstructorRestoresLatch),
            ("manual test trips while disabled", ManualTestTripsWhileDisabled),
            ("GPU power gate integrates with real guard and encoding", GpuPowerGateIntegration),
            ("reset-pending continues enforcement when disabled", ResetPendingEnforces),
            ("failed fresh enforcement refuses reset", FailedEnforcementRefusesReset),
            ("notification exceptions cannot interrupt safety", SubscriberFailuresAreContained),
            ("durable recovery orders stages and unions actions", RecoveryMatrix),
            ("real settings files recover pending/corrupt/legacy states", PersistenceIntegration),
            ("release persistence failure keeps and reasserts hold", ReleaseFailure),
            ("individual restoration releases only verified actions", IndividualRestoration),
            ("multi-action partial failure retains all obligations", PartialRestoration),
            ("automatic applies never release acknowledged holds", AutomaticApply),
            ("profile transition union and failure never disarm", ProfileTransitions),
            ("firmware defaults obey stages and verified completion", FirmwareDefaults),
            ("optional failures remain independent and retry", IndependentActions),
            ("unsupported preferences preserved but not executed", UnsupportedActions),
            ("peak and historical event semantics", PeakAndHistory),
            ("CPU and fan real guard encodings and failures", CpuFanIntegration),
            ("interleaved enforcement/reactivation cannot authorize release", InterruptedRestoration),
            ("stable maintenance checks never replace required writes", MaintenanceVerification),
            ("temperature notifications do not rebuild unchanged state", TemperatureNotifications),
        ];

        int failed = 0;
        foreach (var (name, test) in tests)
        {
            DiagnosticLog.Reset();
            try
            {
                test();
                Console.WriteLine($"PASS  {name}");
            }
            catch (Exception ex)
            {
                failed++;
                Console.Error.WriteLine($"FAIL  {name}");
                Console.Error.WriteLine(ex);
            }
        }

        Console.WriteLine($"{tests.Length - failed}/{tests.Length} tests passed.");
        return failed == 0 ? 0 : 1;
    }

    private static void SettingsDefaultsAndThresholdBounds()
    {
        var settings = new GpuFailsafeSettings();
        Assert.False(settings.Enabled);
        Assert.Equal(62, settings.ThresholdC);
        Assert.Equal(30, GpuFailsafeSettings.MinimumThreshold);
        Assert.Equal(95, GpuFailsafeSettings.MaximumThreshold);

        var rig = new Rig();
        var failsafe = rig.Create();
        Assert.Throws<ArgumentOutOfRangeException>(() => failsafe.Configure(new GpuFailsafeSettings
        {
            Enabled = true,
            ThresholdC = 29
        }));
        Assert.Throws<ArgumentOutOfRangeException>(() => failsafe.Configure(new GpuFailsafeSettings
        {
            Enabled = true,
            ThresholdC = 96
        }));
        Assert.Equal(0, rig.ReadCount);
    }

    private static void SettingsJsonUsesProfileFieldNames()
    {
        string json = JsonSerializer.Serialize(new GpuFailsafeSettings { Enabled = true, ThresholdC = 70 });
        using JsonDocument document = JsonDocument.Parse(json);
        JsonElement root = document.RootElement;

        Assert.True(root.GetProperty("enabled").GetBoolean());
        Assert.Equal(70, root.GetProperty("thresholdC").GetInt32());
        Assert.Equal(5, root.EnumerateObject().Count());
        Assert.False(root.GetProperty("reduceCpuPower").GetBoolean());
        Assert.False(root.GetProperty("fanBoost").GetBoolean());
        Assert.True(root.GetProperty("showMainWindow").GetBoolean());
    }

    private static void DisabledPerformsNoWork()
    {
        var rig = new Rig { Temperature = 80 };
        var failsafe = rig.Create();

        failsafe.Configure(new GpuFailsafeSettings { Enabled = false, ThresholdC = 70 });
        failsafe.CheckBeforeApply();
        failsafe.OnTemperatureSample(90);
        failsafe.OnTemperatureSample(null);
        failsafe.EnforceMinimum();
        failsafe.Reset();

        Assert.Equal(0, rig.ReadCount);
        Assert.Equal(0, rig.ApplyCount);
        Assert.Equal(0, rig.SaveCount);
        Assert.False(failsafe.IsTripped);
        Assert.False(failsafe.RequiresMonitoring);
        Assert.Null(failsafe.TemperatureC);
        Assert.Contains("Disabled", failsafe.Status);
    }

    private static void ConfigureEnabledChecksImmediately()
    {
        var rig = new Rig { Temperature = 61 };
        var failsafe = rig.Create();

        failsafe.Configure(new GpuFailsafeSettings { Enabled = true, ThresholdC = 62 });

        Assert.Equal(1, rig.ReadCount);
        Assert.Equal(61, failsafe.TemperatureC);
        Assert.True(failsafe.Enabled);
        Assert.True(failsafe.RequiresMonitoring);
        Assert.False(failsafe.IsTripped);
        Assert.Equal(0, rig.ApplyCount);
        Assert.Equal(0, rig.SaveCount);
        Assert.Contains("Armed - trips at 62 C", failsafe.Status);
    }

    private static void ThresholdIsInclusive()
    {
        var rig = new Rig { Temperature = 61 };
        var failsafe = rig.Create();
        failsafe.Configure(new GpuFailsafeSettings { Enabled = true, ThresholdC = 62 });

        failsafe.OnTemperatureSample(61);
        Assert.False(failsafe.IsTripped);
        failsafe.OnTemperatureSample(62);

        Assert.True(failsafe.IsTripped);
        Assert.Equal("GPU reached 62 C (threshold 62 C).", failsafe.TripReason);
        Assert.Equal(1, rig.ApplyCount);
        Assert.Equal(2, rig.SaveCount); // Durable intent, then verification results.
        Assert.True(failsafe.LimitsApplied);
    }

    private static void InvalidSamplesTripAndCounterResets()
    {
        var rig = new Rig { Temperature = 50 };
        var failsafe = rig.Create();
        failsafe.Configure(new GpuFailsafeSettings { Enabled = true, ThresholdC = 62 });

        failsafe.OnTemperatureSample(null);
        failsafe.OnTemperatureSample(0);
        failsafe.OnTemperatureSample(255);
        failsafe.OnTemperatureSample(null);
        Assert.False(failsafe.IsTripped);

        failsafe.OnTemperatureSample(50);
        Assert.False(failsafe.IsTripped);

        failsafe.OnTemperatureSample(null);
        failsafe.OnTemperatureSample(0);
        failsafe.OnTemperatureSample(255);
        failsafe.OnTemperatureSample(null);
        Assert.False(failsafe.IsTripped);
        failsafe.OnTemperatureSample(0);

        Assert.True(failsafe.IsTripped);
        Assert.Equal("GPU temperature unavailable for 5 consecutive checks.", failsafe.TripReason);
        Assert.Equal(1, rig.ApplyCount);
        Assert.Equal(2, rig.SaveCount);
    }

    private static void ConfigureUnavailableTripsImmediately()
    {
        var rig = new Rig { Temperature = 255 };
        var failsafe = rig.Create();

        failsafe.Configure(new GpuFailsafeSettings { Enabled = true, ThresholdC = 62 });

        Assert.Equal(1, rig.ReadCount);
        Assert.True(failsafe.IsTripped);
        Assert.Contains("unavailable before applying", failsafe.TripReason);
        Assert.Equal(1, rig.ApplyCount);
        Assert.Equal(2, rig.SaveCount);
    }

    private static void CheckBeforeApplyUnavailableTripsImmediately()
    {
        var rig = new Rig { Temperature = 50 };
        var failsafe = rig.Create();
        failsafe.Configure(new GpuFailsafeSettings { Enabled = true, ThresholdC = 62 });
        rig.ReadOverride = () => throw new InvalidOperationException("sensor offline");

        failsafe.CheckBeforeApply();

        Assert.Equal(2, rig.ReadCount);
        Assert.True(failsafe.IsTripped);
        Assert.Contains("unavailable before applying", failsafe.TripReason);
        Assert.Equal(1, rig.ApplyCount);
    }

    private static void LatchSurvivesReconfigure()
    {
        var rig = new Rig { Temperature = 50 };
        var failsafe = rig.Create();
        failsafe.Configure(new GpuFailsafeSettings { Enabled = true, ThresholdC = 62 });
        failsafe.OnTemperatureSample(62);
        string reason = failsafe.TripReason;

        failsafe.Configure(new GpuFailsafeSettings { Enabled = false, ThresholdC = 80 });
        Assert.True(failsafe.IsTripped);
        Assert.Equal(reason, failsafe.TripReason);
        Assert.False(failsafe.Enabled);
        Assert.True(failsafe.RequiresMonitoring);
        Assert.Equal(59, failsafe.ResetBelowC);

        failsafe.Configure(new GpuFailsafeSettings { Enabled = true, ThresholdC = 90 });
        Assert.True(failsafe.IsTripped);
        Assert.Equal(reason, failsafe.TripReason);
        Assert.Equal(90, failsafe.ThresholdC);
        Assert.Equal(59, failsafe.ResetBelowC);
        Assert.Equal(1, rig.ReadCount);
        Assert.Equal(1, rig.ApplyCount);
        Assert.Equal(2, rig.SaveCount);
    }

    private static void EnforcementRetriesEveryFiveSamples()
    {
        var rig = new Rig { Temperature = 50 };
        bool failApply = true;
        rig.ApplyOverride = () =>
        {
            if (failApply) throw new InvalidOperationException("EC unavailable");
        };
        var failsafe = rig.Create();
        failsafe.Configure(new GpuFailsafeSettings { Enabled = true, ThresholdC = 62 });
        failsafe.OnTemperatureSample(62);

        Assert.True(failsafe.IsTripped);
        Assert.False(failsafe.LimitsApplied);
        Assert.Equal(1, rig.ApplyCount);
        Assert.Contains("GPU not verified", failsafe.Status);

        Repeat(4, () => failsafe.OnTemperatureSample(40));
        Assert.Equal(1, rig.ApplyCount);
        failsafe.OnTemperatureSample(40);
        Assert.Equal(2, rig.ApplyCount);
        Assert.True(failsafe.IsTripped);
        Assert.False(failsafe.LimitsApplied);

        failApply = false;
        Repeat(4, () => failsafe.OnTemperatureSample(40));
        Assert.Equal(2, rig.ApplyCount);
        failsafe.OnTemperatureSample(40);
        Assert.Equal(3, rig.ApplyCount);
        Assert.True(failsafe.IsTripped);
        Assert.True(failsafe.LimitsApplied);
        Assert.Equal(3, rig.SaveCount);
    }

    private static void PersistenceFailureDoesNotBlockHardware()
    {
        var rig = new Rig { Temperature = 50 };
        rig.SaveOverride = (_, _) => throw new IOException("disk full");
        var failsafe = rig.Create();
        failsafe.Configure(new GpuFailsafeSettings { Enabled = true, ThresholdC = 62 });

        failsafe.OnTemperatureSample(62);

        Assert.SequenceEqual(["save", "apply"], rig.Operations);
        Assert.True(failsafe.IsTripped);
        Assert.True(failsafe.LimitsApplied);
        Assert.Equal(1, rig.ApplyCount);
        Assert.Equal(1, rig.SaveCount);
        Assert.Contains("Persistence not verified: disk full", failsafe.Status);

        Repeat(5, () => failsafe.OnTemperatureSample(40));
        Assert.Equal(2, rig.ApplyCount);
        Assert.Equal(2, rig.SaveCount);
        Assert.True(failsafe.IsTripped);
    }

    private static void ResetPersistenceFailureRetainsLatch()
    {
        var rig = new Rig { Temperature = 50 };
        rig.SaveOverride = (reason, _) =>
        {
            if (reason.Length == 0) throw new IOException("settings read-only");
        };
        var failsafe = rig.Create();
        failsafe.Configure(new GpuFailsafeSettings { Enabled = true, ThresholdC = 62 });
        failsafe.OnTemperatureSample(62);
        string reason = failsafe.TripReason;

        Assert.Throws<IOException>(() => failsafe.Reset());

        Assert.True(failsafe.IsTripped);
        Assert.Equal(reason, failsafe.TripReason);
        Assert.True(failsafe.LimitsApplied);
        Assert.Equal(2, rig.ApplyCount);
        Assert.Equal(3, rig.SaveCount);
    }

    private static void ResetRequiresFreshCoolSample()
    {
        var rig = new Rig { Temperature = 50 };
        var failsafe = rig.Create();
        failsafe.Configure(new GpuFailsafeSettings { Enabled = true, ThresholdC = 62 });
        failsafe.OnTemperatureSample(62);
        failsafe.OnTemperatureSample(40); // A cool cached sample must not authorize reset.

        rig.Temperature = 0;
        Assert.Throws<InvalidOperationException>(() => failsafe.Reset());
        Assert.True(failsafe.IsTripped);

        rig.Temperature = 60;
        Assert.Throws<InvalidOperationException>(() => failsafe.Reset());
        Assert.True(failsafe.IsTripped);

        rig.Temperature = 59;
        failsafe.Reset();

        Assert.False(failsafe.IsTripped);
        Assert.True(failsafe.LimitsApplied);
        Assert.Equal(59, failsafe.TemperatureC);
        Assert.True(failsafe.NeedsPowerRestore);
        Assert.Equal(4, rig.ReadCount);
        Assert.Equal(2, rig.ApplyCount);
        Assert.Equal(3, rig.SaveCount);
        Assert.Equal("", rig.Saves[^1].Reason);
        Assert.True(failsafe.Enabled);
        Assert.True(failsafe.RequiresMonitoring);
    }

    private static void ConstructorRestoresLatch()
    {
        var rig = new Rig { Temperature = 50 };
        var failsafe = rig.Create("Persisted over-temperature latch", 62);

        Assert.True(failsafe.IsTripped);
        Assert.True(failsafe.RequiresMonitoring);
        Assert.False(failsafe.Enabled);
        Assert.Equal("Persisted over-temperature latch", failsafe.TripReason);
        Assert.Equal(59, failsafe.ResetBelowC);
        Assert.Equal(0, rig.ReadCount);
        Assert.Equal(0, rig.ApplyCount);
        Assert.Equal(0, rig.SaveCount);

        Repeat(5, () => failsafe.OnTemperatureSample(40));
        Assert.Equal(1, rig.ApplyCount);
        Assert.Equal(2, rig.SaveCount);
        Assert.True(failsafe.IsTripped);
        Assert.True(failsafe.LimitsApplied);
    }

    private static void ManualTestTripsWhileDisabled()
    {
        var rig = new Rig { Temperature = 50 };
        var failsafe = rig.Create();
        int alerts = 0;
        failsafe.Alert += _ => alerts++;

        failsafe.TestTrip();

        Assert.True(failsafe.IsTripped);
        Assert.False(failsafe.Enabled);
        Assert.True(failsafe.RequiresMonitoring);
        Assert.Equal("Manual failsafe test (real emergency settings requested).", failsafe.TripReason);
        Assert.Equal(1, rig.ApplyCount);
        Assert.Equal(2, rig.SaveCount);
        Assert.Equal(1, alerts);
        Assert.Equal(0, rig.ReadCount);

        Assert.Throws<InvalidOperationException>(() => failsafe.TestTrip());
        Assert.Equal(1, rig.ApplyCount);
        Assert.Equal(2, rig.SaveCount);
        Assert.Equal(1, alerts);
    }

    private static void ResetPendingEnforces()
    {
        var rig = new Rig();
        var failsafe = rig.Create();
        failsafe.TestTrip();
        failsafe.Reset();
        failsafe.Configure(new());
        Assert.True(failsafe.RequiresMonitoring);
        int before = rig.ApplyCount;
        Repeat(5, () => failsafe.OnTemperatureSample(40));
        Assert.Equal(before + 1, rig.ApplyCount);
    }

    private static void FailedEnforcementRefusesReset()
    {
        var rig = new Rig();
        var failsafe = rig.Create();
        failsafe.TestTrip();
        rig.ApplyOverride = () => throw new IOException("verification failed");
        Assert.Throws<InvalidOperationException>(() => failsafe.Reset());
        Assert.True(failsafe.IsTripped);
    }

    private static void SubscriberFailuresAreContained()
    {
        var rig = new Rig();
        var failsafe = rig.Create();
        failsafe.Changed += () => throw new Exception("UI failed");
        failsafe.Alert += _ => throw new Exception("notification failed");
        failsafe.TestTrip();
        Repeat(5, () => failsafe.OnTemperatureSample(40));
        Assert.Equal(2, rig.ApplyCount);
    }

    private static void Repeat(int count, Action action)
    {
        for (int i = 0; i < count; i++) action();
    }

    private sealed class Rig
    {
        public int Temperature { get; set; } = 50;
        public Func<int>? ReadOverride { get; set; }
        public Action? ApplyOverride { get; set; }
        public Action<string, int>? SaveOverride { get; set; }
        public int ReadCount { get; private set; }
        public int ApplyCount { get; private set; }
        public int SaveCount { get; private set; }
        public List<string> Operations { get; } = new();
        public List<(string Reason, int Threshold)> Saves { get; } = new();

        public GpuTemperatureFailsafe Create(string tripReason = "", int tripThreshold = GpuFailsafeSettings.DefaultThreshold) =>
            new(Read, _ => Apply(), state => Save(state.Stage == ThermalStage.Tripped ? state.LastTrip!.Reason : "", state.ResetThresholdC ?? tripThreshold),
                string.IsNullOrEmpty(tripReason) ? null : HeldState(ThermalStage.Tripped, ThermalAction.Gpu, tripThreshold, tripReason));

        private int Read()
        {
            ReadCount++;
            return ReadOverride?.Invoke() ?? Temperature;
        }

        private void Apply()
        {
            ApplyCount++;
            Operations.Add("apply");
            ApplyOverride?.Invoke();
        }

        private void Save(string reason, int threshold)
        {
            SaveCount++;
            Operations.Add("save");
            Saves.Add((reason, threshold));
            SaveOverride?.Invoke(reason, threshold);
        }
    }

    private static class Assert
    {
        public static void True(bool condition, string? message = null)
        {
            if (!condition) throw new TestFailureException(message ?? "Expected true, got false.");
        }

        public static void False(bool condition, string? message = null) =>
            True(!condition, message ?? "Expected false, got true.");

        public static void Null(object? value)
        {
            if (value != null) throw new TestFailureException($"Expected null, got '{value}'.");
        }

        public static void Equal<T>(T expected, T actual)
        {
            if (!EqualityComparer<T>.Default.Equals(expected, actual))
                throw new TestFailureException($"Expected '{expected}', got '{actual}'.");
        }

        public static void Contains(string expectedSubstring, string actual)
        {
            if (!actual.Contains(expectedSubstring, StringComparison.Ordinal))
                throw new TestFailureException($"Expected '{actual}' to contain '{expectedSubstring}'.");
        }

        public static void SequenceEqual<T>(IEnumerable<T> expected, IEnumerable<T> actual)
        {
            if (!expected.SequenceEqual(actual))
                throw new TestFailureException($"Expected [{string.Join(", ", expected)}], got [{string.Join(", ", actual)}].");
        }

        public static TException Throws<TException>(Action action) where TException : Exception
        {
            try
            {
                action();
            }
            catch (TException ex)
            {
                return ex;
            }
            catch (Exception ex)
            {
                throw new TestFailureException($"Expected {typeof(TException).Name}, got {ex.GetType().Name}.");
            }

            throw new TestFailureException($"Expected {typeof(TException).Name}, but no exception was thrown.");
        }
    }

    private sealed class TestFailureException(string message) : Exception(message);
}
