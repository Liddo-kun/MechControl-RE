using MechControl.Hardware.Nvidia;
using System.Buffers.Binary;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

static partial class Program
{
    private static void CurrentPolicyPacket()
    {
        foreach (bool alternate in new[] { false, true })
        {
            var fake = new CurrentPolicyDriver(alternate);
            var before = fake.Api.ReadCurrentLimits();
            Equal(new NvidiaCurrentLimit(1000, 120000, 170000, 125000), before.Nvvdd);
            Equal(new NvidiaCurrentLimit(1000, 30000, 45000, 32000), before.Msvdd);
            Equal(new NvidiaCurrentLimit(1, 9000, 15000, 9500), before.BoardInput);
            var after = fake.Api.SetCurrentLimits(null, null, 12500);
            Equal(1, fake.SetCount); Equal(12500u, after.BoardInput!.CurrentMa);
            Equal(before.Nvvdd, after.Nvvdd); Equal(before.Msvdd, after.Msvdd);
            byte[] expected = (byte[])fake.SetSource!.Clone();
            CurrentPolicyDriver.Put(expected, 4, 1u << fake.InputIndex);
            CurrentPolicyDriver.Put(expected, 0x20 + fake.InputIndex * 0x28, 12500);
            // Includes all header metadata, selected entry metadata, every
            // unselected entry, and the unused tail, not just parsed values.
            True(expected.AsSpan().SequenceEqual(fake.SetPacket));
            GC.KeepAlive(fake);
        }
    }

    private static void CurrentPolicyRefusals()
    {
        foreach (uint target in new uint[] { 0, 15001, uint.MaxValue })
        {
            var fake = new CurrentPolicyDriver();
            Throws<ArgumentOutOfRangeException>(() => fake.Api.SetCurrentLimits(null, null, target));
            Equal(0, fake.SetCount); GC.KeepAlive(fake);
        }
        Action<CurrentPolicyDriver>[] unsupported =
        [
            f => CurrentPolicyDriver.Put(f.Info, 4, f.RailMask), // absent
            f => CurrentPolicyDriver.Put(f.Info, f.InputInfo, 0x19), // unknown policy type
            f => CurrentPolicyDriver.Put(f.Info, f.InputInfo + 8, 0), // wrong unit
            f => CurrentPolicyDriver.Put(f.Monitor, f.InputMonitor + 12, 7), // unknown monitor kind
            f => CurrentPolicyDriver.Put(f.Monitor, 16, f.ChannelMask & ~(1u << f.InputChannel)),
            f => f.Policy(20, 4, f.InputChannel, 1, 9000, 15000), // ambiguous
            f => CurrentPolicyDriver.Put(f.Info, f.InputInfo + 16, 0), // default below minimum
        ];
        foreach (var mutation in unsupported)
        {
            // Only board input becomes unavailable; the rails still read and SET.
            var fake = new CurrentPolicyDriver(); mutation(fake);
            var limits = fake.Api.ReadCurrentLimits();
            True(limits.BoardInput == null && limits.Nvvdd.CurrentMa == 125000 && limits.Msvdd.CurrentMa == 32000);
            Throws<NotSupportedException>(() => fake.Api.SetCurrentLimits(null, null, 12000));
            Equal(0, fake.SetCount);
            Equal(126000u, fake.Api.SetCurrentLimits(126000, null, null).Nvvdd.CurrentMa);
            Equal(1, fake.SetCount); GC.KeepAlive(fake);
        }
    }

    private static void CurrentPolicyReadback()
    {
        foreach (bool changedRail in new[] { false, true })
        {
            var fake = new CurrentPolicyDriver { IgnoreSet = !changedRail, ChangeUnselectedRail = changedRail };
            Throws<InvalidOperationException>(() => fake.Api.SetCurrentLimits(null, null, 12000));
            Equal(1, fake.SetCount); GC.KeepAlive(fake);
        }
    }

