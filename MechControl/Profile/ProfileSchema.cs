using System.Text.Json.Serialization;

namespace MechControl.Profile;

class MachineProfile
{
    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = "";

    [JsonPropertyName("projectIdValue")]
    public int ProjectIdValue { get; set; }

    [JsonPropertyName("driver")]
    public DriverInfo Driver { get; set; } = new();

    [JsonPropertyName("registers")]
    public Dictionary<string, string> Registers { get; set; } = new();

    [JsonPropertyName("fanTable")]
    public FanTableInfo FanTable { get; set; } = new();

    [JsonPropertyName("limits")]
    public Dictionary<string, LimitInfo> Limits { get; set; } = new();

    [JsonPropertyName("bitfields")]
    public Dictionary<string, BitfieldInfo> Bitfields { get; set; } = new();

    [JsonPropertyName("batteryModes")]
    public List<BatteryModeInfo> BatteryModes { get; set; } = new();

    /// <summary>
    /// Substrings of Win32_BaseBoard.Product that identify this model
    /// (case-insensitive). Real baseboards (e.g. "YAOSHI Series-X6AR55xY" for
    /// IDY_7Y) don't contain the profile name, so this explicit mapping —
    /// built up from field logs — is what makes WMI fallback detection work.
    /// </summary>
    [JsonPropertyName("baseboardPatterns")]
    public List<string> BaseboardPatterns { get; set; } = new();

    [JsonPropertyName("skipVerify")]
    public List<string> SkipVerify { get; set; } = new();

    [JsonPropertyName("verifyRedirect")]
    public Dictionary<string, VerifyRedirectInfo> VerifyRedirect { get; set; } = new();

    [JsonPropertyName("defaultFanCurve")]
    public DefaultFanCurveInfo? DefaultFanCurve { get; set; }

    [JsonPropertyName("defaultLogo")]
    public DefaultLogoInfo? DefaultLogo { get; set; }

    [JsonPropertyName("modeDefaults")]
    public Dictionary<string, ModeDefaultInfo>? ModeDefaults { get; set; }

    [JsonPropertyName("writeAllowlist")]
    public List<string> WriteAllowlist { get; set; } = new();

    [JsonPropertyName("verified")]
    public bool Verified { get; set; } = false;

    /// <summary>
    /// Parse a hex string like "0x783" to an integer.
    /// </summary>
    public static int ParseHex(string hex) =>
        Convert.ToInt32(hex.Replace("0x", ""), 16);

    /// <summary>TjMax from cpuTempLimit limit, or 105 if not configured.</summary>
    [JsonIgnore]
    public int TjMax => Limits.TryGetValue("cpuTempLimit", out var lim) ? lim.TjMax ?? 105 : 105;

    /// <summary>CPU temp limit encoding: "inverted_offset" (Intel TCC) or "direct" (AMD absolute).</summary>
    [JsonIgnore]
    public string CpuTempEncoding => Limits.TryGetValue("cpuTempLimit", out var lim) ? lim.Encoding : "inverted_offset";

    /// <summary>TGP offset (base TDP), or 95 if not configured.</summary>
    [JsonIgnore]
    public int TgpOffset => Limits.TryGetValue("TGP", out var lim) ? lim.Offset ?? 95 : 95;

    /// <summary>
    /// True if PL4 register 0x785 uses doubled encoding (raw × 2 = watts).
    /// Set at runtime by HardwareDetection from 0x727 bit 7 (CPU_PL4_Double_Flag)
    /// via <see cref="LimitInfo.Encoding"/> on the PL4 limit. Profile JSONs
    /// no longer carry this — it's a hardware fact, not a profile opinion.
    /// </summary>
    [JsonIgnore]
    public bool Pl4Doubled => Limits.TryGetValue("PL4", out var lim) && lim.Encoding == "double";

