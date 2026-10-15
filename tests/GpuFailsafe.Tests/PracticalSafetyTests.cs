using MechControl;
using MechControl.Driver;
using MechControl.Features;
using MechControl.Profile;
using System.Text.Json;

static partial class Program
{
    private static void PracticalRecovery()
    {
        string root = Path.Combine(@"C:\Temp\tandem", "practical-recovery-" + Guid.NewGuid());
        Directory.CreateDirectory(root);
        string path = Path.Combine(root, "settings.json");
        try
        {
            foreach (string json in new[]
            {
                """{"thermalProtection":{"Stage":0,"Held":0,"LastTrip":{"Time":"not-a-date","Reason":"Old diagnostic record"}}}""",
                """{"lastProfile":"not-a-number","thermalProtection":{"Stage":0,"Held":0}}""",
                """{"profileNames":[null],"thermalProtection":{"Stage":0,"Held":0}}"""
            })
            {
                File.WriteAllText(path, json);
                var manager = new UserProfileManager(root);
                Assert.Equal(ThermalStage.None, manager.ThermalProtection.Stage);
                Assert.Equal(ThermalAction.None, manager.ThermalProtection.Held);
                Assert.True(manager.ConfigurationWarning.Length > 0);
                Assert.Equal(0, manager.LastUsedSlot);
            }
            foreach (var stage in new[] { ThermalStage.Tripped, (ThermalStage)1 })
            foreach (var action in new[] { ThermalAction.Gpu, ThermalAction.Cpu, ThermalAction.Fan, ThermalAction.All })
            {
                File.WriteAllText(path, $$$$"""{"thermalProtection":{"Stage":{{{{(int)stage}}}},"Held":{{{{(int)action}}}},"ResetThresholdC":35,"LastTrip":{"Time":"bad","Reason":"Original event","ThresholdC":35}}}""");
                var manager = new UserProfileManager(root);
                var recovered = manager.ThermalProtection;
                bool tripped = stage == ThermalStage.Tripped; // Stage 1 had already been reset.
                Assert.Equal(tripped ? ThermalStage.Tripped : ThermalStage.None, recovered.Stage);
                Assert.Equal(tripped ? action : ThermalAction.None, recovered.Held);
                Assert.Equal(35, recovered.LastTrip!.ThresholdC);
                Assert.Equal("Original event", recovered.LastTrip.Reason);
                Assert.Null(recovered.LastTrip.Time);
            }
            var first = HeldState(ThermalStage.Tripped, ThermalAction.Cpu);
            var second = HeldState(ThermalStage.Tripped, ThermalAction.Fan);
            second.LastTrip!.Time = first.LastTrip!.Time!.Value.AddDays(1);
            var merged = ThermalProtectionState.Recover(first, second, false);
            Assert.Equal(ThermalStage.Tripped, merged.Stage);
            Assert.Equal(ThermalAction.Cpu | ThermalAction.Fan, merged.Held);
            Assert.Equal("", merged.RecoveryDetails); // Different histories are not a recovery problem.
            File.WriteAllText(path, """{"thermalProtection":{"Stage":"broken","Held":4}}""");
            var unknown = new UserProfileManager(root).ThermalProtection;
            Assert.Equal(ThermalAction.Gpu | ThermalAction.Fan, unknown.Held);
            Assert.Null(unknown.LastTrip);
            Assert.Contains("configuration-recovery hold", unknown.RecoveryDetails);
            var cpuOnly = new UserProfileManager(root, gpuRecoverySupported: false).ThermalProtection;
            Assert.Equal(ThermalAction.Fan, cpuOnly.Held);
            File.WriteAllText(path, "{broken");
            cpuOnly = new UserProfileManager(root, gpuRecoverySupported: false).ThermalProtection;
            Assert.Equal(ThermalAction.None, cpuOnly.Held);
            Assert.Contains("no supported GPU fallback", cpuOnly.RecoveryDetails);
            int temp = 90;
            var f = new GpuTemperatureFailsafe(() => temp, _ => { }, _ => { }, unknown, ThermalAction.All);
            Assert.True(f.IsTripped);
            f.Reset(); // A configuration-recovery hold is cleared like any other trip.
            Assert.False(f.IsTripped);
            Assert.Null(f.GetState().LastTrip);
            Assert.Equal("", f.GetState().RecoveryDetails);
            temp = 48;

            foreach (int threshold in new[] { 50, 95 }) new GpuFailsafeSettings { ThresholdC = threshold }.Validate();
            foreach (int threshold in new[] { 0, 29, 30, 49, 96 })
                Assert.Throws<ArgumentOutOfRangeException>(() => new GpuFailsafeSettings { ThresholdC = threshold }.Validate());
            var genuine = new GpuTemperatureFailsafe(() => temp, _ => { }, _ => { });
            genuine.Configure(new() { Enabled = true });
            Sustain(genuine, 50);
            Assert.True(genuine.IsTripped);
            temp = 50;
            Assert.Throws<InvalidOperationException>(genuine.Reset);
            temp = 49; // Average 49.8 C: below the trigger, no extra 3 C margin.
            genuine.Reset();
            Assert.False(genuine.IsTripped);
        }
        finally { Directory.Delete(root, true); }
    }

