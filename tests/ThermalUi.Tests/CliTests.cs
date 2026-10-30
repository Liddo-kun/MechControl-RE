using System.IO.Pipes;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using MechControl;
using MechControl.Cli;
using MechControl.Driver;
using MechControl.Features;
using MechControl.Monitoring;
using MechControl.Profile;

static partial class Program
{
    private sealed record CliRig(CliCommands Cli, EcDevice Ec, UiGpuPower Driver, GpuPowerControl Gpu, GpuTemperatureFailsafe Failsafe)
    {
        public int PowerApplied, FanApplied;
    }

    // The Power tab tests' alternate model (TGP 80-125 W at offset 70; unlocked 48-200 W) plus CPU and fan boost.
    private static CliRig CreateCliRig(bool cpu = true)
    {
        var model = new MachineProfile();
        foreach (var (name, address) in new[] { ("PL1", "0x783"), ("PL2", "0x784"), ("PL4", "0x785"), ("cpuTempLimitWrite", "0x786"),
            ("cpuTempLimitRead", "0x463"), ("tgpDbCtrl", "0x743"), ("tgp", "0x744"), ("dbTotalPower", "0x745"), ("dbMaxTgp", "0x746"),
            ("fanControlMode", "0x751"), ("fanCurveEnable", "0x7C5") })
            model.Registers[name] = address;
        model.FanTable = new() { CpuTempUpBase = "0xF00", CpuTempDownBase = "0xF10", CpuDutyBase = "0xF20",
            GpuTempUpBase = "0xF30", GpuTempDownBase = "0xF40", GpuDutyBase = "0xF50" };
        model.Limits["TGP"] = new() { Min = 80, Max = 125, Offset = 70, Encoding = "offset" };
        model.Limits["PL1"] = new() { Min = 15, Max = 140, Encoding = "direct" };
        model.Limits["PL2"] = new() { Min = 20, Max = 180, Encoding = "direct" };
        model.Limits["PL4"] = new() { Min = 22, Max = 300, Encoding = "direct" };
        model.Limits["cpuTempLimit"] = new() { Min = 80, Max = 95, Encoding = "inverted_offset", TjMax = 105 };
        UniversalDefaults.ApplyToProfile(model);
        var ec = new EcDevice();
        ec.Registers[0x744] = 30; // 100 W
        ec.Registers[0x783] = 45; ec.Registers[0x784] = 65; ec.Registers[0x785] = 90;
        ec.Registers[0x463] = 15; // 90 C
        ec.WriteOverride = (a, b) => { if (a == 0x786) ec.Registers[0x463] = b; };
        var guard = new WriteGuard(ec, model);
        var driver = new UiGpuPower();
        var gpu = new GpuPowerControl(ec, guard, model) { PowerOverride = driver };
        var cpuControl = new CpuPowerControl(ec, guard, model);
        var fan = new FanControl(ec, guard, model);
        var failsafe = new GpuTemperatureFailsafe(() => 40, _ => { }, _ => { }, supported: ThermalAction.All);
        gpu.Failsafe = cpuControl.Failsafe = fan.Failsafe = failsafe;
        var monitor = new MonitorData { CpuFanRpm = 2740, GpuFanRpm = 2700, CpuFanPercent = 50, GpuFanPercent = 49, CpuTemp = 65, GpuTemp = 60, BatteryModeName = "" };
        var cli = new CliCommands("TEST_MODEL", cpu ? cpuControl : null, gpu, fan, failsafe, () => monitor);
        var rig = new CliRig(cli, ec, driver, gpu, failsafe);
        cli.PowerApplied += () => rig.PowerApplied++;
        cli.FanApplied += () => rig.FanApplied++;
        return rig;
    }

