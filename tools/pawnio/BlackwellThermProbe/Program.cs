using System.Globalization;
using System.Text.RegularExpressions;
using MechControl.Hardware.PawnIo;

if (args.Length == 1 && args[0] == "--self-test")
    return RunSelfTest();

if (args.Length == 1 && args[0] == "--discover")
{
    IReadOnlyList<PciBdf> found = NvidiaPciDeviceLocator.FindCandidates();
    foreach (PciBdf candidate in found)
        Console.WriteLine($"{candidate.VendorId:X4}:{candidate.DeviceId:X4} {candidate}");
    return found.Count > 0 ? 0 : 2;
}

if (args.Length is < 2 or > 3)
{
    PrintUsage();
    return 1;
}

string modulePath = Path.GetFullPath(args[0]);
if (!TryParseBdf(args[1], out PciBdf bdf))
{
    Console.Error.WriteLine($"Invalid BDF '{args[1]}' (expected hex bus:device.function, e.g. 02:00.0)");
    PrintUsage();
    return 1;
}

int seconds = 30;
if (args.Length == 3 && (!int.TryParse(args[2], out seconds) || seconds < 1 || seconds > 3600))
{
    PrintUsage();
    return 1;
}

BlackwellGpuThermProvider? provider = BlackwellGpuThermProvider.Create(
    modulePath,
    new[] { bdf },
    diagnosticLogging: false,
    out string reason);
if (provider == null)
{
    Console.Error.WriteLine($"Probe failed: {reason}");
    return 2;
}

using (provider)
{
    Console.Error.WriteLine(
        $"PawnIO {provider.PawnIoVersion}; module {Path.GetFileName(modulePath)}; " +
        $"BDF {bdf}; startup={provider.State} ({reason})");
    Console.Write("timeMs,state,hotspotC");
    foreach (int offset in BlackwellGpuThermProvider.ChannelBarOffsets)
        Console.Write($",0x{offset:X6}_raw,0x{offset:X6}_tempC");
    Console.WriteLine();

    DateTime start = DateTime.UtcNow;
    while ((DateTime.UtcNow - start).TotalSeconds < seconds)
    {
        BlackwellGpuThermSample sample = provider.Poll();
        Console.Write($"{(long)(DateTime.UtcNow - start).TotalMilliseconds},{sample.State},");
        Console.Write(sample.HotspotC?.ToString("F4", CultureInfo.InvariantCulture) ?? "");

        for (int i = 0; i < BlackwellGpuThermProvider.ChannelBarOffsets.Length; i++)
        {
            Console.Write($",0x{sample.RawChannels[i]:X8},");
            Console.Write(sample.ChannelTemperaturesC[i]?.ToString("F4", CultureInfo.InvariantCulture) ?? "");
        }
        Console.WriteLine();
        Thread.Sleep(1000);
    }
}

return 0;

