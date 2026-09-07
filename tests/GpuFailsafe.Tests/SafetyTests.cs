using MechControl;
using MechControl.Driver;
using MechControl.Features;
using MechControl.Profile;
using System.Text.Json;

static partial class Program
{
    private static ThermalProtectionState HeldState(ThermalStage stage = ThermalStage.Tripped,
        ThermalAction actions = ThermalAction.Gpu, int threshold = 62, string reason = "Saved trip") => new()
    {
        Stage = stage, Held = actions, ResetThresholdC = threshold,
        LastTrip = new() { Time = new DateTimeOffset(2026, 9, 7, 0, 0, 0, TimeSpan.Zero),
            Reason = reason, ThresholdC = threshold, Actions = actions }
    };

    private static void RecoveryMatrix()
    {
        foreach (var a in Enum.GetValues<ThermalStage>())
        foreach (var b in Enum.GetValues<ThermalStage>())
        {
            var first = a == ThermalStage.None ? new() : HeldState(a, ThermalAction.Cpu, 75);
            var second = b == ThermalStage.None ? new() : HeldState(b, ThermalAction.Fan, 65);
            var result = ThermalProtectionState.Recover(first, second, false);
            Assert.Equal((ThermalStage)Math.Max((int)a, (int)b), result.Stage);
            Assert.Equal(first.Held | second.Held, result.Held);
            Assert.Equal<int?>(b != ThermalStage.None ? 65 : a != ThermalStage.None ? 75 : null, result.ResetThresholdC);
        }
        var uncertain = ThermalProtectionState.Recover(HeldState(actions: ThermalAction.Fan), null, true);
        Assert.Equal(ThermalAction.Gpu | ThermalAction.Fan, uncertain.Held);
        Assert.Contains("Saved protection state uncertain", uncertain.RecoveryDetails);
        var mixed = ThermalProtectionState.Recover(HeldState(reason: "event A"), HeldState(actions: ThermalAction.Cpu, reason: "event B"), false);
        Assert.Contains("different events", mixed.RecoveryDetails);
        Assert.Contains("event A", mixed.RecoveryDetails);
        Assert.Contains("event B", mixed.RecoveryDetails);
        var invalid = HeldState(actions: ThermalAction.Cpu);
        invalid.ResetThresholdC = 999;
        invalid.Stage = (ThermalStage)9;
        var safe = ThermalProtectionState.Recover(invalid, null, false);
        Assert.Equal(ThermalStage.Tripped, safe.Stage);
        Assert.Equal(ThermalAction.Cpu | ThermalAction.Gpu, safe.Held);
        var nullMetadata = HeldState();
        nullMetadata.RecoveryDetails = null!;
        nullMetadata.LastTrip!.Results = null!;
        var recovered = new GpuTemperatureFailsafe(() => 40, _ => { }, _ => { }, nullMetadata);
        Assert.Contains("PROTECTION INCOMPLETE", recovered.Status);
    }

