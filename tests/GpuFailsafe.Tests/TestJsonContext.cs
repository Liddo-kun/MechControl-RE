using System.Text.Json.Serialization;
using MechControl.Profile;

namespace MechControl
{
    [JsonSourceGenerationOptions(WriteIndented = true, AllowTrailingCommas = true,
        ReadCommentHandling = System.Text.Json.JsonCommentHandling.Skip)]
    [JsonSerializable(typeof(UserProfileSettings))]
    [JsonSerializable(typeof(UserProfile))]
    internal partial class AppJsonContext : JsonSerializerContext { }
}

namespace MechControl.Features
{
    // BLE is outside the hardware-free safety suite; only these profile enums are needed.
    enum PumpVoltage { V8 }
    enum CoolerRgbMode { Static }
}