    private static void CliTests()
    {
        DiagnosticLog.Reset();
        var rig = CreateCliRig();
        var (cli, ec, driver) = (rig.Cli, rig.Ec, rig.Driver);
        CliResult Run(params string[] args) => cli.Execute(args);
        void Expect(CliResult result, int code, string text, string message) =>
            Require(result.ExitCode == code && result.Output.Contains(text), $"{message}: got {result.ExitCode} \"{result.Output}\"");

        // Status: settings with the ranges commands accept, NVIDIA currents in exact milliamps.
        var status = Run("status", "--json");
        Require(status.ExitCode == 0 && !status.Output.Contains('\n'), "JSON status is not one line");
        using (var doc = JsonDocument.Parse(status.Output))
        {
            var root = doc.RootElement;
            var g = root.GetProperty("gpu");
            Require(root.GetProperty("model").GetString() == "TEST_MODEL" && !root.GetProperty("failsafe").GetProperty("tripped").GetBoolean(), "Status header");
            Require(g.GetProperty("tgp").GetProperty("value").GetInt32() == 100 && g.GetProperty("tgp").GetProperty("min").GetInt32() == 48 &&
                g.GetProperty("tgp").GetProperty("max").GetInt32() == 200 && g.GetProperty("unlocked").GetBoolean(), "Unlocked TGP range");
            // Above the live ceiling (220 A) needs a GPU restart, so it is not offered even though the policy allows 252 A.
            var core = g.GetProperty("nvvdd");
            Require(core.GetProperty("limitMa").GetInt32() == 187500 && core.GetProperty("minMa").GetInt32() == 1000 &&
                core.GetProperty("maxMa").GetInt32() == 220000 && core.GetProperty("stockMa").GetInt32() == 180000 &&
                core.GetProperty("measuredMa").GetInt32() == 126883 && core.GetProperty("atClockCapMa").GetInt32() == 152544, "NVVDD readings");
            var input = g.GetProperty("boardInput");
            Require(input.GetProperty("maxMa").GetInt32() == 11053 && input.GetProperty("atClockCapMa").ValueKind == JsonValueKind.Null, "Board input readings");
            Require(g.GetProperty("nvidia").ValueKind == JsonValueKind.Null && g.GetProperty("currentCapMhz").GetInt32() == 2865, "NVIDIA readback");
            var c = root.GetProperty("cpu");
            Require(c.GetProperty("pl1").GetProperty("value").GetInt32() == 45 && c.GetProperty("pl1").GetProperty("max").GetInt32() == 140 &&
                c.GetProperty("tempLimit").GetProperty("value").GetInt32() == 90, "CPU readings");
            var f = root.GetProperty("fans");
            Require(!f.GetProperty("boost").GetBoolean() && f.GetProperty("cpuRpm").GetInt32() == 2740 && f.GetProperty("gpuTempC").GetInt32() == 60, "Fan readings");
        }
        var text = Run("status").Output;
        Require(text.Contains("TGP 100 W (48-200 W)") && text.Contains("NVVDD limit 187.5 A (1-220 A, stock 180 A); measured 126.883 A; at clock cap 152.544 A") &&
            text.Contains("Board input limit 11.053 A (1-11.053 A, stock 11.053 A); measured 8.47 A" + Environment.NewLine) &&
            text.Contains("PL1 45 W (15-140 W)") && text.Contains("boost off; CPU 2740 RPM, 50%, 65 C") && text.All(ch => ch < 128), "Text status");
        Require(ec.Writes.Count == 0 && driver.Configurations == 0 && DiagnosticLog.Messages.Count == 0, "Status wrote or logged");

        // Currents: amps or milliamps, applied live through the Power tab's path; nothing else written.
        Expect(Run("gpu", "--nvvdd", "186.5", "--board-input", "11000mA"), 0, "NVVDD limit 186.5 A", "Live current change");
        Require(driver.Core == 186500 && driver.Input == 11000 && driver.Fabric == 57000 && driver.Configurations == 1 && driver.Restarts == 0 &&
            ec.Writes.Count == 0 && rig.PowerApplied == 1, "Currents were not applied alone");
        Require(DiagnosticLog.Messages.Contains("[CLI] Applied: mcctl gpu --nvvdd 186.5 --board-input 11000mA"), "Write not logged");
        Expect(Run("gpu", "--board-input", "12A"), 1, "needs a GPU restart", "Raising a live ceiling was not refused");
        Expect(Run("gpu", "--msvdd", "0.5"), 1, "outside the range 1-65 A", "Below 1 A was accepted");
        Require(driver.Configurations == 1 && driver.Input == 11000, "A refused current was written");

        // Power: EC registers through WriteGuard, mirrored to the unlocked driver request.
        Expect(Run("gpu", "--tgp", "120", "--db", "20", "--json"), 0, "\"tgp\":{\"value\":120", "TGP change");
        Require(ec.Registers[0x744] == 50 && ec.Registers[0x746] == 20 && (ec.Registers[0x743] & 0x07) == 0x07 && driver.Requested == 140, "TGP/DB writes");
        Expect(Run("gpu", "--db", "off"), 0, "Dynamic Boost off", "DB off");
        Require(ec.Registers[0x744] == 50 && ec.Registers[0x746] == 0 && driver.Requested == 120, "DB off kept TGP");
        ec.Writes.Clear();
        Expect(Run("gpu", "--tgp", "201"), 1, "48-200 W", "Out-of-range TGP");
        Require(ec.Writes.Count == 0, "Refused TGP wrote");

        // Usage errors never touch hardware.
        foreach (var args in new[] { new[] { "gpu" }, new[] { "gpu", "--nvvdd" }, new[] { "gpu", "--nvvdd", "10.0005" }, new[] { "gpu", "--nvvdd", "abc" },
            new[] { "gpu", "--tgp", "-5" }, new[] { "gpu", "--volts", "1" }, new[] { "gpu", "--tgp", "100", "--tgp", "110" }, new[] { "status", "now" },
            new[] { "fan", "boost", "maybe" }, new[] { "cpu", "--pl1", "1.5" }, new[] { "frobnicate" }, Array.Empty<string>() })
            Require(Run(args).ExitCode == CliResult.UsageError, $"Not a usage error: {string.Join(' ', args)}");
        Require(ec.Writes.Count == 0 && driver.Configurations == 1, "A usage error wrote");

        // CPU: unspecified values keep the EC's; the doubled/encoded write goes through the guard.
        Expect(Run("cpu", "--pl1", "100", "--temp-limit", "85"), 0, "PL1 100 W", "CPU change");
        Require(ec.Registers[0x783] == 100 && ec.Registers[0x784] == 65 && ec.Registers[0x785] == 90 && Run("status").Output.Contains("temp limit 85 C"), "CPU writes");
        Expect(Run("cpu", "--pl2", "181"), 1, "20-180", "Out-of-range PL2");

        // Fan boost only flips 0x751 bit 6.
        ec.Registers[0x751] = 0x90;
        Expect(Run("fan", "boost", "on"), 0, "boost on", "Boost on");
        Require(ec.Registers[0x751] == 0xD0 && rig.FanApplied == 1, "Boost write");
        Expect(Run("fan", "boost", "off", "--json"), 0, "\"boost\":false", "Boost off");
        Require(ec.Registers[0x751] == 0x90, "Boost clear");

        // Tripped failsafe: only boost on is accepted (as on the Fan tab).
        rig.Failsafe.Configure(new() { FanBoost = true });
        rig.Failsafe.TestTrip();
        Require(rig.Failsafe.Holds(ThermalAction.Fan), "Test trip did not hold fan boost");
        int configurations = driver.Configurations;
        Expect(Run("gpu", "--nvvdd", "180"), 1, "Thermal failsafe is tripped", "Currents while tripped");
        Expect(Run("cpu", "--pl1", "50"), 1, "Thermal failsafe is tripped", "CPU while tripped");
        Expect(Run("fan", "boost", "off"), 1, "held on", "Boost off while held");
        Expect(Run("fan", "boost", "on"), 0, "boost on", "Boost on while tripped");
        Require(driver.Configurations == configurations && Run("status", "--json").Output.Contains("\"tripped\":true"), "Tripped writes or status");

        // Unopened NVIDIA (stock user): current commands refused without opening it; a sleeping GPU is not read.
        var stock = CreateCliRig(cpu: false);
        bool opened = false;
        stock.Gpu.PowerOverride = null;
        stock.Gpu.OpenPowerOverride = () => { opened = true; throw new Exception("opened"); };
        Expect(stock.Cli.Execute(["gpu", "--nvvdd", "100"]), 1, "turn on the unlock", "Currents without NVIDIA");
        Expect(stock.Cli.Execute(["status"]), 0, "NVIDIA: NVIDIA not connected", "Status without NVIDIA");
        Require(!opened && stock.Cli.Execute(["status", "--json"]).Output.Contains("\"cpu\":null"), "NVIDIA opened or CPU reported");
        Expect(stock.Cli.Execute(["cpu", "--pl1", "50"]), 1, "unavailable on this machine", "Missing CPU control");
        var asleep = CreateCliRig();
        asleep.Driver.Awake = false;
        Expect(asleep.Cli.Execute(["status"]), 0, "NVIDIA: GPU asleep, not read", "Sleeping GPU");
        Require(asleep.Driver.TelemetryReads == 0, "A sleeping GPU was read");

        CliPipeTests();
    }

