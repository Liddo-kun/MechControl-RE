using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using MechControl.Driver;
using MechControl.Features;
using MechControl.Profile;
using MechControl.UI;

namespace MechControl;

static class Program
{
    [STAThread]
    static void Main()
    {
        // Register assembly resolver for self-contained builds that keep DLLs in libs/
        var libsDir = Path.Combine(AppContext.BaseDirectory, "libs");
        if (Directory.Exists(libsDir))
        {
            AssemblyLoadContext.Default.Resolving += (_, name) =>
            {
                var path = Path.Combine(libsDir, $"{name.Name}.dll");
                return File.Exists(path) ? AssemblyLoadContext.Default.LoadFromAssemblyPath(path) : null;
            };
        }

        AppMain();
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void AppMain()
    {
        // Single-instance guard. A second instance (e.g. the Scheduled Task
        // firing while one is already running) would throw in
        // DiagnosticLog.Start() on the locked log file and fight over the EC,
        // so exit quietly before touching anything.
        using var instanceMutex = new Mutex(initiallyOwned: true, @"Global\MechControl", out bool isFirstInstance);
        if (!isFirstInstance)
            return;

        // Global exception handlers — the app usually runs tray-hidden, so an
        // unhandled UI-event exception would otherwise kill it invisibly.
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
        DateTime lastCrashDialogUtc = DateTime.MinValue;
        Application.ThreadException += (_, e) =>
        {
            DiagnosticLog.Error($"Unhandled UI exception: {e.Exception}");
            // Rate-limit the dialog: a fault repeating from a timer tick must
            // not stack modal dialogs. Every occurrence is still logged above.
            if ((DateTime.UtcNow - lastCrashDialogUtc).TotalMinutes < 5)
                return;
            lastCrashDialogUtc = DateTime.UtcNow;
            MessageBox.Show(
                $"An unexpected error occurred:\n\n{e.Exception.Message}\n\nMechControl will keep running. See mechcontrol.log for details.",
                "MechControl \u2014 Unexpected Error",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        };
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            DiagnosticLog.Error($"Fatal unhandled exception (terminating={e.IsTerminating}): {e.ExceptionObject}");
        };

        ApplicationConfiguration.Initialize();

        // Start diagnostic logging
        DiagnosticLog.Start();

        // One-time UEFI NVRAM setup (no-op if flags already correct)
        UefiSetup.EnsureFlags();

        // Check for OEM services
        CheckOemConflict();

        // Open EC device
        EcDevice device;
        try
        {
            device = new EcDevice();
            DiagnosticLog.Write("EC device opened.");
        }
        catch (Exception ex)
        {
            DiagnosticLog.Error($"Failed to open EC driver: {ex.Message}");
            MessageBox.Show(
                $"Failed to open EC driver:\n\n{ex.Message}\n\nMake sure UWACPIDriver is installed and you are running as Administrator.",
                "MechControl \u2014 Driver Error",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            DiagnosticLog.Close();
            return;
        }

        // Detect and load profile (universal defaults applied for unverified)
        MachineProfile? profile;
        try
        {
            profile = ProfileLoader.DetectAndLoad(device);
        }
        catch (Exception ex)
        {
            device.Dispose();
            DiagnosticLog.Error($"Failed to load profile: {ex.Message}");
            MessageBox.Show(
                $"Failed to load profile:\n\n{ex.Message}",
                "MechControl \u2014 Profile Error",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            DiagnosticLog.Close();
            return;
        }

        if (profile == null)
        {
            device.Dispose();
            DiagnosticLog.Write("User cancelled profile selection.");
            DiagnosticLog.Close();
            return;
        }

        DiagnosticLog.Write($"Profile loaded: {profile.ProjectId} (verified={profile.Verified})");

        // Initialize WriteGuard from profile (now has merged universal defaults)
        var guard = new WriteGuard(device, profile);

        // Launch main form (form takes ownership of device disposal)
        Application.Run(new MainForm(device, guard, profile));
    }

    private static void CheckOemConflict()
    {
        var conflicts = new List<string>();
        foreach (string name in new[] { "GCUService", "GCUBridge" })
        {
            try
            {
                var procs = Process.GetProcessesByName(name);
                if (procs.Length > 0)
                    conflicts.Add($"{name} (PID {procs[0].Id})");
                foreach (var p in procs) p.Dispose();
            }
            catch { }
        }

        if (conflicts.Count > 0)
        {
            string msg = "The following OEM processes are running and may overwrite your settings:\n\n"
                + string.Join("\n", conflicts)
                + "\n\nConsider stopping them before using MechControl.";
            MessageBox.Show(msg, "MechControl — OEM Conflict Warning",
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }
}
