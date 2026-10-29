using System.Buffers.Binary;
using System.Runtime.Intrinsics.X86;
using System.Text;
using System.Text.Json;

namespace MechControl.Profile;

// Profile file matching: pure JSON/file logic, no WMI or UI, so the tests can
// compile it against the real Profiles directory.
static partial class ProfileLoader
{
    private static readonly JsonDocumentOptions ProfileJsonOptions = new()
    {
        CommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true
    };

    /// <summary>
    /// CPU vendor from CPUID leaf 0: "intel", "amd", or null if unknown.
    /// </summary>
    public static string? DetectCpuVendor()
    {
        if (!X86Base.IsSupported) return null;
        var (_, ebx, ecx, edx) = X86Base.CpuId(0, 0);
        Span<byte> id = stackalloc byte[12];
        BinaryPrimitives.WriteInt32LittleEndian(id, ebx);
        BinaryPrimitives.WriteInt32LittleEndian(id[4..], edx);
        BinaryPrimitives.WriteInt32LittleEndian(id[8..], ecx);
        return NormalizeCpuVendor(Encoding.ASCII.GetString(id));
    }

    public static string? NormalizeCpuVendor(string cpuidVendor) => cpuidVendor switch
    {
        "GenuineIntel" => "intel",
        "AuthenticAMD" => "amd",
        _ => null
    };

    private static List<(string File, JsonElement Root)> ReadProfileJsons(string profilesDir)
    {
        var result = new List<(string, JsonElement)>();
        foreach (string file in Directory.GetFiles(profilesDir, "*.json").OrderBy(f => f, StringComparer.Ordinal))
        {
            try
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(file), ProfileJsonOptions);
                result.Add((file, doc.RootElement.Clone()));
            }
            catch { }
        }
        return result;
    }

    /// <summary>
    /// A profile that declares cpuVendor only loads on that vendor's CPUs.
    /// Intel and AMD siblings share EC project IDs (X6AR and X6FR both report
    /// 0x740 = 0x1A) but need different CPU temperature-limit encodings.
    /// </summary>
    private static bool IsCpuVendorCompatible(string file, JsonElement root, string? cpuVendor)
    {
        string? declared = root.TryGetProperty("cpuVendor", out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
        if (cpuVendor == null || declared == null || string.Equals(declared, cpuVendor, StringComparison.OrdinalIgnoreCase))
            return true;
        DiagnosticLog.Write($"[Profile] Skipping {Path.GetFileName(file)}: declares cpuVendor '{declared}', CPU is {cpuVendor}");
        return false;
    }

    internal static string? FindProfileByProjectId(string profilesDir, int projectIdValue, string? cpuVendor)
    {
        var matches = new List<string>();
        foreach (var (file, root) in ReadProfileJsons(profilesDir))
        {
            try
            {
                if (root.TryGetProperty("projectIdValue", out var idElem) &&
                    idElem.ValueKind == JsonValueKind.Number && idElem.GetInt32() == projectIdValue &&
                    IsCpuVendorCompatible(file, root, cpuVendor))
                    matches.Add(file);
            }
            catch { }
        }
        if (matches.Count == 0)
            return null;
        if (matches.Count > 1)
            DiagnosticLog.Write($"[Profile] Project ID {projectIdValue} matched multiple profiles: " +
                string.Join(", ", matches.Select(f => Path.GetFileName(f))) + $" -> chose {Path.GetFileName(matches[0])}");
        return matches[0];
    }

    /// <summary>
    /// Match against explicit baseboardPatterns declared in profile JSONs.
    /// Real MECHREVO baseboards (e.g. "YAOSHI Series-X6AR55xY" -> IDY_7Y)
    /// don't contain the profile name at all, so filename matching can't
    /// identify them — this explicit mapping, built up from field logs, is
    /// the reliable path. Longest matching pattern wins.
    /// </summary>
    internal static string? FindProfileByBaseboardPattern(string profilesDir, string baseboard, string? cpuVendor)
    {
        string boardLower = baseboard.ToLowerInvariant();
        string? bestFile = null;
        int bestLen = -1;
        foreach (var (file, root) in ReadProfileJsons(profilesDir))
        {
            try
            {
                if (!root.TryGetProperty("baseboardPatterns", out var patterns) ||
                    patterns.ValueKind != JsonValueKind.Array)
                    continue;
                foreach (var pattern in patterns.EnumerateArray())
                {
                    string? p = pattern.GetString();
                    if (string.IsNullOrWhiteSpace(p)) continue;
                    if (boardLower.Contains(p.ToLowerInvariant()) && p.Length > bestLen &&
                        IsCpuVendorCompatible(file, root, cpuVendor))
                    {
                        bestFile = file;
                        bestLen = p.Length;
                    }
                }
            }
            catch { }
        }
        if (bestFile != null)
            DiagnosticLog.Write($"[Profile] Baseboard pattern match: {Path.GetFileName(bestFile)}");
        return bestFile;
    }

    internal static string? FindProfileByBaseboardFilename(string profilesDir, string baseboard, string? cpuVendor)
    {
        // Token match: the profile filename must appear in the baseboard string
        // bounded by non-alphanumerics (or the string edges). A raw substring
        // match let short profile names (e.g. "GI") false-match any baseboard
        // containing those letters, and enumeration order made ties
        // nondeterministic. Prefer the longest (most specific) match.
        string boardLower = baseboard.ToLowerInvariant();
        var candidates = new List<(string file, string name)>();
        foreach (var (file, root) in ReadProfileJsons(profilesDir))
        {
            string name = Path.GetFileNameWithoutExtension(file).ToLowerInvariant();
            if (ContainsToken(boardLower, name) && IsCpuVendorCompatible(file, root, cpuVendor))
                candidates.Add((file, name));
        }
        if (candidates.Count == 0)
            return null;

        var best = candidates
            .OrderByDescending(c => c.name.Length)
            .ThenBy(c => c.name, StringComparer.Ordinal)
            .First();
        if (candidates.Count > 1)
        {
            DiagnosticLog.Write($"[Profile] Baseboard '{baseboard}' matched multiple profiles: " +
                string.Join(", ", candidates.Select(c => Path.GetFileName(c.file))) +
                $" -> chose {Path.GetFileName(best.file)} (longest match)");
        }
        return best.file;
    }

    /// <summary>
    /// True if <paramref name="token"/> occurs in <paramref name="haystack"/>
    /// with non-alphanumeric characters (or string edges) on both sides.
    /// </summary>
    private static bool ContainsToken(string haystack, string token)
    {
        if (token.Length == 0) return false;
        int idx = 0;
        while ((idx = haystack.IndexOf(token, idx, StringComparison.Ordinal)) >= 0)
        {
            bool startOk = idx == 0 || !char.IsLetterOrDigit(haystack[idx - 1]);
            int end = idx + token.Length;
            bool endOk = end == haystack.Length || !char.IsLetterOrDigit(haystack[end]);
            if (startOk && endOk) return true;
            idx++;
        }
        return false;
    }
}
