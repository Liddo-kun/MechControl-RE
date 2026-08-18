using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace MechControl.Hardware.PawnIo;

/// <summary>
/// Thin, dynamically loaded wrapper around the PawnIOLib user-mode API.
/// One client owns one PawnIO handle and one loaded module. The wrapper is
/// module-agnostic; no NVIDIA (or other device) semantics belong here.
/// </summary>
sealed class PawnIoClient : IPawnIoExecutor
{
    private const string PawnIoRegistryKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO";
    private const string PawnIoLibraryName = "PawnIOLib.dll";

    private readonly object _sync = new();
    private readonly IntPtr _libraryHandle;
    private readonly IntPtr _pawnIoHandle;
    private readonly PawnIoExecute _execute;
    private readonly PawnIoClose _close;
    private bool _disposed;

    public Version LibraryVersion { get; }

    private PawnIoClient(
        IntPtr libraryHandle,
        IntPtr pawnIoHandle,
        PawnIoExecute execute,
        PawnIoClose close,
        Version libraryVersion)
    {
        _libraryHandle = libraryHandle;
        _pawnIoHandle = pawnIoHandle;
        _execute = execute;
        _close = close;
        LibraryVersion = libraryVersion;
    }

    public static bool TryOpen(string modulePath, out PawnIoClient? client, out string reason)
    {
        client = null;
        if (!File.Exists(modulePath))
        {
            reason = $"module file not found: {modulePath}";
            return false;
        }

        string? libraryPath = FindLibraryPath();
        if (libraryPath == null)
        {
            reason = "PawnIO is not installed (PawnIOLib.dll not found)";
            return false;
        }

        IntPtr libraryHandle = IntPtr.Zero;
        IntPtr pawnIoHandle = IntPtr.Zero;
        PawnIoClose? close = null;

        try
        {
            libraryHandle = NativeLibrary.Load(libraryPath);
            var version = GetExport<PawnIoVersion>(libraryHandle, "pawnio_version");
            var open = GetExport<PawnIoOpen>(libraryHandle, "pawnio_open");
            var load = GetExport<PawnIoLoad>(libraryHandle, "pawnio_load");
            var execute = GetExport<PawnIoExecute>(libraryHandle, "pawnio_execute");
            close = GetExport<PawnIoClose>(libraryHandle, "pawnio_close");

            int hr = version(out uint packedVersion);
            if (Failed(hr))
                throw new InvalidOperationException($"pawnio_version failed (HRESULT 0x{hr:X8})");

            hr = open(out pawnIoHandle);
            if (Failed(hr) || pawnIoHandle == IntPtr.Zero)
                throw new InvalidOperationException($"pawnio_open failed (HRESULT 0x{hr:X8})");

            byte[] module = File.ReadAllBytes(modulePath);
            hr = load(pawnIoHandle, module, (nuint)module.Length);
            if (Failed(hr))
                throw new InvalidOperationException($"pawnio_load failed (HRESULT 0x{hr:X8})");

            var libraryVersion = new Version(
                (int)(packedVersion >> 16),
                (int)((packedVersion >> 8) & 0xFF),
                (int)(packedVersion & 0xFF));

            client = new PawnIoClient(libraryHandle, pawnIoHandle, execute, close, libraryVersion);
            reason = "available";
            return true;
        }
        catch (Exception ex)
        {
            if (pawnIoHandle != IntPtr.Zero && close != null)
            {
                try { close(pawnIoHandle); }
                catch { }
            }
            if (libraryHandle != IntPtr.Zero)
                NativeLibrary.Free(libraryHandle);

            reason = ex.Message;
            return false;
        }
    }

    public ulong[] Execute(string functionName, ulong[] input, int outputCount)
    {
        if (string.IsNullOrEmpty(functionName) || functionName.Length > 31)
            throw new ArgumentOutOfRangeException(nameof(functionName));
        if (outputCount < 0)
            throw new ArgumentOutOfRangeException(nameof(outputCount));

        lock (_sync)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);

            var output = new ulong[outputCount];
            int hr = _execute(
                _pawnIoHandle,
                functionName,
                input,
                (nuint)input.Length,
                output,
                (nuint)output.Length,
                out nuint returnedCount);
            if (Failed(hr))
                // Preserve the HRESULT so consumers can distinguish e.g.
                // STATUS_NOT_SUPPORTED (power-gated/unsupported device).
                throw new InvalidOperationException($"{functionName} failed (HRESULT 0x{hr:X8})")
                {
                    HResult = hr,
                };
            if (returnedCount > (nuint)output.Length)
                throw new InvalidDataException($"{functionName} returned an invalid output count ({returnedCount})");

            if (returnedCount != (nuint)output.Length)
                Array.Resize(ref output, (int)returnedCount);
            return output;
        }
    }

    public void Dispose()
    {
        lock (_sync)
        {
            if (_disposed) return;
            _disposed = true;
            try { _close(_pawnIoHandle); }
            finally { NativeLibrary.Free(_libraryHandle); }
        }
    }

    private static T GetExport<T>(IntPtr libraryHandle, string name) where T : Delegate =>
        Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(libraryHandle, name));

    private static bool Failed(int hresult) => hresult < 0;

    private static string? FindLibraryPath()
    {
        foreach (RegistryView view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            try
            {
                using RegistryKey baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view);
                using RegistryKey? key = baseKey.OpenSubKey(PawnIoRegistryKey);
                if (key?.GetValue("InstallLocation") is string installLocation && installLocation.Length > 0)
                {
                    string path = Path.Combine(installLocation, PawnIoLibraryName);
                    if (File.Exists(path)) return path;
                }
            }
            catch
            {
                // Fall through to the fixed installer location.
            }
        }

        string fallback = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            "PawnIO",
            PawnIoLibraryName);
        return File.Exists(fallback) ? fallback : null;
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int PawnIoVersion(out uint version);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int PawnIoOpen(out IntPtr handle);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int PawnIoLoad(IntPtr handle, [In] byte[] blob, nuint size);

    [UnmanagedFunctionPointer(CallingConvention.StdCall, CharSet = CharSet.Ansi)]
    private delegate int PawnIoExecute(
        IntPtr handle,
        [MarshalAs(UnmanagedType.LPStr)] string name,
        [In] ulong[] input,
        nuint inputCount,
        [Out] ulong[] output,
        nuint outputCount,
        out nuint returnedCount);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int PawnIoClose(IntPtr handle);
}
