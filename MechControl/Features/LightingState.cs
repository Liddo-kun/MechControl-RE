using System.Text.Json;
using System.Text.Json.Serialization;

namespace MechControl.Features;

class ZoneState
{
    [JsonPropertyName("enabled")]
    public bool Enabled { get; set; }

    [JsonPropertyName("r")]
    public byte R { get; set; }

    [JsonPropertyName("g")]
    public byte G { get; set; }

    [JsonPropertyName("b")]
    public byte B { get; set; }

    [JsonPropertyName("brightness")]
    public int Brightness { get; set; }
}

class LightingState
{
    [JsonPropertyName("keyboard")]
    public ZoneState Keyboard { get; set; } = new();

    [JsonPropertyName("lightBar")]
    public ZoneState LightBar { get; set; } = new();

    [JsonPropertyName("logo")]
    public ZoneState Logo { get; set; } = new();

    [JsonPropertyName("idleTimerSeconds")]
    public int IdleTimerSeconds { get; set; }

    private static string GetPath() =>
        Path.Combine(AppContext.BaseDirectory, "lighting_state.json");



    public static LightingState Load()
    {
        try
        {
            string path = GetPath();
            if (File.Exists(path))
            {
                string json = File.ReadAllText(path);
                return JsonSerializer.Deserialize(json, AppJsonContext.Default.LightingState) ?? new();
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[LightingState] Load failed: {ex.Message}");
        }
        return new();
    }

    public void Save()
    {
        try
        {
            string json = JsonSerializer.Serialize(this, AppJsonContext.Default.LightingState);
            File.WriteAllText(GetPath(), json);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[LightingState] Save failed: {ex.Message}");
        }
    }
}
