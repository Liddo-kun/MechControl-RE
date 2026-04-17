using MechControl.Driver;
using MechControl.Profile;

namespace MechControl.Features;

/// <summary>
/// Validates EC register layout at startup by reading registers and checking
/// that decoded values match expected patterns. Determines which features
/// are safe to enable on this machine.
/// </summary>
class StartupValidator
{
    private readonly EcDevice _device;
    private readonly MachineProfile _profile;

    /// <summary>Features that passed validation and are safe to use.</summary>
    public HashSet<string> EnabledFeatures { get; } = new();

    /// <summary>Features that failed validation with reason.</summary>
    public Dictionary<string, string> DisabledFeatures { get; } = new();

    /// <summary>Warnings to show the user.</summary>
    public List<string> Warnings { get; } = new();

    private readonly byte _moduleIdGpu;
    private readonly bool _hasGpuFan;
    private readonly HashSet<string> _enrichedLimits;

    public StartupValidator(EcDevice device, MachineProfile profile,
        byte moduleIdGpu = 0xFF, bool hasGpuFan = true,
        HashSet<string>? enrichedLimits = null)
    {
        _device = device;
        _profile = profile;
        _moduleIdGpu = moduleIdGpu;
        _hasGpuFan = hasGpuFan;
        _enrichedLimits = enrichedLimits ?? new();
    }

    /// <summary>
    /// Check if a feature has been confirmed by ECSpec enrichment.
    /// If ECSpec provided valid limits for any register in a feature group,
    /// the register layout is confirmed and sanity check failures should not
    /// disable the feature.
    /// </summary>
    private bool IsFeatureEnriched(string feature) => feature switch
    {
        "cpuPower" => _enrichedLimits.Overlaps(new[] { "PL1", "PL2", "PL4", "cpuTempLimit" }),
        "gpuPower" => _enrichedLimits.Overlaps(new[] { "TGP", "dynamicBoost" }),
        _ => false
    };

    /// <summary>
    /// Run all validation checks. Call after UniversalDefaults.ApplyToProfile()
    /// and before creating feature controllers.
    /// </summary>
    public void Validate()
    {
        DiagnosticLog.Section("Startup Validation");

        if (_profile.Verified)
        {
            // Verified profiles: enable everything, still log values
            DiagnosticLog.Write("Profile is verified — all features enabled.");
            EnabledFeatures.Add("cpuPower");
            EnabledFeatures.Add("gpuPower");
            EnabledFeatures.Add("fanControl");
            EnabledFeatures.Add("battery");
            EnabledFeatures.Add("lighting");
            ValidateBitfields(); // log-only for verified
            ValidateFeatureRegisters(); // log-only for verified
            return;
        }

        DiagnosticLog.Write("Profile is unverified — validating register layout...");

        ValidateBitfields();
        ValidateFeatureRegisters();

        // Summarize
        DiagnosticLog.Section("Validation Summary");
        foreach (var f in EnabledFeatures)
            DiagnosticLog.Write($"  Enabled:  {f}");
        foreach (var kvp in DisabledFeatures)
        {
            DiagnosticLog.Write($"  Disabled: {kvp.Key} — {kvp.Value}");
            Warnings.Add($"{kvp.Key}: {kvp.Value}");
        }
    }

    private void ValidateBitfields()
    {
        DiagnosticLog.Write("Bitfield validation:");

        foreach (var kvp in _profile.Bitfields)
        {
            string name = kvp.Key;
            var bf = kvp.Value;

            try
            {
                int addr = bf.GetRegisterAddr();
                byte raw = _device.ReadByte(addr);
                byte mask = bf.GetMask();
                byte masked = (byte)(raw & mask);

                bool valid = true;
                if (UniversalDefaults.BitfieldValidValues.TryGetValue(name, out var validSet))
                    valid = validSet.Contains(masked);

                DiagnosticLog.LogBitfieldValidation(name, raw, masked, valid);

                if (!valid && !_profile.Verified)
                {
                    string feature = MapBitfieldToFeature(name);
                    if (!string.IsNullOrEmpty(feature))
                    {
                        if (IsFeatureEnriched(feature))
                            DiagnosticLog.Write($"  [SKIP] Bitfield '{name}' 0x{masked:X2} not in expected set, but {feature} confirmed by ECSpec");
                        else
                            DisabledFeatures.TryAdd(feature,
                                $"Bitfield '{name}' at 0x{addr:X3}: masked value 0x{masked:X2} not in expected set");
                    }
                }
            }
            catch (Exception ex)
            {
                DiagnosticLog.Write($"  [ERROR] {name}: {ex.Message}");
                if (!_profile.Verified)
                {
                    string feature = MapBitfieldToFeature(name);
                    if (!string.IsNullOrEmpty(feature))
                        DisabledFeatures.TryAdd(feature, $"Bitfield '{name}' read failed: {ex.Message}");
                }
            }
        }
    }

