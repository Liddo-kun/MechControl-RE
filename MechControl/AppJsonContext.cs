using System.Text.Json;
using System.Text.Json.Serialization;
using MechControl.Features;
using MechControl.Profile;

namespace MechControl;

/// <summary>
/// Source-generated JSON serializer context — makes all JSON operations trim-safe
/// by replacing reflection with compile-time generated metadata.
/// </summary>
[JsonSourceGenerationOptions(
    WriteIndented = true,
    AllowTrailingCommas = true,
    ReadCommentHandling = JsonCommentHandling.Skip)]
[JsonSerializable(typeof(MachineProfile))]
[JsonSerializable(typeof(UserProfile))]
[JsonSerializable(typeof(UserProfileSettings))]
[JsonSerializable(typeof(LightingState))]
[JsonSerializable(typeof(LiquidCoolerState))]
internal partial class AppJsonContext : JsonSerializerContext { }