static bool TryParseBdf(string text, out PciBdf bdf)
{
    bdf = default;
    Match match = Regex.Match(text, @"^([0-9A-Fa-f]{1,2}):([0-9A-Fa-f]{1,2})\.([0-7])$");
    if (!match.Success)
        return false;

    int bus = int.Parse(match.Groups[1].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
    int device = int.Parse(match.Groups[2].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
    int function = int.Parse(match.Groups[3].Value, CultureInfo.InvariantCulture);
    if (device > 0x1F)
        return false;

    bdf = new PciBdf(bus, device, function);
    return true;
}

static int RunSelfTest()
{
    int failures = 0;

    CheckDecode(0x40004180, expectedValid: true, expectedTemperature: 65.5);
    CheckDecode(0xFFFFFFFF, expectedValid: false, expectedTemperature: 0);
    CheckDecode(0x00004180, expectedValid: false, expectedTemperature: 0);
    CheckDecode(0x40000000, expectedValid: false, expectedTemperature: 0);
    CheckDecode(0x40008200, expectedValid: false, expectedTemperature: 0);

    var fallbackChannels = new double?[] { 60, 65, null, 63, null, 64 };
    Check(BlackwellGpuThermProvider.SelectHotspot(fallbackChannels) == 65,
        "fallback selects max individual channel");

    var aggregateChannels = new double?[] { 60, 65, 66, 63, 67.5, 64 };
    Check(BlackwellGpuThermProvider.SelectHotspot(aggregateChannels) == 67.5,
        "valid 0xAD0AA0 aggregate has priority");

    Check(TryParseBdf("02:00.0", out PciBdf parsed) && parsed.Bus == 2 && parsed.Device == 0 && parsed.Function == 0,
        "BDF parse 02:00.0");
    Check(TryParseBdf("ff:1f.7", out parsed) && parsed.Bus == 0xFF && parsed.Device == 0x1F && parsed.Function == 7,
        "BDF parse ff:1f.7");
    Check(!TryParseBdf("100:00.0", out _), "BDF reject bus > 0xFF");
    Check(!TryParseBdf("00:20.0", out _), "BDF reject device > 0x1F");
    Check(!TryParseBdf("00:00.8", out _), "BDF reject function > 7");

    Check(NvidiaPciDeviceLocator.TryParseVendorDevice(
            @"PCI\VEN_10DE&DEV_2C18&SUBSYS_1D051558&REV_A1", out ushort vid, out ushort did)
        && vid == 0x10DE && did == 0x2C18,
        "hardware ID parse (NVIDIA)");
    Check(NvidiaPciDeviceLocator.TryParseVendorDevice(
            @"pci\ven_8086&dev_7d55", out vid, out did) && vid == 0x8086 && did == 0x7D55,
        "hardware ID parse (lowercase iGPU)");
    Check(!NvidiaPciDeviceLocator.TryParseVendorDevice(@"USB\VID_046D&PID_C52B", out _, out _),
        "hardware ID reject non-PCI");

    Check(NvidiaPciDeviceLocator.TryDecodeAddress(2, 0x00000000, 0x10DE, 0x2C18, out PciBdf decoded)
        && decoded.Bus == 2 && decoded.Device == 0 && decoded.Function == 0,
        "address decode 02:00.0");
    Check(NvidiaPciDeviceLocator.TryDecodeAddress(0xE1, 0x001F0007, 0, 0, out decoded)
        && decoded.Bus == 0xE1 && decoded.Device == 0x1F && decoded.Function == 7,
        "address decode E1:1F.7");
    Check(!NvidiaPciDeviceLocator.TryDecodeAddress(0x100, 0, 0, 0, out _),
        "address reject bus > 0xFF");
    Check(!NvidiaPciDeviceLocator.TryDecodeAddress(0, 0x00200000, 0, 0, out _),
        "address reject device > 0x1F");
    Check(!NvidiaPciDeviceLocator.TryDecodeAddress(0, 0x00000008, 0, 0, out _),
        "address reject function > 7");

    RunStateMachineTests(Check);

    Console.WriteLine(failures == 0 ? "Self-test PASS" : $"Self-test FAIL ({failures})");
    return failures == 0 ? 0 : 3;

    void CheckDecode(uint raw, bool expectedValid, double expectedTemperature)
    {
        bool valid = BlackwellGpuThermProvider.TryDecode(raw, out double temperature);
        Check(valid == expectedValid && (!valid || Math.Abs(temperature - expectedTemperature) < 0.0001),
            $"decode 0x{raw:X8}");
    }

    void Check(bool condition, string name)
    {
        if (condition)
        {
            Console.WriteLine($"PASS: {name}");
        }
        else
        {
            failures++;
            Console.WriteLine($"FAIL: {name}");
        }
    }
}

static void RunStateMachineTests(Action<bool, string> check)
{
    const int NotSupportedNt = unchecked((int)0xC00000BB);
    const int NotSupportedHr = unchecked((int)0xD00000BB);
    const int GenericFail = unchecked((int)0x80004005);

    var bdfA = new PciBdf(2, 0, 0);
    var bdfB = new PciBdf(3, 0, 0);

    static ulong[] Valid() => new ulong[]
        { 0x40004000, 0x40004100, 0x40004200, 0x40004300, 0x40004400, 0x40004500 };
    static ulong[] AllOnes() => Enumerable.Repeat<ulong>(0xFFFFFFFF, 6).ToArray();
    static Func<ulong[]> Throw(int hresult) =>
        () => throw new InvalidOperationException($"fake 0x{hresult:X8}") { HResult = hresult };

    // 1. Pending startup failure, quiet retry cadence, then success.
    {
        var fake = new FakeExecutor();
        fake.Enqueue(Throw(NotSupportedNt));
        fake.Enqueue(Valid);
        var p = new BlackwellGpuThermProvider(fake, new[] { bdfA }, false, pendingRetryTicks: 3);
        check(p.Poll().State == BlackwellGpuThermState.Pending && !p.SupportConfirmed && fake.Calls == 1,
            "state: startup failure -> Pending");
        check(p.Poll().State == BlackwellGpuThermState.Pending && fake.Calls == 1,
            "state: pending tick 1 does not touch the module");
        check(p.Poll().State == BlackwellGpuThermState.Pending && fake.Calls == 1,
            "state: pending tick 2 does not touch the module");
        BlackwellGpuThermSample s = p.Poll();
        check(s.State == BlackwellGpuThermState.Available && p.SupportConfirmed
            && p.BoundBdf.Equals(bdfA) && fake.Calls == 2 && s.HotspotC == 68.0,
            "state: retry confirms support (0xAD0AA0 hotspot)");
    }

    // 2. First success all-ones -> Asleep with support confirmed.
    {
        var fake = new FakeExecutor();
        fake.Enqueue(AllOnes);
        var p = new BlackwellGpuThermProvider(fake, new[] { bdfA }, false);
        check(p.Poll().State == BlackwellGpuThermState.Asleep && p.SupportConfirmed,
            "state: first all-ones sample confirms support as Asleep");
    }

    // 3. Available -> deep power-gate (NOT_SUPPORTED) -> Asleep -> recovery.
    {
        var fake = new FakeExecutor();
        fake.Enqueue(Valid);
        fake.Enqueue(Throw(NotSupportedHr));
        fake.Enqueue(Valid);
        var p = new BlackwellGpuThermProvider(fake, new[] { bdfA }, false);
        check(p.Poll().State == BlackwellGpuThermState.Available, "state: available baseline");
        check(p.Poll().State == BlackwellGpuThermState.Asleep,
            "state: NOT_SUPPORTED after confirmation means asleep (D3cold)");
        check(p.Poll().State == BlackwellGpuThermState.Available, "state: wake recovery");
    }

    // 4. Available -> transport failure -> Faulted -> recovery.
    {
        var fake = new FakeExecutor();
        fake.Enqueue(Valid);
        fake.Enqueue(Throw(GenericFail));
        fake.Enqueue(Valid);
        var p = new BlackwellGpuThermProvider(fake, new[] { bdfA }, false);
        check(p.Poll().State == BlackwellGpuThermState.Available, "state: available baseline 2");
        check(p.Poll().State == BlackwellGpuThermState.Faulted,
            "state: generic failure after confirmation means Faulted");
        check(p.Poll().State == BlackwellGpuThermState.Available, "state: fault recovery");
    }

    // 5. Malformed output count after confirmation -> Faulted.
    {
        var fake = new FakeExecutor();
        fake.Enqueue(Valid);
        fake.Enqueue(() => new ulong[3]);
        var p = new BlackwellGpuThermProvider(fake, new[] { bdfA }, false);
        p.Poll();
        check(p.Poll().State == BlackwellGpuThermState.Faulted,
            "state: malformed output count means Faulted");
    }

    // 6. Multiple candidates: first fails the gate, second binds.
    {
        var fake = new FakeExecutor();
        fake.Enqueue(Throw(NotSupportedNt));
        fake.Enqueue(Valid);
        var p = new BlackwellGpuThermProvider(fake, new[] { bdfA, bdfB }, false);
        check(p.Poll().State == BlackwellGpuThermState.Available && p.BoundBdf.Equals(bdfB),
            "state: second candidate binds when first is rejected");
    }
}

static void PrintUsage()
{
    Console.Error.WriteLine("usage: BlackwellThermProbe <Nvidia.bin> <bus:device.function> [seconds 1-3600]");
    Console.Error.WriteLine("       BlackwellThermProbe --discover   (list NVIDIA PCI candidates, passive)");
    Console.Error.WriteLine("       BlackwellThermProbe --self-test");
    Console.Error.WriteLine("example: BlackwellThermProbe C:\\Temp\\tandem\\pr77\\Nvidia.bin 02:00.0 60");
}

sealed class FakeExecutor : IPawnIoExecutor
{
    private readonly Queue<Func<ulong[]>> _script = new();
    public int Calls { get; private set; }
    public Version LibraryVersion { get; } = new(0, 0, 0);

    public void Enqueue(Func<ulong[]> response) => _script.Enqueue(response);

    public ulong[] Execute(string functionName, ulong[] input, int outputCount)
    {
        Calls++;
        if (_script.Count == 0)
            throw new InvalidOperationException("fake script exhausted");
        return _script.Dequeue()();
    }

    public void Dispose() { }
}