    private void ValidateFeatureRegisters()
    {
        DiagnosticLog.Write("Feature register validation:");

        // CPU power: PL1, PL2, PL4, temp limit
        bool cpuOk = true;
        cpuOk &= ValidateRegister("PL1", v => RegisterEncoding.DecodePL1(v));
        cpuOk &= ValidateRegister("PL2", v => RegisterEncoding.DecodePL1(v));
        cpuOk &= ValidateRegister("PL4", v => RegisterEncoding.DecodePL4(v, _profile.Pl4Doubled));
        cpuOk &= ValidateRegister("cpuTempLimit", v =>
        {
            if (_profile.CpuTempEncoding == "direct")
                return v; // absolute temperature
            return RegisterEncoding.DecodeCpuTempLimit(v, _profile.TjMax);
        }, readRegister: "cpuTempLimitRead");

        if (cpuOk && !DisabledFeatures.ContainsKey("cpuPower"))
            EnabledFeatures.Add("cpuPower");

        // GPU power: TGP, Dynamic Boost
        bool gpuOk = true;
        gpuOk &= ValidateRegister("TGP", v =>
        {
            int offset = _profile.TgpOffset;
            return RegisterEncoding.DecodeTGP(v, offset);
        }, readRegister: "tgp");
        gpuOk &= ValidateRegister("dynamicBoost", v => RegisterEncoding.DecodeDB(v), readRegister: "dbMaxTgp");

        // Check GPU module presence (defense in depth for CPU-only machines)
        if (_moduleIdGpu == 0 && !_profile.Verified)
        {
            gpuOk = false;
            DisabledFeatures.TryAdd("gpuPower", "GPU module not detected (ModuleIdGpu=0)");
            DiagnosticLog.Write("  [INFO] ModuleIdGpu=0 — GPU module not detected");
        }
        if (!_hasGpuFan)
            DiagnosticLog.Write("  [INFO] No GPU fan table detected");

        if (gpuOk && !DisabledFeatures.ContainsKey("gpuPower"))
            EnabledFeatures.Add("gpuPower");

        // Fan control: always enable if bitfield is valid (reads are safe,
        // fan table is validated separately during EC init)
        if (!DisabledFeatures.ContainsKey("fanControl"))
            EnabledFeatures.Add("fanControl");

        // Battery: enable if bitfield is valid
        if (!DisabledFeatures.ContainsKey("battery"))
            EnabledFeatures.Add("battery");

        // Lighting: check logo registers
        bool lightingOk = true;
        lightingOk &= ValidateLogoRegisters();
        if (lightingOk && !DisabledFeatures.ContainsKey("lighting"))
            EnabledFeatures.Add("lighting");
    }

    private bool ValidateRegister(string limitName, Func<byte, int> decode, string? readRegister = null)
    {
        string regName = readRegister ?? limitName;
        if (!_profile.Registers.TryGetValue(regName, out string? hex))
        {
            DiagnosticLog.Write($"  [SKIP] {limitName}: register '{regName}' not in profile");
            return true; // missing register is not a failure
        }

        try
        {
            int addr = MachineProfile.ParseHex(hex);
            byte raw = _device.ReadByte(addr);
            int decoded = decode(raw);

            bool valid = true;
            string failReason = "";
            if (!_profile.Verified && UniversalDefaults.SanityRanges.TryGetValue(limitName, out var range))
            {
                valid = decoded >= range.Min && decoded <= range.Max;
                if (!valid)
                    failReason = $"outside sanity range {range.Min}-{range.Max}";
            }

            DiagnosticLog.LogFeatureValidation(limitName, raw, decoded, valid, failReason);

            if (!valid && !_profile.Verified)
            {
                string feature = MapLimitToFeature(limitName);
                if (!string.IsNullOrEmpty(feature))
                {
                    if (IsFeatureEnriched(feature))
                    {
                        DiagnosticLog.Write($"  [SKIP] {limitName} decoded to {decoded} ({failReason}), but {feature} confirmed by ECSpec");
                        // ECSpec confirms the register layout is correct; the
                        // out-of-range value is just a sentinel (e.g. PL1=0
                        // means "EC controls power"). Don't let it cascade
                        // into cpuOk and silently disable the feature.
                        valid = true;
                    }
                    else
                    {
                        DisabledFeatures.TryAdd(feature,
                            $"Register '{limitName}' decoded to {decoded}, outside expected range");
                    }
                }
            }

            return valid;
        }
        catch (Exception ex)
        {
            DiagnosticLog.Write($"  [ERROR] {limitName}: {ex.Message}");
            return true; // read failure is not a definitive layout mismatch
        }
    }

    private bool ValidateLogoRegisters()
    {
        // Check if logo RGB registers contain values in the valid EC range (0-60)
        foreach (string regName in new[] { "logoR", "logoG", "logoB" })
        {
            if (!_profile.Registers.TryGetValue(regName, out string? hex))
                continue;
            try
            {
                int addr = MachineProfile.ParseHex(hex);
                byte raw = _device.ReadByte(addr);
                if (raw > 60)
                {
                    DiagnosticLog.LogFeatureValidation(regName, raw, raw, false, "EC logo RGB max is 60");
                    if (!_profile.Verified)
                    {
                        DisabledFeatures.TryAdd("lighting",
                            $"Logo register '{regName}' value {raw} exceeds expected max 60");
                        return false;
                    }
                }
                else
                {
                    DiagnosticLog.LogFeatureValidation(regName, raw, raw, true);
                }
            }
            catch (Exception ex)
            {
                DiagnosticLog.Write($"  [ERROR] {regName}: {ex.Message}");
            }
        }
        return true;
    }

    private static string MapBitfieldToFeature(string bitfieldName) => bitfieldName switch
    {
        "fanCurveEnable" => "fanControl",
        "batteryMode"    => "battery",
        "dynamicBoostEn" => "gpuPower",
        "logoOnOff"      => "lighting",
        _ => ""
    };

    private static string MapLimitToFeature(string limitName) => limitName switch
    {
        "PL1" or "PL2" or "PL4" or "cpuTempLimit" => "cpuPower",
        "TGP" or "dynamicBoost" => "gpuPower",
        _ => ""
    };

    public bool IsFeatureEnabled(string feature) => EnabledFeatures.Contains(feature);
}
