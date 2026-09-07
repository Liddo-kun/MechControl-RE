using System.Text.Json;
using System.Text.Json.Serialization;

namespace MechControl.Profile;

class UserProfileSettings
{
    [JsonPropertyName("lastProfile")]
    public int LastProfile { get; set; }

    [JsonPropertyName("profileNames")]
    public List<string> ProfileNames { get; set; } = new()
        { "Profile 1", "Profile 2", "Profile 3", "Profile 4", "Profile 5" };

    [JsonPropertyName("gpuHotspotEnabled")]
    public bool GpuHotspotEnabled { get; set; } = true;

    [JsonPropertyName("gpuHotspotDiagnosticLogging")]
    public bool GpuHotspotDiagnosticLogging { get; set; }

    [JsonPropertyName("gpuFailsafeTripReason")]
    public string GpuFailsafeTripReason { get; set; } = "";

    [JsonPropertyName("gpuFailsafeTripThresholdC")]
    public int GpuFailsafeTripThresholdC { get; set; } = GpuFailsafeSettings.DefaultThreshold;

    [JsonPropertyName("thermalProtection")]
    public ThermalProtectionState? ThermalProtection { get; set; }
}

class UserProfileManager
{
    private readonly string _dir;
    private UserProfileSettings _settings;



    public UserProfileManager(string? directory = null)
    {
        _dir = directory ?? AppContext.BaseDirectory;
        _settings = LoadSettings();
    }

    public int LastUsedSlot
    {
        get => _settings.LastProfile;
        set
        {
            int old = _settings.LastProfile;
            _settings.LastProfile = value;
            try { SaveSettings(); } catch { _settings.LastProfile = old; throw; }
        }
    }

    public bool GpuHotspotEnabled
    {
        get => _settings.GpuHotspotEnabled;
        set { _settings.GpuHotspotEnabled = value; SaveSettings(); }
    }

    public bool GpuHotspotDiagnosticLogging => _settings.GpuHotspotDiagnosticLogging;

    public ThermalProtectionState ThermalProtection => _settings.ThermalProtection!.Copy();

    public void SaveThermalProtection(ThermalProtectionState state)
    {
        var old = _settings.ThermalProtection;
        _settings.ThermalProtection = state.Copy();
        _settings.GpuFailsafeTripReason = ""; // Legacy read-only migration fields.
        try { SaveSettings(); }
        catch
        {
            // An unrelated settings save must not erase an unsuccessfully saved
            // trip intent or turn a failed reset/release into an authorization.
            _settings.ThermalProtection = ThermalProtectionState.Recover(old, state, false);
            throw;
        }
    }

    public string[] GetProfileNames() => _settings.ProfileNames.ToArray();

    public void RenameProfile(int slot, string name)
    {
        _settings.ProfileNames[slot - 1] = name;
        SaveSettings();
    }

    public bool SlotExists(int slot)
    {
        return File.Exists(GetSlotPath(slot));
    }

    public UserProfile? LoadProfile(int slot)
    {
        string path = GetSlotPath(slot);
        if (!File.Exists(path)) return null;
        try
        {
            string json = File.ReadAllText(path);
            return JsonSerializer.Deserialize(json, AppJsonContext.Default.UserProfile);
        }
        catch (Exception ex)
        {
            DiagnosticLog.Warn($"[UserProfile] Failed to load slot {slot}: {ex.Message}");
            return null;
        }
    }

    public void SaveProfile(int slot, UserProfile profile)
    {
        string json = JsonSerializer.Serialize(profile, AppJsonContext.Default.UserProfile);
        File.WriteAllText(GetSlotPath(slot), json);
        _settings.LastProfile = slot;
        SaveSettings();
    }

    private string GetSlotPath(int slot) =>
        Path.Combine(_dir, $"userprofile_{slot}.json");

    private string GetSettingsPath() =>
        Path.Combine(_dir, "settings.json");

