namespace MechControl.Profile;

/// <summary>
/// Universal defaults for the Uniwill EC platform.
/// These values are shared across all known models — derived from
/// ACPIDriverDll.dll disassembly and verified on IDY_7Y.
/// Profile JSON values override these when populated.
/// </summary>
static class UniversalDefaults
{
    // ── Bitfields (register + mask) ──────────────────────────────
    // Extracted from DLL code, not per-model config. Universal by design.

    public static readonly Dictionary<string, BitfieldInfo> Bitfields = new()
    {
        ["fanCurveEnable"] = new BitfieldInfo { Register = "0x7C5", Bit = 7 },
        ["batteryMode"]    = new BitfieldInfo { Register = "0x7A6", Mask = "0x38" },
        ["dynamicBoostEn"] = new BitfieldInfo { Register = "0x743", Mask = "0x07" },
        ["logoOnOff"]      = new BitfieldInfo { Register = "0x7AB", Bit = 7 },
        // 0x751 is shared: the EC keeps temp-policy mode bits in bits 4 and 7
        // and clears boost itself when the machine is cool (X6FR5 EC firmware).
        ["fanBoost"]       = new BitfieldInfo { Register = "0x751", Bit = 6 },
    };

    // ── Bitfield validation: known-valid masked values ───────────
    // After reading a register and applying the mask, the result must
    // be in the valid set. If not, the feature is disabled.

    public static readonly Dictionary<string, HashSet<byte>> BitfieldValidValues = new()
    {
        // 0x743 & 0x07: 0x00 = pre-init, 0x02 = observed on hardware, 0x05 = DB off (bits 0,2), 0x07 = DB on (bits 0,1,2)
        ["dynamicBoostEn"] = [0x00, 0x02, 0x05, 0x07],

        // 0x7A6 & 0x38: three battery modes.
        // Intel: bit 3 set → {0x08, 0x18, 0x28}. AMD: bit 3 clear → {0x00, 0x10, 0x20}.
        ["batteryMode"] = [0x00, 0x08, 0x10, 0x18, 0x20, 0x28],

        // 0x7C5 & 0x80: bit 7 on or off
        ["fanCurveEnable"] = [0x00, 0x80],

        // 0x7AB & 0x80: bit 7 on or off
        ["logoOnOff"] = [0x00, 0x80],
    };

    // ── Conservative limits for unverified profiles ──────────────
    // Bounded conservatively within the range seen across all 44 models.

    public static readonly Dictionary<string, LimitInfo> ConservativeLimits = new()
    {
        ["PL1"]          = new LimitInfo { Min = 15,  Max = 150, Encoding = "direct" },
        ["PL2"]          = new LimitInfo { Min = 15,  Max = 150, Encoding = "direct" },
        ["PL4"]          = new LimitInfo { Min = 30,  Max = 300, Encoding = "double" },
        ["cpuTempLimit"] = new LimitInfo { Min = 80,  Max = 100, Encoding = "inverted_offset", TjMax = 105 },
        ["TGP"]          = new LimitInfo { Min = 95,  Max = 150, Encoding = "offset", Offset = 95 },
        ["dynamicBoost"] = new LimitInfo { Min = 5,   Max = 25,  Encoding = "direct" },
        ["fanDuty"]      = new LimitInfo { Min = 0,   Max = 200, Encoding = "raw_div_2_percent" },
        ["fanTemp"]      = new LimitInfo { Min = 0,   Max = 105, Encoding = "direct" },
        ["fanSwitchSpeed"] = new LimitInfo { Min = 100, Max = 12700, Encoding = "fan_switch_speed" },
    };

    // ── Opt-in TGP unlock ────────────────────────────────────────
    // UI unlock headroom, not a hardware rating. The controller intersects this
    // extension with the detected BIOS ceiling and the EC register encoding.
    public const int TgpUnlockHeadroomWatts = 75;
    public const int GpuCurrentUnlockPercent = 140;
    // Unlocked TGP floor as a percentage of the model's minimum (its base TGP), so it
    // scales with the GPU. Below the EC minimum only the NVIDIA driver request applies.
    public const int TgpUnlockMinimumPercent = 60;

    // ── Universal write allowlist ────────────────────────────────
    // Init registers + feature registers safe to write on all Uniwill models.