    private static void Telemetry()
    {
        foreach (bool alternate in new[] { false, true })
        {
            var fake = new CurrentPolicyDriver(alternate); fake.AddTelemetry();
            var expected = new NvidiaTelemetry(126883, 47772, 8470, 152544, 59298, 2865000, 2940000, 3090000);
            Equal(expected, fake.Api.ReadTelemetry());
            Equal(expected, fake.Api.ReadTelemetry()); // Reused status buffer.
            GC.KeepAlive(fake);
        }
    }

    private static void TelemetryFailsClosed()
    {
        // The current controller's layout or identity doesn't check out: no predictions or cap, the rest stays.
        Action<CurrentPolicyDriver>[] current =
        [
            f => CurrentPolicyDriver.Put(f.Status, f.Union(f.CurrentIndex) + 0x60, 1), // observed total
            f => CurrentPolicyDriver.Put(f.Status, f.Union(f.CurrentIndex) + 0x15d0, 1), // predicted total
            f => CurrentPolicyDriver.Put(f.Status, f.Union(f.CurrentIndex) + 4, 3100000), // cap above its ceiling
            f => CurrentPolicyDriver.Put(f.Status, f.Union(f.CurrentIndex) + 4, 0),
            f => CurrentPolicyDriver.Put(f.Status, f.StatusEntry(f.CurrentIndex), 0x14), // other policy type
            f => f.Controller(f.CurrentIndex, 2865000, 47772, 126883, 59298, 152544), // rails in the other order
            f => { f.Policy(25, 0x15, 26, 1, 5001000, 5001000); f.Controller(25, 2865000, 126883, 47772, 152544, 59298); }, // ambiguous
        ];
        foreach (var mutation in current)
        {
            var fake = new CurrentPolicyDriver(); fake.AddTelemetry(); mutation(fake);
            var t = fake.Api.ReadTelemetry();
            True(t.NvvddPredictedMa == null && t.MsvddPredictedMa == null && t.CurrentCapKhz == null);
            True(t.NvvddMa == 126883 && t.MsvddMa == 47772 && t.BoardInputMa == 8470 && t.PowerCapKhz == 2940000 && t.CeilingKhz == 3090000);
            GC.KeepAlive(fake);
        }
        // No unique board power policy feeding a controller: no power cap.
        Action<CurrentPolicyDriver>[] power =
        [
            f => f.Arbitration(f.PowerIndex, 254),
            f => f.Status[f.StatusEntry(f.PowerIndex) + 0xe1] = 9, // more inputs than the structure holds
            f => { f.Policy(20, 0, 0, 5000, 95000, 250000, unit: 0); f.StatusValue(20, 0, 1000); }, // ambiguous board power
            f => CurrentPolicyDriver.Put(f.Info, 0x3c + f.BoardPowerIndex * 0x58 + 8, 1), // board power in mA
            f => f.StatusValue(f.BoardPowerIndex, 0x15, 173336), // status disagrees with INFO
        ];
        foreach (var mutation in power)
        {
            var fake = new CurrentPolicyDriver(); fake.AddTelemetry(); mutation(fake);
            var t = fake.Api.ReadTelemetry();
            True(t.PowerCapKhz == null && t.CurrentCapKhz == 2865000 && t.MsvddPredictedMa == 59298);
            GC.KeepAlive(fake);
        }
        var input = new CurrentPolicyDriver(); input.AddTelemetry(); input.StatusValue(input.InputIndex, 0x15, 8470);
        True(input.Api.ReadTelemetry() is { BoardInputMa: null, NvvddMa: 126883, CurrentCapKhz: 2865000 });
        var rail = new CurrentPolicyDriver(); rail.AddTelemetry(); rail.StatusValue(rail.NvIndex, 4, 126883);
        Throws<NotSupportedException>(() => rail.Api.ReadTelemetry());
        GC.KeepAlive(input); GC.KeepAlive(rail);
    }

