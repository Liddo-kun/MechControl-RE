using MechControl;
using MechControl.Driver;
using MechControl.Features;
using MechControl.Profile;

// Regressions for the X6FR5 EC firmware review (notes/ec_firmware_X6FR5_findings.md).
static partial class Program
{
    private static void CpuTempReadbackMasksFlag()
    {
        // 0x463 bit 7 is an EC "new value" flag, not part of the temperature.
        Assert.Equal(95, RegisterEncoding.DecodeCpuTempLimit(0x0A, direct: false));
        Assert.Equal(95, RegisterEncoding.DecodeCpuTempLimit(0x8A, direct: false));
        Assert.Equal(95, RegisterEncoding.DecodeCpuTempLimit(0x5F, direct: true));
        Assert.Equal(95, RegisterEncoding.DecodeCpuTempLimit(0xDF, direct: true));

        foreach (bool direct in new[] { false, true })
            foreach (bool flag in new[] { false, true })
            {
                var (device, profile) = CpuValidatorRig(direct);
                byte payload = direct ? (byte)95 : (byte)10;
                device.Registers[0x463] = flag ? (byte)(payload | 0x80) : payload;
                var validator = new StartupValidator(device, profile, cpuVendor: direct ? "amd" : "intel");
                validator.Validate();
                Assert.True(validator.IsFeatureEnabled("cpuPower"), $"direct={direct} flag={flag}: {string.Join("; ", validator.DisabledFeatures.Values)}");
                Assert.Equal(95, new CpuPowerControl(device, new WriteGuard(device, profile), profile).ReadCpuPower().TempLimitC);
            }
    }

    private static void CpuTempEncodingMustMatchVendor()
    {
        foreach (bool direct in new[] { false, true })
            foreach (string? vendor in new[] { "intel", "amd", null })
                foreach (bool confirmed in new[] { false, true })
                {
                    var (device, profile) = CpuValidatorRig(direct);
                    var validator = new StartupValidator(device, profile, cpuVendor: vendor, cpuTempEncodingConfirmed: confirmed);
                    validator.Validate();
                    bool mismatch = vendor != null && (vendor == "amd") != direct;
                    bool expectEnabled = confirmed || !mismatch;
                    Assert.Equal(expectEnabled, validator.IsFeatureEnabled("cpuPower"));
                    if (!expectEnabled)
                        Assert.Contains("does not match", validator.DisabledFeatures["cpuPower"]);
                }

        // Verified profiles skip register sanity checks but not this one.
        var (d, p) = CpuValidatorRig(direct: false);
        p.Verified = true;
        var verified = new StartupValidator(d, p, cpuVendor: "amd");
        verified.Validate();
        Assert.False(verified.IsFeatureEnabled("cpuPower"));
        Assert.True(verified.IsFeatureEnabled("gpuPower"));
    }

    private static (EcDevice, MachineProfile) CpuValidatorRig(bool direct)
    {
        var device = new EcDevice();
        var profile = new MachineProfile();
        foreach (var (name, addr) in new[] { ("PL1", "0x783"), ("PL2", "0x784"), ("PL4", "0x785"),
            ("cpuTempLimitWrite", "0x786"), ("cpuTempLimitRead", "0x463") })
            profile.Registers[name] = addr;
        profile.Limits["PL1"] = new() { Min = 10, Max = 160, Encoding = "direct" };
        profile.Limits["PL2"] = new() { Min = 10, Max = 160, Encoding = "direct" };
        profile.Limits["PL4"] = new() { Min = 20, Max = 300, Encoding = "double" };
        profile.Limits["cpuTempLimit"] = direct
            ? new() { Min = 80, Max = 98, Encoding = "direct" }
            : new() { Min = 80, Max = 95, Encoding = "inverted_offset", TjMax = 105 };
        UniversalDefaults.ApplyToProfile(profile);
        device.Registers[0x783] = 45;
        device.Registers[0x784] = 65;
        device.Registers[0x785] = 45;
        device.Registers[0x463] = direct ? (byte)95 : (byte)10;
        return (device, profile);
    }