    private static void CliPipeTests()
    {
        string name = "MechControl-test-" + Guid.NewGuid().ToString("N");
        using var server = new CliServer(args => new(args.Length == 0 ? 2 : 0, string.Join("|", args)), new SynchronizationContext(), name);
        server.Start();
        (int Code, string Text) Send(int version, params string[] args)
        {
            using var pipe = new NamedPipeClientStream(".", name, PipeDirection.InOut);
            pipe.Connect(5000);
            byte[] payload = Encoding.UTF8.GetBytes(string.Join("\0", args));
            pipe.Write([.. BitConverter.GetBytes(version), .. BitConverter.GetBytes(payload.Length), .. payload]); // One write, as mcctl.
            var reader = new BinaryReader(pipe);
            int code = reader.ReadInt32();
            return (code, Encoding.UTF8.GetString(reader.ReadBytes(reader.ReadInt32())));
        }
        bool elevated = new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator);
        if (!elevated)
        {
            // Administrators only: a non-elevated caller is refused by the pipe's DACL.
            bool denied = false;
            try { Send(CliServer.Version, "status"); } catch (UnauthorizedAccessException) { denied = true; }
            Require(denied, "A non-elevated client reached the pipe");
            return;
        }
        Require(Send(CliServer.Version, "gpu", "--board-input", "10.5A") == (0, "gpu|--board-input|10.5A"), "Pipe round trip");
        Require(Send(CliServer.Version, "fan", "boost", "on") == (0, "fan|boost|on"), "Second request on the same pipe instance");
        Require(Send(CliServer.Version) == (2, ""), "Empty request");
        var mismatch = Send(99, "status");
        Require(mismatch.Code == CliResult.UsageError && mismatch.Text.Contains("protocol 99"), "Protocol mismatch");

        // A pipe someone else created under the name is never joined.
        string squatted = "MechControl-test-" + Guid.NewGuid().ToString("N");
        using var squatter = new NamedPipeServerStream(squatted, PipeDirection.InOut, NamedPipeServerStream.MaxAllowedServerInstances);
        using var blocked = new CliServer(_ => new(0, "joined"), new SynchronizationContext(), squatted);
        blocked.Start();
        for (int i = 0; i < 50 && !DiagnosticLog.Messages.ToArray().Any(m => m.StartsWith("[CLI] Command-line pipe unavailable")); i++) Thread.Sleep(100);
        Require(DiagnosticLog.Messages.ToArray().Any(m => m.StartsWith("[CLI] Command-line pipe unavailable")), "Joined a squatted pipe name");
    }

    // Hardware-free target for a real mcctl.exe: the CLI rig on the production pipe name.
    private static void HostCli(int seconds)
    {
        var rig = CreateCliRig();
        using var server = new CliServer(rig.Cli.Execute, new SynchronizationContext());
        server.Start();
        Console.WriteLine($@"Serving fake hardware on \\.\pipe\{CliServer.DefaultPipeName} for {seconds} s.");
        Thread.Sleep(seconds * 1000);
    }
}
