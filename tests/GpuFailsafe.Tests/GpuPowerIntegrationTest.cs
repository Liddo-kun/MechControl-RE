using MechControl.Driver;
using MechControl.Features;
using MechControl.Profile;

static partial class Program
{
    private const int GpuTemp = 0x440;
    private const int TgpDbCtrl = 0x743;
    private const int Tgp = 0x744;
    private const int DbTotal = 0x745;
    private const int DbMax = 0x746;

    private static void GpuPowerGateIntegration()
    {
        var device = new EcDevice();
        device.Registers[GpuTemp] = 61;
        device.Registers[TgpDbCtrl] = 0xA2; // Preserve unrelated high bits.
        GpuPowerControl power = CreatePower(device, CreateGpuProfile(95, 95, 150));
        Assert.True(power.CanApplyMinimumTgp);
        power.ValidateFailsafeRegisters(gpuPresent: true);
        Assert.True(power.CanApplyMinimumTgp);
        var failsafe = new GpuTemperatureFailsafe(
            () => device.ReadByte(GpuTemp),
            _ => power.ApplyGpuPower(power.MinimumTgpWatts, false, 0),
            _ => { });
        power.Failsafe = failsafe;
        failsafe.Configure(new GpuFailsafeSettings { Enabled = true, ThresholdC = 62 });

        Repeat(2, () => failsafe.OnTemperatureSample(61));
        device.Registers[GpuTemp] = 80; // Fresh pre-write check lifts the average to 65.75 C.
        device.Writes.Clear();
        power.ApplyGpuPower(150, true, 25);

        Assert.True(failsafe.IsTripped);
        Assert.True(failsafe.LimitsApplied);
        AssertGpuWrites(device.Writes, 0xA5, 0, 0, repetitions: 2);

        // Loading disabled settings cannot bypass an existing process-wide latch.
        failsafe.Configure(new GpuFailsafeSettings { Enabled = false, ThresholdC = 80 });
        device.Writes.Clear();
        power.ApplyGpuPower(150, true, 25);
        Assert.True(failsafe.IsTripped);
        AssertGpuWrites(device.Writes, 0xA5, 0, 0, repetitions: 1);

        device.Writes.Clear();
        failsafe.Reset();
        Assert.Equal(0, device.Writes.Count);
        Assert.False(failsafe.IsTripped);

        failsafe.ApplyExplicit(() => power.ApplyGpuPower(150, true, 25));
        AssertGpuWrites(device.Writes, 0xA7, 55, 25, repetitions: 1);
        GpuPowerValues restored = power.ReadGpuPower();
        Assert.Equal(150, restored.TgpWatts);
        Assert.True(restored.DynamicBoostEnabled);
        Assert.Equal(25, restored.DbWatts);

        // A different model's offset/minimum must encode from its profile.
        var alternateDevice = new EcDevice();
        alternateDevice.Registers[TgpDbCtrl] = 0x40;
        GpuPowerControl alternate = CreatePower(alternateDevice, CreateGpuProfile(70, 80, 125));
        Assert.Equal(80, alternate.MinimumTgpWatts);
        Assert.True(alternate.CanApplyMinimumTgp);
        alternate.ApplyGpuPower(alternate.MinimumTgpWatts, false, 0);
        AssertGpuWrites(alternateDevice.Writes, 0x45, 10, 0, repetitions: 1);

        GpuPowerControl unsafeMinimum = CreatePower(new EcDevice(), CreateGpuProfile(70, 60, 125));
        Assert.Equal(70, unsafeMinimum.MinimumTgpWatts);
        Assert.False(unsafeMinimum.CanApplyMinimumTgp);
        unsafeMinimum.Failsafe = new GpuTemperatureFailsafe(() => 40, _ => { }, _ => { }, HeldState());
        Assert.Throws<InvalidOperationException>(() => unsafeMinimum.ApplyGpuPower(125, true, 25));

        var fallbackProfile = CreateGpuProfile(95, 95, 150);
        fallbackProfile.Limits.Clear();
        UniversalDefaults.ApplyToProfile(fallbackProfile);
        Assert.True(fallbackProfile.TgpLimitIsFallback);
        Assert.False(CreatePower(new EcDevice(), fallbackProfile).CanApplyMinimumTgp);

        GpuPowerControl missingGpu = CreatePower(new EcDevice(), CreateGpuProfile(95, 95, 150));
        missingGpu.ValidateFailsafeRegisters(gpuPresent: false);
        Assert.False(missingGpu.CanApplyMinimumTgp);
        var invalidDevice = new EcDevice();
        invalidDevice.Registers[Tgp] = 255;
        GpuPowerControl invalidRegisters = CreatePower(invalidDevice, CreateGpuProfile(95, 95, 150));
        invalidRegisters.ValidateFailsafeRegisters(gpuPresent: true);
        Assert.False(invalidRegisters.CanApplyMinimumTgp);
    }