    private static void FanBoostPreservesSharedBits()
    {
        var device = new EcDevice();
        var profile = new MachineProfile();
        profile.Registers["fanControlMode"] = "0x751";
        profile.Registers["fanCurveEnable"] = "0x7C5";
        profile.FanTable = new() { CpuTempUpBase = "0xF00", CpuTempDownBase = "0xF10", CpuDutyBase = "0xF20",
            GpuTempUpBase = "0xF30", GpuTempDownBase = "0xF40", GpuDutyBase = "0xF50" };
        UniversalDefaults.ApplyToProfile(profile);
        Assert.Equal((byte)0x40, profile.Bitfields["fanBoost"].GetMask());
        var guard = new WriteGuard(device, profile);
        var fan = new FanControl(device, guard, profile);

        // Bits 4 and 7 are EC temp-policy mode bits.
        device.Registers[0x751] = 0x90;
        fan.ValidateFailsafeRegisters();
        Assert.True(fan.CanApplyBoost);
        Assert.False(fan.ReadFanBoost());
        fan.SetFanBoost(true);
        Assert.Equal((byte)0xD0, device.Registers[0x751]);
        Assert.True(fan.ReadFanBoost());
        Assert.True(fan.IsBoostRequested());
        fan.SetFanBoost(false);
        Assert.Equal((byte)0x90, device.Registers[0x751]);

        device.Registers[0x751] = 0xFF;
        fan.ValidateFailsafeRegisters();
        Assert.False(fan.CanApplyBoost);

        // A profile's own bitfields keep the universal fan boost entry.
        var declared = new MachineProfile();
        declared.Bitfields["fanCurveEnable"] = new BitfieldInfo { Register = "0x7C5", Bit = 7 };
        UniversalDefaults.ApplyToProfile(declared);
        Assert.True(declared.Bitfields.ContainsKey("fanBoost"));
        Assert.Equal((byte)0x80, declared.Bitfields["fanCurveEnable"].GetMask());
    }

    private static void ProfileDetectionSeparatesCpuVendors()
    {
        string dir = FindProfilesDirectory();
        string Name(string? path) => path == null ? "(none)" : Path.GetFileName(path);

        Assert.Equal("intel", ProfileLoader.NormalizeCpuVendor("GenuineIntel"));
        Assert.Equal("amd", ProfileLoader.NormalizeCpuVendor("AuthenticAMD"));
        Assert.Null(ProfileLoader.NormalizeCpuVendor("HygonGenuine"));

        // Both X6 siblings report 0x740 = 0x1A (shifted 13).
        Assert.Equal("IDY_7Y.json", Name(ProfileLoader.FindProfileByProjectId(dir, 13, "intel")));
        Assert.Equal("X6FR5xxY.json", Name(ProfileLoader.FindProfileByProjectId(dir, 13, "amd")));
        Assert.Equal("IDY_7Y.json", Name(ProfileLoader.FindProfileByProjectId(dir, 13, null)));
        Assert.Equal("PH6TRX1.json", Name(ProfileLoader.FindProfileByProjectId(dir, 21, "amd")));

        foreach (string board in new[] { "X6FR559Y", "X6FR5xxY" })
        {
            Assert.Equal("X6FR5xxY.json", Name(ProfileLoader.FindProfileByBaseboardPattern(dir, board, "amd")));
            Assert.Null(ProfileLoader.FindProfileByBaseboardPattern(dir, board, "intel"));
            Assert.Null(ProfileLoader.FindProfileByBaseboardFilename(dir, board, "intel"));
        }
        Assert.Equal("IDY_7Y.json", Name(ProfileLoader.FindProfileByBaseboardPattern(dir, "YAOSHI Series-X6AR55xY", "intel")));
        Assert.Null(ProfileLoader.FindProfileByBaseboardPattern(dir, "YAOSHI Series-X6AR55xY", "amd"));
    }

    private static string FindProfilesDirectory()
    {
        for (string? d = AppContext.BaseDirectory; d != null; d = Path.GetDirectoryName(d))
        {
            string candidate = Path.Combine(d, "MechControl", "Profiles", "MECHREVO");
            if (Directory.Exists(candidate)) return candidate;
        }
        throw new DirectoryNotFoundException("MechControl/Profiles/MECHREVO not found above the test binary.");
    }
}
