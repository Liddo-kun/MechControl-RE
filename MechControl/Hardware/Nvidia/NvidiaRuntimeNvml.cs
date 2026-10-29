using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Text;

namespace MechControl.Hardware.Nvidia;

/// <summary>Read-only NVML identity and independently enforced power-limit telemetry. Never NVML SET.</summary>
internal sealed class NvidiaRuntimeNvml : IDisposable
{
    private IntPtr library, device;
    private bool initialized;
    public uint Domain { get; private set; }
    public uint Device { get; private set; }
    public uint Function { get; private set; }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int NoArgs();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Count(out uint count);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Handle(uint index, out IntPtr handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Pci(IntPtr handle, IntPtr info);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Limit(IntPtr handle, out uint value);

    public NvidiaRuntimeNvml(uint pciId, uint subsystem, uint bus)
    {
        try
        {
            library = NativeLibrary.Load(Path.Combine(Environment.SystemDirectory, "nvml.dll"));
            Check(Get<NoArgs>("nvmlInit_v2")()); initialized = true;
            Check(Get<Count>("nvmlDeviceGetCount_v2")(out uint count));
            for (uint i = 0; i < count; i++)
            {
                Check(Get<Handle>("nvmlDeviceGetHandleByIndex_v2")(i, out var candidate));
                var data = new byte[68];
                var pin = GCHandle.Alloc(data, GCHandleType.Pinned);
                try { Check(Get<Pci>("nvmlDeviceGetPciInfo_v3")(candidate, pin.AddrOfPinnedObject())); }
                finally { pin.Free(); }
                uint U(int offset) => BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(offset));
                if (U(20) != bus || U(28) != pciId || U(32) != subsystem) continue;
                if (device != IntPtr.Zero) throw new NotSupportedException("Ambiguous NVAPI/NVML PCI identity.");
                string bdf = Encoding.ASCII.GetString(data, 36, 32).TrimEnd('\0');
                int dot = bdf.LastIndexOf('.');
                if (dot < 0 || !uint.TryParse(bdf.AsSpan(dot + 1), out uint function) || function > 7 || U(24) > 31)
                    throw new NotSupportedException("Unsupported NVIDIA PCI function identity.");
                device = candidate; Domain = U(16); Device = U(24); Function = function;
            }
            if (device == IntPtr.Zero) throw new NotSupportedException("NVML adapter does not match NVAPI PCI identity.");
        }
        catch { Dispose(); throw; }
    }
    public uint ReadEnforcedPowerLimit()
    {
        if (!initialized) throw new ObjectDisposedException(nameof(NvidiaRuntimeNvml));
        Check(Get<Limit>("nvmlDeviceGetEnforcedPowerLimit")(device, out uint value));
        return value;
    }
    private T Get<T>(string name) where T : Delegate => Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(library, name));
    private static void Check(int status) { if (status != 0) throw new InvalidOperationException($"NVIDIA NVML read failed (status {status})."); }
    public void Dispose()
    {
        if (initialized) { initialized = false; Get<NoArgs>("nvmlShutdown")(); }
        if (library != IntPtr.Zero) { NativeLibrary.Free(library); library = IntPtr.Zero; }
    }
}
