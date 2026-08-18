using System.Runtime.InteropServices;

namespace MechControl.Hardware.PawnIo;

/// <summary>
/// Passive SetupAPI enumeration of present NVIDIA display-class PCI devices,
/// returning the bus/device/function candidates that PR #77's
/// <c>ioctl_read_thermal_registers</c> requires the consumer to supply.
/// Reads PnP properties only — never NVAPI/NVML/D3D — so discovery cannot
/// wake a power-gated discrete GPU.
/// </summary>
static class NvidiaPciDeviceLocator
{
    private const ushort NvidiaVendorId = 0x10DE;

    public static IReadOnlyList<PciBdf> FindCandidates()
    {
        var candidates = new List<PciBdf>();

        Guid displayClass = DisplayAdapterClassGuid;
        IntPtr devInfo = SetupDiGetClassDevsW(ref displayClass, "PCI", IntPtr.Zero, DIGCF_PRESENT);
        if (devInfo == INVALID_HANDLE_VALUE)
        {
            DiagnosticLog.Write($"[GPU Hot Spot] SetupDiGetClassDevs failed (Win32 {Marshal.GetLastWin32Error()})");
            return candidates;
        }

        try
        {
            var data = new SP_DEVINFO_DATA { cbSize = (uint)Marshal.SizeOf<SP_DEVINFO_DATA>() };
            for (uint index = 0; SetupDiEnumDeviceInfo(devInfo, index, ref data); index++)
            {
                string instanceId = GetInstanceId(devInfo, ref data) ?? "<unknown>";
                string? hardwareId = GetFirstHardwareId(devInfo, ref data);
                if (hardwareId == null
                    || !TryParseVendorDevice(hardwareId, out ushort vendorId, out ushort deviceId)
                    || vendorId != NvidiaVendorId)
                {
                    continue;
                }

                if (!TryGetDwordProperty(devInfo, ref data, SPDRP_BUSNUMBER, out uint busNumber)
                    || !TryGetDwordProperty(devInfo, ref data, SPDRP_ADDRESS, out uint address))
                {
                    DiagnosticLog.Write($"[GPU Hot Spot] {instanceId}: NVIDIA device without PCI bus/address properties; skipped");
                    continue;
                }

                if (!TryDecodeAddress(busNumber, address, vendorId, deviceId, out PciBdf bdf))
                {
                    DiagnosticLog.Write($"[GPU Hot Spot] {instanceId}: PCI address out of range " +
                        $"(bus=0x{busNumber:X}, address=0x{address:X8}); skipped");
                    continue;
                }

                DiagnosticLog.Write($"[GPU Hot Spot] candidate {vendorId:X4}:{deviceId:X4} at {bdf} ({instanceId})");
                candidates.Add(bdf);
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(devInfo);
        }

        if (candidates.Count == 0)
            DiagnosticLog.Write("[GPU Hot Spot] no NVIDIA display-class PCI device present");
        return candidates;
    }

    /// <summary>
    /// Extracts VEN_/DEV_ from a PnP hardware ID such as
    /// <c>PCI\VEN_10DE&amp;DEV_2C18&amp;SUBSYS_...</c>. Deterministic and
    /// covered by the probe self-test.
    /// </summary>
    internal static bool TryParseVendorDevice(string hardwareId, out ushort vendorId, out ushort deviceId)
    {
        vendorId = 0;
        deviceId = 0;
        return hardwareId.StartsWith(@"PCI\", StringComparison.OrdinalIgnoreCase)
            && TryParseHexField(hardwareId, "VEN_", out vendorId)
            && TryParseHexField(hardwareId, "DEV_", out deviceId);
    }

    /// <summary>
    /// Decodes the Windows PCI properties: SPDRP_BUSNUMBER is the bus, and
    /// SPDRP_ADDRESS packs (device &lt;&lt; 16) | function. Verified against
    /// Device Manager ("PCI bus B, device D, function F") during Phase 3
    /// validation; deterministic and covered by the probe self-test.
    /// </summary>
    internal static bool TryDecodeAddress(uint busNumber, uint address, ushort vendorId, ushort deviceId, out PciBdf bdf)
    {
        bdf = default;
        uint device = address >> 16;
        uint function = address & 0xFFFF;
        if (busNumber > 0xFF || device > 0x1F || function > 0x07)
            return false;

        bdf = new PciBdf((int)busNumber, (int)device, (int)function, vendorId, deviceId);
        return true;
    }

    private static bool TryParseHexField(string hardwareId, string prefix, out ushort value)
    {
        value = 0;
        int start = hardwareId.IndexOf(prefix, StringComparison.OrdinalIgnoreCase);
        if (start < 0 || start + prefix.Length + 4 > hardwareId.Length)
            return false;

        return ushort.TryParse(
            hardwareId.AsSpan(start + prefix.Length, 4),
            System.Globalization.NumberStyles.HexNumber,
            System.Globalization.CultureInfo.InvariantCulture,
            out value);
    }

    private static string? GetInstanceId(IntPtr devInfo, ref SP_DEVINFO_DATA data)
    {
        var buffer = new char[512];
        return SetupDiGetDeviceInstanceIdW(devInfo, ref data, buffer, (uint)buffer.Length, out uint required)
            ? new string(buffer, 0, (int)Math.Max(required, 1) - 1)
            : null;
    }

    private static string? GetFirstHardwareId(IntPtr devInfo, ref SP_DEVINFO_DATA data)
    {
        // SPDRP_HARDWAREID is REG_MULTI_SZ; the first (most specific) entry
        // carries VEN_/DEV_.
        var buffer = new byte[2048];
        if (!SetupDiGetDeviceRegistryPropertyW(devInfo, ref data, SPDRP_HARDWAREID,
                out _, buffer, (uint)buffer.Length, out uint size) || size < 2)
        {
            return null;
        }

        string multiSz = System.Text.Encoding.Unicode.GetString(buffer, 0, (int)size);
        int terminator = multiSz.IndexOf('\0');
        return terminator > 0 ? multiSz[..terminator] : null;
    }

    private static bool TryGetDwordProperty(IntPtr devInfo, ref SP_DEVINFO_DATA data, uint property, out uint value)
    {
        value = 0;
        var buffer = new byte[4];
        if (!SetupDiGetDeviceRegistryPropertyW(devInfo, ref data, property,
                out uint type, buffer, (uint)buffer.Length, out uint size)
            || type != REG_DWORD || size != 4)
        {
            return false;
        }

        value = BitConverter.ToUInt32(buffer, 0);
        return true;
    }

    // "Display adapters" setup class.
    private static readonly Guid DisplayAdapterClassGuid = new("4d36e968-e325-11ce-bfc1-08002be10318");

    private static readonly IntPtr INVALID_HANDLE_VALUE = new(-1);
    private const uint DIGCF_PRESENT = 0x02;
    private const uint SPDRP_HARDWAREID = 0x01;
    private const uint SPDRP_BUSNUMBER = 0x15;
    private const uint SPDRP_ADDRESS = 0x1C;
    private const uint REG_DWORD = 4;

    [StructLayout(LayoutKind.Sequential)]
    private struct SP_DEVINFO_DATA
    {
        public uint cbSize;
        public Guid ClassGuid;
        public uint DevInst;
        public IntPtr Reserved;
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SetupDiGetClassDevsW(
        ref Guid classGuid, string? enumerator, IntPtr hwndParent, uint flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiEnumDeviceInfo(
        IntPtr deviceInfoSet, uint memberIndex, ref SP_DEVINFO_DATA deviceInfoData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiGetDeviceInstanceIdW(
        IntPtr deviceInfoSet, ref SP_DEVINFO_DATA deviceInfoData,
        [Out] char[] deviceInstanceId, uint deviceInstanceIdSize, out uint requiredSize);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiGetDeviceRegistryPropertyW(
        IntPtr deviceInfoSet, ref SP_DEVINFO_DATA deviceInfoData, uint property,
        out uint propertyRegDataType, [Out] byte[]? propertyBuffer,
        uint propertyBufferSize, out uint requiredSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);
}
