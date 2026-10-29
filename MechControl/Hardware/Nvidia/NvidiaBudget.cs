using System.Runtime.InteropServices;

namespace MechControl.Hardware.Nvidia;

// Laptop GPU budget (F7) and the shared CPU+GPU platform budget.
internal sealed record NvidiaBudgetSnapshot(uint F7Mw, uint PlatformMw)
{
    public NvidiaBudgetSnapshot RaiseTo(uint requested, NvidiaBudgetSnapshot baseline)
    {
        uint f7 = Math.Max(baseline.F7Mw, requested);
        // Always derive shared headroom from the baseline. Firmware can reset F7
        // alone; repeatedly adding its delta would inflate the total budget.
        return this with { F7Mw = f7, PlatformMw = checked(baseline.PlatformMw + (f7 - baseline.F7Mw)) };
    }
}

internal sealed partial class NvidiaApi
{
    private NvidiaBudgetTransport? budgetTransport;
    private bool platformInitialized;
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int DriverTypeCall(uint driverType);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int PlatformCall(IntPtr data);

    /// <summary>Read-only snapshot. InitializeEx(1) opens the platform driver, paired with UnloadEx(1).</summary>
    public NvidiaBudgetSnapshot ReadPowerBudget()
    {
        InitializeBudget();
        return new(budgetTransport!.Read(gpu, ReadBoard().Index), U32(ReadPlatformBudget(), 0x2c));
    }

    /// <summary>Change laptop F7 and platform headroom. The caller keeps the originals and coordinates F8.</summary>
    public void WritePowerBudget(NvidiaBudgetSnapshot desired)
    {
        var current = ReadPowerBudget();
        var board = ReadBoard();
        if (desired.F7Mw < board.Min || desired.F7Mw > board.Max || desired.PlatformMw is 0 or uint.MaxValue)
            throw new InvalidOperationException("NVIDIA budget target is outside the live range.");
        // Expand shared headroom before raising GPU demand; reduce GPU demand before shrinking headroom.
        if (desired.PlatformMw > current.PlatformMw) WritePlatformBudget(desired.PlatformMw);
        if (desired.F7Mw != current.F7Mw) budgetTransport!.Write(gpu, board.Index, desired.F7Mw);
        if (desired.PlatformMw < current.PlatformMw) WritePlatformBudget(desired.PlatformMw);
    }

    private void InitializeBudget()
    {
        if (query == null) throw new ObjectDisposedException(nameof(NvidiaApi));
        budgetTransport ??= new NvidiaBudgetTransport(query(0x33ab0353), query(0x17695269));
        if (!platformInitialized)
        {
            // NV_DRIVER_TYPE: DISPLAY=0, PLATFORM=1. This is driver-session initialization,
            // not DynamicBoostSetStatus or a controller write. Header + wrapper refcounts verified.
            _ = Resolve<DriverTypeCall>(0xd7c61344);
            Check(Resolve<DriverTypeCall>(0xad298d3f)(1), "InitializeEx(platform)");
            platformInitialized = true;
        }
    }

    private byte[] ReadPlatformBudget()
    {
        var data = new byte[0xc88]; Put(data, 0, 1); Put(data, 4, 0x10c88);
        CallPlatform(0x93456591, data, "PCF_ControllerGetControl(V1)");
        if (U32(data, 0) != 1 || U32(data, 4) != 0x10c88 || data[8] != 1 || U32(data, 0x2c) is 0 or uint.MaxValue)
            throw Unsupported("PCF QBOOST controller/version/target");
        return data;
    }

    private void WritePlatformBudget(uint target)
    {
        var data = ReadPlatformBudget();
        // Preserve all GET metadata and unrelated controller fields; only targetTppmW changes.
        Put(data, 0x2c, target);
        CallPlatform(0xb78734ab, data, "PCF_ControllerSetControl(V1)");
        if (U32(ReadPlatformBudget(), 0x2c) != target) throw new InvalidOperationException("NVIDIA platform budget did not retain the exact target.");
    }

    private void CallPlatform(uint id, byte[] data, string name)
    {
        var guarded = new byte[data.Length + 8192]; guarded.AsSpan().Fill(0xcd); data.CopyTo(guarded, 4096);
        var pin = GCHandle.Alloc(guarded, GCHandleType.Pinned);
        int status;
        try { status = Resolve<PlatformCall>(id)(pin.AddrOfPinnedObject() + 4096); }
        finally { pin.Free(); }
        if (guarded.AsSpan(0, 4096).IndexOfAnyExcept((byte)0xcd) >= 0 || guarded.AsSpan(4096 + data.Length).IndexOfAnyExcept((byte)0xcd) >= 0)
            throw Unsupported($"{name} buffer overrun");
        Check(status, name); guarded.AsSpan(4096, data.Length).CopyTo(data);
    }

    private void CloseBudget()
    {
        budgetTransport = null;
        if (!platformInitialized) return;
        platformInitialized = false;
        if (query != null)
        {
            var unload = query(0xd7c61344);
            if (unload != IntPtr.Zero) Marshal.GetDelegateForFunctionPointer<DriverTypeCall>(unload)(1);
        }
    }
}
