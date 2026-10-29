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

    // Only managed callbacks are supplied to the production marshaling path.
    // Skip NvidiaApi's constructor entirely: no native library, NVML, GPU,
    // registry or SetupAPI is opened. Exact private delegate types avoid
    // Marshal's function-pointer cache returning a different delegate type.
    private sealed class CurrentPolicyDriver
    {
        public readonly NvidiaApi Api;
        public readonly byte[] Info = new byte[0xa70], Monitor = new byte[0xca8];
        private readonly byte[] control = Enumerable.Range(0, 0xa4c).Select(i => (byte)(i * 37 + 11)).ToArray();
        private readonly List<Delegate> callbacks = [];
        private readonly Dictionary<uint, IntPtr> functions = [];
        public readonly int NvIndex, MsIndex, InputIndex, InputChannel;
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
            Api = (NvidiaApi)RuntimeHelpers.GetUninitializedObject(typeof(NvidiaApi));
            var queryType = typeof(NvidiaApi).GetNestedType("QueryInterface", BindingFlags.NonPublic)!;
            var query = Delegate.CreateDelegate(queryType, this, GetType().GetMethod(nameof(Query), BindingFlags.NonPublic | BindingFlags.Instance)!);
            callbacks.Add(query);
            typeof(NvidiaApi).GetField("query", BindingFlags.NonPublic | BindingFlags.Instance)!.SetValue(Api, query);
        }

        public static void Put(byte[] bytes, int offset, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset), value);
        public void Policy(int index, uint type, int channel, uint min, uint def, uint max)
        {
            Put(Info, 4, BinaryPrimitives.ReadUInt32LittleEndian(Info.AsSpan(4)) | (1u << index));
            int p = 0x3c + index * 0x58;
            Put(Info, p, type); Put(Info, p + 4, (uint)channel); Put(Info, p + 8, 1);
            Put(Info, p + 12, min); Put(Info, p + 16, def); Put(Info, p + 20, max);
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
