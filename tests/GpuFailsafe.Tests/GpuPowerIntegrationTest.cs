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
