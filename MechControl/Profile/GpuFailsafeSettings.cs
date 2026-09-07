using System.Text.Json.Serialization;

namespace MechControl.Profile;

class GpuFailsafeSettings
{
    public const int DefaultThreshold = 62;
    public const int MinimumThreshold = 30;
    public const int MaximumThreshold = 95;

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
            throw new ArgumentOutOfRangeException(nameof(ThresholdC), "Failsafe threshold must be between 30 and 95 C.");
    }

    public static GpuFailsafeSettings Transition(GpuFailsafeSettings current, GpuFailsafeSettings destination) => new()
    {
        Enabled = current.Enabled || destination.Enabled,
        ThresholdC = current.Enabled && destination.Enabled ? Math.Min(current.ThresholdC, destination.ThresholdC)
            : current.Enabled ? current.ThresholdC : destination.ThresholdC,
        ReduceCpuPower = (current.Enabled && current.ReduceCpuPower) || (destination.Enabled && destination.ReduceCpuPower),
        FanBoost = (current.Enabled && current.FanBoost) || (destination.Enabled && destination.FanBoost),
        ShowMainWindow = current.ShowMainWindow
    };
}