    /// <summary>
    /// Get the integer address for a named register.
    /// </summary>
    public int GetRegister(string name)
    {
        if (!Registers.TryGetValue(name, out string? hex))
            throw new KeyNotFoundException($"Register '{name}' not found in profile");
        return ParseHex(hex);
    }

    /// <summary>
    /// Expand the writeAllowlist (handles ranges like "0xF00-0xF5F").
    /// </summary>
    public HashSet<int> ExpandAllowlist()
    {
        var set = new HashSet<int>();
        // Always include init registers — required for EC activation on all models
        AddEntries(set, UniversalDefaults.InitAllowlist);
        AddEntries(set, WriteAllowlist);
        return set;
    }

    private static void AddEntries(HashSet<int> set, IEnumerable<string> entries)
    {
        foreach (string entry in entries)
        {
            if (entry.Contains('-'))
            {
                var parts = entry.Split('-');
                int start = ParseHex(parts[0]);
                int end = ParseHex(parts[1]);
                for (int addr = start; addr <= end; addr++)
                    set.Add(addr);
            }
            else
            {
                set.Add(ParseHex(entry));
            }
        }
    }

    /// <summary>
    /// Expand skipVerify list to integer addresses.
    /// </summary>
    public HashSet<int> ExpandSkipVerify()
    {
        var set = new HashSet<int>();
        foreach (string hex in UniversalDefaults.SkipVerify)
            set.Add(ParseHex(hex));
        foreach (string hex in SkipVerify)
            set.Add(ParseHex(hex));
        return set;
    }

    /// <summary>
    /// Expand verifyRedirect map to integer-keyed dictionary.
    /// </summary>
    public Dictionary<int, VerifyRedirectInfo> ExpandVerifyRedirect()
    {
        var dict = new Dictionary<int, VerifyRedirectInfo>();
        foreach (var kvp in VerifyRedirect)
            dict[ParseHex(kvp.Key)] = kvp.Value;
        return dict;
    }
}

class DriverInfo
{
    [JsonPropertyName("devicePath")]
    public string DevicePath { get; set; } = @"\\.\ACPIDriver";
}

class FanTableInfo
{
    [JsonPropertyName("format")]
    public string Format { get; set; } = "RamFan1";

    [JsonPropertyName("cpuTempUpBase")]
    public string CpuTempUpBase { get; set; } = "";

    [JsonPropertyName("cpuTempDownBase")]
    public string CpuTempDownBase { get; set; } = "";

    [JsonPropertyName("cpuDutyBase")]
    public string CpuDutyBase { get; set; } = "";

    [JsonPropertyName("gpuTempUpBase")]
    public string GpuTempUpBase { get; set; } = "";

    [JsonPropertyName("gpuTempDownBase")]
    public string GpuTempDownBase { get; set; } = "";

    [JsonPropertyName("gpuDutyBase")]
    public string GpuDutyBase { get; set; } = "";

    [JsonPropertyName("entriesPerArray")]
    public int EntriesPerArray { get; set; } = 16;

    [JsonPropertyName("activeEntries")]
    public int ActiveEntries { get; set; } = 10;

    [JsonPropertyName("paddingTemp")]
    public int PaddingTemp { get; set; } = 255;

    /// <summary>Runtime-detected: whether GPU fan table is present in EC.</summary>
    [System.Text.Json.Serialization.JsonIgnore]
    public bool HasGpuFan { get; set; } = true;

    // ── Parsed integer addresses (cached on first access) ────────────
    [System.Text.Json.Serialization.JsonIgnore] public int CpuTempUpAddr => _cpuTempUpAddr ??= MachineProfile.ParseHex(CpuTempUpBase);
    [System.Text.Json.Serialization.JsonIgnore] public int CpuTempDownAddr => _cpuTempDownAddr ??= MachineProfile.ParseHex(CpuTempDownBase);
    [System.Text.Json.Serialization.JsonIgnore] public int CpuDutyAddr => _cpuDutyAddr ??= MachineProfile.ParseHex(CpuDutyBase);
    [System.Text.Json.Serialization.JsonIgnore] public int GpuTempUpAddr => _gpuTempUpAddr ??= MachineProfile.ParseHex(GpuTempUpBase);
    [System.Text.Json.Serialization.JsonIgnore] public int GpuTempDownAddr => _gpuTempDownAddr ??= MachineProfile.ParseHex(GpuTempDownBase);
    [System.Text.Json.Serialization.JsonIgnore] public int GpuDutyAddr => _gpuDutyAddr ??= MachineProfile.ParseHex(GpuDutyBase);

