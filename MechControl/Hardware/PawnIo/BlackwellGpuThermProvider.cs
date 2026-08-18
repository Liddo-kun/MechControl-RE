namespace MechControl.Hardware.PawnIo;

enum BlackwellGpuThermState
{
    /// <summary>Module loaded, candidates exist, GB20x support not yet proven
    /// (e.g. GPU power-gated at startup). Retried quietly.</summary>
    Pending,
    Available,
    Asleep,
    /// <summary>Support was proven earlier but this read failed.</summary>
    Faulted,
}

readonly struct BlackwellGpuThermSample
{
    public BlackwellGpuThermState State { get; }
    public double? HotspotC { get; }
    public double?[] ChannelTemperaturesC { get; }
    public uint[] RawChannels { get; }

    public BlackwellGpuThermSample(
        BlackwellGpuThermState state,
        double? hotspotC,
        double?[] channelTemperaturesC,
        uint[] rawChannels)
    {
        State = state;
        HotspotC = hotspotC;
        ChannelTemperaturesC = channelTemperaturesC;
        RawChannels = rawChannels;
    }
}

/// <summary>
/// Minimal seam between the provider and PawnIOLib so the state machine is
/// testable with a scripted fake (probe self-test).
/// </summary>
interface IPawnIoExecutor : IDisposable
{
    Version LibraryVersion { get; }
    ulong[] Execute(string functionName, ulong[] input, int outputCount);
}

/// <summary>
/// Reads the six NVIDIA GB20x die-temperature channels through the upstream
/// PawnIO.Modules NVIDIA module (PR #77). The module returns raw DWORDs only;
/// all decoding, plausibility checks, and hot-spot selection happen here.
/// This provider never initializes NVAPI/NVML, so polling does not
/// deliberately wake a power-gated discrete GPU.
/// </summary>
sealed class BlackwellGpuThermProvider : IDisposable
{
    // Upstream ABI names, kept in one place: PR #77 has an open maintainer
    // question (2026-07-18) about splitting the general NVIDIA module into
    // per-domain modules, which would rename both of these.
    public const string ModuleFileName = "Nvidia.bin";
    private const string IoctlReadThermalRegisters = "ioctl_read_thermal_registers";

    /// <summary>BAR0 offsets of the six channels, in module output order.</summary>
    internal static readonly int[] ChannelBarOffsets =
        { 0xAD0A90, 0xAD0A94, 0xAD0A98, 0xAD0A9C, 0xAD0AA0, 0xAD0AA4 };

    // 0xAD0AA0: empirical hardware-maximum aggregate (MechControl display
    // policy, not part of the upstream ABI).
    private const int AggregateChannelIndex = 4;

    // Quiet retry cadence while support is unproven (one attempt per
    // PendingRetryTicks calls of the 1 Hz Poll).
    private const int DefaultPendingRetryTicks = 15;

    private readonly IPawnIoExecutor _executor;
    private readonly IReadOnlyList<PciBdf> _candidates;
    private readonly bool _diagnosticLogging;
    private readonly int _pendingRetryTicks;
    private int _retryTicksLeft;
    private BlackwellGpuThermState _state = BlackwellGpuThermState.Pending;
    private bool _disposed;

    /// <summary>Set once a candidate passes the module's GB20x gate.</summary>
    public bool SupportConfirmed { get; private set; }
    public PciBdf? BoundBdf { get; private set; }
    public BlackwellGpuThermState State => _state;
    public Version PawnIoVersion => _executor.LibraryVersion;

    internal BlackwellGpuThermProvider(
        IPawnIoExecutor executor,
        IReadOnlyList<PciBdf> candidates,
        bool diagnosticLogging,
        int pendingRetryTicks = DefaultPendingRetryTicks)
    {
        _executor = executor;
        _candidates = candidates;
        _diagnosticLogging = diagnosticLogging;
        _pendingRetryTicks = pendingRetryTicks;
    }

    /// <summary>
    /// Creates the provider whenever the module loads and at least one NVIDIA
    /// candidate exists; support is then proven by the first successful IOCTL
    /// (immediately, or later via Poll retries — a GPU that is power-gated at
    /// startup must not permanently disable the feature). Returns null only
    /// for genuinely terminal conditions (no candidates, no PawnIO, no module).
    /// </summary>
    public static BlackwellGpuThermProvider? Create(
        string modulePath,
        IReadOnlyList<PciBdf> candidates,
        bool diagnosticLogging,
        out string reason)
    {
        if (candidates.Count == 0)
        {
            // No NVIDIA candidates means no PawnIO DLL/driver access at all.
            reason = "no NVIDIA display-class PCI device found";
            return null;
        }

        if (!PawnIoClient.TryOpen(modulePath, out PawnIoClient? client, out reason) || client == null)
            return null;

        var provider = new BlackwellGpuThermProvider(client, candidates, diagnosticLogging);
        provider.TryConfirmSupport();
        reason = provider._state switch
        {
            BlackwellGpuThermState.Available => "supported",
            BlackwellGpuThermState.Asleep => "supported; GPU is power-gated",
            _ => "support not yet proven; will keep retrying",
        };
        return provider;
    }

