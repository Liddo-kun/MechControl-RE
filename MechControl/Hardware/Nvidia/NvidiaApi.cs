using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Text;

namespace MechControl.Hardware.Nvidia;

internal sealed record NvidiaIdentity(ushort DeviceId, uint SubsystemId, uint Domain, uint Bus,
    uint Device, uint Function, string VbiosVersion, string Name, string DriverVersion)
{
    public string Bdf => $"{Domain:X4}:{Bus:X2}:{Device:X2}.{Function}";
}

internal sealed record NvidiaPowerLimits(uint MinimumMw, uint DefaultMw, uint MaximumMw,
    uint EnforcedMw, uint? RequestedMw);
internal sealed record NvidiaCurrentLimit(uint MinimumMa, uint DefaultMa, uint MaximumMa, uint CurrentMa);
internal sealed record NvidiaCurrentLimits(NvidiaCurrentLimit Nvvdd, NvidiaCurrentLimit Msvdd, NvidiaCurrentLimit? BoardInput);
// Null fields were not recognized on this read.
internal sealed record NvidiaTelemetry(uint NvvddMa, uint MsvddMa, uint? BoardInputMa, uint? NvvddPredictedMa,
    uint? MsvddPredictedMa, uint? CurrentCapKhz, uint? PowerCapKhz, uint? CeilingKhz)
{
    // Active current limits (null outside the policy's range).
    public uint? NvvddLimitMa { get; init; }
    public uint? MsvddLimitMa { get; init; }
    public uint? BoardInputLimitMa { get; init; }
    // Board power policy: measured GPU power; the power controller's rails.
    public uint? GpuPowerMw { get; init; }
    public uint? CorePowerMw { get; init; }
    // NVML.
    public uint? ClockMhz { get; init; }
    public uint? EnforcedMw { get; init; }
    // Share of time since the previous read the inline power controller acted (0-1).
    public double? InlinePowerActivity { get; init; }
}

