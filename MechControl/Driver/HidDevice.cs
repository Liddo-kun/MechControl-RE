using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace MechControl.Driver;

/// <summary>
/// Enumerates ITE Tech USB HID devices and sends feature reports for lighting control.
/// Each ITE device exposes multiple HID interfaces (one per zone).
/// </summary>
sealed class HidDevice : IDisposable
{
    private readonly object _lock = new();
    private readonly SafeFileHandle[] _zoneHandles = new SafeFileHandle[4];
    private bool _disposed;

    // ITE Tech VID
    private const ushort ITE_VID = 0x048D;

    // HIDP_STATUS_SUCCESS is an NTSTATUS, not zero
    private const int HIDP_STATUS_SUCCESS = 0x00110000;

    #region P/Invoke

    [DllImport("hid.dll", SetLastError = true)]
    private static extern void HidD_GetHidGuid(out Guid hidGuid);

    [DllImport("hid.dll", SetLastError = true)]
    private static extern bool HidD_GetAttributes(SafeFileHandle HidDeviceObject, ref HIDD_ATTRIBUTES Attributes);

    [DllImport("hid.dll", SetLastError = true)]
    private static extern bool HidD_SetFeature(SafeFileHandle HidDeviceObject, byte[] ReportBuffer, int ReportBufferLength);

    [DllImport("hid.dll", SetLastError = true)]
    private static extern bool HidD_GetFeature(SafeFileHandle HidDeviceObject, byte[] ReportBuffer, int ReportBufferLength);

    [DllImport("hid.dll", SetLastError = true)]
    private static extern bool HidD_GetPreparsedData(SafeFileHandle HidDeviceObject, out IntPtr PreparsedData);

    [DllImport("hid.dll", SetLastError = true)]
    private static extern bool HidD_FreePreparsedData(IntPtr PreparsedData);

