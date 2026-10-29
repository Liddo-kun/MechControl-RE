namespace MechControl;

static class DiagnosticLog
{
    public static readonly List<string> Messages = new();

    public static void Write(string message) => Messages.Add(message);
    public static void Warn(string message) => Messages.Add(message);
    public static void Error(string message) => Messages.Add(message);
    public static void Section(string header) => Messages.Add($"== {header}");
    public static void LogBitfieldValidation(string name, byte rawValue, byte maskedValue, bool valid) =>
        Messages.Add($"{name} 0x{rawValue:X2}/0x{maskedValue:X2} {(valid ? "PASS" : "FAIL")}");
    public static void LogFeatureValidation(string name, byte rawValue, int decodedValue, bool valid, string reason = "") =>
        Messages.Add($"{name} 0x{rawValue:X2} -> {decodedValue} {(valid ? "PASS" : "FAIL")} {reason}");
    public static void Reset() => Messages.Clear();
}
