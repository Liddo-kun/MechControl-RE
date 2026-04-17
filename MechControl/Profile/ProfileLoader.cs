using System.Management;
using System.Text.Json;
using MechControl.Driver;

namespace MechControl.Profile;

static class ProfileLoader
{


    /// <summary>
    /// Detection method used for the last successful load (for logging).
    /// </summary>
    public static string LastDetectionMethod { get; private set; } = "none";

    /// <summary>WMI baseboard product string (for titlebar display).</summary>
    public static string? LastBaseboard { get; private set; }

    /// <summary>CPU model name from WMI.</summary>
    public static string? LastCpuName { get; private set; }

    /// <summary>
    /// Detect the machine profile and load it. Returns null if user cancels manual selection.
    /// Applies universal defaults for unverified profiles.
    /// </summary>
    public static MachineProfile? DetectAndLoad(EcDevice? device)
    {
        string profilesDir = GetProfilesDirectory();

        // Always read baseboard + CPU name (for titlebar + logging + system info)
        try
        {
            LastBaseboard = GetBaseboardProduct();
            LastCpuName = GetCpuName();
            DiagnosticLog.LogBaseboard(LastBaseboard);
        }
        catch { }

        // Try EC-based detection first (if device is available)
        if (device != null)
        {
            try
            {
                byte raw = device.ReadByte(0x740);
                Console.WriteLine($"[Profile] EC 0x740 = {raw} (0x{raw:X2})");

                // Try direct match
                string? path = FindProfileByProjectId(profilesDir, raw);
                if (path != null)
                {
                    Console.WriteLine($"[Profile] Direct match: {Path.GetFileName(path)}");
                    LastDetectionMethod = $"EC direct (0x740={raw})";
                    return LoadAndPrepare(path);
                }

                // Try raw >> 1 (newer models store ProjectID * 2)
                int shifted = raw >> 1;
                path = FindProfileByProjectId(profilesDir, shifted);
                if (path != null)
                {
                    Console.WriteLine($"[Profile] Shifted match: {raw} >> 1 = {shifted} -> {Path.GetFileName(path)}");
                    LastDetectionMethod = $"EC shifted (0x740={raw}, >>1={shifted})";
                    return LoadAndPrepare(path);
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Profile] EC detection failed: {ex.Message}");
            }
        }

        // Try WMI baseboard name match

        try
        {
            if (LastBaseboard != null)
            {
                Console.WriteLine($"[Profile] Baseboard: {LastBaseboard}");
                string? path = FindProfileByBaseboard(profilesDir, LastBaseboard);
                if (path != null)
                {
                    Console.WriteLine($"[Profile] Baseboard match: {Path.GetFileName(path)}");
                    LastDetectionMethod = $"WMI baseboard ({LastBaseboard})";
                    return LoadAndPrepare(path);
                }
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[Profile] WMI detection failed: {ex.Message}");
        }

        // Manual selection
        LastDetectionMethod = "manual selection";
        var profile = PromptUserForProfile(profilesDir);
        if (profile != null)
            UniversalDefaults.ApplyToProfile(profile);
        return profile;
    }

    /// <summary>
    /// Load a profile and apply universal defaults if unverified.
    /// </summary>
    private static MachineProfile LoadAndPrepare(string path)
    {
        var profile = LoadFromFile(path);
        UniversalDefaults.ApplyToProfile(profile);
        return profile;
    }

    /// <summary>
    /// Load a profile directly from a file path (for testing or manual selection).
    /// </summary>
    public static MachineProfile LoadFromFile(string path)
    {
        string json = File.ReadAllText(path);
        var profile = JsonSerializer.Deserialize(json, AppJsonContext.Default.MachineProfile)
            ?? throw new InvalidDataException($"Failed to deserialize profile: {path}");

        ValidateProfile(profile, path);
        return profile;
    }

    private static string GetProfilesDirectory()
    {
        // Look next to the executable
        string exeDir = AppContext.BaseDirectory;
        string profilesDir = Path.Combine(exeDir, "Profiles", "MECHREVO");
        if (Directory.Exists(profilesDir))
            return profilesDir;

        // Fallback: look in the project source tree (development)
        string? devDir = FindDevProfilesDir(exeDir);
        if (devDir != null)
            return devDir;

        throw new DirectoryNotFoundException(
            $"Profiles directory not found. Expected at: {profilesDir}");
    }

    private static string? FindDevProfilesDir(string startDir)
    {
        // Walk up from bin/Release/net8.0-windows/ to find Profiles/MECHREVO/
        string? dir = startDir;
        for (int i = 0; i < 6 && dir != null; i++)
        {
            string candidate = Path.Combine(dir, "Profiles", "MECHREVO");
            if (Directory.Exists(candidate))
                return candidate;
            dir = Path.GetDirectoryName(dir);
        }
        return null;
    }

    private static string? FindProfileByProjectId(string profilesDir, int projectIdValue)
    {
        foreach (string file in Directory.GetFiles(profilesDir, "*.json"))
        {
            try
            {
                string json = File.ReadAllText(file);
                using var doc = JsonDocument.Parse(json, new JsonDocumentOptions
                {
                    CommentHandling = JsonCommentHandling.Skip,
                    AllowTrailingCommas = true
                });
                if (doc.RootElement.TryGetProperty("projectIdValue", out var idElem) &&
                    idElem.GetInt32() == projectIdValue)
                {
                    return file;
                }
            }
            catch { }
        }
        return null;
    }

    private static string? FindProfileByBaseboard(string profilesDir, string baseboard)
    {
        // Simple substring match — profile filename or projectId in the baseboard string
        string boardLower = baseboard.ToLowerInvariant();
        foreach (string file in Directory.GetFiles(profilesDir, "*.json"))
        {
            string name = Path.GetFileNameWithoutExtension(file).ToLowerInvariant();
            if (boardLower.Contains(name))
                return file;
        }
        return null;
    }

    private static string? GetBaseboardProduct()
    {
        try
        {
            using var searcher = new ManagementObjectSearcher("SELECT Product FROM Win32_BaseBoard");
            foreach (var obj in searcher.Get())
            {
                string? product = obj["Product"]?.ToString();
                if (!string.IsNullOrWhiteSpace(product))
                    return product;
            }
        }
        catch { }
        return null;
    }

    private static string? GetCpuName()
    {
        try
        {
            using var searcher = new ManagementObjectSearcher("SELECT Name FROM Win32_Processor");
            foreach (var obj in searcher.Get())
            {
                string? name = obj["Name"]?.ToString();
                if (!string.IsNullOrWhiteSpace(name))
                    return name;
            }
        }
        catch { }
        return null;
    }

    private static MachineProfile? PromptUserForProfile(string profilesDir)
    {
        string[] files;
        try
        {
            files = Directory.GetFiles(profilesDir, "*.json");
        }
        catch
        {
            MessageBox.Show(
                "No profile directory found. Cannot continue.",
                "MechControl — Profile Error",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            return null;
        }

        if (files.Length == 0)
        {
            MessageBox.Show(
                "No profile files found in Profiles directory.",
                "MechControl — Profile Error",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            return null;
        }

        var names = files.Select(f => Path.GetFileNameWithoutExtension(f)).OrderBy(n => n).ToArray();

        // Use a simple input dialog
        string? selected = ShowProfileSelectionDialog(names, files);
        if (selected == null)
            return null;

        return LoadFromFile(selected);
    }

    private static string? ShowProfileSelectionDialog(string[] names, string[] files)
    {
        var form = new Form
        {
            Text = "MechControl — Select Profile",
            Size = new System.Drawing.Size(350, 300),
            StartPosition = FormStartPosition.CenterScreen,
            FormBorderStyle = FormBorderStyle.FixedDialog,
            MaximizeBox = false,
            MinimizeBox = false
        };

        var label = new Label
        {
            Text = "Could not auto-detect machine model.\nPlease select your profile:",
            Location = new System.Drawing.Point(12, 12),
            AutoSize = true
        };

        var listBox = new ListBox
        {
            Location = new System.Drawing.Point(12, 50),
            Size = new System.Drawing.Size(310, 160)
        };
        listBox.Items.AddRange(names);
        if (listBox.Items.Count > 0)
            listBox.SelectedIndex = 0;

        var okButton = new Button
        {
            Text = "OK",
            DialogResult = DialogResult.OK,
            Location = new System.Drawing.Point(166, 220),
            Size = new System.Drawing.Size(75, 28)
        };

        var cancelButton = new Button
        {
            Text = "Cancel",
            DialogResult = DialogResult.Cancel,
            Location = new System.Drawing.Point(247, 220),
            Size = new System.Drawing.Size(75, 28)
        };

        form.AcceptButton = okButton;
        form.CancelButton = cancelButton;
        form.Controls.AddRange(new Control[] { label, listBox, okButton, cancelButton });

        if (form.ShowDialog() == DialogResult.OK && listBox.SelectedIndex >= 0)
            return files.First(f => Path.GetFileNameWithoutExtension(f) == (string)listBox.SelectedItem!);

        return null;
    }

    private static void ValidateProfile(MachineProfile profile, string path)
    {
        if (string.IsNullOrEmpty(profile.ProjectId))
            throw new InvalidDataException($"Profile missing 'projectId': {path}");
        if (profile.Registers.Count == 0)
            throw new InvalidDataException($"Profile missing 'registers': {path}");
        if (profile.WriteAllowlist.Count == 0)
            throw new InvalidDataException($"Profile missing 'writeAllowlist': {path}");
    }
}
