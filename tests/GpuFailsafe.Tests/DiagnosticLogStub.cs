namespace MechControl;

static class DiagnosticLog
{
    public static readonly List<string> Messages = new();

    public static void Write(string message) => Messages.Add(message);
    public static void Warn(string message) => Messages.Add(message);
    public static void Error(string message) => Messages.Add(message);
    public static void Reset() => Messages.Clear();
}
