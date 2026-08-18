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
}

class UserProfileManager
{
    private readonly string _dir;
    private UserProfileSettings _settings;



    public UserProfileManager()
    {
        _dir = AppContext.BaseDirectory;
        _settings = LoadSettings();
    }

    public int LastUsedSlot
    {
        get => _settings.LastProfile;
        set { _settings.LastProfile = value; SaveSettings(); }
    }

    public bool GpuHotspotEnabled
    {
        get => _settings.GpuHotspotEnabled;
        set { _settings.GpuHotspotEnabled = value; SaveSettings(); }
    }

    public bool GpuHotspotDiagnosticLogging => _settings.GpuHotspotDiagnosticLogging;

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
        if (!File.Exists(path)) return new UserProfileSettings();
        try
        {
            string json = File.ReadAllText(path);
            return JsonSerializer.Deserialize(json, AppJsonContext.Default.UserProfileSettings)
                ?? new UserProfileSettings();
        }
        catch
        {
            return new UserProfileSettings();
        }
    }

    private void SaveSettings()
    {
        string json = JsonSerializer.Serialize(_settings, AppJsonContext.Default.UserProfileSettings);
        File.WriteAllText(GetSettingsPath(), json);
    }
}
