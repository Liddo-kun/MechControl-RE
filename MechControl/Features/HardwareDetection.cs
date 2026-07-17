using MechControl.Driver;
using MechControl.Profile;

namespace MechControl.Features;

/// <summary>
/// Reads hardware capability registers and ECSpec config at startup.
/// Support bytes are bitfield flags the EC uses to advertise available features.
/// ECSpec config (IOCTL 0x9C40A500) provides model-specific parameters.
/// </summary>
class HardwareDetection
{
    // ── Support bytes ────────────────────────────────────────────────

    /// <summary>Feature support flags (0x765). IDY_7Y reads 0xE1.</summary>
    public byte Support1 { get; }

    /// <summary>LED capability flags (0x766). IDY_7Y reads 0x93.</summary>
    public byte Support2 { get; }

    /// <summary>Support byte 5 (0x742). IDY_7Y reads 0x22.</summary>
    public byte Support5 { get; }

    /// <summary>Support byte 6 (0x78E).</summary>
    public byte Support6 { get; }

    /// <summary>AP BIOS byte (0x7A4). Bit 7 = liquid cooling hardware support.</summary>
    public byte ApBiosByte { get; }

    /// <summary>True if the machine supports an external liquid cooler (0x7A4 bit 7).</summary>
    public bool SupportsLiquidCooling { get; }

    // ── Identity registers ───────────────────────────────────────────

    /// <summary>Main module identifier (0x7D3).</summary>
    public byte ModuleId { get; }

    /// <summary>GPU module identifier (0x7D2).</summary>
    public byte ModuleIdGpu { get; }

    /// <summary>EC ROM ID (0x771).</summary>
    public byte RomId1 { get; }

    /// <summary>EC ROM ID 2 (0x772).</summary>
    public byte RomId2 { get; }

    /// <summary>EC firmware version (0x454).</summary>
    public byte EcVersion { get; }

    /// <summary>EC firmware sub-version (0x47D).</summary>
    public byte EcSubVersion { get; }

    /// <summary>System ID (0x456).</summary>
    public byte SystemId { get; }

    // ── PL4 double flag (0x727 bit 7) ────────────────────────────────

    /// <summary>
    /// Raw value of 0x727 at startup, before any init writes.
    /// Bit 7 = CPU_PL4_Double_Flag (PL4D). Bit 6 = CUME.
    /// </summary>
    public byte CpuDoubleFlagByte { get; }

    /// <summary>
    /// True if the EC's PL4D bit (0x727 bit 7) is set, meaning register
    /// 0x785 stores watts/2 (raw × 2 = watts). False means direct
    /// encoding (raw = watts). Firmware-level hardware fact — set by
    /// BIOS/DSDT at boot and stable for the lifetime of the machine.
    /// </summary>
    public bool Pl4Doubled { get; }

    // ── Per-mode PL defaults from EC ─────────────────────────────────

    /// <summary>Gaming mode defaults: PL1(0x730), PL2(0x731), PL4(0x732), D(0x733), TCC(0x7D8).</summary>
    public byte GamingPL1 { get; }
    public byte GamingPL2 { get; }
    public byte GamingPL4Raw { get; }
    public byte GamingD { get; }
    public byte GamingTcc { get; }

    /// <summary>Office mode defaults: PL1(0x734), PL2(0x735), PL4(0x736), D(0x737), TCC(0x7D9).</summary>
    public byte OfficePL1 { get; }
    public byte OfficePL2 { get; }
    public byte OfficePL4Raw { get; }
    public byte OfficeD { get; }
    public byte OfficeTcc { get; }

    /// <summary>Battery saver defaults: PL1(0x7A7), PL2(0x7A8), PL4(0x7A9), D(0x7AA).</summary>
    public byte BatterySaverPL1 { get; }
    public byte BatterySaverPL2 { get; }
    public byte BatterySaverPL4Raw { get; }
    public byte BatterySaverD { get; }

    /// <summary>Turbo TCC offset default (0x7DA).</summary>
    public byte TurboTcc { get; }

    // ── VRM current limits ───────────────────────────────────────────

    /// <summary>CPU VRM current limit (0x753).</summary>
    public byte VrmCurrentLimit { get; }

    /// <summary>CPU VRM maximum current limit (0x754).</summary>
    public byte VrmMaxCurrentLimit { get; }

