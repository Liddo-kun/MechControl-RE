using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        string root = AppContext.BaseDirectory;
        AssemblyLoadContext.Default.Resolving += (_, name) =>
        {
            string path = Path.Combine(root, "libs", name.Name + ".dll");
            return File.Exists(path) ? AssemblyLoadContext.Default.LoadFromAssemblyPath(path) : null;
        };
        try
        {
            bool headless = Array.IndexOf(args, "--headless") >= 0;
            if (headless) TestProfiles(root);
            else Test(root);
            File.WriteAllText(Path.Combine(root, "smoke-result.txt"), headless
                ? "PASS: packaged settings/profile recovery, migration, atomic saves and shared warning/hold indicators; no windows, sound or hardware access."
                : "PASS: packaged failsafe sound and nonmodal alert; message loop active; no hardware access.");
            return 0;
        }
        catch (Exception ex)
        {
            File.WriteAllText(Path.Combine(root, "smoke-result.txt"), "FAIL: " + ex);
            return 1;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void TestProfiles(string root)
    {
        var assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(root, "MechControl.target.dll"));
        var managerType = assembly.GetType("MechControl.Profile.UserProfileManager", true)!;
        string directory = Path.Combine(root, "serialization-fixtures");
        Directory.CreateDirectory(directory);
        string settingsPath = Path.Combine(directory, "settings.json");
        object Manager() => Activator.CreateInstance(managerType, new object[] { directory, true })!;
        object? Get(object obj, string name) => obj.GetType().GetProperty(name)!.GetValue(obj);
        foreach (string json in new[]
        {
            """{"thermalProtection":{"Stage":0,"Held":0,"LastTrip":{"Time":"bad","Reason":"Old history"}}}""",
            """{"lastProfile":"bad","thermalProtection":{"Stage":0,"Held":0}}"""
        })
        {
            File.WriteAllText(settingsPath, json);
            var state = Get(Manager(), "ThermalProtection")!;
            if (Convert.ToInt32(Get(state, "Held")) != 0) throw new Exception("Optional corruption manufactured a packaged hold.");
        }
        File.WriteAllText(settingsPath, """{"thermalProtection":{"Stage":2,"Held":7,"ResetThresholdC":30,"LastTrip":{"Time":"bad","ThresholdC":30,"Reason":"Original event"}}}""");
        var manager = Manager();
        var held = Get(manager, "ThermalProtection")!;
        if (Convert.ToInt32(Get(held, "Held")) != 7 || Convert.ToInt32(Get(held, "Stage")) != 2)
            throw new Exception("Packaged recovery lost holds.");
        managerType.GetMethod("SaveThermalProtection")!.Invoke(manager, new[] { held });
        var history = Get(Get(Manager(), "ThermalProtection")!, "LastTrip")!;
        if (Get(history, "Time") != null || Convert.ToInt32(Get(history, "ThresholdC")) != 30)
            throw new Exception("Packaged round trip fabricated history.");
        File.WriteAllText(Path.Combine(directory, "userprofile_1.json"), """{"gpuFailsafe":{"enabled":false,"thresholdC":35,"fanBoost":true},"gpu":{"tgp":180,"biosOverrideEnabled":true,"nvidiaAdapter":"test-gpu-bios","nvvddMilliamps":211500,"msvddMilliamps":61000}}""");
        var profile = managerType.GetMethod("LoadProfile")!.Invoke(manager, new object[] { 1 })!;
        var protection = Get(profile, "GpuFailsafe")!;
        if ((bool)Get(protection, "Enabled")! || !(bool)Get(protection, "FanBoost")! || Convert.ToInt32(Get(protection, "ThresholdC")) != 50)
            throw new Exception("Packaged profile migration changed detection/options.");
        managerType.GetMethod("SaveProfile")!.Invoke(manager, new[] { (object)1, profile });
        profile = managerType.GetMethod("LoadProfile")!.Invoke(Manager(), new object[] { 1 })!;
        var gpu = Get(profile, "Gpu")!;
        // The legacy "nvidiaAdapter" key above must be ignored without failing the load.
        if (!(bool)Get(gpu, "BiosOverrideEnabled")! ||
            Convert.ToInt32(Get(gpu, "NvvddMilliamps")) != 211500 || Convert.ToInt32(Get(gpu, "MsvddMilliamps")) != 61000)
            throw new Exception("Packaged serialization lost NVIDIA override/current settings.");
        if ((bool)Get(profile, "RequiresSave")! || File.Exists(settingsPath + ".tmp"))
            throw new Exception("Packaged atomic save did not commit migrated profile/settings.");
        TestIndicators(assembly, held);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void TestIndicators(Assembly assembly, object held)
    {
        const BindingFlags fields = BindingFlags.Instance | BindingFlags.NonPublic;
        var mainType = assembly.GetType("MechControl.UI.MainForm", true)!;
        var failsafeType = assembly.GetType("MechControl.Features.GpuTemperatureFailsafe", true)!;
        var actionType = assembly.GetType("MechControl.Profile.ThermalAction", true)!;
        var tabType = assembly.GetType("MechControl.UI.FailsafeTab", true)!;
        var barType = assembly.GetType("MechControl.UI.ProfileBar", true)!;
        for (int i = 0; i < 4; i++)
        {
            bool hasHold = i % 2 != 0;
            string startupWarning = i >= 2 ? "Saved protection state uncertain" : "";
            // No MainForm/ProfileBar constructor, shown window or visible tray icon.
            var owner = RuntimeHelpers.GetUninitializedObject(mainType);
            var bar = RuntimeHelpers.GetUninitializedObject(barType);
            GC.SuppressFinalize(owner);
            GC.SuppressFinalize(bar);
            var failsafe = Activator.CreateInstance(failsafeType, new object?[]
            {
                (Func<int>)(() => throw new Exception("Unexpected sensor access")), NoOpFor(actionType), NoOpFor(held.GetType()),
                hasHold ? held : null, Enum.ToObject(actionType, 0), null, null
            })!;
            using var tab = (Control)Activator.CreateInstance(tabType, new object[] { failsafe, startupWarning })!;
            using var page = new TabPage();
            using var tray = new NotifyIcon();
            mainType.GetField("_gpuFailsafe", fields)!.SetValue(owner, failsafe);
            mainType.GetField("_failsafeTab", fields)!.SetValue(owner, tab);
            mainType.GetField("_failsafePage", fields)!.SetValue(owner, page);
            mainType.GetField("_profileBar", fields)!.SetValue(owner, bar);
            mainType.GetField("_defaultTrayIcon", fields)!.SetValue(owner, SystemIcons.Application);
            var update = (Action)mainType.GetMethod("UpdateTrayIcon", fields)!.CreateDelegate(typeof(Action), owner);
            tabType.GetEvent("ConfigurationWarningChanged")!.AddEventHandler(tab, update);
            var warn = tabType.GetMethod("ShowConfigurationWarning")!;
            update(); // MainForm initializes indicators before constructing the tray.
            if (startupWarning.Length > 0 && !page.Text.Contains(hasHold ? "(!)" : "configuration"))
                throw new Exception("Initial startup warning/hold title missing.");
            warn.Invoke(tab, new object[] { "Requested profile failed" }); // Tray not constructed yet.
            if (!page.Text.Contains(hasHold ? "(!)" : "configuration")) throw new Exception("Early warning/hold title not updated.");
            mainType.GetField("_trayIcon", fields)!.SetValue(owner, tray);
            update();
            if (!tray.Text.Contains(hasHold ? "THERMAL PROTECTION HOLD" : "CONFIGURATION WARNING"))
                throw new Exception("Warning indicator or hold priority lost.");
            warn.Invoke(tab, new object[] { "" });
            if (page.Text != (hasHold ? "Thermal Failsafe (!)" : startupWarning.Length > 0 ? "Thermal Failsafe (configuration !)" : "Thermal Failsafe") ||
                tray.Text != (hasHold ? "MechControl - THERMAL PROTECTION HOLD" : startupWarning.Length > 0 ? "MechControl - CONFIGURATION WARNING" : "MechControl"))
                throw new Exception("Resolved warning was not cleared or incorrectly cleared the thermal hold indicator.");
            if (tray.Icon!.Handle != (hasHold ? SystemIcons.Error : startupWarning.Length > 0 ? SystemIcons.Warning : SystemIcons.Application).Handle)
                throw new Exception("Resolved warning left the wrong tray icon.");
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

    private static Delegate NoOpFor(Type valueType) => typeof(Program).GetMethod(nameof(NoOp), BindingFlags.Static | BindingFlags.NonPublic)!
        .MakeGenericMethod(valueType).CreateDelegate(typeof(Action<>).MakeGenericType(valueType));
}