    // Only managed callbacks are supplied to the production marshaling path.
    // Skip NvidiaApi's constructor entirely: no native library, NVML, GPU,
    // registry or SetupAPI is opened. Exact private delegate types avoid
    // Marshal's function-pointer cache returning a different delegate type.
    private sealed class CurrentPolicyDriver
    {
        public readonly NvidiaApi Api;
        public readonly byte[] Info = new byte[0xa70], Monitor = new byte[0xca8], Status = new byte[0xb00b8];
        private readonly byte[] control = Enumerable.Range(0, 0xa4c).Select(i => (byte)(i * 37 + 11)).ToArray();
        private readonly List<Delegate> callbacks = [];
        private readonly Dictionary<uint, IntPtr> functions = [];
        public readonly int NvIndex, MsIndex, InputIndex, InputChannel;
        public readonly int BoardPowerIndex, CurrentIndex, PowerIndex;
        public uint RailMask => (1u << NvIndex) | (1u << MsIndex);
        public uint ChannelMask { get; }
        public int InputInfo => 0x3c + InputIndex * 0x58;
        public int InputMonitor => 0x28 + InputChannel * 0x3c;
        public int SetCount { get; private set; }
        public byte[]? SetSource { get; private set; }
        public byte[]? SetPacket { get; private set; }
        private byte[]? lastGet;
        public bool IgnoreSet, ChangeUnselectedRail;

        public CurrentPolicyDriver(bool alternate = false)
        {
            (NvIndex, MsIndex, InputIndex) = alternate ? (17, 2, 21) : (3, 11, 6);
            (BoardPowerIndex, CurrentIndex, PowerIndex) = alternate ? (9, 4, 28) : (1, 12, 0);
            (int nvChannel, int msChannel, int inputChannel) = alternate ? (15, 4, 27) : (2, 19, 13);
            InputChannel = inputChannel;
            ChannelMask = (1u << nvChannel) | (1u << msChannel) | (1u << inputChannel);
            Put(Info, 0, 0x40a70); Put(Monitor, 0, 0x30ca8); Put(Monitor, 16, ChannelMask);
            Policy(NvIndex, 19, nvChannel, 1000, 120000, 170000);
            Policy(MsIndex, 19, msChannel, 1000, 30000, 45000);
            Policy(InputIndex, 4, inputChannel, 1, 9000, 15000);
            MonitorChannel(nvChannel, 6, 1); MonitorChannel(msChannel, 6, 16); MonitorChannel(inputChannel, 8, 0xda);
            Put(control, 0, 0x10a4c);
            ControlEntry(NvIndex, 19, 125000); ControlEntry(MsIndex, 19, 32000); ControlEntry(InputIndex, 4, 9500);
            Bind(0x67f31384, nameof(GetInfo)); Bind(0xc12eb19e, nameof(GetMonitor));
            Bind(0x8b3e7343, nameof(GetControl)); Bind(0xaffc2279, nameof(SetControl));
            Put(Status, 0, 0xb00b8); Bind(0x31b7a4cd, nameof(GetStatus));
            Api = (NvidiaApi)RuntimeHelpers.GetUninitializedObject(typeof(NvidiaApi));
            var queryType = typeof(NvidiaApi).GetNestedType("QueryInterface", BindingFlags.NonPublic)!;
            var query = Delegate.CreateDelegate(queryType, this, GetType().GetMethod(nameof(Query), BindingFlags.NonPublic | BindingFlags.Instance)!);
            callbacks.Add(query);
            typeof(NvidiaApi).GetField("query", BindingFlags.NonPublic | BindingFlags.Instance)!.SetValue(Api, query);
        }

        public static void Put(byte[] bytes, int offset, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset), value);
        public void Policy(int index, uint type, int channel, uint min, uint def, uint max, uint unit = 1)
        {
            Put(Info, 4, BinaryPrimitives.ReadUInt32LittleEndian(Info.AsSpan(4)) | (1u << index));
            int p = 0x3c + index * 0x58;
            Put(Info, p, type); Put(Info, p + 4, (uint)channel); Put(Info, p + 8, unit);
            Put(Info, p + 12, min); Put(Info, p + 16, def); Put(Info, p + 20, max);
        }