    private static void TgpUnlockAndOverLimitReadback()
    {
        // A value left above the model range (earlier unlock or another tool)
        // is a real reading; the failsafe stays available to write over it.
        var device = new EcDevice();
        device.Registers[Tgp] = 0x82; // 225 W at offset 95
        GpuPowerControl power = CreatePower(device, CreateGpuProfile(95, 95, 150));
        power.ValidateFailsafeRegisters(gpuPresent: true);
        Assert.True(power.CanApplyMinimumTgp);

        // Locked: model range only, with a pointer to the unlock, and no driver request.
        var driver = new FakeGpuPowerOverride();
        power.PowerOverride = driver;
        Assert.True(power.CanUnlockTgp);
        Assert.Equal(150, power.MaxTgpWatts);
        Assert.Equal(225, power.UnlockedMaxTgpWatts);
        var locked = Assert.Throws<ArgumentOutOfRangeException>(() => power.ValidatePower(200, false, 0));
        Assert.Contains("Enable the TGP unlock", locked.Message);
        power.ApplyGpuPower(150, true, 25);
        Assert.Equal(0, driver.PowerRequests);

        // Unlocked: up to EC max + headroom, and no further. A request above stock schedules
        // the check that re-raises it after firmware resets the laptop budget.
        driver.Enabled = true;
        int checks = 0;
        power.ScheduleDriverCheck = () => checks++;
        device.Writes.Clear();
        power.ApplyGpuPower(225, true, 25);
        AssertGpuWrites(device.Writes, 0x07, 130, 25, repetitions: 1);
        Assert.Equal(225, power.ReadGpuPower().TgpWatts);
        Assert.Equal(250, driver.PowerWatts);
        Assert.Equal(1, checks);
        int raised = driver.PowerRequests;
        power.ReassertDriverRequest();
        Assert.Equal(raised, driver.PowerRequests);
        driver.BudgetReset = true;
        power.ReassertDriverRequest();
        Assert.True(driver.PowerRequests == raised + 1 && !driver.BudgetReset && driver.PowerWatts == 250);
        Assert.Throws<ArgumentOutOfRangeException>(() => power.ValidatePower(226, false, 0));

        // Unlocked floor: 60% of the model minimum. Below it the EC stays at its minimum and
        // only the driver request is lower; readback reports that request.
        Assert.Equal(57, power.MinTgpWatts);
        device.Writes.Clear();
        checks = 0;
        power.ApplyGpuPower(60, true, 10);
        AssertGpuWrites(device.Writes, 0x07, 0, 10, repetitions: 1);
        Assert.Equal(70, driver.PowerWatts);
        Assert.Equal(0, checks);
        Assert.Equal(60, power.ReadGpuPower().TgpWatts);
        Assert.Throws<ArgumentOutOfRangeException>(() => power.ValidatePower(56, false, 0));
        var lowSaved = new GpuProfileSettings { Tgp = 60 };
        power.CaptureDriverSettings(lowSaved);
        Assert.True(lowSaved.BiosOverrideEnabled == true);

        // Another model's offset/limit gets the same headroom rule.
        GpuPowerControl alternate = CreatePower(new EcDevice(), CreateGpuProfile(80, 80, 125));
        alternate.PowerOverride = new FakeGpuPowerOverride { Enabled = true };
        Assert.Equal(200, alternate.MaxTgpWatts);
        alternate.ValidatePower(200, false, 0);

        // Headroom never reaches the reserved 0xFF register value.
        Assert.Equal(95 + 0xFE, CreatePower(new EcDevice(), CreateGpuProfile(95, 95, 300)).UnlockedMaxTgpWatts);

        // Fallback (unknown) limits never unlock.
        var fallbackProfile = CreateGpuProfile(95, 95, 150);
        fallbackProfile.Limits.Clear();
        UniversalDefaults.ApplyToProfile(fallbackProfile);
        GpuPowerControl fallback = CreatePower(new EcDevice(), fallbackProfile);
        fallback.PowerOverride = new FakeGpuPowerOverride { Enabled = true };
        Assert.False(fallback.CanUnlockTgp);
        Assert.Equal(150, fallback.MaxTgpWatts);

        // Profile round-trip retains exact driver units; only values beyond stock ask for the unlock.
        string root = Path.Combine(Path.GetTempPath(), "mc-tgp-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        UserProfile loaded;
        try
        {
            var manager = new UserProfileManager(root);
            var saved = new UserProfile();
            saved.Gpu.Tgp = 225;
            power.CaptureDriverSettings(saved.Gpu);
            manager.SaveProfile(1, saved);
            loaded = new UserProfileManager(root).LoadProfile(1)!;
            Assert.True(loaded.Gpu.BiosOverrideEnabled == true);
            Assert.Equal(205500, loaded.Gpu.NvvddMilliamps!.Value);
            Assert.Equal(60000, loaded.Gpu.MsvddMilliamps!.Value);
            var stock = new GpuProfileSettings { Tgp = 150 };
            power.CaptureDriverSettings(stock);
            Assert.True(stock.BiosOverrideEnabled == false);
        }
        finally { Directory.Delete(root, true); }

        // Unlocked: the profile applies as saved without a restart.
        var plan = power.PlanProfile(loaded.Gpu, automatic: true);
        Assert.True(plan.Unlock && !plan.Restart && plan.Notice == "");
        Assert.Equal(225, plan.Settings.Tgp);
        Assert.Equal(205500, plan.Settings.NvvddMilliamps!.Value);

        // A profile that doesn't need the unlock (legacy: no currents) runs at stock currents, live.
        plan = power.PlanProfile(new GpuProfileSettings { Tgp = 150 }, automatic: true);
        Assert.True(!plan.Restart && plan.Notice == "");
        Assert.True(plan.Settings.NvvddMilliamps == 210000 && plan.Settings.MsvddMilliamps == 60000);
        power.ConfigureProfile(plan);
        Assert.True(driver.Nvvdd == 210000 && driver.Restarts == 0);
        driver.Nvvdd = 205500;

        // A higher current ceiling: manual loads restart, automatic loads clamp to the live ceiling.
        loaded.Gpu.NvvddMilliamps = 400000;
        Assert.True(power.PlanProfile(loaded.Gpu, automatic: false).Restart);
        plan = power.PlanProfile(loaded.Gpu, automatic: true);
        Assert.False(plan.Restart);
        Assert.Equal(300000, plan.Settings.NvvddMilliamps!.Value);
        Assert.Contains("restart", plan.Notice);
        power.ConfigureProfile(plan);
        Assert.Equal(300000, driver.Nvvdd);
        Assert.Equal(0, driver.Restarts);

        // Locked: startup clamps to the normal range and still applies; a manual load unlocks.
        driver.Enabled = false;
        plan = power.PlanProfile(loaded.Gpu, automatic: true);
        Assert.True(!plan.Unlock && !plan.Restart && plan.Notice.Length > 0);
        Assert.Equal(150, plan.Settings.Tgp);
        Assert.True(plan.Settings.NvvddMilliamps == null && plan.Settings.MsvddMilliamps == null);
        plan = power.PlanProfile(new GpuProfileSettings { Tgp = 60, BiosOverrideEnabled = true }, automatic: true);
        Assert.True(!plan.Unlock && plan.Settings.Tgp == 95 && plan.Notice.Length > 0);
        Assert.Throws<ArgumentOutOfRangeException>(() => power.PlanProfile(new GpuProfileSettings { Tgp = 50 }, automatic: true));
        plan = power.PlanProfile(loaded.Gpu, automatic: false);
        Assert.True(plan.Unlock && plan.Restart);
        power.ConfigureProfile(plan);
        Assert.True(driver.Enabled);
        Assert.Equal(1, driver.Restarts);

        // A held GPU action reduces the higher-priority NVIDIA request too, and blocks restarts.
        power.ApplyGpuPower(225, true, 25);
        power.Failsafe = new GpuTemperatureFailsafe(() => 40, _ => { }, _ => { }, HeldState());
        power.ApplyGpuPower(225, true, 25);
        Assert.Equal(power.MinimumTgpWatts, driver.PowerWatts);
        Assert.True(power.IsMinimumPowerRequested());
        driver.PowerWatts = 250;
        Assert.False(power.IsMinimumPowerRequested());
        Assert.Throws<InvalidOperationException>(() => power.SetUnlock(false));
        driver.FailWrite = true;
        Assert.Throws<IOException>(() => power.ApplyGpuPower(225, true, 25));

        // The hold never raises a lower unlocked request; resume reapplies a dropped one.
        var low = CreatePower(new EcDevice(), CreateGpuProfile(95, 95, 150));
        var lowDriver = new FakeGpuPowerOverride { Enabled = true };
        low.PowerOverride = lowDriver;
        low.ApplyGpuPower(60, false, 0);
        low.Failsafe = new GpuTemperatureFailsafe(() => 40, _ => { }, _ => { }, HeldState());
        low.ApplyGpuPower(low.MinimumTgpWatts, false, 0);
        Assert.Equal(60, lowDriver.PowerWatts);
        Assert.True(low.IsMinimumPowerRequested());
        int requests = lowDriver.PowerRequests;
        low.ReassertDriverRequest();
        Assert.Equal(requests, lowDriver.PowerRequests);
        lowDriver.PowerWatts = 175; // Driver request lost.
        low.ReassertDriverRequest();
        Assert.Equal(60, lowDriver.PowerWatts);
    }

    private static void NvidiaOpensOnlyOnDemand()
    {
        var device = new EcDevice();
        GpuPowerControl power = CreatePower(device, CreateGpuProfile(95, 95, 150));
        int opens = 0;
        var driver = new FakeGpuPowerOverride();
        power.OpenPowerOverride = () => { opens++; return driver; };
        Assert.True(power.CanUnlockTgp);
        Assert.False(power.TgpUnlocked);

        // Stock use (apply, save, automatic loads) never initializes NVIDIA.
        power.ApplyGpuPower(150, true, 25);
        var saved = new GpuProfileSettings { Tgp = 150 };
        power.CaptureDriverSettings(saved);
        Assert.True(saved.BiosOverrideEnabled == null && saved.NvvddMilliamps == null);
        var unlockedProfile = new GpuProfileSettings { Tgp = 225, BiosOverrideEnabled = true, NvvddMilliamps = 250000 };
        var plan = power.PlanProfile(unlockedProfile, automatic: true);
        Assert.Equal(150, plan.Settings.Tgp);
        Assert.Contains("Load it manually", plan.Notice);
        Assert.Equal(0, opens);

        // A manual load of an unlocked profile opens the driver and plans the restart.
        plan = power.PlanProfile(unlockedProfile, automatic: false);
        Assert.True(plan.Unlock && plan.Restart);
        Assert.Equal(1, opens);

        // An unsupported GPU/driver stops offering the unlock and says why.
        GpuPowerControl unsupported = CreatePower(new EcDevice(), CreateGpuProfile(95, 95, 150));
        unsupported.OpenPowerOverride = () => throw new NotSupportedException("driver too old");
        plan = unsupported.PlanProfile(unlockedProfile, automatic: false);
        Assert.True(!plan.Unlock && plan.Settings.Tgp == 150);
        Assert.Contains("driver too old", plan.Notice);
        Assert.False(unsupported.CanUnlockTgp);
    }

    private sealed class FakeGpuPowerOverride : IGpuPowerOverride
    {
        public bool Enabled { get; set; }
        public int StockPowerLimitWatts => 175;
        public int MinimumPowerLimitWatts => 5;
        public GpuPowerLimits ReadPowerLimits() => new(Enabled ? 250 : 175, PowerWatts, PowerWatts);
        public int PowerWatts { get; set; } = 175;
        public int PowerRequests, Restarts, Nvvdd = 205500, Msvdd = 60000;
        public bool BudgetReset; // Firmware reset the laptop budget below the request.
        public bool FailWrite { get; set; }
        private int LiveNvvddMaximum => Enabled ? 300000 : 210000;
        public GpuCurrentLimits ReadCurrentLimits() => new(new(1000, 210000, LiveNvvddMaximum, Nvvdd, 210000), new(1000, 60000, 60000, Msvdd, 60000));
        public bool RequiresRestart(bool enabled, int? nvvddMilliamps, int? msvddMilliamps) =>
            enabled != Enabled || nvvddMilliamps > LiveNvvddMaximum;
        public void Configure(bool enabled, int? nvvddMilliamps, int? msvddMilliamps, bool allowRestart)
        {
            if (RequiresRestart(enabled, nvvddMilliamps, msvddMilliamps))
            {
                if (!allowRestart) throw new InvalidOperationException("restart required");
                Restarts++;
                Enabled = enabled;
            }
            Nvvdd = nvvddMilliamps ?? Nvvdd;
            Msvdd = msvddMilliamps ?? Msvdd;
        }
        public void ApplyPowerLimit(int watts)
        {
            if (FailWrite) throw new IOException("Driver rejected power request");
            PowerRequests++;
            PowerWatts = watts;
            BudgetReset = false;
        }
        public bool IsPowerLimitAtMost(int watts) => PowerWatts <= watts;
        public bool IsPowerLimitApplied(int watts) => PowerWatts == watts && !BudgetReset;
    }

    private static GpuPowerControl CreatePower(EcDevice device, MachineProfile profile) =>
        new(device, new WriteGuard(device, profile), profile);

    private static MachineProfile CreateGpuProfile(int offset, int minimum, int maximum)
    {
        var profile = new MachineProfile
        {
            Registers = new Dictionary<string, string>
            {
                ["gpuTemp"] = "0x440",
                ["tgpDbCtrl"] = "0x743",
                ["tgp"] = "0x744",
                ["dbTotalPower"] = "0x745",
                ["dbMaxTgp"] = "0x746"
            },
            Limits = new Dictionary<string, LimitInfo>
            {
                ["TGP"] = new() { Min = minimum, Max = maximum, Encoding = "offset", Offset = offset }
            }
        };
        UniversalDefaults.ApplyToProfile(profile);
        return profile;
    }

    private static void AssertGpuWrites(List<(int Address, byte Value)> actual,
        byte control, byte encodedTgp, byte dbWatts, int repetitions)
    {
        var onePass = new (int Address, byte Value)[]
        {
            (TgpDbCtrl, control),
            (Tgp, encodedTgp),
            (DbTotal, dbWatts == 0 ? (byte)0 : (byte)255),
            (DbMax, dbWatts)
        };
        Assert.SequenceEqual(Enumerable.Repeat(onePass, repetitions).SelectMany(pass => pass), actual);
    }
}
