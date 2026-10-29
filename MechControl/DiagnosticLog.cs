using MechControl.Driver;
using MechControl.Features;
using MechControl.Profile;

namespace MechControl;

/// <summary>
/// Writes a comprehensive diagnostic log for each session.
/// Captures everything needed to build a verified profile from a remote machine.
/// </summary>
static class DiagnosticLog
{
    private static string? _logPath;
    private static StreamWriter? _writer;

    // Serializes writer access — the log is written from the UI thread plus
    // background threads (activation watchdog timer, resume recovery).
    private static readonly object _sync = new();

    public static string LogPath => _logPath ?? "(not initialized)";

    public static void Start()
    {
        lock (_sync)
        {
            _logPath = Path.Combine(AppContext.BaseDirectory, "mechcontrol.log");
            TrimOldSessions(_logPath);
            _writer = new StreamWriter(_logPath, append: true);
            _writer.AutoFlush = true;
            _writer.WriteLine();
            _writer.WriteLine(new string('=', 72));
            _writer.WriteLine($"Session started: {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
            _writer.WriteLine(new string('=', 72));
        }
    }

    // Retention: keep the very first session (origin state of the machine) plus
    // the KeepRecent most-recent sessions. Drop anything between.
    // Opportunistic — any IO failure silently skips rotation for this launch.
    private const int KeepRecent = 5;

    private static void TrimOldSessions(string path)
    {
        if (!File.Exists(path)) return;
        string[] lines;
        try { lines = File.ReadAllLines(path); }
        catch { return; }

        // A session block starts with a 72-char '=' separator immediately
        // followed by a "Session started:" line. Anchoring on both lines
        // avoids false matches on body text that happens to contain the phrase.
        string sep = new string('=', 72);
        var blockStarts = new List<int>();
        for (int i = 0; i < lines.Length - 1; i++)
        {
            if (lines[i] == sep && lines[i + 1].StartsWith("Session started:", StringComparison.Ordinal))
                blockStarts.Add(i);
        }
        if (blockStarts.Count <= 1 + KeepRecent) return;

        int firstStart = blockStarts[0];
        int firstEnd = blockStarts[1]; // exclusive
        int recentStart = blockStarts[blockStarts.Count - KeepRecent];

        var result = new List<string>(capacity: lines.Length);
        for (int i = firstStart; i < firstEnd; i++) result.Add(lines[i]);
        for (int i = recentStart; i < lines.Length; i++) result.Add(lines[i]);

        try { File.WriteAllLines(path, result); }
        catch { /* opportunistic */ }
    }

    public static void Write(string message)
    {
        lock (_sync)
        {
            _writer?.WriteLine(message);
            Console.WriteLine(message);
        }
    }

    public static void Section(string header)
    {
        lock (_sync)
        {
            _writer?.WriteLine();
            _writer?.WriteLine($"--- {header} ---");
        }
    }

    /// <summary>
    /// Log profile detection results.
    /// </summary>
    public static void LogProfileDetection(MachineProfile profile, string detectionMethod)
    {
        Section("Profile Detection");
        Write($"Profile:    {profile.ProjectId}");
        Write($"ProjectID:  {profile.ProjectIdValue}");
        Write($"Verified:   {profile.Verified}");
        Write($"Method:     {detectionMethod}");
    }

    /// <summary>
    /// Log WMI baseboard and CPU info.
    /// </summary>
    public static void LogBaseboard(string? baseboard)
    {
        Write($"Baseboard:  {baseboard ?? "(not available)"}");
        Write($"CPU:        {Profile.ProfileLoader.LastCpuName ?? "(not available)"}");
        Write($"CPU vendor: {Profile.ProfileLoader.LastCpuVendor ?? "(unknown)"}");
    }

    /// <summary>
    /// Log hardware detection results (support bytes + ECSpec).
    /// </summary>
    public static void LogHardwareDetection(HardwareDetection? hw)
    {
        Section("Hardware Detection");
        if (hw == null)
        {
            Write("Hardware detection failed — no data available.");
            return;
        }

        Write($"Support1 (0x765): 0x{hw.Support1:X2}");
        Write($"Support2 (0x766): 0x{hw.Support2:X2}");
        Write($"Support5 (0x742): 0x{hw.Support5:X2}");

        if (hw.EcSpecAvailable)
        {
            for (int i = 0; i < hw.EcSpecConfig.Length; i++)
                Write($"ECSpec[0x{i:X2}]: 0x{hw.EcSpecConfig[i]:X8}");
        }
        else
        {
            Write("ECSpec config: not available");
        }
    }

    /// <summary>
    /// Log decoded ECSpec SMAPCTABLE buffer values.
    /// </summary>
    public static void LogEcSpecDecoded(HardwareDetection hw)
    {
        Section("ECSpec Decoded Limits");
        if (!hw.EcSpecDecoded)
        {
            Write(hw.EcSpecAvailable
                ? "ECSpec buffer format unexpected (index 9 did not echo 0xBB)"
                : "ECSpec not available");
            return;
        }

        byte[] b = hw.EcSpecBytes;
        string Fmt(byte v, string unit) => v == 0xFF ? "N/A (0xFF)" : $"{v}{unit}";

        Write($"  PL1 max:      {Fmt(b[0], "W")} (raw=0x{b[0]:X2})");
        Write($"  PL2 max:      {Fmt(b[1], "W")} (raw=0x{b[1]:X2})");
        {
            int pl4Watts = hw.Pl4Doubled ? b[2] * 2 : b[2];
            string pl4Tag = hw.Pl4Doubled ? "x2" : "direct";
            Write($"  PL4 max:      {(b[2] == 0xFF ? "N/A (0xFF)" : $"{pl4Watts}W")} (raw=0x{b[2]:X2}, {pl4Tag})");
        }

        if (b[3] <= 40)
            Write($"  CPU temp max: {105 - b[3]}C (TccOffset={b[3]}, TjMax=105, encoding=inverted_offset)");
        else if (b[3] >= 80 && b[3] <= 120)
            Write($"  CPU temp max: {b[3]}C (absolute, encoding=direct)");
        else
            Write($"  CPU temp max: unknown (raw={b[3]}, 0x{b[3]:X2})");

        byte defaultTgp = b[7];
        Write($"  Default TGP:  {Fmt(defaultTgp, "W")} (raw=0x{defaultTgp:X2})");

        if (defaultTgp != 0xFF && defaultTgp >= 50 && defaultTgp <= 200 && b[4] != 0xFF)
            Write($"  TGP range:    {defaultTgp}-{b[4] + defaultTgp}W (configurable={b[4]})");
        else
            Write($"  TGP config:   {Fmt(b[4], "")} (raw=0x{b[4]:X2})");

        Write($"  DB max:       {Fmt(b[5], "W")} (raw=0x{b[5]:X2})");
        Write($"  GPU target:   {Fmt(b[6], "C")} (raw=0x{b[6]:X2})");
        Write($"  DB CPU TDP:   {(b[8] == 0xFF ? "N/A" : $"{b[8]}W")} (raw=0x{b[8]:X2})");

        var limits = hw.ExtractLimits();
        if (limits != null)
        {
            Write($"  Extractable limits: {string.Join(", ", limits.Keys)}");
        }
        else
        {
            Write("  No limits passed sanity validation");
        }
    }

    /// <summary>
    /// Log EC identity register.
    /// </summary>
    public static void LogEcIdentity(EcDevice device)
    {
        Section("EC Identity");
        try
        {
            byte raw = device.ReadByte(0x740);
            Write($"EC 0x740 (ProjectID): {raw} (0x{raw:X2}), shifted: {raw >> 1}");
        }
        catch (Exception ex)
        {
            Write($"EC 0x740 read failed: {ex.Message}");
        }
    }

    /// <summary>
    /// Log extended identity and capability registers from HardwareDetection.
    /// </summary>
    public static void LogExtendedIdentity(HardwareDetection hw)
    {
        Section("Extended Identity");
        Write($"  ModuleID:      0x{hw.ModuleId:X2}");
        Write($"  ModuleID_GPU:  0x{hw.ModuleIdGpu:X2}");
        Write($"  ROMID:         0x{hw.RomId1:X2} / 0x{hw.RomId2:X2}");
        Write($"  EC Version:    {hw.EcVersion}.{hw.EcSubVersion}");
        Write($"  SystemID:      0x{hw.SystemId:X2}");
        Write($"  Support6:      0x{hw.Support6:X2}");
        Write($"  ApBiosByte:    0x{hw.ApBiosByte:X2} (LiquidCooling={hw.SupportsLiquidCooling})");
        Write($"  VRM Current:   {hw.VrmCurrentLimit}A (max {hw.VrmMaxCurrentLimit}A)");
        Write($"  0x727:         0x{hw.CpuDoubleFlagByte:X2} (PL4D={(hw.Pl4Doubled ? 1 : 0)} -> PL4 encoding={(hw.Pl4Doubled ? "double" : "direct")})");
    }

    /// <summary>
    /// Log per-mode PL defaults read from EC and the resulting modeDefaults.
    /// </summary>
    public static void LogModeDefaults(HardwareDetection hw, MachineProfile profile)
    {
        Section("Per-Mode PL Defaults (EC Registers)");
        string Fmt(byte v) => v == 0xFF ? "N/A" : v == 0 ? "0" : $"{v}";
        bool pl4Doubled = hw.Pl4Doubled;
        string FmtPL4(byte v) => v == 0xFF ? "N/A"
            : v == 0 ? "0"
            : pl4Doubled ? $"{v * 2}W (raw={v})"
                         : $"{v}W (raw={v})";
        bool absTemp = profile.CpuTempEncoding == "direct";
        string FmtTcc(byte v) => v == 0xFF ? "N/A"
            : absTemp ? $"{v}C (absolute)"
            : v > 40 ? $"raw={v} (unknown)"
            : $"{105 - v}C (offset={v})";

        Write($"  Gaming:  PL1={Fmt(hw.GamingPL1)}W  PL2={Fmt(hw.GamingPL2)}W  PL4={FmtPL4(hw.GamingPL4Raw)}  D=0x{hw.GamingD:X2}  TCC={FmtTcc(hw.GamingTcc)}");
        Write($"  Office:  PL1={Fmt(hw.OfficePL1)}W  PL2={Fmt(hw.OfficePL2)}W  PL4={FmtPL4(hw.OfficePL4Raw)}  D=0x{hw.OfficeD:X2}  TCC={FmtTcc(hw.OfficeTcc)}");
        Write($"  BatSave: PL1={Fmt(hw.BatterySaverPL1)}W  PL2={Fmt(hw.BatterySaverPL2)}W  PL4={FmtPL4(hw.BatterySaverPL4Raw)}  D=0x{hw.BatterySaverD:X2}");
        Write($"  Turbo TCC: {FmtTcc(hw.TurboTcc)}");

        if (profile.ModeDefaults != null)
        {
            Section("Effective Mode Defaults");
            foreach (var kvp in profile.ModeDefaults)
            {
                var m = kvp.Value;
                string gpu = m.CTGP.HasValue ? $"CTGP={m.CTGP}W DB={m.DynamicBoost ?? 0}W" : "no GPU";
                string tcc = m.TccOffset.HasValue ? $"TCC={m.TccOffset}C" : "no TCC";
                string pl4 = m.PL4.HasValue ? $"PL4={m.PL4}W" : "no PL4";
                string curves = m.FanCurve != null ? "has fan curves" : "no fan curves";
                Write($"  {kvp.Key}: PL1={m.PL1}W PL2={m.PL2}W {pl4} {gpu} {tcc} [{curves}]");
            }
        }
    }

    /// <summary>
    /// Log all feature register values (raw + decoded).
    /// </summary>
    public static void LogFeatureRegisters(EcDevice device, MachineProfile profile)
    {
        Section("Feature Register Values");

        LogRegister(device, profile, "PL1", v => $"{RegisterEncoding.DecodePL1(v)}W");
        LogRegister(device, profile, "PL2", v => $"{RegisterEncoding.DecodePL1(v)}W");
        LogRegister(device, profile, "PL4", v => $"{RegisterEncoding.DecodePL4(v, profile.Pl4Doubled)}W");
        LogRegister(device, profile, "cpuTempLimitRead", v =>
            profile.CpuTempEncoding == "direct"
                ? $"{RegisterEncoding.DecodeCpuTempLimit(v, direct: true)}C (absolute)"
                : $"{RegisterEncoding.DecodeCpuTempLimit(v, direct: false, profile.TjMax)}C (tjMax={profile.TjMax})");
        LogRegister(device, profile, "tgpDbCtrl", v => $"bits: {Convert.ToString(v, 2).PadLeft(8, '0')}");
        LogRegister(device, profile, "tgp", v =>
            $"{RegisterEncoding.DecodeTGP(v, profile.TgpOffset)}W (offset={profile.TgpOffset})");
        LogRegister(device, profile, "dbTotalPower", v => $"raw={v}");
        LogRegister(device, profile, "dbMaxTgp", v => $"{RegisterEncoding.DecodeDB(v)}W");
        LogRegister(device, profile, "fanControlMode", v =>
            $"boost {((v & profile.Bitfields["fanBoost"].GetMask()) != 0 ? "ON" : "OFF")} (raw=0x{v:X2})");
        LogRegister(device, profile, "fanCurveEnable", v => $"bits: {Convert.ToString(v, 2).PadLeft(8, '0')}");
        LogRegister(device, profile, "batteryMode", v => $"raw=0x{v:X2}, mode={RegisterEncoding.DecodeBatteryMode(v)}");
        LogRegister(device, profile, "batterySoc", v => $"{v}%");
        LogRegister(device, profile, "cpuFanDuty", v => $"{RegisterEncoding.DecodeFanDuty(v)}%");
        LogRegister(device, profile, "gpuFanDuty", v => $"{RegisterEncoding.DecodeFanDuty(v)}%");
        LogRegister(device, profile, "logoOnOff", v => $"raw=0x{v:X2}");
        LogRegister(device, profile, "logoR", v => $"EC={v}, HID~{(int)Math.Round(v * 255.0 / 60.0)}");
        LogRegister(device, profile, "logoG", v => $"EC={v}, HID~{(int)Math.Round(v * 255.0 / 60.0)}");
        LogRegister(device, profile, "logoB", v => $"EC={v}, HID~{(int)Math.Round(v * 255.0 / 60.0)}");
    }

    /// <summary>
    /// Log fan table dump from a pre-captured snapshot.
    /// No EC reads — consumes the cached <see cref="FanSnapshot"/> so startup
    /// reads the fan table exactly once.
    /// </summary>
    public static void LogFanTable(FanSnapshot snapshot, string sectionLabel = "Fan Table Dump")
    {
        Section(sectionLabel);
        WriteHexRow("CPU tempUp  ", snapshot.CpuTempUp);
        WriteHexRow("CPU tempDown", snapshot.CpuTempDown);
        WriteHexRow("CPU duty    ", snapshot.CpuDuty);
        WriteHexRow("GPU tempUp  ", snapshot.GpuTempUp);
        WriteHexRow("GPU tempDown", snapshot.GpuTempDown);
        WriteHexRow("GPU duty    ", snapshot.GpuDuty);
        WriteHexRowBytes("RamFan2 probe", snapshot.RamFan2Probe);
        Write($"  0x7C5 = 0x{snapshot.CurveEnableReg:X2}  " +
              $"0x7C6 = 0x{snapshot.CurveModeReg:X2}  " +
              $"customMode = {snapshot.IsCustomCurveModeActive()}");
    }

    private static void WriteHexRow(string label, IReadOnlyList<int> values)
    {
        var parts = new string[values.Count];
        for (int i = 0; i < values.Count; i++) parts[i] = $"{values[i]:X2}";
        Write($"  {label}: {string.Join(" ", parts)}");
    }

    private static void WriteHexRowBytes(string label, byte[] values)
    {
        var parts = new string[values.Length];
        for (int i = 0; i < values.Length; i++) parts[i] = $"{values[i]:X2}";
        Write($"  {label}: {string.Join(" ", parts)}");
    }

    /// <summary>
    /// Log HID device enumeration results.
    /// </summary>
    public static void LogHidDevices(HidDevice hid)
    {
        Section("HID Devices");
        Write($"Keyboard zone (0): {(hid.IsZoneAvailable(0) ? "available" : "not found")}");
        Write($"Light bar zone (2): {(hid.IsZoneAvailable(2) ? "available" : "not found")}");
        Write($"Logo zone (3): {(hid.IsZoneAvailable(3) ? "available" : "not found")}");
    }

    /// <summary>
    /// Log the final effective limits that the UI will use.
    /// </summary>
    public static void LogEffectiveLimits(MachineProfile profile)
    {
        Section("Effective Limits");
        if (profile.Limits.Count == 0)
        {
            Write("  (no limits configured)");
            return;
        }
        foreach (var kvp in profile.Limits)
        {
            var l = kvp.Value;
            string extra = "";
            if (l.Offset.HasValue) extra += $", offset={l.Offset}";
            if (l.TjMax.HasValue) extra += $", tjMax={l.TjMax}";
            Write($"  {kvp.Key}: {l.Min}-{l.Max} (encoding={l.Encoding}{extra})");
        }
    }

    /// <summary>
    /// Log bitfield validation results.
    /// </summary>
    public static void LogBitfieldValidation(string name, byte rawValue, byte maskedValue, bool valid)
    {
        string status = valid ? "PASS" : "FAIL";
        Write($"  [{status}] {name}: raw=0x{rawValue:X2}, masked=0x{maskedValue:X2}");
    }

    /// <summary>
    /// Log feature register validation results.
    /// </summary>
    public static void LogFeatureValidation(string name, byte rawValue, int decodedValue, bool valid, string reason = "")
    {
        string status = valid ? "PASS" : "FAIL";
        string extra = string.IsNullOrEmpty(reason) ? "" : $" ({reason})";
        Write($"  [{status}] {name}: raw=0x{rawValue:X2}, decoded={decodedValue}{extra}");
    }

    /// <summary>
    /// Log EC init step results.
    /// </summary>
    public static void LogInitStep(string step, bool success, string? error = null)
    {
        if (success)
            Write($"  [OK] {step}");
        else
            Write($"  [FAIL] {step}: {error}");
    }

    /// <summary>
    /// Log a warning message.
    /// </summary>
    public static void Warn(string message)
    {
        Write($"[WARN] {message}");
    }

    /// <summary>
    /// Log an error message.
    /// </summary>
    public static void Error(string message)
    {
        Write($"[ERROR] {message}");
    }

    public static void Close()
    {
        lock (_sync)
        {
            _writer?.Dispose();
            _writer = null;
        }
    }

    private static void LogRegister(EcDevice device, MachineProfile profile, string name, Func<byte, string> decode)
    {
        if (!profile.Registers.TryGetValue(name, out string? hex))
        {
            Write($"  {name}: (not in profile)");
            return;
        }
        try
        {
            int addr = MachineProfile.ParseHex(hex);
            byte raw = device.ReadByte(addr);
            Write($"  {name} (0x{addr:X3}): 0x{raw:X2} -> {decode(raw)}");
        }
        catch (Exception ex)
        {
            Write($"  {name} ({hex}): read failed: {ex.Message}");
        }
    }

}