    private static void PracticalProfiles()
    {
        string root = Path.Combine(@"C:\Temp\tandem", "practical-profiles-" + Guid.NewGuid());
        Directory.CreateDirectory(root);
        string path = Path.Combine(root, "userprofile_1.json"), settings = Path.Combine(root, "settings.json");
        try
        {
            var manager = new UserProfileManager(root);
            Assert.Null(manager.LoadProfile(1));
            foreach (string json in new[] { "{broken", "null", """{"gpuFailsafe":{"thresholdC":"50"}}""", """{"gpuFailsafe":{"thresholdC":29}}""", """{"gpuFailsafe":{"thresholdC":96}}""" })
            {
                File.WriteAllText(path, json);
                Assert.Throws<IOException>(() => manager.LoadProfile(1));
            }
            foreach (bool enabled in new[] { true, false })
            {
                File.WriteAllText(path, $$$"""{"gpuFailsafe":{"enabled":{{{enabled.ToString().ToLowerInvariant()}}},"thresholdC":30,"fanBoost":true}}""");
                var profile = manager.LoadProfile(1)!;
                Assert.Equal(enabled, profile.GpuFailsafe.Enabled);
                Assert.True(profile.GpuFailsafe.FanBoost);
                Assert.Equal(50, profile.GpuFailsafe.ThresholdC);
            }
            manager.SaveProfile(1, new() { Name = "committed" });
            manager.LastUsedSlot = 2;
            using (var locked = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
                Assert.Throws<UnauthorizedAccessException>(() => manager.SaveProfile(1, new() { Name = "unfinished" }));
            Assert.Equal("committed", manager.LoadProfile(1)!.Name);
            Assert.Equal(2, manager.LastUsedSlot);
            Assert.True(File.Exists(path + ".tmp"));
            Assert.Equal("committed", new UserProfileManager(root).LoadProfile(1)!.Name);
            using (var locked = new FileStream(settings, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                var error = Assert.Throws<IOException>(() => manager.SaveProfile(1, new() { Name = "replaced" }));
                Assert.Contains("was saved, but", error.Message);
            }
            Assert.Equal("replaced", manager.LoadProfile(1)!.Name);
            Assert.Equal(2, manager.LastUsedSlot);
            Assert.True(manager.SlotExists(1));
            File.Copy(path, path + ".tmp", overwrite: true);
            File.Delete(path);
            Assert.Null(manager.LoadProfile(1)); // Even with an unfinished temporary file.

        }
        finally { Directory.Delete(root, true); }
    }

    private static void PracticalGpuLimits()
    {
        var device = new EcDevice();
        var model = CreateGpuProfile(70, 80, 100);
        model.Limits["dynamicBoost"] = new() { Min = 10, Max = 20, Encoding = "direct" };
        var gpu = CreatePower(device, model);
        foreach (var request in new[] { (125, false, 0), (79, false, 0), (90, true, 9), (90, true, 21) })
        {
            Assert.Throws<ArgumentOutOfRangeException>(() => gpu.ApplyGpuPower(request.Item1, request.Item2, request.Item3));
            Assert.Equal(0, device.Writes.Count);
        }
        gpu.ApplyGpuPower(100, true, 20);
        Assert.Equal(100, gpu.ReadGpuPower().TgpWatts);
        gpu.ApplyGpuPower(80, false, 20);
        Assert.Equal((byte)0, device.Registers[DbTotal]);
        Assert.Equal((byte)0, device.Registers[DbMax]);
        var f = new GpuTemperatureFailsafe(() => 40, _ => gpu.ApplyGpuPower(80, false, 0), _ => { },
            checkEmergency: _ => gpu.IsMinimumPowerRequested());
        gpu.Failsafe = f;
        f.TestTrip();
        device.Writes.Clear();
        Repeat(10, () => f.OnTemperatureSample(40));
        Assert.Equal(0, device.Writes.Count);
        device.Registers[TgpDbCtrl] |= 0x80;
        Repeat(5, () => f.OnTemperatureSample(40));
        Assert.Equal(0, device.Writes.Count);
        foreach (int address in new[] { TgpDbCtrl, Tgp, DbTotal, DbMax })
        {
            device.Registers[address] ^= 2;
            device.Writes.Clear();
            Repeat(5, () => f.OnTemperatureSample(40));
            Assert.SequenceEqual(new[] { TgpDbCtrl, Tgp, DbTotal, DbMax }, device.Writes.Select(w => w.Address));
            Assert.True(gpu.IsMinimumPowerRequested());
            Assert.Equal((byte)0x85, device.Registers[TgpDbCtrl]);
        }
        bool failOnce = true;
        device.ReadOverride = a => a == DbMax && failOnce ? ThrowRead() : device.Registers.GetValueOrDefault(a);
        byte ThrowRead() { failOnce = false; throw new IOException("transient read"); }
        device.Writes.Clear();
        Repeat(5, () => f.OnTemperatureSample(40));
        Assert.Equal(4, device.Writes.Count);
        Assert.False(f.ProtectionIncomplete);
        device.ReadOverride = null;
        device.Writes.Clear();
        f.Reset();
        Assert.Equal(0, device.Writes.Count);
        Assert.False(f.IsTripped);
        f.InvalidateVerification();
        Repeat(5, () => f.OnTemperatureSample(40));
        Assert.Equal(0, device.Writes.Count); // Released: nothing left to enforce.

        model.SkipVerify.Add("0x744");
        var exceptional = CreatePower(device, model);
        Assert.False(exceptional.IsMinimumPowerRequested()); // No inferred readback authority.
    }

    private static void PracticalPersistenceOrder()
    {
        foreach (var failed in new[] { ThermalAction.None, ThermalAction.Gpu, ThermalAction.Fan, ThermalAction.Cpu })
        {
            var order = new List<string>();
            GpuTemperatureFailsafe f = null!;
            f = new(() => 40, a =>
            {
                Assert.Equal(ThermalAction.All, f.HeldActions);
                order.Add(a.ToString());
                if (a == failed) throw new IOException("action failure");
            }, _ => { order.Add("save"); throw new IOException("disk failure"); }, supported: ThermalAction.All);
            f.Configure(new() { FanBoost = true, ReduceCpuPower = true });
            f.TestTrip();
            Assert.SequenceEqual(new[] { "Gpu", "Fan", "Cpu", "save" }, order);
            Assert.Equal(ThermalAction.All, f.HeldActions);
            Assert.True(f.ProtectionIncomplete);
        }
    }
}
