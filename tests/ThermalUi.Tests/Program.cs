using System.Reflection;
using MechControl.Driver;
using MechControl.Features;
using MechControl.Monitoring;
using MechControl.Profile;
using MechControl.UI;

static partial class Program
{
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    private const BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    private static T Field<T>(object instance, string name) => (T)instance.GetType().GetField(name, Private)!.GetValue(instance)!;
    private static void Require(bool condition, string message) { if (!condition) throw new Exception(message); }

    [STAThread]
    private static int Main(string[] args)
    {
        try
        {
            bool native = args.Contains("--native");
            Application.SetHighDpiMode(native ? HighDpiMode.SystemAware : HighDpiMode.DpiUnaware);
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.SetDefaultFont(new Font("Segoe UI", 9));
            MonitorTests();
            LinkedFanTests();
            RefreshTests();
            if (args.Contains("--headless"))
            {
                Console.WriteLine("PASS: shared monitor, linked-fan cadence/read/write counts and failures, hidden/unchanged UI refresh; no windows shown or hardware accessed.");
                return 0;
            }
            string output = args.FirstOrDefault() ?? @"C:\Temp\tandem\thermal-ui-validation";
            Directory.CreateDirectory(output);
            foreach (float scale in native ? new[] { 1f } : new[] { 1f, 1.5f, 2f })
                foreach (string state in new[] { "disabled", "armed", "pending-edits", "applied-unsaved", "tripped", "incomplete", "reset-pending", "unsupported" })
                    Capture(output, state, scale);
            Console.WriteLine("PASS: shared-monitor read counts/failures; UI state/edits/save/test controls; " +
                (native ? "8 states at native system DPI." : "24 layout captures at 100/150/200% simulated scale."));
            Console.WriteLine("Screenshots: " + output);
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }

    private static MachineProfile MonitorProfile()
    {
        var profile = new MachineProfile();
        string[] registers = { "cpuFanDuty", "gpuFanDuty", "cpuFanRpmLo", "cpuFanRpmHi", "gpuFanRpmLo", "gpuFanRpmHi",
            "cpuTempLimitRead", "batterySoc", "batteryMode", "statusByte", "fanCurveEnable", "cpuTemp", "gpuTemp" };
        for (int i = 0; i < registers.Length; i++) profile.Registers[registers[i]] = "0x" + (0x500 + i).ToString("X");
        return profile;
    }

    private static void MonitorTests()
    {
        var profile = MonitorProfile();
        var device = new EcDevice();
        int gpu = profile.GetRegister("gpuTemp");
        device.Registers[gpu] = 40;
        using var monitor = new EcMonitor(device, profile);
        monitor.Start();
        var tick = typeof(EcMonitor).GetMethod("OnTick", Private)!;
        void Tick() => tick.Invoke(monitor, new object?[] { null, EventArgs.Empty });
        foreach (bool tray in new[] { false, true })
        {
            int samples = 0;
            monitor.Lightweight = tray;
            Action<int?> sample = t => { Require(t == 40, "Wrong shared sample"); samples++; };
            Action<int?> broken = _ => throw new Exception("UI subscriber failed");
            monitor.GpuTemperatureSample += broken;
            monitor.GpuTemperatureSample += sample;
            device.Reads.Clear();
            Tick();
            Require(device.Reads.Count(a => a == gpu) == 1, "Duplicate GPU-temperature read");
            Require(device.Reads[0] == gpu, "Safety was not sampled first");
            Require(samples == 1, "Broken subscriber suppressed safety");
            device.ReadOverride = a => a == gpu ? (byte)40 : throw new IOException("Unrelated telemetry failed");
            for (int i = 0; i < 6; i++) Tick();
            Require(samples == 7, "Telemetry failure stopped safety sampling");
            Require(Field<System.Windows.Forms.Timer>(monitor, "_timer").Enabled, "Telemetry failure stopped the shared timer");
            device.ReadOverride = null;
            monitor.GpuTemperatureSample -= broken;
            monitor.GpuTemperatureSample -= sample;
            device.Reads.Clear();
            Tick();
            Require(device.Reads.Count(a => a == gpu) == (tray ? 0 : 1), "Disabled polling overhead");
        }
    }

    private static void Capture(string output, string state, float scale)
    {
        int hardwareCalls = 0;
        var f = new GpuTemperatureFailsafe(() => 35, action =>
        {
            hardwareCalls++;
            if (state == "incomplete" && action == ThermalAction.Cpu)
                throw new IOException("CPU temperature-limit read failed. Current limit cannot be preserved; check EC access. GPU and fan actions continue independently.");
        }, _ => { }, supported: state == "unsupported" ? ThermalAction.None : ThermalAction.All,
            describe: actions => string.Join("; ", new[] { ThermalAction.Gpu, ThermalAction.Cpu, ThermalAction.Fan }.Where(a => (actions & a) != 0).Select(a => a switch
            {
                ThermalAction.Gpu => "GPU TGP 95 W, Dynamic Boost off",
                ThermalAction.Cpu => "CPU PL1/PL2/PL4 15/20/22 W",
                _ => "fan boost on"
            })));
        if (state != "unsupported") f.Configure(new() { Enabled = state != "disabled", ThresholdC = 64, FanBoost = true, ReduceCpuPower = state == "incomplete" });
        using var window = new Form { Text = "MechControl - Thermal Failsafe (hardware-free validation)", ClientSize = new Size(800, 650),
            AutoScaleMode = AutoScaleMode.None, StartPosition = FormStartPosition.Manual, Location = new Point(10, 10) };
        float nativeScale = window.DeviceDpi / 96f;
        if (nativeScale > 1) window.ClientSize = new Size((int)(800 * nativeScale), (int)(580 * nativeScale));
        using var tab = new FailsafeTab(f, state == "unsupported" ? null : 95);
        var pages = new TabControl { Dock = DockStyle.Fill };
        pages.TabPages.Add("Power"); pages.TabPages.Add("Fan Control"); pages.TabPages.Add("Battery");
        pages.TabPages.Add("Lighting"); pages.TabPages.Add("Liquid Cooler");
        var page = new TabPage("Thermal Failsafe"); page.Controls.Add(tab); pages.TabPages.Add(page);
        pages.TabPages.Add("System Info"); pages.SelectedTab = page;
        window.Controls.Add(pages);
        var profiles = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 36, WrapContents = false };
        for (int i = 1; i <= 5; i++) profiles.Controls.Add(new Button { Text = "Profile " + i, Width = (int)(80 * nativeScale), Height = 28,
            BackColor = i == 2 ? SystemColors.Highlight : SystemColors.Control, ForeColor = i == 2 ? Color.White : Color.Black });
        profiles.Controls.Add(new Button { Text = "Save", Width = 65, Height = 28 });
        window.Controls.Add(profiles);
        tab.RefreshConfiguration(saved: true);
        window.Show();
        if (state == "armed") Console.WriteLine($"Capture device DPI: {window.DeviceDpi}; geometry/font scale: {scale}");
        if (state == "armed")
        {
            if (nativeScale > 1)
            {
                var original = window.ClientSize;
                window.ClientSize = new Size(800, original.Height);
                Application.DoEvents();
                var wrapped = Field<CheckBox>(tab, "_window");
                int narrowWidth = wrapped.Width;
                Require(wrapped.Height >= wrapped.Font.Height * 2, "Native-DPI checkbox text was clipped");
                using var narrow = new Bitmap(window.Width, window.Height);
                window.DrawToBitmap(narrow, new Rectangle(Point.Empty, narrow.Size));
                narrow.Save(Path.Combine(output, "armed-native-narrow.png"));
                window.ClientSize = original;
                Application.DoEvents();
                Require(wrapped.Width > narrowWidth + 200, "Checkbox retained narrow bounds after enlarging the window");
            }
            var slider = Field<TrackBar>(tab, "_threshold");
            Require(slider.Focus(), "Slider could not take keyboard focus");
            SendMessage(slider.Handle, 0x100, (IntPtr)0x27, IntPtr.Zero); // Right arrow to this test control only.
            Require(slider.Value == 65, "Slider keyboard step was not one degree");
            SendMessage(slider.Handle, 0x100, (IntPtr)0x25, IntPtr.Zero);
            Require(!tab.HasPendingEdits, "Keyboard value restoration left a spurious edit");
        }
        if (state is "pending-edits" or "applied-unsaved")
        {
            int before = hardwareCalls;
            Field<TrackBar>(tab, "_threshold").Value = 70;
            Require(tab.HasPendingEdits, "Pending edits missing");
            f.OnTemperatureSample(36);
            Require(tab.HasPendingEdits && Field<TrackBar>(tab, "_threshold").Value == 70, "Telemetry overwrote edits");
            Require(hardwareCalls == before, "An edit/refresh wrote hardware");
            Require(tab.ReadSettings().ThresholdC == 64, "Save read unapplied controls");
            tab.MarkSaved();
            Require(tab.HasPendingEdits, "Save silently applied pending edits");
            if (state == "applied-unsaved") { f.Configure(new() { Enabled = true, ThresholdC = 70 }); tab.RefreshConfiguration(); }
        }
        if (state is "tripped" or "incomplete" or "reset-pending")
        {
            f.TestTrip();
            Require(!Field<Button>(tab, "_test").Enabled, "Test enabled during trip");
            Require(Field<Button>(tab, "_reset").Enabled, "Reset unavailable during trip");
            if (state == "reset-pending") f.Reset();
            Require(!Field<Button>(tab, "_test").Enabled, "Test enabled during restoration hold");
        }
        if (state == "unsupported") Require(!Field<CheckBox>(tab, "_enabled").Enabled, "Unsupported protection enabled");
        Require(Field<TrackBar>(tab, "_threshold").SmallChange == 1 && Field<TrackBar>(tab, "_threshold").TabStop, "Slider keyboard navigation unavailable");

        // Simulated layout scale, not a claim to change Windows monitor DPI.
        // Native per-monitor DPI switching must be checked on real displays.
        var fonts = new List<Font>();
        // Explicit fonts used by the actual tab must scale along with geometry.
        if (scale != 1)
        {
            var originalFonts = Descendants(window).ToDictionary(c => c, c => c.Font);
            foreach (var pair in originalFonts)
            {
                var font = new Font(pair.Value.FontFamily, pair.Value.Size * scale, pair.Value.Style);
                fonts.Add(font); pair.Key.Font = font;
            }
            window.Scale(new SizeF(scale, scale));
        }
        window.PerformLayout();
        Application.DoEvents();
        using var bitmap = new Bitmap(window.Width, window.Height);
        window.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
        bitmap.Save(Path.Combine(output, $"{state}-{scale * 100:0}.png"));
        var stack = (TableLayoutPanel)tab.Controls[0];
        stack.ScrollControlIntoView(Field<Button>(tab, "_reset"));
        Application.DoEvents();
        using var statusBitmap = new Bitmap(window.Width, window.Height);
        window.DrawToBitmap(statusBitmap, new Rectangle(Point.Empty, statusBitmap.Size));
        statusBitmap.Save(Path.Combine(output, $"{state}-{scale * 100:0}-status.png"));
        window.Close();
        foreach (var font in fonts) font.Dispose();
    }

    private static IEnumerable<Control> Descendants(Control root)
    {
        yield return root;
        foreach (Control child in root.Controls)
            foreach (var descendant in Descendants(child)) yield return descendant;
    }
}