    // ── ECSpec ───────────────────────────────────────────────────────

    /// <summary>ECSpec config entries (12 x 4 bytes), indexed 0x00-0x0B.</summary>
    public uint[] EcSpecConfig { get; }

    /// <summary>True if ECSpec config read succeeded.</summary>
    public bool EcSpecAvailable { get; }

    /// <summary>Extracted individual bytes from ECSpec SMAPCTABLE buffer (indices 0-8).</summary>
    public byte[] EcSpecBytes { get; } = new byte[9];

    /// <summary>True if ECSpec bytes were extracted and buffer format validated.</summary>
    public bool EcSpecDecoded { get; }

    public HardwareDetection(EcDevice device)
    {
        // ── Support bytes (plain EC reads, always available) ─────────
        Support1 = device.ReadByte(0x765);
        Support2 = device.ReadByte(0x766);
        Support5 = device.ReadByte(0x742);
        Support6 = device.ReadByte(0x78E);

        ApBiosByte = device.ReadByte(0x7A4);
        SupportsLiquidCooling = (ApBiosByte & 0x80) != 0;

        Console.WriteLine($"[HW] Support1 (0x765) = 0x{Support1:X2}");
        Console.WriteLine($"[HW] Support2 (0x766) = 0x{Support2:X2}");
        Console.WriteLine($"[HW] Support5 (0x742) = 0x{Support5:X2}");
        Console.WriteLine($"[HW] Support6 (0x78E) = 0x{Support6:X2}");
        Console.WriteLine($"[HW] ApBiosByte (0x7A4) = 0x{ApBiosByte:X2} (LiquidCooling={SupportsLiquidCooling})");

        // ── Identity registers ───────────────────────────────────────
        ModuleId = device.ReadByte(0x7D3);
        ModuleIdGpu = device.ReadByte(0x7D2);
        RomId1 = device.ReadByte(0x771);
        RomId2 = device.ReadByte(0x772);
        EcVersion = device.ReadByte(0x454);
        EcSubVersion = device.ReadByte(0x47D);
        SystemId = device.ReadByte(0x456);

        Console.WriteLine($"[HW] ModuleID=0x{ModuleId:X2} GPU=0x{ModuleIdGpu:X2} ROM={RomId1:X2}/{RomId2:X2} EC={EcVersion}.{EcSubVersion} Sys=0x{SystemId:X2}");

        // ── PL4 double flag (0x727 bit 7) ────────────────────────────
        // Read BEFORE EcActivationControl runs, since our init OR's 0x40
        // into bit 6 (CUME) — bit 7 (PL4D) is left alone either way, but
        // capturing the pristine value makes the log unambiguous.
        CpuDoubleFlagByte = device.ReadByte(0x727);
        Pl4Doubled = (CpuDoubleFlagByte & 0x80) != 0;
        Console.WriteLine($"[HW] 0x727 = 0x{CpuDoubleFlagByte:X2} (PL4D={(Pl4Doubled ? 1 : 0)}, CUME={((CpuDoubleFlagByte & 0x40) != 0 ? 1 : 0)})");

        // ── Per-mode PL defaults ─────────────────────────────────────
        GamingPL1 = device.ReadByte(0x730);
        GamingPL2 = device.ReadByte(0x731);
        GamingPL4Raw = device.ReadByte(0x732);
        GamingD = device.ReadByte(0x733);
        GamingTcc = device.ReadByte(0x7D8);

        OfficePL1 = device.ReadByte(0x734);
        OfficePL2 = device.ReadByte(0x735);
        OfficePL4Raw = device.ReadByte(0x736);
        OfficeD = device.ReadByte(0x737);
        OfficeTcc = device.ReadByte(0x7D9);

        BatterySaverPL1 = device.ReadByte(0x7A7);
        BatterySaverPL2 = device.ReadByte(0x7A8);
        BatterySaverPL4Raw = device.ReadByte(0x7A9);
        BatterySaverD = device.ReadByte(0x7AA);

        TurboTcc = device.ReadByte(0x7DA);

        // ── VRM current limits ───────────────────────────────────────
        VrmCurrentLimit = device.ReadByte(0x753);
        VrmMaxCurrentLimit = device.ReadByte(0x754);

        // ── ECSpec config via SMAPCTABLE IOCTL ───────────────────────
        EcSpecConfig = new uint[12];
        try
        {
            for (int i = 0; i < 12; i++)
            {
                EcSpecConfig[i] = device.ReadSmapcTable(i);
                Console.WriteLine($"[HW] ECSpec[0x{i:X2}] = 0x{EcSpecConfig[i]:X8}");
            }
            EcSpecAvailable = true;
        }
        catch (Exception ex)
        {
            DiagnosticLog.Warn($"[HW] ECSpec config read failed: {ex.Message}");
            EcSpecAvailable = false;
        }

        // Extract individual bytes from the sliding-window DWORD reads
        if (EcSpecAvailable)
        {
            for (int i = 0; i < 9; i++)
                EcSpecBytes[i] = (byte)(EcSpecConfig[i] & 0xFF);

            // Validate buffer format: index 9 should echo 0xBB (out-of-range marker)
            EcSpecDecoded = (EcSpecConfig[9] & 0xFF) == 0xBB;
            if (!EcSpecDecoded)
                DiagnosticLog.Warn($"[HW] ECSpec buffer format unexpected: index 9 LSB = 0x{(EcSpecConfig[9] & 0xFF):X2}, expected 0xBB");
        }
    }