    private static void PersistenceIntegration()
    {
        string root = Path.Combine(OperatingSystem.IsWindows() ? @"C:\Temp\tandem" : Path.GetTempPath(), "thermal-tests-" + Guid.NewGuid());
        Directory.CreateDirectory(root);
        string path = Path.Combine(root, "settings.json");
        void Write(string file, ThermalProtectionState state) => File.WriteAllText(file,
            JsonSerializer.Serialize(new UserProfileSettings { ThermalProtection = state }, AppJsonContext.Default.UserProfileSettings));
        try
        {
            Assert.Equal(ThermalStage.None, new UserProfileManager(root).ThermalProtection.Stage);
            File.WriteAllText(path, "{\"gpuFailsafeTripReason\":\"Legacy trip\",\"gpuFailsafeTripThresholdC\":70}");
            var legacy = new UserProfileManager(root).ThermalProtection;
            Assert.Equal(ThermalAction.Gpu, legacy.Held);
            Assert.Equal(70, legacy.ResetThresholdC);
            Assert.Equal("Legacy trip", legacy.LastTrip!.Reason);

            Write(path, HeldState(actions: ThermalAction.Cpu, threshold: 70));
            Write(path + ".tmp", HeldState(ThermalStage.ResetPending, ThermalAction.Fan, 65));
            var manager = new UserProfileManager(root);
            Assert.Equal(ThermalStage.Tripped, manager.ThermalProtection.Stage);
            Assert.Equal(ThermalAction.Cpu | ThermalAction.Fan, manager.ThermalProtection.Held);
            Assert.Equal(65, manager.ThermalProtection.ResetThresholdC);
            manager.SaveThermalProtection(manager.ThermalProtection);
            Assert.False(File.Exists(path + ".tmp"));

            Write(path + ".tmp", new()); // Interrupted release cannot clear committed actions.
            Assert.Equal(ThermalAction.Cpu | ThermalAction.Fan, new UserProfileManager(root).ThermalProtection.Held);
            File.WriteAllText(path + ".tmp", "{broken");
            Assert.Equal(ThermalAction.All, new UserProfileManager(root).ThermalProtection.Held);
            File.WriteAllText(path, "null");
            Write(path + ".tmp", HeldState(ThermalStage.ResetPending, ThermalAction.Fan));
            Assert.Equal(ThermalAction.Gpu | ThermalAction.Fan, new UserProfileManager(root).ThermalProtection.Held);
            File.Delete(path);
            Assert.Equal(ThermalStage.ResetPending, new UserProfileManager(root).ThermalProtection.Stage);

            manager = new UserProfileManager(root);
            manager.SaveThermalProtection(HeldState(ThermalStage.ResetPending, ThermalAction.All));
            var restarted = new GpuTemperatureFailsafe(() => 50, _ => { }, manager.SaveThermalProtection, new UserProfileManager(root).ThermalProtection);
            Assert.True(restarted.NeedsPowerRestore);
            Assert.True(restarted.ProtectionIncomplete); // Saved verification is never trusted.
            restarted.EnforceMinimum();
            Assert.False(restarted.ProtectionIncomplete);

            if (OperatingSystem.IsWindows())
            {
                using (var locked = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
                    Assert.Throws<UnauthorizedAccessException>(() => manager.SaveThermalProtection(new()));
                Assert.Equal(ThermalStage.ResetPending, manager.ThermalProtection.Stage);
                Assert.Equal(ThermalAction.All, new UserProfileManager(root).ThermalProtection.Held);
            }
            manager.SaveThermalProtection(new());
            Assert.Equal(ThermalStage.None, new UserProfileManager(root).ThermalProtection.Stage);
            Assert.False(File.Exists(path + ".tmp"));
            File.WriteAllText(path, "{\"thermalProtection\":{\"Stage\":\"invalid\",\"Held\":6,\"ResetThresholdC\":60}}");
            Assert.Equal(ThermalAction.All, new UserProfileManager(root).ThermalProtection.Held);
            File.WriteAllText(path, "{\"thermalProtection\":null}");
            Assert.Equal(ThermalStage.Tripped, new UserProfileManager(root).ThermalProtection.Stage);
            manager.SaveThermalProtection(new());

            var profile = JsonSerializer.Deserialize("{\"gpuFailsafe\":{\"enabled\":true}}", AppJsonContext.Default.UserProfile)!;
            Assert.False(profile.GpuFailsafe.FanBoost);
            Assert.False(profile.GpuFailsafe.ReduceCpuPower);
            Assert.True(profile.GpuFailsafe.ShowMainWindow);
            manager.SaveProfile(2, profile);
            Assert.True(manager.LoadProfile(2)!.GpuFailsafe.Enabled);
        }
        finally { Directory.Delete(root, true); }
    }

    private static void ReleaseFailure()
    {
        int writes = 0;
        var f = new GpuTemperatureFailsafe(() => 50, _ => writes++, state =>
        {
            if (state.Stage == ThermalStage.None) throw new IOException("release disk failure");
        }, HeldState(ThermalStage.ResetPending));
        Assert.Throws<IOException>(() => f.ApplyExplicit(ThermalAction.Gpu, p => f.ConfirmWrite(ThermalAction.Gpu, p)));
        Assert.True(f.NeedsPowerRestore);
        Assert.True(writes > 0);
        Assert.True(f.Holds(ThermalAction.Gpu));
    }

    private static void IndividualRestoration()
    {
        var f = new GpuTemperatureFailsafe(() => 50, _ => { }, _ => { }, HeldState(ThermalStage.ResetPending, ThermalAction.All));
        Assert.Throws<InvalidOperationException>(f.EnsureCanSaveProfile);
        f.OnTemperatureSample(45);
        GpuTemperatureFailsafe.Restoration? leaked = null;
        f.ApplyExplicit(ThermalAction.Gpu, p => { leaked = p; Assert.False(f.Holds(ThermalAction.Gpu, p)); f.ConfirmWrite(ThermalAction.Gpu, p); });
        Assert.Equal(ThermalAction.Cpu | ThermalAction.Fan, f.HeldActions);
        Assert.True(f.Holds(ThermalAction.Cpu, leaked));
        f.ApplyExplicit(ThermalAction.Cpu, p => f.ConfirmWrite(ThermalAction.Cpu, p));
        Assert.True(f.HasHold);
        f.ApplyExplicit(ThermalAction.Fan, p => f.ConfirmWrite(ThermalAction.Fan, p));
        Assert.False(f.HasHold);
        Assert.False(f.RequiresMonitoring);
        Assert.Null(f.TemperatureC);
        f.EnsureCanSaveProfile();
    }

    private static void InterruptedRestoration()
    {
        foreach (bool reactivate in new[] { false, true })
        {
            var f = new GpuTemperatureFailsafe(() => 40, _ => { }, _ => { }, HeldState(ThermalStage.ResetPending));
            f.EnforceMinimum();
            Assert.False(f.ProtectionIncomplete);
            Assert.Throws<InvalidOperationException>(() => f.ApplyExplicit(ThermalAction.Gpu, permit =>
            {
                f.ConfirmWrite(ThermalAction.Gpu, permit);
                if (reactivate) { f.InvalidateVerification(); Assert.True(f.ProtectionIncomplete); }
                else f.EnforceMinimum();
            }));
            Assert.True(f.HasHold);
        }
    }

    private static void PartialRestoration()
    {
        var writes = new List<ThermalAction>();
        var f = new GpuTemperatureFailsafe(() => 50, writes.Add, _ => { }, HeldState(ThermalStage.ResetPending, ThermalAction.All));
        Assert.Throws<IOException>(() => f.ApplyExplicit(ThermalAction.All, p =>
        {
            f.ConfirmWrite(ThermalAction.Gpu, p);
            throw new IOException("CPU restore failed");
        }));
        Assert.Equal(ThermalAction.All, f.HeldActions);
        Assert.SequenceEqual(new[] { ThermalAction.Gpu, ThermalAction.Fan, ThermalAction.Cpu }, writes);
        Assert.Throws<InvalidOperationException>(() => f.ApplyExplicit(ThermalAction.All, _ => { }));
    }

    private static void AutomaticApply()
    {
        var f = new GpuTemperatureFailsafe(() => 50, _ => { }, _ => { }, HeldState(ThermalStage.ResetPending, ThermalAction.All));
        f.ApplyExplicit(ThermalAction.All, p =>
        {
            Assert.Null(p);
            Assert.True(f.Holds(ThermalAction.Gpu));
            f.ConfirmWrite(ThermalAction.All, p);
        }, deliberate: false);
        Assert.True(f.NeedsPowerRestore);
        f.EnforceMinimum();
        Assert.True(f.LimitsApplied);
    }

    private static void ProfileTransitions()
    {
        foreach (bool oldEnabled in new[] { false, true })
        foreach (bool newEnabled in new[] { false, true })
        foreach (string failure in new[] { "fan", "cpu", "gpu", "settings" })
        {
            var f = new GpuTemperatureFailsafe(() => 40, _ => { }, _ => { }, supported: ThermalAction.All);
            var old = new GpuFailsafeSettings { Enabled = oldEnabled, ThresholdC = 62, ReduceCpuPower = true };
            var next = new GpuFailsafeSettings { Enabled = newEnabled, ThresholdC = 80, FanBoost = true };
            f.Configure(old);
            bool highlighted = true;
            Assert.Throws<IOException>(() => f.ApplyProfile(next, ThermalAction.All, _ =>
            {
                highlighted = false;
                Assert.Equal(oldEnabled || newEnabled, f.Enabled);
                Assert.Equal(oldEnabled ? 62 : 80, f.ThresholdC);
                throw new IOException(failure);
            }, deliberate: true));
            Assert.False(highlighted);
            Assert.True(f.ConfigurationPartial);
            Assert.Equal(oldEnabled || newEnabled, f.Enabled);
            Assert.Equal(oldEnabled, f.GetSettings().ReduceCpuPower);
            Assert.Equal(newEnabled, f.GetSettings().FanBoost);
        }
        int temperature = 40;
        var trip = new GpuTemperatureFailsafe(() => temperature, _ => { }, _ => { });
        trip.Configure(new() { Enabled = true });
        Assert.Throws<IOException>(() => trip.ApplyProfile(new(), ThermalAction.All, _ =>
        {
            temperature = 80;
            trip.CheckBeforeApply();
            throw new IOException("partial apply");
        }, true));
        Assert.True(trip.IsTripped);
        Assert.True(trip.Enabled);
    }

    private static void FirmwareDefaults()
    {
        var f = new GpuTemperatureFailsafe(() => 40, _ => { }, _ => { });
        f.Configure(new() { Enabled = true });
        Assert.Throws<InvalidOperationException>(() => f.ApplyFirmwareDefaults(_ => { }));
        f.TestTrip();
        f.Configure(new());
        Assert.Throws<InvalidOperationException>(() => f.ApplyFirmwareDefaults(_ => { }));
        f.Reset();
        Assert.Throws<IOException>(() => f.ApplyFirmwareDefaults(_ => throw new IOException("firmware write")));
        Assert.True(f.HasHold);
        f.ApplyFirmwareDefaults(p => f.ConfirmWrite(ThermalAction.All, p));
        Assert.False(f.HasHold);
    }

    private static void IndependentActions()
    {
        foreach (var failure in GpuTemperatureFailsafe.Actions(ThermalAction.All))
        {
            bool failing = true;
            var calls = new List<ThermalAction>();
            var f = new GpuTemperatureFailsafe(() => 40, a =>
            {
                calls.Add(a);
                if (a == failure && failing) throw new IOException(a + " unavailable");
            }, _ => { }, supported: ThermalAction.All);
            f.Configure(new() { ReduceCpuPower = true, FanBoost = true });
            f.TestTrip();
            Assert.SequenceEqual(new[] { ThermalAction.Gpu, ThermalAction.Fan, ThermalAction.Cpu }, calls);
            Assert.True(f.ProtectionIncomplete);
            Assert.Throws<InvalidOperationException>(f.Reset);
            f.Configure(new() { ShowMainWindow = false }); // Actions survive; the foreground preference can still change.
            Assert.False(f.ShowMainWindow);
            Assert.Equal(ThermalAction.All, f.HeldActions);
            failing = false;
            Repeat(5, () => f.OnTemperatureSample(40));
            Assert.False(f.ProtectionIncomplete);
            f.Reset();
            Assert.True(f.NeedsPowerRestore);
        }
    }

    private static void UnsupportedActions()
    {
        var calls = new List<ThermalAction>();
        var f = new GpuTemperatureFailsafe(() => 40, calls.Add, _ => { });
        f.Configure(new() { Enabled = true, ReduceCpuPower = true, FanBoost = true });
        Assert.True(f.GetSettings().ReduceCpuPower);
        Assert.Contains("CPU reduction unavailable", f.DescribeActions(f.GetSettings()));
        f.TestTrip();
        Assert.SequenceEqual(new[] { ThermalAction.Gpu }, calls);
        Assert.Equal(ThermalAction.Gpu, f.HeldActions);
    }

    private static void PeakAndHistory()
    {
        int reads = 0;
        var f = new GpuTemperatureFailsafe(() => { reads++; return 40; }, _ => { }, _ => { });
        f.Configure(new() { Enabled = true });
        f.OnTemperatureSample(55);
        f.OnTemperatureSample(null);
        Assert.Null(f.TemperatureC);
        Assert.Equal(55, f.PeakTemperatureC);
        f.Configure(f.GetSettings());
        Assert.Equal(55, f.PeakTemperatureC);
        int before = reads;
        _ = f.StateText; _ = f.LastTripText; _ = f.PeakTemperatureC;
        Assert.Equal(before, reads);
        f.TestTrip();
        Assert.Null(f.GetState().LastTrip!.TemperatureC);
        string last = f.LastTripText;
        f.Reset();
        Assert.Equal(last, f.LastTripText);
        Assert.Equal(40, f.PeakTemperatureC);
    }

    private static void CpuFanIntegration()
    {
        foreach (bool direct in new[] { false, true })
        {
            var device = new EcDevice();
            var profile = CreateGpuProfile(70, 80, 125);
            profile.FanTable = new() { CpuTempUpBase = "0xF00", CpuTempDownBase = "0xF10", CpuDutyBase = "0xF20",
                GpuTempUpBase = "0xF30", GpuTempDownBase = "0xF40", GpuDutyBase = "0xF50" };
            foreach (var (name, addr) in new[] { ("PL1", "0x783"), ("PL2", "0x784"), ("PL4", "0x785"),
                ("cpuTempLimitWrite", "0x786"), ("cpuTempLimitRead", "0x463"), ("fanControlMode", "0x751"), ("fanCurveEnable", "0x7C5") })
                profile.Registers[name] = addr;
            profile.Limits["PL1"] = new() { Min = 15, Max = 140, Encoding = "direct" };
            profile.Limits["PL2"] = new() { Min = 20, Max = 180, Encoding = "direct" };
            profile.Limits["PL4"] = new() { Min = 22, Max = 300, Encoding = direct ? "direct" : "double" };
            profile.Limits["cpuTempLimit"] = new() { Min = 80, Max = 95, Encoding = direct ? "direct" : "inverted_offset", TjMax = 105 };
            device.Registers[0x463] = direct ? (byte)90 : (byte)15;
            profile.VerifyRedirect["0x786"] = new() { ReadAddr = "0x463", Transform = direct ? "direct" : "inverted_offset" };
            device.WriteOverride = (a, b) => { if (a == 0x786) device.Registers[0x463] = b; };
            var guard = new WriteGuard(device, profile);
            var cpu = new CpuPowerControl(device, guard, profile);
            cpu.ValidateFailsafeRegisters();
            Assert.True(cpu.CanApplyMinimum);
            var fan = new FanControl(device, guard, profile);
            fan.ValidateFailsafeRegisters();
            Assert.True(fan.CanApplyBoost);
            var gpu = new GpuPowerControl(device, guard, profile);
            var f = new GpuTemperatureFailsafe(() => 40, a =>
            {
                if (a == ThermalAction.Gpu) gpu.ApplyGpuPower(gpu.MinimumTgpWatts, false, 0);
                if (a == ThermalAction.Cpu) cpu.ApplyMinimumPower();
                if (a == ThermalAction.Fan) fan.SetFanBoost(true);
            }, _ => { }, supported: ThermalAction.All,
                checkEmergency: a => a == ThermalAction.Cpu && cpu.IsMinimumPowerRequested());
            cpu.Failsafe = f; fan.Failsafe = f; gpu.Failsafe = f;
            f.Configure(new() { ReduceCpuPower = true, FanBoost = true });
            f.TestTrip();
            Assert.False(f.ProtectionIncomplete);
            Assert.Equal((byte)15, device.Registers[0x783]);
            Assert.Equal((byte)20, device.Registers[0x784]);
            Assert.Equal(direct ? (byte)22 : (byte)11, device.Registers[0x785]);
            Assert.Equal(direct ? (byte)0xDA : (byte)0x8F, device.Registers[0x786]);
            Assert.Equal((byte)0x40, device.Registers[0x751]);
            device.Writes.Clear();
            Repeat(10, () => f.OnTemperatureSample(40));
            Assert.False(device.Writes.Any(w => w.Address is >= 0x783 and <= 0x786));
            Assert.False(f.ProtectionIncomplete);
            foreach (int address in new[] { 0x783, 0x784, 0x785 })
            {
                device.Registers[address] = 100;
                device.Writes.Clear();
                Repeat(5, () => f.OnTemperatureSample(40));
                Assert.SequenceEqual(new[] { 0x783, 0x784, 0x785, 0x786 },
                    device.Writes.Where(w => w.Address is >= 0x783 and <= 0x786).Select(w => w.Address));
                Assert.True(cpu.IsMinimumPowerRequested());
            }
            foreach (byte invalid in new byte[] { 0, 127, 255 })
            {
                device.Registers[0x463] = invalid;
                device.Writes.Clear();
                Repeat(5, () => f.OnTemperatureSample(40));
                Assert.True(f.ProtectionIncomplete);
                Assert.False(device.Writes.Any(w => w.Address is >= 0x783 and <= 0x786));
                device.Registers[0x463] = direct ? (byte)90 : (byte)15;
                Repeat(5, () => f.OnTemperatureSample(40));
                Assert.False(f.ProtectionIncomplete);
            }
            fan.SetFanBoost(false);
            cpu.ApplyCpuPower(140, 180, 300, 95);
            Assert.Equal((byte)0x40, device.Registers[0x751]);
            Assert.Equal(90, cpu.ReadCpuPower().TempLimitC);
            // Firmware can change a valid temperature limit across resume. The
            // emergency contract preserves the fresh value, not a cached 90 C.
            foreach (int freshLimit in new[] { 85, 95 })
            {
                device.Registers[0x463] = direct ? (byte)freshLimit : (byte)(105 - freshLimit);
                device.Writes.Clear();
                Repeat(5, () => f.OnTemperatureSample(40));
                Assert.False(device.Writes.Any(w => w.Address is >= 0x783 and <= 0x786));
                Assert.Equal(freshLimit, cpu.ReadCpuPower().TempLimitC);
                f.InvalidateVerification();
                f.EnforceMinimum();
                Assert.False(f.ProtectionIncomplete);
                Assert.Equal(freshLimit, cpu.ReadCpuPower().TempLimitC);
                Assert.Equal(direct ? (byte)(0x80 | freshLimit) : (byte)(0x80 | (105 - freshLimit)), device.Registers[0x786]);
                cpu.ApplyCpuPower(140, 180, 300, 90);
                Assert.Equal(freshLimit, cpu.ReadCpuPower().TempLimitC);
                Assert.Equal((byte)15, device.Registers[0x783]);
            }
            device.ReadOverride = a => a == 0x751 ? (byte)0 : device.Registers.GetValueOrDefault(a);
            Assert.Throws<IOException>(() => fan.SetFanBoost(false));
            Assert.True(f.ProtectionIncomplete);
            Assert.Contains("fan boost not verified", f.Status);
            device.ReadOverride = null;
            f.EnforceMinimum();
            Assert.False(f.ProtectionIncomplete);
            device.ReadOverride = a => a == 0x463 ? throw new IOException("CPU temp read failed") : device.Registers.GetValueOrDefault(a);
            f.EnforceMinimum();
            Assert.True(f.ProtectionIncomplete);
            Assert.Contains("CPU not verified", f.Status);
            Assert.Contains("GPU: written and verified", f.LastTripText);
            device.ReadOverride = null;
            f.Reset();
            f.ApplyExplicit(ThermalAction.Cpu, p => cpu.ApplyCpuPower(100, 120, 200, 90, p));
            Assert.Equal(ThermalAction.Gpu | ThermalAction.Fan, f.HeldActions);
            Assert.Equal((byte)100, device.Registers[0x783]);

            profile.CpuLimitsAreFallback = true;
            Assert.False(new CpuPowerControl(device, guard, profile).CanApplyMinimum);
            profile.CpuLimitsAreFallback = false;
            profile.Limits["PL4"].Encoding = "double";
            profile.Limits["PL4"].Min = 23;
            Assert.False(new CpuPowerControl(device, guard, profile).CanApplyMinimum);
        }
    }

    private static void MaintenanceVerification()
    {
        int writes = 0, checks = 0, saves = 0;
        bool matches = true, fail = false, invalidateDuringCheck = false;
        GpuTemperatureFailsafe f = null!;
        f = new(() => 40, _ => { writes++; if (fail) throw new IOException("write failed"); }, _ => saves++,
            HeldState(actions: ThermalAction.Cpu), supported: ThermalAction.All, checkEmergency: _ =>
            {
                checks++;
                if (invalidateDuringCheck) f.InvalidateVerification();
                return matches;
            });
        void Tick() => Repeat(5, () => f.OnTemperatureSample(40));
        Tick(); // Restart has no trusted verification, even if registers match.
        Assert.Equal(1, writes);
        Assert.Equal(0, checks);
        int saved = saves;
        Tick();
        Assert.Equal(1, writes);
        Assert.Equal(1, checks);
        Assert.Equal(saved, saves);
        matches = false;
        Tick();
        Assert.Equal(2, writes);
        matches = true;
        f.InvalidateVerification();
        Tick();
        Assert.Equal(3, writes);
        Assert.Equal(2, checks);
        invalidateDuringCheck = true;
        Tick();
        Assert.True(f.ProtectionIncomplete);
        invalidateDuringCheck = false;
        Tick();
        Assert.Equal(4, writes);
        Assert.False(f.ProtectionIncomplete);
        fail = true;
        f.EnforceMinimum(); // Explicit enforcement cannot use the fast path.
        Assert.True(f.ProtectionIncomplete);
        fail = false;
        Tick();
        Assert.Equal(6, writes);
        Assert.False(f.ProtectionIncomplete);
        f.ReportWriteFailure(ThermalAction.Cpu, new IOException("ordinary write failed"));
        Tick();
        Assert.Equal(7, writes);
        f.Reset();
        Assert.Equal(8, writes);
        Assert.True(f.NeedsPowerRestore);
        Tick();
        Assert.Equal(8, writes); // Acknowledged/disabled holds also use fresh checks.
    }

    private static void TemperatureNotifications()
    {
        int changes = 0, samples = 0;
        var f = new GpuTemperatureFailsafe(() => 40, _ => { }, _ => { });
        f.Configure(new() { Enabled = true });
        f.Changed += () => changes++;
        f.TemperatureChanged += () => throw new IOException("temperature UI failed");
        f.TemperatureChanged += () => samples++;
        Repeat(20, () => f.OnTemperatureSample(45));
        Assert.Equal(20, samples);
        Assert.Equal(0, changes);
        f.OnTemperatureSample(62);
        Assert.True(f.IsTripped);
        Assert.True(changes > 0);
        changes = 0;
        Repeat(20, () => f.OnTemperatureSample(40));
        Assert.Equal(0, changes); // Stable verified results/history have not changed.
        f.Reset();
        Assert.True(changes > 0);
    }
}