        // Live status as read on Jon's GPU (driver 617.14), at this fake's policy indices.
        public void AddTelemetry()
        {
            Policy(BoardPowerIndex, 0, 0, 5000, 95000, 250000, unit: 0);
            Policy(CurrentIndex, 0x15, 26, 1, 5001000, 5001000);
            Policy(PowerIndex, 0x15, 5, 1, 5001000, 5001000, unit: 0);
            StatusValue(NvIndex, 19, 126883); StatusValue(MsIndex, 19, 47772); StatusValue(InputIndex, 4, 8470);
            StatusValue(BoardPowerIndex, 0, 173336);
            Controller(CurrentIndex, 2865000, 126883, 47772, 152544, 59298);
            Controller(PowerIndex, 2940000, 85576, 47526, 121715, 71465);
            Arbitration(PowerIndex, 254, BoardPowerIndex);
        }
        public int StatusEntry(int index) => 0x11b8 + index * 0x3768;
        public int Union(int index) => StatusEntry(index) + 0x968;
        public void StatusValue(int index, uint type, uint value) { Put(Status, StatusEntry(index), type); Put(Status, StatusEntry(index) + 8, value); }
        public void Controller(int index, uint capKhz, uint observed0, uint observed1, uint predicted0, uint predicted1)
        {
            int u = Union(index);
            Put(Status, StatusEntry(index), 0x15); Put(Status, u + 4, capKhz); Put(Status, u + 0x24, 3090000);
            Put(Status, u + 0x60, observed0 + observed1); Put(Status, u + 0x134, observed0); Put(Status, u + 0xb84, observed1);
            Put(Status, u + 0x15d0, predicted0 + predicted1); Put(Status, u + 0x1690, predicted0); Put(Status, u + 0x1e70, predicted1);
        }
        public void Arbitration(int index, params int[] sources)
        {
            int a = StatusEntry(index) + 0xe0;
            Status[a + 1] = (byte)sources.Length;
            for (int k = 0; k < sources.Length; k++) Status[a + 0x48 + k * 8] = (byte)sources[k];
        }
        private void MonitorChannel(int channel, uint kind, uint rail)
        { int p = 0x28 + channel * 0x3c; Put(Monitor, p + 12, kind); Put(Monitor, p + 16, rail); }
        private void ControlEntry(int index, uint type, uint value)
        { int p = 0x1c + index * 0x28; Put(control, p, type); Put(control, p + 4, value); }
        private void Bind(uint id, string method)
        {
            var type = typeof(NvidiaApi).GetNestedType("BufferCall", BindingFlags.NonPublic)!;
            var callback = Delegate.CreateDelegate(type, this, GetType().GetMethod(method, BindingFlags.NonPublic | BindingFlags.Instance)!);
            callbacks.Add(callback); functions.Add(id, Marshal.GetFunctionPointerForDelegate(callback));
        }
        private IntPtr Query(uint id) => functions.TryGetValue(id, out var address) ? address : IntPtr.Zero;
        private int GetInfo(IntPtr gpu, IntPtr data) { Marshal.Copy(Info, 0, data, Info.Length); return 0; }
        private int GetMonitor(IntPtr gpu, IntPtr data) { Marshal.Copy(Monitor, 0, data, Monitor.Length); return 0; }
        private int GetStatus(IntPtr gpu, IntPtr data) { Marshal.Copy(Status, 0, data, Status.Length); return 0; }
        private int GetControl(IntPtr gpu, IntPtr data)
        {
            byte[] result = (byte[])control.Clone();
            Put(result, 4, unchecked((uint)Marshal.ReadInt32(data, 4)));
            lastGet = result; Marshal.Copy(result, 0, data, result.Length); return 0;
        }
        private int SetControl(IntPtr gpu, IntPtr data)
        {
            SetCount++; SetSource = lastGet;
            var packet = new byte[control.Length]; Marshal.Copy(data, packet, 0, packet.Length); SetPacket = packet;
            uint mask = BinaryPrimitives.ReadUInt32LittleEndian(packet.AsSpan(4));
            if (!IgnoreSet)
                for (int i = 0; i < 32; i++)
                    if ((mask & (1u << i)) != 0) packet.AsSpan(0x20 + i * 0x28, 4).CopyTo(control.AsSpan(0x20 + i * 0x28));
            if (ChangeUnselectedRail) Put(control, 0x20 + NvIndex * 0x28, 126000);
            return 0;
        }
    }
}
