using System.Buffers.Binary;
using System.Runtime.InteropServices;

namespace MechControl.Hardware.Nvidia;

/// <summary>Fail-closed discovery of the native F7 transport from the paired NVAPI wrappers.</summary>
internal sealed class NvidiaBudgetTransport
{
    [DllImport("ntdll.dll")] private static extern IntPtr RtlLookupFunctionEntry(ulong pc, out ulong imageBase, IntPtr history);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int Dispatch(uint escape, IntPtr data, uint size, ulong reserved, byte one, byte zero, uint flags);
    private readonly Dispatch dispatch;
    private readonly int size;

    public NvidiaBudgetTransport(IntPtr get, IntPtr set)
    {
        var g = Body(get); var s = Body(set);
        byte[] prefix = Convert.FromHexString("C744243000000000C644242800C6442420014533C941B8");
        int p = Find(g.Code, prefix) + prefix.Length;
        size = checked((int)U32(g.Code, p));
        // Both sizes use the full native header/12-byte source records. A new ABI is unsupported.
        if (size != 0x122d70 && size != 0x60b30) throw Unsupported();
        byte[] tail = Convert.FromHexString("498BD6B946000007E8");
        int q = p + 4;
        if (!g.Code.AsSpan(q, tail.Length).SequenceEqual(tail)) throw Unsupported();
        _ = Find(g.Code, Convert.FromHexString("45897E3041C7463432A68020"));
        _ = Find(s.Code, Convert.FromHexString("33E68020"));
        int t = Find(s.Code, tail);
        IntPtr target = get + q + tail.Length + 4 + BinaryPrimitives.ReadInt32LittleEndian(g.Code.AsSpan(q + tail.Length));
        IntPtr other = set + t + tail.Length + 4 + BinaryPrimitives.ReadInt32LittleEndian(s.Code.AsSpan(t + tail.Length));
        if (target != other || g.Image != s.Image || Body(target).Image != g.Image) throw Unsupported();
        dispatch = Marshal.GetDelegateForFunctionPointer<Dispatch>(target);
    }

    public uint Read(IntPtr gpu, int index) => Invoke(gpu, index, null);
    public void Write(IntPtr gpu, int index, uint value)
    {
        if (value is 0 or uint.MaxValue) throw Unsupported();
        _ = Invoke(gpu, index, value);
        if (Read(gpu, index) != value) throw new InvalidOperationException("NVIDIA F7 budget did not retain the exact target.");
    }

    private uint Invoke(IntPtr gpu, int index, uint? value)
    {
        if ((uint)index >= 32 || gpu.ToInt64() <= 0 || (ulong)gpu.ToInt64() > uint.MaxValue) throw Unsupported();
        uint handle = (uint)gpu.ToInt64(), method = value.HasValue ? 0x2080e633u : 0x2080a632u;
        int offset = 0x64 + index * 12;
        var guarded = new byte[size + 8192];
        guarded.AsSpan().Fill(0xcd);
        var data = guarded.AsSpan(4096, size); data.Clear();
        Put(data, 0x30, handle); Put(data, 0x34, method); Put(data, 0x3c, 1u << index);
        Put(data, offset, value ?? 0); Put(data, offset + 4, 0xf7);
        var pin = GCHandle.Alloc(guarded, GCHandleType.Pinned);
        int status;
        try { status = dispatch(0x7000046, pin.AddrOfPinnedObject() + 4096, (uint)size, 0, 1, 0, 0); }
        finally { pin.Free(); }
        if (guarded.AsSpan(0, 4096).IndexOfAnyExcept((byte)0xcd) >= 0 || guarded.AsSpan(4096 + size).IndexOfAnyExcept((byte)0xcd) >= 0
            || U32(data, 0x30) != handle || U32(data, 0x34) != method || U32(data, 0x3c) != 1u << index || U32(data, offset + 4) != 0xf7)
            throw Unsupported();
        // Native GET may return only the global and per-record status bytes outside the selection.
        for (int i = 0x38; i < size; i++)
            if (i != 0x38 && i != offset - 4 && i != offset - 3 && !(i >= 0x3c && i < 0x40)
                && !(i >= offset && i < offset + 8) && data[i] != 0) throw Unsupported();
        if (status != 0 || U32(data, 0x2c) != 1) throw new InvalidOperationException($"NVIDIA F7 transport failed ({status}, completion {U32(data, 0x2c)}).");
        uint result = U32(data, offset);
        if (result is 0 or uint.MaxValue) throw Unsupported();
        return result;
    }

    private static (byte[] Code, ulong Image) Body(IntPtr entry)
    {
        if (entry == IntPtr.Zero) throw Unsupported();
        var function = RtlLookupFunctionEntry((ulong)entry.ToInt64(), out ulong image, IntPtr.Zero);
        if (function == IntPtr.Zero) throw Unsupported();
        uint begin = (uint)Marshal.ReadInt32(function), end = (uint)Marshal.ReadInt32(function, 4);
        if (image + begin != (ulong)entry.ToInt64() || end <= begin || end - begin > 0x4000) throw Unsupported();
        var bytes = new byte[end - begin]; Marshal.Copy(entry, bytes, 0, bytes.Length);
        return (bytes, image);
    }
    private static int Find(byte[] bytes, byte[] pattern)
    {
        int found = -1;
        for (int i = 0; i <= bytes.Length - pattern.Length; i++)
            if (bytes.AsSpan(i, pattern.Length).SequenceEqual(pattern))
            { if (found >= 0) throw Unsupported(); found = i; }
        return found >= 0 ? found : throw Unsupported();
    }
    private static uint U32(ReadOnlySpan<byte> bytes, int offset) => BinaryPrimitives.ReadUInt32LittleEndian(bytes[offset..]);
    private static void Put(Span<byte> bytes, int offset, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(bytes[offset..], value);
    private static Exception Unsupported() => new NotSupportedException("Unsupported NVIDIA native F7 transport layout.");
}
