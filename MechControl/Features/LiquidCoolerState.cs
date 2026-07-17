using System.Text.Json;
using System.Text.Json.Serialization;

namespace MechControl.Features;

/// <summary>
/// Persisted state for the BLE liquid cooler (next to exe, like lighting_state.json).
/// Stores saved device address, auto-connect preference, and last-used settings.
/// </summary>
class LiquidCoolerState
{
    [JsonPropertyName("savedAddress")]
    public ulong SavedAddress { get; set; }

    [JsonPropertyName("savedDeviceName")]
    public string? SavedDeviceName { get; set; }

    [JsonPropertyName("autoConnect")]
    public bool AutoConnect { get; set; }

    [JsonPropertyName("fanOn")]
    public bool FanOn { get; set; }

    [JsonPropertyName("fanDuty")]
    public int FanDuty { get; set; } = 50;

    [JsonPropertyName("pumpOn")]
    public bool PumpOn { get; set; }

    [JsonPropertyName("pumpDuty")]
    public int PumpDuty { get; set; } = 60;

    [JsonPropertyName("pumpVoltage")]
    public PumpVoltage PumpVoltage { get; set; } = PumpVoltage.V8;

    [JsonPropertyName("rgbOn")]
    public bool RgbOn { get; set; }

    [JsonPropertyName("r")]
    public byte R { get; set; }

    [JsonPropertyName("g")]
    public byte G { get; set; } = 128;

    [JsonPropertyName("b")]
    public byte B { get; set; }

    [JsonPropertyName("rgbMode")]
    public CoolerRgbMode RgbMode { get; set; } = CoolerRgbMode.Static;

    private static string GetPath() =>
        Path.Combine(AppContext.BaseDirectory, "liquid_cooler_state.json");



    public static LiquidCoolerState Load()
    {
        try
        {
            string path = GetPath();
            if (File.Exists(path))
            {
                string json = File.ReadAllText(path);
                return JsonSerializer.Deserialize(json, AppJsonContext.Default.LiquidCoolerState) ?? new();
            }
        }
        catch (Exception ex)
        {
            DiagnosticLog.Warn($"[LiquidCoolerState] Load failed: {ex.Message}");
        }
        return new();
    }

    public void Save()
    {
        try
        {
            string json = JsonSerializer.Serialize(this, AppJsonContext.Default.LiquidCoolerState);
            File.WriteAllText(GetPath(), json);
        }
        catch (Exception ex)
        {
            DiagnosticLog.Warn($"[LiquidCoolerState] Save failed: {ex.Message}");
        }
    }
}