    // EC init handshake — required for EC to accept any writes
    public static readonly string[] InitAllowlist =
    [
        "0x708", "0x709",       // handshake
        "0x726", "0x727",       // EC power management activation (OEM Phase 5)
        "0x741",                // activation
        "0x748",                // feature enable
        "0x7C5",                // fan curve enable
        "0x7C7",                // fan curve param
        "0x7C6",                // fan curve commit
        "0x706",                // BIOS_CTRL init complete
        "0xF00-0xF5F",          // fan table (RamFan1 range)
    ];

    // Feature write registers — allowed after startup validation
    public static readonly string[] FeatureAllowlist =
    [
        "0x783", "0x784", "0x785", "0x786",  // PL1, PL2, PL4, CPU temp
        "0x743", "0x744", "0x745", "0x746",  // TGP/DB ctrl, TGP, DB total, DB max
        "0x751",                              // fan boost
        "0x7A6",                              // battery mode
        "0x767", "0x769", "0x76A", "0x76B",  // logo trigger, R, G, B
        "0x7AB", "0x78C",                     // logo on/off, logo commit
        "0x787",                              // fan switch speed
    ];

    // ── Universal skipVerify ─────────────────────────────────────
    // Registers where write-then-verify doesn't work (HID triggers,
    // handshake registers, mode flags).

    public static readonly string[] SkipVerify =
    [
        "0x767",  // logo trigger (write-only)
        "0x78C",  // logo commit (write-only)
        "0x726",  // EC power management activation
        "0x727",  // EC power management activation (CUME bit)
        "0x741",  // activation (may be cleared by EC)
        "0x7C6",  // fan curve commit
        "0x708",  // handshake
        "0x709",  // handshake
        "0x748",  // feature enable
        "0x706",  // BIOS_CTRL
        "0x7C7",  // fan curve param
    ];

    // ── Universal verifyRedirect ─────────────────────────────────
    // Registers where the write address differs from the read address.

    public static readonly Dictionary<string, VerifyRedirectInfo> VerifyRedirect = new()
    {
        ["0x786"] = new VerifyRedirectInfo
        {
            ReadAddr = "0x463",
            Transform = "inverted_offset",
            TjMax = 105
        },
    };

    // ── Feature register sanity ranges ───────────────────────────
    // After reading a register and decoding, the value must fall within
    // this range to be considered valid. Used at startup to confirm
    // register layout before enabling writes.

    public static readonly Dictionary<string, (int Min, int Max)> SanityRanges = new()
    {
        ["PL1"]          = (5, 300),     // watts
        ["PL2"]          = (5, 300),     // watts
        ["PL4"]          = (10, 600),    // watts (double encoding)
        ["cpuTempLimit"] = (60, 105),    // degrees C
        ["TGP"]          = (50, 250),    // watts
        ["dynamicBoost"] = (0, 50),      // watts
    };

    /// <summary>
    /// Merge universal defaults into a profile that has empty fields.
    /// Profile JSON values always take priority when populated.
    /// </summary>
    public static void ApplyToProfile(MachineProfile profile)
    {
        // Bitfields: fill any the profile doesn't declare
        foreach (var kvp in Bitfields)
            profile.Bitfields.TryAdd(kvp.Key, kvp.Value);

        // Limits: fill from conservative defaults if empty
        if (profile.Limits.Count == 0)
        {
            profile.TgpLimitIsFallback = true;
            profile.CpuLimitsAreFallback = true;
            foreach (var kvp in ConservativeLimits)
                profile.Limits[kvp.Key] = kvp.Value;
        }

        // WriteAllowlist: fill from universal defaults for unverified profiles
        if (!profile.Verified)
        {
            profile.WriteAllowlist.Clear();
            foreach (var entry in InitAllowlist)
                profile.WriteAllowlist.Add(entry);
            foreach (var entry in FeatureAllowlist)
                profile.WriteAllowlist.Add(entry);
        }

        // SkipVerify: fill from universal defaults for unverified profiles
        if (!profile.Verified)
        {
            profile.SkipVerify.Clear();
            profile.SkipVerify.AddRange(SkipVerify);
        }

        // VerifyRedirect: fill if empty
        if (profile.VerifyRedirect.Count == 0)
        {
            foreach (var kvp in VerifyRedirect)
                profile.VerifyRedirect[kvp.Key] = kvp.Value;
        }

        // BatteryModes: fill if empty
        if (profile.BatteryModes.Count == 0)
        {
            profile.BatteryModes.AddRange(new[]
            {
                new BatteryModeInfo { Name = "High Capacity", Value = "0x08" },
                new BatteryModeInfo { Name = "Balanced",      Value = "0x18" },
                new BatteryModeInfo { Name = "Stationary",    Value = "0x28" },
            });
        }
    }
}