    [DllImport("hid.dll", SetLastError = true)]
    private static extern int HidP_GetCaps(IntPtr PreparsedData, out HIDP_CAPS Capabilities);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr SetupDiGetClassDevs(
        ref Guid ClassGuid, IntPtr Enumerator, IntPtr hwndParent, uint Flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiEnumDeviceInterfaces(
        IntPtr DeviceInfoSet, IntPtr DeviceInfoData, ref Guid InterfaceClassGuid,
        uint MemberIndex, ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool SetupDiGetDeviceInterfaceDetail(
        IntPtr DeviceInfoSet, ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
        IntPtr DeviceInterfaceDetailData, uint DeviceInterfaceDetailDataSize,
        out uint RequiredSize, IntPtr DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern SafeFileHandle CreateFile(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    private const uint DIGCF_PRESENT = 0x02;
    private const uint DIGCF_DEVICEINTERFACE = 0x10;
    private const uint GENERIC_READ = 0x80000000;
    private const uint GENERIC_WRITE = 0x40000000;
    private const uint FILE_SHARE_READ = 0x01;
    private const uint FILE_SHARE_WRITE = 0x02;
    private const uint OPEN_EXISTING = 0x03;

    [StructLayout(LayoutKind.Sequential)]
    private struct HIDD_ATTRIBUTES
    {
        public int Size;
        public ushort VendorID;
        public ushort ProductID;
        public ushort VersionNumber;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct HIDP_CAPS
    {
        public ushort Usage;
        public ushort UsagePage;
        public ushort InputReportByteLength;
        public ushort OutputReportByteLength;
        public ushort FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)]
        public ushort[] Reserved;
        public ushort NumberLinkCollectionNodes;
        public ushort NumberInputButtonCaps;
        public ushort NumberInputValueCaps;
        public ushort NumberInputDataIndices;
        public ushort NumberOutputButtonCaps;
        public ushort NumberOutputValueCaps;
        public ushort NumberOutputDataIndices;
        public ushort NumberFeatureButtonCaps;
        public ushort NumberFeatureValueCaps;
        public ushort NumberFeatureDataIndices;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SP_DEVICE_INTERFACE_DATA
    {
        public int cbSize;
        public Guid InterfaceClassGuid;
        public int Flags;
        public IntPtr Reserved;
    }

    #endregion

    /// <summary>
    /// Enumerate all ITE HID device paths (VID=0x048D).
    /// </summary>
    private static List<string> FindIteDevicePaths()
    {
        var paths = new List<string>();

        HidD_GetHidGuid(out Guid hidGuid);
        IntPtr devInfoSet = SetupDiGetClassDevs(ref hidGuid, IntPtr.Zero, IntPtr.Zero,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

        if (devInfoSet == new IntPtr(-1))
            return paths;

        try
        {
            var interfaceData = new SP_DEVICE_INTERFACE_DATA();
            interfaceData.cbSize = Marshal.SizeOf(interfaceData);

            for (uint i = 0; SetupDiEnumDeviceInterfaces(devInfoSet, IntPtr.Zero, ref hidGuid, i, ref interfaceData); i++)
            {
                // Get required size
                SetupDiGetDeviceInterfaceDetail(devInfoSet, ref interfaceData, IntPtr.Zero, 0, out uint requiredSize, IntPtr.Zero);

                // Allocate and fill detail struct
                IntPtr detailData = Marshal.AllocHGlobal((int)requiredSize);
                try
                {
                    // Set cbSize for SP_DEVICE_INTERFACE_DETAIL_DATA (varies by platform)
                    Marshal.WriteInt32(detailData, IntPtr.Size == 8 ? 8 : 6);

                    if (!SetupDiGetDeviceInterfaceDetail(devInfoSet, ref interfaceData, detailData, requiredSize, out _, IntPtr.Zero))
                        continue;

                    // DevicePath starts at offset 4
                    string devicePath = Marshal.PtrToStringUni(detailData + 4) ?? "";
                    if (string.IsNullOrEmpty(devicePath))
                        continue;

                    // Filter by VID
                    if (devicePath.Contains("vid_048d", StringComparison.OrdinalIgnoreCase))
                        paths.Add(devicePath);
                }
                finally
                {
                    Marshal.FreeHGlobal(detailData);
                }
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(devInfoSet);
        }

        return paths;
    }

    /// <summary>
    /// Open all ITE HID devices and identify zones by feature report length.
    /// Devices with 9-byte feature reports are lighting controllers.
    /// Zone assignment: sort qualifying devices by path and assign zones 0,1,2,3 in order.
    /// </summary>
    public bool Open()
    {
        lock (_lock)
        {
            var itePaths = FindIteDevicePaths();
            DiagnosticLog.Write($"[HID] Found {itePaths.Count} ITE HID device paths");

            // Probe each device: get feature report length and usage to identify zone mapping
            var lightingDevices = new List<(string path, ushort usage)>();
            foreach (string path in itePaths)
            {
                DiagnosticLog.Write($"[HID]   Probing: {path}");

                // Open with zero access for probing — some HID devices reject
                // GENERIC_READ if the OS input stack holds them exclusively
                SafeFileHandle? probeHandle = null;
                bool opened = false;
                foreach (uint access in new[] { 0u, GENERIC_READ | GENERIC_WRITE, GENERIC_WRITE })
                {
                    probeHandle = CreateFile(path, access,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
                    if (!probeHandle.IsInvalid)
                    {
                        opened = true;
                        break;
                    }
                    probeHandle.Dispose();
                }

                if (!opened || probeHandle == null || probeHandle.IsInvalid)
                {
                    int err = Marshal.GetLastWin32Error();
                    DiagnosticLog.Write($"[HID]     Cannot open (error {err})");
                    continue;
                }

                try
                {
                    if (!HidD_GetPreparsedData(probeHandle, out IntPtr preparsed))
                    {
                        DiagnosticLog.Write("[HID]     GetPreparsedData failed");
                        continue;
                    }

                    try
                    {
                        int status = HidP_GetCaps(preparsed, out HIDP_CAPS caps);
                        DiagnosticLog.Write($"[HID]     GetCaps status=0x{status:X8} feature={caps.FeatureReportByteLength} input={caps.InputReportByteLength} output={caps.OutputReportByteLength} usage=0x{caps.Usage:X4} usagePage=0x{caps.UsagePage:X4}");

                        if (status == HIDP_STATUS_SUCCESS && caps.FeatureReportByteLength == 9)
                        {
                            lightingDevices.Add((path, caps.Usage));
                            DiagnosticLog.Write("[HID]     -> Lighting controller");
                        }
                    }
                    finally
                    {
                        HidD_FreePreparsedData(preparsed);
                    }
                }
                catch (Exception ex)
                {
                    DiagnosticLog.Write($"[HID]     Exception: {ex.Message}");
                }
                finally
                {
                    probeHandle.Dispose();
                }
            }

            DiagnosticLog.Write($"[HID] {lightingDevices.Count} lighting devices (9-byte feature reports)");

            // Map zones by usage value:
            //   usage 0x0001 → zone 0 (keyboard), zone 1 (secondary)
            //   usage 0x0002 → zone 2 (light bar), zone 3 (logo)
            // Fallback: if usages don't match, assign sequentially.
            // Sort by path first — enumeration order is arbitrary, and with
            // first-wins TryAdd below, two interfaces sharing a usage value
            // would otherwise make the zone pick nondeterministic.
            lightingDevices.Sort((a, b) => StringComparer.OrdinalIgnoreCase.Compare(a.path, b.path));
            var byUsage = new Dictionary<ushort, string>();
            foreach (var (path, usage) in lightingDevices)
                byUsage.TryAdd(usage, path);

            bool usageMapped = false;
            if (byUsage.ContainsKey(0x0001) || byUsage.ContainsKey(0x0002))
            {
                usageMapped = true;
                if (byUsage.TryGetValue(0x0001, out string? kbPath))
                {
                    _zoneHandles[0] = OpenWriteHandle(kbPath);
                    DiagnosticLog.Write($"[HID]   Zone 0 (keyboard): {(_zoneHandles[0] != null && !_zoneHandles[0].IsInvalid ? "opened" : "FAILED")}");
                }
                if (byUsage.TryGetValue(0x0002, out string? barPath))
                {
                    _zoneHandles[2] = OpenWriteHandle(barPath);
                    _zoneHandles[3] = OpenWriteHandle(barPath); // logo = same device, separate handle
                    DiagnosticLog.Write($"[HID]   Zone 2 (light bar): {(_zoneHandles[2] != null && !_zoneHandles[2].IsInvalid ? "opened" : "FAILED")}");
                    DiagnosticLog.Write($"[HID]   Zone 3 (logo): {(_zoneHandles[3] != null && !_zoneHandles[3].IsInvalid ? "opened" : "FAILED")}");
                }
            }

            if (!usageMapped)
            {
                // Fallback: assign sequentially
                var sorted = lightingDevices.OrderBy(d => d.path, StringComparer.OrdinalIgnoreCase).ToList();
                for (int z = 0; z < Math.Min(sorted.Count, 4); z++)
                {
                    _zoneHandles[z] = OpenWriteHandle(sorted[z].path);
                    DiagnosticLog.Write($"[HID]   Zone {z}: {(_zoneHandles[z] != null && !_zoneHandles[z].IsInvalid ? "opened" : "FAILED")}");
                }
            }

            return _zoneHandles.Any(h => h != null && !h.IsInvalid);
        }
    }

    private static SafeFileHandle OpenWriteHandle(string path)
    {
        var h = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (h.IsInvalid)
        {
            h.Dispose();
            h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        }
        return h;
    }

    /// <summary>
    /// Check if a specific zone handle is available.
    /// </summary>
    public bool IsZoneAvailable(int zone) =>
        zone >= 0 && zone < 4 && _zoneHandles[zone] != null && !_zoneHandles[zone].IsInvalid;

    /// <summary>
    /// Send a 9-byte feature report to the specified zone's HID device.
    /// </summary>
    public void SendFeatureReport(int zone, byte[] report)
    {
        if (report.Length != 9)
            throw new ArgumentException("Feature report must be exactly 9 bytes");
        if (!IsZoneAvailable(zone))
            throw new InvalidOperationException($"HID zone {zone} is not available");

        lock (_lock)
        {
            if (!HidD_SetFeature(_zoneHandles[zone], report, report.Length))
            {
                int err = Marshal.GetLastWin32Error();
                throw new IOException($"HidD_SetFeature failed for zone {zone} (error {err})");
            }
        }
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            _disposed = true;
            foreach (var h in _zoneHandles)
                h?.Dispose();
        }
    }
}