    private UserProfileSettings LoadSettings()
    {
        string path = GetSettingsPath();
        bool unreadable = false;
        UserProfileSettings? ReadSettingsFile(string candidate)
        {
            string? json = null;
            try
            {
                json = File.ReadAllText(candidate);
                var settings = JsonSerializer.Deserialize(json, AppJsonContext.Default.UserProfileSettings)
                    ?? throw new IOException("Settings were null.");
                if (settings.GpuFailsafeTripReason == null) throw new IOException("Trip state was null.");
                using var document = JsonDocument.Parse(json, new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
                if (document.RootElement.TryGetProperty("thermalProtection", out var block) && block.ValueKind == JsonValueKind.Null)
                    unreadable = true;
                settings.ThermalProtection ??= string.IsNullOrEmpty(settings.GpuFailsafeTripReason) ? new() : new()
                {
                    Stage = ThermalStage.Tripped, Held = ThermalAction.Gpu,
                    ResetThresholdC = settings.GpuFailsafeTripThresholdC,
                    LastTrip = new() { Reason = settings.GpuFailsafeTripReason, ThresholdC = settings.GpuFailsafeTripThresholdC,
                        Actions = ThermalAction.Gpu, ActionDescription = "Recovered legacy GPU-only protection." }
                };
                return settings;
            }
            catch (FileNotFoundException) { return null; }
            catch (DirectoryNotFoundException) { return null; }
            catch (Exception ex)
            {
                unreadable = true;
                DiagnosticLog.Error($"[GPU Failsafe] Saved protection state uncertain ({Path.GetFileName(candidate)}): {ex.Message}");
                // A bad timestamp/enum/metadata field must not discard a still
                // readable CPU/fan obligation in the same JSON record.
                try
                {
                    using var document = JsonDocument.Parse(json ?? "null");
                    if (document.RootElement.ValueKind == JsonValueKind.Object &&
                        document.RootElement.TryGetProperty("thermalProtection", out var block) && block.ValueKind == JsonValueKind.Object &&
                        block.TryGetProperty("Held", out var actions) && actions.ValueKind == JsonValueKind.Number && actions.TryGetInt32(out int bits))
                    {
                        int? threshold = block.TryGetProperty("ResetThresholdC", out var t) && t.ValueKind == JsonValueKind.Number && t.TryGetInt32(out int n) && n is >= 30 and <= 95 ? n : null;
                        return new() { ThermalProtection = new()
                        {
                            Stage = ThermalStage.Tripped, Held = ((ThermalAction)bits & ThermalAction.All) | ThermalAction.Gpu,
                            ResetThresholdC = threshold, LastTrip = new() { Reason = "Saved protection state uncertain; event metadata unreadable." }
                        } };
                    }
                }
                catch (JsonException) { }
                return null;
            }
        }
        var committed = ReadSettingsFile(path);
        var pending = ReadSettingsFile(path + ".tmp");
        var recovered = ThermalProtectionState.Recover(committed?.ThermalProtection, pending?.ThermalProtection, unreadable);
        var settings = committed ?? pending ?? new UserProfileSettings();
        settings.ThermalProtection = recovered;
        settings.ProfileNames = Enumerable.Range(0, 5).Select(i =>
            settings.ProfileNames != null && i < settings.ProfileNames.Count && !string.IsNullOrWhiteSpace(settings.ProfileNames[i])
                ? settings.ProfileNames[i] : $"Profile {i + 1}").ToList();
        return settings;
    }

    private void SaveSettings()
    {
        string json = JsonSerializer.Serialize(_settings, AppJsonContext.Default.UserProfileSettings);
        // Atomic replacement prevents a partially written trip latch on restart.
        string path = GetSettingsPath();
        using (var file = new FileStream(path + ".tmp", FileMode.Create, FileAccess.Write, FileShare.None))
        {
            file.Write(System.Text.Encoding.UTF8.GetBytes(json));
            file.Flush(flushToDisk: true);
        }
        File.Move(path + ".tmp", path, overwrite: true);
    }
}