/// <summary>
/// User-mode NVIDIA backend. No registry, EC, driver patching, restart or external tuning-tool dependency.
/// Private buffers use explicitly versioned layouts, not driver RVAs. Unsupported versions fail closed.
/// Not thread-safe: the caller serializes all calls.
/// </summary>
internal sealed partial class NvidiaApi : IDisposable
{
    private IntPtr library, gpu;
    private QueryInterface? query;
    private NvidiaRuntimeNvml? nvml;
    private bool initialized;
    private byte[]? statusBuffer; // Reused: 704 KB, read twice a second while the Power tab is shown.
    public NvidiaIdentity Identity { get; private set; } = null!;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr QueryInterface(uint id);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int NoArgs();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Enumerate([Out] IntPtr[] handles, out uint count);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int BufferCall(IntPtr gpu, IntPtr data);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int GetUInt(IntPtr gpu, out uint value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int PciIds(IntPtr gpu, out uint device, out uint subsystem, out uint revision, out uint extDevice);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int DriverVersion(out uint version, [Out] byte[] branch);

    public NvidiaApi()
    {
        try { Initialize(); } catch { Dispose(); throw; }
    }

    // Recreate all handles after a device restart.
    public void Reinitialize()
    {
        var before = Identity;
        Close();
        try { Initialize(); } catch { Close(); throw; }
        if (before.DeviceId != Identity.DeviceId || before.SubsystemId != Identity.SubsystemId || before.Bdf != Identity.Bdf)
        {
            Close();
            throw new InvalidOperationException("The NVIDIA adapter identity changed after restart.");
        }
    }

    private void Initialize()
    {
        library = NativeLibrary.Load(Path.Combine(Environment.SystemDirectory, "nvapi64.dll"));
        query = Marshal.GetDelegateForFunctionPointer<QueryInterface>(NativeLibrary.GetExport(library, "nvapi_QueryInterface"));
        Check(Resolve<NoArgs>(0x0150e828)(), "Initialize");
        initialized = true;
        var handles = new IntPtr[64];
        Check(Resolve<Enumerate>(0xe5ac921f)(handles, out uint count), "EnumPhysicalGPUs");
        if (count != 1) throw new NotSupportedException("NVIDIA power overrides currently require one NVIDIA GPU; the EC target is ambiguous with multiple adapters.");
        gpu = handles[0];
        Check(Resolve<PciIds>(0x2ddfb66e)(gpu, out uint pci, out uint subsystem, out _, out _), "GetPCIIdentifiers");
        Check(Resolve<GetUInt>(0x1be0b8e5)(gpu, out uint bus), "GetBusId");
        Check(Resolve<DriverVersion>(0x2926aaad)(out uint driver, new byte[64]), "GetDriverAndBranchVersion");
        nvml = new NvidiaRuntimeNvml(pci, subsystem, bus);
        Identity = new((ushort)(pci >> 16), subsystem, nvml.Domain, bus, nvml.Device, nvml.Function,
            ReadString(0xa561fd7d), ReadString(0xceee8e9f), $"{driver / 100}.{driver % 100:00}");
    }

    public byte[] ReadBios()
    {
        // NV_VBIOS_IMAGE_V2: { version, bufferSize, byte* buffer }. The driver
        // supplies the consumed image (including software overrides), not necessarily physical flash.
        var bytes = new byte[1024000];
        var pinned = GCHandle.Alloc(bytes, GCHandleType.Pinned);
        try
        {
            var info = NewBuffer(16, 0x20010);
            Put(info, 4, (uint)bytes.Length);
            BinaryPrimitives.WriteInt64LittleEndian(info.AsSpan(8), pinned.AddrOfPinnedObject().ToInt64());
            Call(0xfc13ee11, info, "GetVbiosImage");
            uint size = U32(info, 4);
            if (size < 512 || size > bytes.Length) throw Unsupported("VBIOS image length");
            return bytes.AsSpan(0, (int)size).ToArray();
        }
        finally { pinned.Free(); }
    }

    public NvidiaPowerLimits ReadPowerLimits()
    {
        var board = ReadBoard();
        var request = ReadPowerRequest(board.Index);
        return new(board.Min, board.Default, board.Max, nvml!.ReadEnforcedPowerLimit(), request);
    }

    public NvidiaPowerLimits SetPowerLimit(uint milliwatts)
    {
        var board = ReadBoard();
        if (milliwatts < board.Min || milliwatts > board.Max) throw new ArgumentOutOfRangeException(nameof(milliwatts), "Request is outside the live NVIDIA power range.");
        var control = PowerControl(board.Index, 0xf8);
        Put(control, 0x28 + board.Index * 0x28, milliwatts);
        // F8 is the laptop kernel-client request. Ordinary NVML power SET is not a substitute.
        Call(0x17695269, control, "ClientPwrPoliciesSetControl(F8)");
        // A lower request is enforced after a short delay; the thermal hold relies on it.
        for (int i = 0; ; i++)
        {
            var readback = ReadPowerLimits();
            if (readback.RequestedMw == milliwatts && readback.EnforcedMw <= milliwatts) return readback;
            if (i == 15) throw new InvalidOperationException($"NVIDIA F8/enforced readback did not verify {milliwatts} mW (request {readback.RequestedMw}, enforced {readback.EnforcedMw}).");
            Thread.Sleep(100);
        }
    }

    public NvidiaCurrentLimits ReadCurrentLimits()
    {
        var rails = ReadRails();
        var control = ReadCurrentControl(rails.Mask);
        return new(ReadRail(rails.Nv, control), ReadRail(rails.Ms, control),
            rails.Input is { } input ? ReadRail(input, control) : null);
    }

    public NvidiaCurrentLimits SetCurrentLimits(uint? nvvddMa, uint? msvddMa, uint? boardInputMa)
    {
        var rails = ReadRails();
        var control = ReadCurrentControl(rails.Mask);
        var beforeNv = ReadRail(rails.Nv, control);
        var beforeMs = ReadRail(rails.Ms, control);
        var beforeInput = rails.Input is { } input ? ReadRail(input, control) : null;
        uint selected = 0;
        void Change(Rail? rail, uint? value)
        {
            if (!value.HasValue) return;
            if (rail == null) throw Unsupported("board input current policy");
            if (value.Value < rail.Min || value.Value > rail.Max) throw new ArgumentOutOfRangeException(nameof(value), "Current request is outside the live NVIDIA range.");
            Put(control, 0x20 + rail.Index * 0x28, value.Value);
            selected |= 1u << rail.Index;
        }
        Change(rails.Nv, nvvddMa); Change(rails.Ms, msvddMa); Change(rails.Input, boardInputMa);
        if (selected == 0) return ReadCurrentLimits();
        // Preserve complete GET metadata; select only requested policies. The driver merges
        // this versioned compact control into native full control. Never construct a raw RM SET.
        Put(control, 4, selected);
        Call(0xaffc2279, control, "PowerPolicySetControl");
        var result = ReadCurrentLimits();
        if ((nvvddMa.HasValue && result.Nvvdd.CurrentMa != nvvddMa.Value) || (msvddMa.HasValue && result.Msvdd.CurrentMa != msvddMa.Value) ||
            (boardInputMa.HasValue && result.BoardInput?.CurrentMa != boardInputMa.Value))
            throw new InvalidOperationException("NVIDIA current-policy readback did not retain the requested values.");
        if ((!nvvddMa.HasValue && result.Nvvdd != beforeNv) || (!msvddMa.HasValue && result.Msvdd != beforeMs) ||
            (!boardInputMa.HasValue && result.BoardInput != beforeInput))
            throw new InvalidOperationException("NVIDIA current-policy readback changed an unselected current limit.");
        return result;
    }

    /// <summary>
    /// Live values the driver publishes every 20 ms: each current policy's measured value, the
    /// clock caps of the current and power controllers, and the current controller's prediction
    /// of each rail at its cap (it lowers the cap until neither exceeds its limit).
    /// </summary>
    public NvidiaTelemetry ReadTelemetry()
    {
        var rails = ReadRails();
        uint mask = U32(rails.Info, 4);
        // NV_GPU_POWER_POLICY_STATUS_PARAMS_V10: entries {type, limitCurr, valueCurr} at 0x11B8, stride 0x3768.
        const int size = 0xb00b8;
        var status = statusBuffer ??= new byte[size + GuardBytes];
        status.AsSpan(0, size).Clear();
        Put(status, 0, 0xb00b8); Put(status, 4, mask);
        CallGuarded(0x31b7a4cd, status, size, "PowerPolicyGetStatus(V10)");
        RequireVersion(status, 0xb00b8);
        static int Entry(int index) => 0x11b8 + index * 0x3768;
        uint Measured(Rail rail) => U32(status, Entry(rail.Index)) == rail.Type ? U32(status, Entry(rail.Index) + 8) : throw Unsupported("current-policy status identity");
        uint nv = Measured(rails.Nv), ms = Measured(rails.Ms);
        uint? input = rails.Input is { } board && U32(status, Entry(board.Index)) == board.Type ? U32(status, Entry(board.Index) + 8) : null;

        // Controllers are status type 0x15. Their union (+0x968) was mapped on driver 617.14 (the
        // public header's layout differs), so it is checked on every read: clock cap (kHz) +0x04 within
        // the ceiling +0x24; observed total +0x60 = rail 0 +0x134 + rail 1 +0xB84; predicted at the
        // cap: total +0x15D0 = rail 0 +0x1690 + rail 1 +0x1E70.
        uint At(int index, int offset) => U32(status, Entry(index) + 0x968 + offset);
        bool Controller(int i) => U32(status, Entry(i)) == 0x15 && At(i, 4) != 0 && At(i, 4) <= At(i, 0x24) &&
            At(i, 0x60) == (ulong)At(i, 0x134) + At(i, 0xb84) && At(i, 0x15d0) == (ulong)At(i, 0x1690) + At(i, 0x1e70);
        // The current controller observes exactly the NVVDD and MSVDD readings.
        int? current = Unique(mask, i => Controller(i) && At(i, 0x134) == nv && At(i, 0xb84) == ms);
        // The power controller's limit comes from the board power policy (TOTAL_GPU, mW): its PMU
        // limit arbitration (+0xE0: input count +1, inputs {u8 policy, u32 limit} from +0x48).
        uint Info(int index, int offset) => U32(rails.Info, 0x3c + index * 0x58 + offset);
        int? boardPower = Unique(mask, i => Info(i, 0) == 0 && Info(i, 8) == 0 && Info(i, 20) != 0 && U32(status, Entry(i)) == 0);
        bool LimitedBy(int i, int source)
        {
            int arbitration = Entry(i) + 0xe0, count = status[arbitration + 1];
            return count <= 8 && Enumerable.Range(0, count).Any(k => status[arbitration + 0x48 + k * 8] == source);
        }
        int? power = boardPower is int source ? Unique(mask, i => Controller(i) && LimitedBy(i, source)) : null;
        uint? Value(int? index, int offset) => index is int i ? At(i, offset) : null;
        uint? Limit(Rail? rail) => rail is { } r && U32(status, Entry(r.Index)) == r.Type && U32(status, Entry(r.Index) + 4) is uint v &&
            v >= r.Min && v <= r.Max ? v : null;
        // The board power policy's measured value (0 when unset).
        uint? gpuPower = boardPower is int b && U32(status, Entry(b) + 8) is uint w and > 0 ? w : null;
        uint? Nvml(Func<NvidiaRuntimeNvml, uint> read)
        {
            try { return nvml is { } n ? read(n) : null; }
            catch (InvalidOperationException) { return null; }
        }
        return new(nv, ms, input, Value(current, 0x1690), Value(current, 0x1e70), Value(current, 4), Value(power, 4),
            Value(current, 0x24) ?? Value(power, 0x24))
        {
            NvvddLimitMa = Limit(rails.Nv), MsvddLimitMa = Limit(rails.Ms), BoardInputLimitMa = Limit(rails.Input),
            GpuPowerMw = gpuPower, CorePowerMw = Value(power, 0x60),
            ClockMhz = Nvml(n => n.ReadGraphicsClockMhz()), EnforcedMw = Nvml(n => n.ReadEnforcedPowerLimit()),
            InlinePowerActivity = ReadInlinePowerActivity(),
        };
    }

    // The only policy in the mask that matches, or null when none or several do.
    private static int? Unique(uint mask, Func<int, bool> match)
    {
        int? found = null;
        for (int i = 0; i < 32; i++)
        {
            if ((mask & (1u << i)) == 0 || !match(i)) continue;
            if (found != null) return null;
            found = i;
        }
        return found;
    }

    private sealed record Board(int Index, uint Min, uint Default, uint Max);
    private Board ReadBoard()
    {
        var info = NewBuffer(0x728, 0x10728);
        Call(0x54924bf5, info, "ClientPwrPoliciesGetInfo");
        RequireVersion(info, 0x10728);
        Board? board = null;
        uint mask = U32(info, 4);
        for (int i = 0; i < 32; i++)
        {
            if ((mask & (1u << i)) == 0) continue;
            int p = 0x28 + i * 0x38;
            uint kind = U32(info, p);
            if (kind > 1) continue;
            if (U32(info, p + 4) != 0) throw Unsupported("board power unit");
            if (board != null) throw Unsupported("ambiguous board-power policies");
            board = new(i, U32(info, p + 8), U32(info, p + 12), U32(info, p + 16));
            RequireRange(board.Min, board.Default, board.Max);
        }
        return board ?? throw Unsupported("missing board-power policy");
    }

    private uint? ReadPowerRequest(int index)
    {
        var control = PowerControl(index, 0xf8);
        Call(0x33ab0353, control, "ClientPwrPoliciesGetControl(F8)");
        RequireVersion(control, 0x10528);
        int p = 0x28 + index * 0x28;
        if (U32(control, 4) != 1u << index || U32(control, p + 4) != 0xf8) throw Unsupported("F8 selection readback");
        uint value = U32(control, p);
        return value is 0 or uint.MaxValue ? null : value;
    }

    private static byte[] PowerControl(int index, uint source)
    {
        var b = NewBuffer(0x528, 0x10528);
        Put(b, 4, 1u << index); Put(b, 0x2c + index * 0x28, source);
        return b;
    }

    private sealed record Rail(int Index, uint Type, uint Min, uint Default, uint Max);
    private (Rail Nv, Rail Ms, Rail? Input, uint Mask, byte[] Info) ReadRails()
    {
        // Layouts independently corroborated by mVolt FUN_1401fc9f0/1401fc540/
        // 1401fe260 and NV_GPU_POWER_POLICY_INFO_PARAMS_V4 / CONTROL_PARAMS_V2.
        // Newer drivers refusing these versions are unsupported, never accessed via copied RVAs.
        var info = NewBuffer(0xa70, 0x40a70);
        Call(0x67f31384, info, "PowerPolicyGetInfo(V4)");
        RequireVersion(info, 0x40a70);
        var monitor = NewBuffer(0xca8, 0x30ca8);
        Call(0xc12eb19e, monitor, "PowerMonitorGetInfo(V3)");
        RequireVersion(monitor, 0x30ca8);
        Rail? nv = null, ms = null, input = null;
        int inputs = 0;
        uint mask = U32(info, 4), channels = U32(monitor, 16);
        for (int i = 0; i < 32; i++)
        {
            if ((mask & (1u << i)) == 0) continue;
            int p = 0x3c + i * 0x58;
            uint type = U32(info, p), channel = U32(info, p + 4);
            if ((type != 19 && type != 4) || U32(info, p + 8) != 1 || channel >= 32 || (channels & (1u << (int)channel)) == 0) continue;
            int m = 0x28 + (int)channel * 0x3c;
            // mVolt FUN_1402373c0: HW_THRESHOLD in mA on SENSOR_CLIENT_ALIGNED.
            // Identify the monitor kind, never this machine's channel number (8).
            if (type == 4)
            {
                if (U32(monitor, m + 12) != 8 || U32(monitor, m + 16) == uint.MaxValue) continue;
                input = new(i, type, U32(info, p + 12), U32(info, p + 16), U32(info, p + 20));
                inputs++;
                continue;
            }
            if (U32(monitor, m + 12) != 6) continue; // SENSOR channel, rail from driver, never channel-number guesses.
            uint railId = U32(monitor, m + 16) & 255;
            if (railId != 1 && railId != 16) continue; // OUTPUT_NVVDD / OUTPUT_MSVDD
            var rail = new Rail(i, type, U32(info, p + 12), U32(info, p + 16), U32(info, p + 20));
            RequireRange(rail.Min, rail.Default, rail.Max);
            if (railId == 1) { if (nv != null) throw Unsupported("ambiguous NVVDD policies"); nv = rail; }
            else { if (ms != null) throw Unsupported("ambiguous MSVDD policies"); ms = rail; }
        }
        if (nv == null || ms == null) throw Unsupported("NVVDD/MSVDD sensor policies");
        // Board input is optional: an ambiguous or out-of-range policy is never written
        // and disables only that limit, not the rails.
        if (inputs > 1 || (input != null && !ValidRange(input.Min, input.Default, input.Max))) input = null;
        return (nv, ms, input, (1u << nv.Index) | (1u << ms.Index) | (input == null ? 0 : 1u << input.Index), info);
    }

    private byte[] ReadCurrentControl(uint mask)
    {
        var control = NewBuffer(0xa4c, 0x10a4c);
        Put(control, 4, mask);
        Call(0x8b3e7343, control, "PowerPolicyGetControl");
        RequireVersion(control, 0x10a4c);
        if (U32(control, 4) != mask) throw Unsupported("current-policy selection mask");
        return control;
    }

    private static NvidiaCurrentLimit ReadRail(Rail rail, byte[] control)
    {
        int p = 0x1c + rail.Index * 0x28;
        uint current = U32(control, p + 4);
        if (U32(control, p) != rail.Type || current < rail.Min || current > rail.Max) throw Unsupported("current-policy identity/range");
        return new(rail.Min, rail.Default, rail.Max, current);
    }

    private T Resolve<T>(uint id) where T : Delegate
    {
        if (query == null) throw new ObjectDisposedException(nameof(NvidiaApi));
        IntPtr address = query(id);
        if (address == IntPtr.Zero) throw new NotSupportedException($"NVIDIA interface 0x{id:X8} is unavailable.");
        return Marshal.GetDelegateForFunctionPointer<T>(address);
    }

    private const int GuardBytes = 4096;

    private void Call(uint id, byte[] data, string name)
    {
        var guarded = new byte[data.Length + GuardBytes];
        data.CopyTo(guarded, 0);
        CallGuarded(id, guarded, data.Length, name);
        guarded.AsSpan(0, data.Length).CopyTo(data);
    }

    // Guard tail catches an ABI mismatch instead of parsing corrupted data.
    private void CallGuarded(uint id, byte[] guarded, int length, string name)
    {
        guarded.AsSpan(length).Fill(0xcd);
        var pin = GCHandle.Alloc(guarded, GCHandleType.Pinned);
        int status;
        try { status = Resolve<BufferCall>(id)(gpu, pin.AddrOfPinnedObject()); }
        finally { pin.Free(); }
        if (guarded.AsSpan(length).IndexOfAnyExcept((byte)0xcd) >= 0) throw Unsupported($"{name} buffer overrun");
        Check(status, name);
    }

    private string ReadString(uint id)
    {
        var data = new byte[64]; Call(id, data, "GPU identity string");
        int end = Array.IndexOf(data, (byte)0);
        return Encoding.ASCII.GetString(data, 0, end < 0 ? data.Length : end);
    }
    private static byte[] NewBuffer(int size, uint version) { var b = new byte[size]; Put(b, 0, version); return b; }
    private static uint U32(byte[] b, int p) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(p));
    private static void Put(byte[] b, int p, uint v) => BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(p), v);
    private static void RequireVersion(byte[] b, uint v) { if (U32(b, 0) != v) throw Unsupported("structure version"); }
    private static bool ValidRange(uint min, uint def, uint max) => min <= def && def <= max && max != 0 && max <= int.MaxValue;
    private static void RequireRange(uint min, uint def, uint max) { if (!ValidRange(min, def, max)) throw Unsupported("policy range"); }
    private static Exception Unsupported(string detail) => new NotSupportedException($"Unsupported NVIDIA {detail}.");
    private static void Check(int status, string name) { if (status != 0) throw new InvalidOperationException($"NVIDIA {name} failed (status {status})."); }

    public void Dispose() => Close();
    private void Close()
    {
        CloseBudget();
        CloseInlinePower();
        nvml?.Dispose(); nvml = null;
        if (initialized && query != null) { initialized = false; var p = query(0xd22bdd7e); if (p != IntPtr.Zero) Marshal.GetDelegateForFunctionPointer<NoArgs>(p)(); }
        gpu = IntPtr.Zero; query = null;
        if (library != IntPtr.Zero) { NativeLibrary.Free(library); library = IntPtr.Zero; }
    }
}