    /// <summary>
    /// Extract per-model power limits from the decoded ECSpec SMAPCTABLE buffer.
    /// Returns null if ECSpec is unavailable or buffer format is unexpected.
    /// Only includes limits that pass sanity validation.
    ///
    /// Buffer layout:
    ///   [0] PL1 max (direct, watts)
    ///   [1] PL2 max (direct, watts)
    ///   [2] PL4 max raw — encoding depends on CPU_PL4_Double_Flag (0x727 bit 7):
    ///       PL4D=1 -> raw * 2 = watts; PL4D=0 -> raw = watts
    ///   [3] TccOffset (inverted: 105 - raw = max temp)
    ///   [4] ConfigurableTGP max raw (offset: raw + DefaultTGP = watts)
    ///   [5] DynamicBoost max (direct, watts)
    ///   [6] GPU TargetTemperature (direct, degrees C)
    ///   [7] DefaultTGP (direct, watts — TGP base/minimum)
    ///   [8] DynamicBoostCpuTdp (0xFF = unused)
    /// </summary>
    public Dictionary<string, LimitInfo>? ExtractLimits()
    {
        if (!EcSpecDecoded) return null;

        byte pl1Max = EcSpecBytes[0];
        byte pl2Max = EcSpecBytes[1];
        byte pl4MaxRaw = EcSpecBytes[2];
        byte tccOffset = EcSpecBytes[3];
        byte tgpMaxRaw = EcSpecBytes[4];
        byte dbMax = EcSpecBytes[5];
        byte defaultTgp = EcSpecBytes[7];

        var limits = new Dictionary<string, LimitInfo>();

        // PL1: direct, watts
        if (pl1Max != 0xFF && pl1Max >= 15 && pl1Max <= 253)
            limits["PL1"] = new LimitInfo { Min = 10, Max = pl1Max, Encoding = "direct" };

        // PL2: direct, watts
        if (pl2Max != 0xFF && pl2Max >= 15 && pl2Max <= 253)
            limits["PL2"] = new LimitInfo { Min = 10, Max = pl2Max, Encoding = "direct" };

        // PL4: encoding determined by CPU_PL4_Double_Flag (0x727 bit 7).
        // PL4D=1: raw * 2 = watts. PL4D=0: raw = watts.
        if (pl4MaxRaw != 0xFF && pl4MaxRaw >= 15 && pl4MaxRaw <= 250)
        {
            int pl4Max = Pl4Doubled ? pl4MaxRaw * 2 : pl4MaxRaw;
            limits["PL4"] = new LimitInfo
            {
                Min = 20,
                Max = pl4Max,
                Encoding = Pl4Doubled ? "double" : "direct"
            };
        }

        // CPU temp limit: encoding depends on CPU type.
        // Intel-style ECs store a TCC offset (small value, <= 40).
        // AMD-style ECs store the absolute max temperature (> 40).
        if (tccOffset <= 40)
        {
            int tempMax = 105 - tccOffset;
            limits["cpuTempLimit"] = new LimitInfo
            {
                Min = 80, Max = tempMax, Encoding = "inverted_offset", TjMax = 105
            };
        }
        else if (tccOffset >= 80 && tccOffset <= 120)
        {
            limits["cpuTempLimit"] = new LimitInfo
            {
                Min = 80, Max = tccOffset, Encoding = "direct"
            };
        }

        // TGP: offset encoding, requires valid DefaultTGP
        if (defaultTgp != 0xFF && defaultTgp >= 50 && defaultTgp <= 200)
        {
            int tgpMax = tgpMaxRaw + defaultTgp;
            if (tgpMaxRaw != 0xFF && tgpMax >= 50 && tgpMax <= 300)
            {
                limits["TGP"] = new LimitInfo
                {
                    Min = defaultTgp, Max = tgpMax, Encoding = "offset", Offset = defaultTgp
                };
            }
        }

        // Dynamic Boost: direct, watts
        if (dbMax != 0xFF && dbMax <= 50)
            limits["dynamicBoost"] = new LimitInfo { Min = 5, Max = dbMax, Encoding = "direct" };

        return limits.Count > 0 ? limits : null;
    }

