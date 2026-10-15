using System.Text.Json.Serialization;

namespace MechControl.Profile;

class GpuFailsafeSettings
{
    public const int DefaultThreshold = 50;
    public const int MinimumThreshold = 50;
    public const int MaximumThreshold = 95;
    public const int LegacyMinimumThreshold = 30;

    [JsonPropertyName("enabled")]
    public bool Enabled { get; set; }

    [JsonPropertyName("thresholdC")]
    public int ThresholdC { get; set; } = DefaultThreshold;

    [JsonPropertyName("reduceCpuPower")]
    public bool ReduceCpuPower { get; set; }

    [JsonPropertyName("fanBoost")]
    public bool FanBoost { get; set; }

    [JsonPropertyName("showMainWindow")]
    public bool ShowMainWindow { get; set; } = true;

    public GpuFailsafeSettings Copy() => (GpuFailsafeSettings)MemberwiseClone();
    public bool SameAs(GpuFailsafeSettings other) => Enabled == other.Enabled && ThresholdC == other.ThresholdC &&
        ReduceCpuPower == other.ReduceCpuPower && FanBoost == other.FanBoost && ShowMainWindow == other.ShowMainWindow;

    public void Validate()
    {
        if (ThresholdC < MinimumThreshold || ThresholdC > MaximumThreshold)
            throw new ArgumentOutOfRangeException(nameof(ThresholdC), $"Failsafe threshold must be between {MinimumThreshold} and {MaximumThreshold} C.");
    }
}