    public BlackwellGpuThermSample Poll()
    {
        if (_disposed)
            return EmptySample(BlackwellGpuThermState.Faulted);

        if (!SupportConfirmed)
        {
            if (--_retryTicksLeft > 0)
                return EmptySample(BlackwellGpuThermState.Pending);

            BlackwellGpuThermSample? confirmed = TryConfirmSupport();
            return confirmed ?? EmptySample(BlackwellGpuThermState.Pending);
        }

        try
        {
            ReadChannels(BoundBdf!.Value, out uint[] raw, out double?[] temperatures);
            return BuildSample(raw, temperatures);
        }
        catch (Exception ex)
        {
            // A deeply power-gated GPU (D3cold) fails the module's PCI
            // config-space identity check with STATUS_NOT_SUPPORTED instead of
            // returning all-ones data, so that specific failure after
            // confirmed support means asleep, not fault.
            BlackwellGpuThermState state = IsNotSupportedStatus(ex.HResult)
                ? BlackwellGpuThermState.Asleep
                : BlackwellGpuThermState.Faulted;
            TransitionTo(state, ex.Message);
            return EmptySample(state);
        }
    }

    /// <summary>
    /// One attempt across all candidates. Confirms support only on a sample
    /// that is clearly Blackwell (valid channels, or the distinctive all-ones
    /// power-gated pattern after passing the module's architecture gate).
    /// </summary>
    private BlackwellGpuThermSample? TryConfirmSupport()
    {
        _retryTicksLeft = _pendingRetryTicks;

        foreach (PciBdf bdf in _candidates)
        {
            uint[] raw;
            double?[] temperatures;
            try
            {
                ReadChannels(bdf, out raw, out temperatures);
            }
            catch
            {
                // Includes STATUS_NOT_SUPPORTED for pre-Blackwell devices and
                // config-space failures from a deeply power-gated GPU.
                continue;
            }

            bool anyValid = temperatures.Any(t => t.HasValue);
            bool allOnes = raw.All(v => v == uint.MaxValue);
            if (!anyValid && !allOnes)
                continue;

            SupportConfirmed = true;
            BoundBdf = bdf;
            return BuildSample(raw, temperatures);
        }

        return null;
    }

    private BlackwellGpuThermSample BuildSample(uint[] raw, double?[] temperatures)
    {
        if (_diagnosticLogging)
        {
            DiagnosticLog.Write($"[GPU Hot Spot][diag] {BoundBdf} " +
                $"channels={string.Join(",", raw.Select(v => $"0x{v:X8}"))}");
        }

        if (raw.All(v => v == uint.MaxValue))
        {
            TransitionTo(BlackwellGpuThermState.Asleep, "GPU power-gated");
            return new BlackwellGpuThermSample(BlackwellGpuThermState.Asleep, null, temperatures, raw);
        }

        double? hotspotC = SelectHotspot(temperatures);
        if (!hotspotC.HasValue)
        {
            TransitionTo(BlackwellGpuThermState.Faulted, "no valid, plausible thermal channel");
            return new BlackwellGpuThermSample(BlackwellGpuThermState.Faulted, null, temperatures, raw);
        }

        TransitionTo(BlackwellGpuThermState.Available, $"{hotspotC.Value:F1}C");
        return new BlackwellGpuThermSample(BlackwellGpuThermState.Available, hotspotC, temperatures, raw);
    }

    internal static bool TryDecode(uint raw, out double temperatureC)
    {
        temperatureC = 0;
        if (raw == uint.MaxValue || (raw & 0x40000000) == 0)
            return false;

        temperatureC = (raw & 0xFFFF) / 256.0;
        return temperatureC > 0 && temperatureC < 130;
    }

    internal static double? SelectHotspot(double?[] temperatures)
    {
        if (temperatures.Length != ChannelBarOffsets.Length)
            throw new ArgumentException("Expected six thermal channels", nameof(temperatures));

        if (temperatures[AggregateChannelIndex].HasValue)
            return temperatures[AggregateChannelIndex];

        double? max = null;
        for (int i = 0; i < temperatures.Length; i++)
        {
            if (i == AggregateChannelIndex || !temperatures[i].HasValue)
                continue;
            if (!max.HasValue || temperatures[i]!.Value > max.Value)
                max = temperatures[i];
        }
        return max;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _executor.Dispose();
    }

    // STATUS_NOT_SUPPORTED as a raw NTSTATUS or wrapped via HRESULT_FROM_NT.
    private static bool IsNotSupportedStatus(int hresult) =>
        hresult is unchecked((int)0xC00000BB) or unchecked((int)0xD00000BB);

    private void TransitionTo(BlackwellGpuThermState state, string detail)
    {
        if (state == _state) return;
        _state = state;
        DiagnosticLog.Write($"[GPU Hot Spot] {BoundBdf?.ToString() ?? "(unbound)"} -> {state}: {detail}");
    }

    private void ReadChannels(PciBdf bdf, out uint[] raw, out double?[] temperatures)
    {
        ulong[] output = _executor.Execute(
            IoctlReadThermalRegisters,
            new ulong[] { bdf.Bus, bdf.Device, bdf.Function },
            ChannelBarOffsets.Length);
        if (output.Length != ChannelBarOffsets.Length)
            throw new InvalidDataException(
                $"{IoctlReadThermalRegisters} returned {output.Length} values, expected {ChannelBarOffsets.Length}");

        raw = new uint[ChannelBarOffsets.Length];
        temperatures = new double?[ChannelBarOffsets.Length];
        for (int i = 0; i < ChannelBarOffsets.Length; i++)
        {
            raw[i] = (uint)output[i];
            if (TryDecode(raw[i], out double temperatureC))
                temperatures[i] = temperatureC;
        }
    }

    private static BlackwellGpuThermSample EmptySample(BlackwellGpuThermState state) => new(
        state,
        null,
        new double?[ChannelBarOffsets.Length],
        new uint[ChannelBarOffsets.Length]);
}
