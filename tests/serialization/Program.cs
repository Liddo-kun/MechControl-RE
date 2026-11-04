using System.Text.Json;
using MechControl;
using MechControl.Profile;
using MechControl.Features;

if (args.Length != 2) throw new Exception("Expected native fixtures and output directory.");
string input = args[0], output = args[1];
Directory.CreateDirectory(output);
void Check(bool ok, string message) { if (!ok) throw new Exception(message); }
var context = AppJsonContext.Default;
var profile = JsonSerializer.Deserialize(File.ReadAllText(Path.Combine(input, "userprofile.json")), context.UserProfile)!;
Check(profile.ProjectId == "IDY_7Y" && profile.Name == "Native fixture ☃" && profile.Cpu.PL1 == 80 && profile.Cpu.PL4 == 200, "Identity, UTF-8 and CPU");
Check(profile.Gpu.MsvddMilliamps == 120000 && profile.Gpu.NvvddMilliamps == null && profile.Gpu.BoardInputMilliamps == null && profile.Gpu.BiosOverrideEnabled == false, "GPU nullable fields");
Check(profile.LiquidCooler!.PumpVoltage == PumpVoltage.V7 && profile.LiquidCooler.RgbMode == CoolerRgbMode.BreatheColor, "Cooler slot numeric wire enums");
Check(profile.LiquidCooler.FanDuty == 50 && profile.LiquidCooler.PumpDuty == 60 && profile.LiquidCooler.G == 0, "Cooler slot defaults");
Check(profile.GpuFailsafe.ThresholdC == 50 && !profile.GpuFailsafe.Enabled && profile.GpuFailsafe.ShowMainWindow && profile.Fan.SwitchSpeedMs == 100, "Slot defaults");
var settings = JsonSerializer.Deserialize(File.ReadAllText(Path.Combine(input, "settings.json")), context.UserProfileSettings)!;
Check(settings.LastProfile == 4 && settings.DisableDlppc && settings.ProfileNames.Count == 5, "Settings");
Check(settings.ThermalProtection!.Stage == ThermalStage.Tripped && settings.ThermalProtection.Held == (ThermalAction.Cpu | ThermalAction.Fan), "Thermal operational state");
Check(settings.ThermalProtection.LastTrip!.Time != null && settings.ThermalProtection.LastTrip.TemperatureC == null && settings.ThermalProtection.LastTrip.ThresholdC == null, "Unknown history retained");
var lighting = JsonSerializer.Deserialize(File.ReadAllText(Path.Combine(input, "lighting_state.json")), context.LightingState)!;
Check(lighting.Keyboard.Enabled && lighting.Keyboard.G == 128 && lighting.IdleTimerSeconds == 30, "Lighting");
var cooler = JsonSerializer.Deserialize(File.ReadAllText(Path.Combine(input, "liquid_cooler_state.json")), context.LiquidCoolerState)!;
Check(cooler.SavedAddress == 0xFEDCBA9876543210UL && cooler.G == 128 && cooler.PumpVoltage == PumpVoltage.V12, "Cooler state ulong and enum");
File.WriteAllText(Path.Combine(output, "userprofile_1.json"), JsonSerializer.Serialize(profile, context.UserProfile));
File.WriteAllText(Path.Combine(output, "settings.json"), JsonSerializer.Serialize(settings, context.UserProfileSettings));
File.WriteAllText(Path.Combine(output, "lighting_state.json"), JsonSerializer.Serialize(lighting, context.LightingState));
File.WriteAllText(Path.Combine(output, "liquid_cooler_state.json"), JsonSerializer.Serialize(cooler, context.LiquidCoolerState));
var text = new[] { "ÄÉΣА", "İıKſ", "ẞß", "𐐀𐐨", "Აა" };
var pairs = new[] { ("K", "k"), ("ſ", "s"), ("İ", "i"), ("ı", "I"), ("ẞ", "ß"), ("Ä", "ä"), ("𐐀", "𐐨"), ("Ა", "ა"), ("", ""), ("", "i") };
var hexInputs = new[] { "0x10000", "FFFFFFFF", "80000000", "0x0xFF", "0XFF", "+FF", "+0xFF", " FF ", "-1", "100000000", "", "+", "0000000001", "0x+1", "00xxFF" };
var compatibility = new {
    casing = text.Select(s => new { input = s, lower = s.ToLowerInvariant() }),
    ordinal = pairs.Select(p => new { left = p.Item1, right = p.Item2, equal = string.Equals(p.Item1, p.Item2, StringComparison.OrdinalIgnoreCase) }),
    characters = new[] { '界', 'Ⅷ', '٥', '\u0301', '\u00a0', '\u2007', '\u3000', '\u0085', '\ud801' }.Select(c => new { unit = (int)c, letterOrDigit = char.IsLetterOrDigit(c), whiteSpace = char.IsWhiteSpace(c) }),
    hex = hexInputs.Select(s => { int? value; try { value = MachineProfile.ParseHex(s); } catch { value = null; } return new { input = s, value }; }),
    bits = new[] { -1, 0, 7, 8, 31, 32, 33 }.Select(b => new { bit = b, mask = (int)new BitfieldInfo { Bit = b }.GetMask() }),
    depth = new[] { 63, 64 }.Select(n => {
        string json = "{\"unused\":" + new string('[', n) + "0" + new string(']', n) + "}";
        bool accepted; try { JsonSerializer.Deserialize(json, context.UserProfile); accepted = true; } catch { accepted = false; }
        return new { json, accepted };
    }),
    dates = new[] { "0001-01-01", "2024-02-29", "2025-02-29", "2026-11-03T12:34", "2026-11-03T12:34Z", "2026-11-03T12:34+02", "2026-11-03T12:34:56.1234567890123456Z", "2026-11-03T12:34:56.12345678901234567Z", "0001-01-01T00:00:00+14:00", "9999-12-31T23:59:59-14:00", "0001-01-01T00:00:00Z", "9999-12-31T23:59:59.9999999Z", "2026-11-03T12:34:56+14:01", "2026-11-03t12:34:56z", "2026-11-03T24:00:00Z", "2026-11-03T12:34:56+02" }.Select(s => {
        bool date, offset; string? canonical = null;
        try { var p = JsonSerializer.Deserialize("{\"savedAt\":" + JsonSerializer.Serialize(s) + "}", context.UserProfile)!; date = true; canonical = p.SavedAt.ToString("O"); } catch { date = false; }
        try { JsonSerializer.Deserialize("{\"Time\":" + JsonSerializer.Serialize(s) + "}", context.ThermalTripRecord); offset = true; } catch { offset = false; }
        return new { input = s, date, offset, canonical };
    })
};
File.WriteAllText(Path.Combine(output, "compatibility.json"), JsonSerializer.Serialize(compatibility));
foreach (var encoding in new System.Text.Encoding[] { new System.Text.UTF8Encoding(true), new System.Text.UnicodeEncoding(false, true), new System.Text.UnicodeEncoding(true, true), new System.Text.UTF32Encoding(false, true), new System.Text.UTF32Encoding(true, true) }) {
    string path = Path.Combine(output, "bom-" + encoding.CodePage + ".json");
    File.WriteAllText(path, "{\"name\":\"snow ☃\",\"cpu\":{\"pl1\":80},}", encoding);
    Check(JsonSerializer.Deserialize(File.ReadAllText(path), context.UserProfile)!.Cpu.PL1 == 80, "Original BOM file decoding");
}
foreach (var field in new[] { "cpu", "gpu", "fan", "battery", "lighting", "gpuFailsafe" }) {
    string json = "{\"" + field + "\":null}";
    // The source deserializer preserves null; application preflight refuses these groups.
    var nullable = JsonSerializer.Deserialize(json, context.UserProfile)!;
    Check(field switch { "cpu" => nullable.Cpu == null, "gpu" => nullable.Gpu == null, "fan" => nullable.Fan == null, "battery" => nullable.Battery == null, "lighting" => nullable.Lighting == null, _ => nullable.GpuFailsafe == null }, "Original nullable group");
    File.WriteAllText(Path.Combine(output, "null-" + field + ".json"), json);
}
Console.WriteLine("PASS: original source-generated C# representation reads native fixtures and writes round-trip fixtures.");
var uncertain = ThermalProtectionState.Recover(new(), new() { Stage = ThermalStage.None, Held = ThermalAction.Cpu | ThermalAction.Fan }, false, false);
Console.WriteLine($"Reference corrupt-pending recovery: Held={(int)uncertain.Held}; native deliberately retains known CPU/Fan obligations (6).");

namespace MechControl {
    // Only diagnostic output is substituted. No application/hardware entry point is linked.
    static class DiagnosticLog { public static void Warn(string text) => Console.WriteLine(text); }
}
namespace MechControl.Driver {
    // Link the real enum-owning source without permitting a peripheral operation.
    class BleDevice {
        public Task WriteAsync(byte[] _) => throw new Exception("Unexpected hardware operation");
        public Task<byte[]?> WriteAndReceiveAsync(byte[] _) => throw new Exception("Unexpected hardware operation");
    }
}
