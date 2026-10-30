using System.Text.Json.Serialization;

namespace MechControl.Cli;

// mcctl status/readback. Absent features are null (not supported or not validated on this
// machine), so agents can tell what the model offers. Currents are exact driver milliamps.
sealed record CliStatus(string Model, CliFailsafe Failsafe, CliCpu? Cpu, CliGpu? Gpu, CliFans Fans);

sealed record CliFailsafe(bool Tripped, string State);

// A setting and the range a command accepts for it right now.
sealed record CliSetting(int Value, int Min, int Max);

sealed record CliCpu(CliSetting Pl1, CliSetting Pl2, CliSetting Pl4, CliSetting TempLimit);

sealed record CliGpu(
    CliSetting Tgp, bool DynamicBoost, CliSetting DynamicBoostWatts, bool Unlocked, bool UnlockAvailable,
    // Why the NVIDIA values below are null; null when they were read.
    string? Nvidia, int? CeilingWatts, int? EnforcedWatts, int? RequestedWatts,
    CliCurrent? Nvvdd, CliCurrent? Msvdd, CliCurrent? BoardInput,
    int? CurrentCapMhz, int? PowerCapMhz, int? MaxClockMhz);

// MinMa/MaxMa: what a command accepts without a GPU restart. AtClockCapMa is the driver's
// prediction at the current controller's clock cap (none for board input).
sealed record CliCurrent(int LimitMa, int MinMa, int MaxMa, int StockMa, int? MeasuredMa, int? AtClockCapMa);

// Boost is null when fan control is unavailable on this model.
sealed record CliFans(bool? Boost, int CpuRpm, int GpuRpm, int CpuDutyPercent, int GpuDutyPercent, int CpuTempC, int GpuTempC);

// One compact line per object, so `mcctl watch --json` is JSON Lines. Write commands
// return only the group they changed.
[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(CliStatus))]
[JsonSerializable(typeof(CliCpu))]
[JsonSerializable(typeof(CliGpu))]
[JsonSerializable(typeof(CliFans))]
internal partial class CliJsonContext : JsonSerializerContext { }