    /// <summary>
    /// Extract per-mode power defaults from EC registers.
    /// Returns null if no valid mode data was found.
    /// CTGP/DynamicBoost are NOT populated here — the EC doesn't store per-mode
    /// GPU defaults in simple registers. Those only come from profile JSON.
    /// </summary>
    public Dictionary<string, ModeDefaultInfo>? ExtractModeDefaults(bool absoluteTemp = false)
    {
        var modes = new Dictionary<string, ModeDefaultInfo>();

        // Gaming mode (0x730-0x733, 0x7D8)
        var gaming = BuildModeDefault(GamingPL1, GamingPL2, GamingPL4Raw, GamingTcc, absoluteTemp, Pl4Doubled);
        if (gaming != null)
            modes["gaming"] = gaming;

        // Office mode (0x734-0x737, 0x7D9)
        var office = BuildModeDefault(OfficePL1, OfficePL2, OfficePL4Raw, OfficeTcc, absoluteTemp, Pl4Doubled);
        if (office != null)
            modes["office"] = office;

        // Battery saver mode (0x7A7-0x7AA, no TCC register)
        var battery = BuildModeDefault(BatterySaverPL1, BatterySaverPL2, BatterySaverPL4Raw, 0xFF, absoluteTemp, Pl4Doubled);
        if (battery != null)
            modes["batterySaver"] = battery;

        return modes.Count > 0 ? modes : null;
    }

    /// <summary>
    /// Build a ModeDefaultInfo from raw EC register values.
    /// Returns null if PL1 fails sanity check (mode not configured in firmware).
    /// </summary>
    private static ModeDefaultInfo? BuildModeDefault(byte pl1, byte pl2, byte pl4Raw, byte tccRaw, bool absoluteTemp, bool pl4Doubled)
    {
        // PL1 is the gatekeeper — if it's 0x00 or 0xFF, this mode isn't populated
        if (pl1 == 0 || pl1 == 0xFF || pl1 < 5 || pl1 > 200)
            return null;

        var mode = new ModeDefaultInfo
        {
            PL1 = pl1,
            PL2 = (pl2 != 0 && pl2 != 0xFF && pl2 >= 5 && pl2 <= 200) ? pl2 : pl1,
        };

        // PL4: encoding determined by CPU_PL4_Double_Flag (0x727 bit 7).
        if (pl4Raw != 0 && pl4Raw != 0xFF && pl4Raw >= 5 && pl4Raw <= 250)
            mode.PL4 = pl4Doubled ? pl4Raw * 2 : pl4Raw;

        // TCC: depends on CPU temp encoding
        if (tccRaw != 0xFF)
        {
            if (absoluteTemp && tccRaw >= 60 && tccRaw <= 120)
                mode.TccOffset = tccRaw; // absolute temperature
            else if (!absoluteTemp && tccRaw <= 40)
                mode.TccOffset = 105 - tccRaw; // inverted offset
        }

        return mode;
    }

    /// <summary>Short string for titlebar display.</summary>
    public string ToTitleString()
    {
        return $"EC:{Support1:X2}/{Support2:X2}/{Support5:X2} Mod:{ModuleId:X2}/{ModuleIdGpu:X2}";
    }
}
