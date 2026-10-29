using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;

namespace MechControl.Profile;

class UserProfileSettings
{
    [JsonPropertyName("lastProfile")]
    public int LastProfile { get; set; }

    [JsonPropertyName("profileNames")]
    public List<string> ProfileNames { get; set; } = new()
        { "Profile 1", "Profile 2", "Profile 3", "Profile 4", "Profile 5" };

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
    private readonly bool _gpuRecoverySupported;
    private UserProfileSettings _settings;
    public string ConfigurationWarning { get; private set; } = "";

    private void Warn(string message)
    {
        if (ConfigurationWarning.Contains(message, StringComparison.Ordinal)) return;
        DiagnosticLog.Warn("[Configuration] " + message);
        ConfigurationWarning += (ConfigurationWarning.Length == 0 ? "" : "\n") + message;
    }

    public UserProfileManager(string? directory = null, bool gpuRecoverySupported = true)
    {
        _dir = directory ?? AppContext.BaseDirectory;
        _gpuRecoverySupported = gpuRecoverySupported;
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
            _settings.ThermalProtection = ThermalProtectionState.Recover(old, state, false, _gpuRecoverySupported);
            throw;
        }
    }

    public string[] GetProfileNames() => _settings.ProfileNames.ToArray();

    public void RenameProfile(int slot, string name)
    {
        string old = _settings.ProfileNames[slot - 1];
        _settings.ProfileNames[slot - 1] = name;
        try { SaveSettings(); } catch { _settings.ProfileNames[slot - 1] = old; throw; }
    }

    public void ClearProfile(int slot)
    {
        int oldLast = _settings.LastProfile;
        string oldName = _settings.ProfileNames[slot - 1];
        // Settings first: a failed save leaves the slot intact, and startup
        // auto-load never points at a deleted file.
        if (oldLast == slot) _settings.LastProfile = 0;
        _settings.ProfileNames[slot - 1] = $"Profile {slot}";
        try { SaveSettings(); }
        catch { _settings.LastProfile = oldLast; _settings.ProfileNames[slot - 1] = oldName; throw; }
        File.Delete(GetSlotPath(slot));
    }

    public bool SlotExists(int slot)
    {
        return File.Exists(GetSlotPath(slot));
    }

    public UserProfile? LoadProfile(int slot)
    {
        string path = GetSlotPath(slot);
        try
        {
            string json = File.ReadAllText(path);
            var profile = JsonSerializer.Deserialize(json, AppJsonContext.Default.UserProfile)
                ?? throw new JsonException("Profile is null.");
            if (profile.GpuFailsafe == null) throw new JsonException("Protection settings are null.");
            if (profile.GpuFailsafe.ThresholdC is >= GpuFailsafeSettings.LegacyMinimumThreshold and < GpuFailsafeSettings.MinimumThreshold)
            {
                profile.GpuFailsafe.ThresholdC = GpuFailsafeSettings.MinimumThreshold;
                profile.RequiresSave = true;
                Warn($"Slot {slot}: legacy trigger migrated to {GpuFailsafeSettings.MinimumThreshold} C in memory; detection enabled/options unchanged.");
            }
            profile.GpuFailsafe.Validate();
            return profile;
        }
        catch (FileNotFoundException) { return null; }
        catch (DirectoryNotFoundException) { return null; }
        catch (Exception ex)
        {
            throw new IOException($"Slot {slot} could not load: {ex.Message}", ex);
        }
    }

    public void SaveProfile(int slot, UserProfile profile)
    {
        string json = JsonSerializer.Serialize(profile, AppJsonContext.Default.UserProfile);
        WriteAtomic(GetSlotPath(slot), json);
        try { LastUsedSlot = slot; }
        catch (Exception ex) { throw new IOException($"Profile slot {slot} was saved, but selected-slot settings were not saved. Selection is unchanged: {ex.Message}", ex); }
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
            try
            {
                using var document = JsonDocument.Parse(File.ReadAllText(candidate), new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
                var root = document.RootElement;
                if (root.ValueKind != JsonValueKind.Object) throw new JsonException("Settings must be an object.");
                T Read<T>(JsonElement parent, string name, JsonTypeInfo<T> type, T fallback, bool operational = false)
                {
                    if (!parent.TryGetProperty(name, out var value))
                    {
                        if (operational) { unreadable = true; Warn($"Missing operational {name} in {candidate}."); }
                        return fallback;
                    }
                    if (value.ValueKind == JsonValueKind.Null && fallback is null) return fallback;
                    try { return value.Deserialize(type) ?? throw new JsonException("Value is null."); }
                    catch (JsonException ex)
                    {
                        unreadable |= operational;
                        Warn($"Invalid {name} in {Path.GetFileName(candidate)}: {ex.Message}");
                        if (name == "lastProfile") Warn("Requested profile identity could not be recovered; its protection settings were not armed. Select a valid profile.");
                        return fallback;
                    }
                }
                var context = AppJsonContext.Default;
                var settings = new UserProfileSettings();
                settings.LastProfile = Read(root, "lastProfile", context.Int32, 0);
                if (settings.LastProfile is < 0 or > 5) { Warn("Invalid lastProfile; requested protection settings were not armed. Select a valid profile."); settings.LastProfile = 0; }
                settings.ProfileNames = Read(root, "profileNames", context.ListString, settings.ProfileNames);
                if (settings.ProfileNames.Count != 5 || settings.ProfileNames.Any(string.IsNullOrWhiteSpace)) Warn("Invalid profile names repaired; operational protection is unchanged.");
                var state = new ThermalProtectionState();
                if (root.TryGetProperty("thermalProtection", out var block))
                {
                    if (block.ValueKind != JsonValueKind.Object) throw new JsonException("Operational state must be an object.");
                    state.Stage = Read(block, "Stage", context.ThermalStage, ThermalStage.None, true);
                    state.Held = Read(block, "Held", context.ThermalAction, ThermalAction.None, true);
                    state.RecoveryDetails = Read(block, "RecoveryDetails", context.String, "");
                    if (block.TryGetProperty("LastTrip", out var trip) && trip.ValueKind != JsonValueKind.Null)
                    {
                        if (trip.ValueKind != JsonValueKind.Object) Warn("Invalid optional trip history; operational holds retained.");
                        else state.LastTrip = new()
                        {
                            Time = Read(trip, "Time", context.NullableDateTimeOffset, (DateTimeOffset?)null),
                            Reason = Read(trip, "Reason", context.String, ""),
                            TemperatureC = Read(trip, "TemperatureC", context.NullableInt32, (int?)null),
                            ThresholdC = Read(trip, "ThresholdC", context.NullableInt32, (int?)null),
                            Actions = Read(trip, "Actions", context.ThermalAction, ThermalAction.None),
                            ActionDescription = Read(trip, "ActionDescription", context.String, ""),
                            ShowMainWindow = Read(trip, "ShowMainWindow", context.Boolean, true),
                            Results = Read(trip, "Results", context.DictionaryStringString, new Dictionary<string, string>())
                        };
                    }
                }
                else
                {
                    string reason = Read(root, "gpuFailsafeTripReason", context.String, "", root.TryGetProperty("gpuFailsafeTripReason", out _));
                    if (reason.Length > 0)
                    {
                        int threshold = Read(root, "gpuFailsafeTripThresholdC", context.Int32, GpuFailsafeSettings.DefaultThreshold);
                        state = new() { Stage = ThermalStage.Tripped, Held = ThermalAction.Gpu,
                            LastTrip = new() { Reason = reason, ThresholdC = threshold, Actions = ThermalAction.Gpu,
                                ActionDescription = "Recovered legacy GPU-only protection." } };
                    }
                }
                settings.ThermalProtection = state;
                return settings;
            }
            catch (FileNotFoundException) { return null; }
            catch (DirectoryNotFoundException) { return null; }
            catch (Exception ex)
            {
                unreadable = true;
                Warn($"Saved protection state uncertain ({Path.GetFileName(candidate)}): {ex.Message}");
                return null;
            }
        }
        var committed = ReadSettingsFile(path);
        var pending = ReadSettingsFile(path + ".tmp");
        var recovered = ThermalProtectionState.Recover(committed?.ThermalProtection, pending?.ThermalProtection, unreadable, _gpuRecoverySupported);
        var settings = committed ?? pending ?? new UserProfileSettings();
        settings.ThermalProtection = recovered;
        if (recovered.RecoveryDetails.Length > 0) Warn(recovered.RecoveryDetails);
        settings.ProfileNames = Enumerable.Range(0, 5).Select(i =>
            settings.ProfileNames != null && i < settings.ProfileNames.Count && !string.IsNullOrWhiteSpace(settings.ProfileNames[i])
                ? settings.ProfileNames[i] : $"Profile {i + 1}").ToList();
        return settings;
    }

    private void SaveSettings()
    {
        string json = JsonSerializer.Serialize(_settings, AppJsonContext.Default.UserProfileSettings);
        WriteAtomic(GetSettingsPath(), json);
    }

    private static void WriteAtomic(string path, string json)
    {
        // Same-directory replacement preserves the last committed file on failure.
        using (var file = new FileStream(path + ".tmp", FileMode.Create, FileAccess.Write, FileShare.None))
        {
            file.Write(System.Text.Encoding.UTF8.GetBytes(json));
            file.Flush(flushToDisk: true);
        }
        File.Move(path + ".tmp", path, overwrite: true);
    }
}
