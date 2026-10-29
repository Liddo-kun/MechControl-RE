using System.Diagnostics;

namespace MechControl.Hardware.Nvidia;

static class NvidiaDeviceRestart
{
    public static void Restart(string instanceId)
    {
        var start = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "pnputil.exe"))
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        start.ArgumentList.Add("/restart-device");
        start.ArgumentList.Add(instanceId);
        using var process = Process.Start(start) ?? throw new IOException("Could not start the GPU restart.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(60000))
            throw new IOException("GPU restart did not finish within 60 seconds. Restart Windows before applying NVIDIA limits.");
        if (process.ExitCode != 0)
            throw new IOException($"GPU restart returned {process.ExitCode}. Restart Windows to activate the staged limits. {output.GetAwaiter().GetResult()} {error.GetAwaiter().GetResult()}");
    }
}