    private int? _cpuTempUpAddr, _cpuTempDownAddr, _cpuDutyAddr;
    private int? _gpuTempUpAddr, _gpuTempDownAddr, _gpuDutyAddr;
}

class LimitInfo
{
    [JsonPropertyName("min")]
    public int Min { get; set; }

    [JsonPropertyName("max")]
    public int Max { get; set; }

    [JsonPropertyName("encoding")]
    public string Encoding { get; set; } = "direct";

    [JsonPropertyName("tjMax")]
    public int? TjMax { get; set; }

    [JsonPropertyName("offset")]
    public int? Offset { get; set; }
}

class BitfieldInfo
{
    [JsonPropertyName("register")]
    public string Register { get; set; } = "";

    [JsonPropertyName("bit")]
    public int? Bit { get; set; }

    [JsonPropertyName("mask")]
    public string? Mask { get; set; }

    public byte GetMask()
    {
        if (Mask != null)
            return (byte)Convert.ToInt32(Mask.Replace("0x", ""), 16);
        if (Bit.HasValue)
            return (byte)(1 << Bit.Value);
        throw new InvalidOperationException("BitfieldInfo has neither bit nor mask");
    }

    public int GetRegisterAddr() => MachineProfile.ParseHex(Register);
}

class BatteryModeInfo
{
    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("value")]
    public string Value { get; set; } = "";

    public byte GetValue() => (byte)Convert.ToInt32(Value.Replace("0x", ""), 16);
}

class DefaultFanCurveInfo
{
    [JsonPropertyName("cpu")]
    public FanCurveArrays? Cpu { get; set; }

    [JsonPropertyName("gpu")]
    public FanCurveArrays? Gpu { get; set; }
}

class FanCurveArrays
{
    [JsonPropertyName("tempUp")]
    public List<int> TempUp { get; set; } = new();

    [JsonPropertyName("tempDown")]
    public List<int> TempDown { get; set; } = new();

    [JsonPropertyName("duty")]
    public List<int> Duty { get; set; } = new();
}

class ModeDefaultInfo
{
    [JsonPropertyName("PL1")]
    public int PL1 { get; set; }

    [JsonPropertyName("PL2")]
    public int PL2 { get; set; }

    [JsonPropertyName("PL4")]
    public int? PL4 { get; set; }

    [JsonPropertyName("CTGP")]
    public int? CTGP { get; set; }

    [JsonPropertyName("dynamicBoost")]
    public int? DynamicBoost { get; set; }

    [JsonPropertyName("tccOffset")]
    public int? TccOffset { get; set; }

    [JsonPropertyName("fanCurve")]
    public DefaultFanCurveInfo? FanCurve { get; set; }
}

class DefaultLogoInfo
{
    [JsonPropertyName("r")]
    public byte R { get; set; }

    [JsonPropertyName("g")]
    public byte G { get; set; }

    [JsonPropertyName("b")]
    public byte B { get; set; }

    [JsonPropertyName("onOff")]
    public byte OnOff { get; set; }
}

class VerifyRedirectInfo
{
    [JsonPropertyName("readAddr")]
    public string ReadAddr { get; set; } = "";

    [JsonPropertyName("transform")]
    public string Transform { get; set; } = "";

    [JsonPropertyName("tjMax")]
    public int? TjMax { get; set; }

    public int GetReadAddr() => MachineProfile.ParseHex(ReadAddr);
}
