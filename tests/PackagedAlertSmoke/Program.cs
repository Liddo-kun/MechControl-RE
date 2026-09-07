using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;

internal static class Program
{
    [STAThread]
    private static int Main()
    {
        string root = AppContext.BaseDirectory;
        AssemblyLoadContext.Default.Resolving += (_, name) =>
        {
            string path = Path.Combine(root, "libs", name.Name + ".dll");
            return File.Exists(path) ? AssemblyLoadContext.Default.LoadFromAssemblyPath(path) : null;
        };
        try
        {
            Test(root);
            File.WriteAllText(Path.Combine(root, "smoke-result.txt"), "PASS: packaged failsafe sound and nonmodal alert; message loop active; no hardware access.");
            return 0;
        }
        catch (Exception ex)
        {
            File.WriteAllText(Path.Combine(root, "smoke-result.txt"), "FAIL: " + ex);
            return 1;
        }
    }

    // As in the app entry point, defer WinForms resolution until after the libs/
    // resolver is installed. Only the package's runtime and DLLs are available.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Test(string root)
    {
        var assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(root, "MechControl.target.dll"));
        var type = assembly.GetType("MechControl.UI.MainForm", throwOnError: true)!;
        // Never run MainForm's constructor: it initializes the EC and writes
        // hardware. This narrow test exercises only ShowFailsafeAlert and its UI.
        var owner = RuntimeHelpers.GetUninitializedObject(type);
        GC.SuppressFinalize(owner);
        const BindingFlags instance = BindingFlags.Instance | BindingFlags.NonPublic;
        using var tray = new NotifyIcon { Icon = SystemIcons.Application, Visible = true, Text = "MechControl alert smoke test" };
        type.GetField("_trayIcon", instance)!.SetValue(owner, tray);
        var show = type.GetMethod("ShowFailsafeAlert", instance)!;
        var failsafeType = assembly.GetType("MechControl.Features.GpuTemperatureFailsafe", true)!;
        var settingsType = assembly.GetType("MechControl.Profile.GpuFailsafeSettings", true)!;
        var actionType = assembly.GetType("MechControl.Profile.ThermalAction", true)!;
        var stateType = assembly.GetType("MechControl.Profile.ThermalProtectionState", true)!;
        Delegate NoOpFor(Type valueType) => typeof(Program).GetMethod(nameof(NoOp), BindingFlags.Static | BindingFlags.NonPublic)!
            .MakeGenericMethod(valueType).CreateDelegate(typeof(Action<>).MakeGenericType(valueType));
        var failsafe = Activator.CreateInstance(failsafeType, new object?[]
        {
            (Func<int>)(() => 35), NoOpFor(actionType), NoOpFor(stateType), null, Enum.ToObject(actionType, 1), null, null
        })!;
        type.GetField("_gpuFailsafe", instance)!.SetValue(owner, failsafe);
        int activations = 0;
        type.GetField("_activateFailsafeWindow", instance)!.SetValue(owner, (Action)(() => activations++));
        Form? alert = null;
        try
        {
            for (int i = 0; i < 2; i++)
            {
                var settings = Activator.CreateInstance(settingsType)!;
                settingsType.GetProperty("ShowMainWindow")!.SetValue(settings, i == 1);
                failsafeType.GetMethod("Configure")!.Invoke(failsafe, new[] { settings });
                show.Invoke(owner, new object[] { "Packaging smoke test only. No GPU power settings have been changed." });
                if (activations != i) throw new Exception("Foreground preference did not exclusively control main-window activation.");
                if (alert != null && !alert.IsDisposed) throw new Exception("Previous warning window was not closed.");
                alert = (Form)type.GetField("_failsafeAlert", instance)!.GetValue(owner)!;
                if (!alert.Visible || !alert.TopMost || alert.Modal) throw new Exception("Warning window is not visible, topmost and nonmodal.");
                if (!alert.Controls.OfType<TextBox>().Any(t => t.Text.Contains("Packaging smoke test")))
                    throw new Exception("Warning message missing.");
                bool ticked = false;
                using var timer = new System.Windows.Forms.Timer { Interval = 30 };
                timer.Tick += (_, _) => { ticked = true; alert.Close(); };
                timer.Start();
                Application.Run(alert);
                if (!ticked) throw new Exception("Alert blocked the message loop.");
            }
        }
        finally
        {
            alert?.Dispose();
            tray.Visible = false;
        }
    }

    private static void NoOp<T>(T value) { }
}
