namespace MechControl;

static class DiagnosticLog
{
    public static void Write(string message) => Console.Error.WriteLine(message);
}
