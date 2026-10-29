using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;
using System.Buffers.Binary;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace MechControl.Hardware.Nvidia;

// The original BIOS values at every offset MechControl may patch. Saved before the first override is written.
internal sealed record NvidiaOverrideRecovery(string Version, Dictionary<int, uint> Originals);

/// <summary>
/// The GPU's software BIOS overrides (driver-key romOverride values) and the original values
/// they replace. Never takes over another tool's overrides.
/// </summary>
internal sealed class NvidiaOverrideStore : IDisposable
{
    private const string ExternalOverrides = "External NVIDIA ROM overrides exist. MechControl will not take ownership. " +
        "Use the owning tool's original-limits/remove-override command and restart the GPU first.";
    private static readonly Guid DisplayClass = new("4d36e968-e325-11ce-bfc1-08002be10318");
    private readonly RegistryKey key;
    public string InstanceId { get; }
    public string RecoveryPath { get; }

    // Binds to the PnP device at the NVAPI GPU's PCI address, never the first NVIDIA device.
    public NvidiaOverrideStore(ushort deviceId, uint subsystemId, uint bus, uint device, uint function, string recoveryPath)
    {
        RecoveryPath = recoveryPath;
        string prefix = $@"PCI\VEN_10DE&DEV_{deviceId:X4}&SUBSYS_{subsystemId:X8}";
        foreach (var (set, info, hardware) in DisplayAdapters())
        {
            var data = info;
            if ((!hardware.StartsWith(prefix + "&", StringComparison.OrdinalIgnoreCase) && !hardware.Equals(prefix, StringComparison.OrdinalIgnoreCase))
                || Number(set, ref data, 0x15) != bus || Number(set, ref data, 0x1c) != (device << 16 | function)) continue;
            char[] id = new char[512];
            if (!SetupDiGetDeviceInstanceIdW(set, ref data, id, (uint)id.Length, out uint length)) throw new Win32Exception();
            IntPtr handle = SetupDiOpenDevRegKey(set, ref data, 1, 0, 2, 0x2001f);
            if (handle == new IntPtr(-1)) throw new Win32Exception();
            key = RegistryKey.FromHandle(new SafeRegistryHandle(handle, true), RegistryView.Registry64);
            InstanceId = new string(id, 0, (int)length - 1);
            return;
        }
        throw new InvalidOperationException("No present NVIDIA PnP adapter matches the selected NVAPI GPU.");
    }

    /// <summary>PnP-only presence check. Unlike NVAPI/NVML it cannot wake a sleeping discrete GPU.</summary>
    public static bool HasNvidiaAdapter()
    {
        try { return DisplayAdapters().Any(a => a.HardwareId.StartsWith(@"PCI\VEN_10DE&", StringComparison.OrdinalIgnoreCase)); }
        catch (Win32Exception) { return false; }
    }

    /// <summary>PnP power data only: true when Windows last put this GPU in D0. Never wakes it.</summary>
    public bool IsAwake()
    {
        if (CM_Locate_DevNodeW(out uint node, InstanceId, 0) != 0) return false;
        var key = new DevPropKey { Fmtid = new("a45c254e-df1c-4efd-8020-67d146a850e0"), Pid = 32 }; // DEVPKEY_Device_PowerData
        byte[] data = new byte[64];
        uint size = (uint)data.Length;
        // CM_POWER_DATA: PD_Size, then PD_MostRecentPowerState (PowerDeviceD0 = 1).
        return CM_Get_DevNode_PropertyW(node, ref key, out _, data, ref size, 0) == 0 && size >= 8 && BitConverter.ToInt32(data, 4) == 1;
    }

    /// <summary>
    /// The unmodified BIOS and whether owned overrides are staged. Refuses other tools'
    /// overrides. Owned overrides that can no longer apply (replaced VBIOS, or removed by a
    /// driver reinstall) are deleted with their originals and reported as discarded.
    /// </summary>
    public NvidiaBios Open(byte[] activeRom, out bool staged, out bool discarded)
    {
        var active = NvidiaBios.Parse(activeRom);
        var current = ReadRegistry();
        var recovery = LoadRecovery();
        discarded = recovery != null && (recovery.Version != active.Version ||
            (current.Count == 0 && recovery.Originals.All(o => active.ValueAt(o.Key) == o.Value)));
        if (discarded)
        {
            foreach (string name in current.Where(p => IsOwned(p.Value, recovery!)).Select(p => p.Key)) key.DeleteValue(name);
            File.Delete(RecoveryPath);
            current = ReadRegistry();
            recovery = null;
        }
        RequireOwned(current, recovery);
        staged = current.Count != 0;
        return recovery == null ? active : Restore(activeRom, recovery);
    }

    /// <summary>True when the registry holds exactly these records.</summary>
    public bool Matches(IReadOnlyList<NvidiaOverrideRecord> records)
    {
        var current = ReadRegistry();
        return current.Count == records.Count &&
            records.All(r => current.TryGetValue(r.Name, out var v) && v.AsSpan().SequenceEqual(r.Data));
    }

    /// <summary>Write exactly these records. The originals are saved first, so they never depend on a patched image.</summary>
    public void Stage(NvidiaBios original, IReadOnlyList<NvidiaOverrideRecord> records)
    {
        var current = ReadRegistry();
        var recovery = LoadRecovery();
        RequireOwned(current, recovery);
        if (records.Count != 0)
        {
            var complete = CompleteRecovery(original, recovery);
            if (recovery == null || complete.Originals.Count != recovery.Originals.Count)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(RecoveryPath)!);
                string text = string.Join("\n", [complete.Version, .. complete.Originals.Select(o => $"{o.Key}={o.Value}")]);
                using (var file = new FileStream(RecoveryPath + ".tmp", FileMode.Create, FileAccess.Write, FileShare.None))
                { file.Write(Encoding.UTF8.GetBytes(text)); file.Flush(true); }
                File.Move(RecoveryPath + ".tmp", RecoveryPath, true);
            }
        }
        foreach (string name in current.Keys.Except(records.Select(r => r.Name), StringComparer.OrdinalIgnoreCase)) key.DeleteValue(name, false);
        foreach (var record in records) key.SetValue(record.Name, record.Data, RegistryValueKind.Binary);
        key.Flush();
        if (!Matches(records)) throw new IOException("NVIDIA override registry readback failed.");
    }

    /// <summary>Forget the originals once no owned override remains staged or active.</summary>
    public void DeleteRecovery()
    {
        RequireOwned(ReadRegistry(), null);
        File.Delete(RecoveryPath);
    }

    // Append newly supported fields from Open's recovered stock BIOS, never
    // replace originals already saved by an older version.
    private static NvidiaOverrideRecovery CompleteRecovery(NvidiaBios original, NvidiaOverrideRecovery? recovery)
    {
        var originals = recovery == null ? new Dictionary<int, uint>() : new Dictionary<int, uint>(recovery.Originals);
        foreach (int offset in original.PatchOffsets) originals.TryAdd(offset, original.ValueAt(offset));
        return new(recovery?.Version ?? original.Version, originals);
    }

    // Text: the BIOS version, then one "offset=original" line per patchable place.
    private NvidiaOverrideRecovery? LoadRecovery()
    {
        if (!File.Exists(RecoveryPath)) return null;
        string[] lines = File.ReadAllLines(RecoveryPath);
        return new(lines[0], lines.Skip(1).Select(l => l.Split('=')).ToDictionary(p => int.Parse(p[0]), p => uint.Parse(p[1])));
    }

    // The active image with the saved originals written back.
    private static NvidiaBios Restore(byte[] activeRom, NvidiaOverrideRecovery recovery)
    {
        byte[] image = (byte[])activeRom.Clone();
        foreach (var (offset, value) in recovery.Originals) BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(offset), value);
        return NvidiaBios.Parse(image);
    }

    // Ours: a direct 4-byte patch for the saved BIOS version at an offset whose original was saved.
    private static bool IsOwned(byte[] data, NvidiaOverrideRecovery recovery) => data.Length == 14 && data[9] == 4 &&
        string.Join(".", data.Take(5).Select(b => b.ToString("X2"))) == recovery.Version &&
        recovery.Originals.ContainsKey(BitConverter.ToInt32(data, 5));

    private static void RequireOwned(Dictionary<string, byte[]> current, NvidiaOverrideRecovery? recovery)
    {
        if (current.Values.Any(v => recovery == null || !IsOwned(v, recovery))) throw new InvalidOperationException(ExternalOverrides);
    }

    private Dictionary<string, byte[]> ReadRegistry()
    {
        var result = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        foreach (string name in key.GetValueNames())
        {
            bool direct = name.StartsWith("romOverride", StringComparison.OrdinalIgnoreCase);
            if (name.Equals("vbios", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("An external BIOS-file override exists; restore it in its owning tool first.");
            if (!direct) continue;
            if (name.Length != 13 || !byte.TryParse(name.AsSpan(11), System.Globalization.NumberStyles.HexNumber,
                    System.Globalization.CultureInfo.InvariantCulture, out _) || key.GetValueKind(name) != RegistryValueKind.Binary
                || key.GetValue(name) is not byte[] bytes) throw new InvalidOperationException("Unrecognized external ROM override.");
            result.Add(name, bytes);
        }
        return result;
    }

    private static IEnumerable<(IntPtr Set, DeviceInfo Data, string HardwareId)> DisplayAdapters()
    {
        Guid display = DisplayClass;
        IntPtr set = SetupDiGetClassDevsW(ref display, "PCI", IntPtr.Zero, 2);
        if (set == new IntPtr(-1)) throw new Win32Exception();
        try
        {
            byte[] property = new byte[4096];
            for (uint i = 0; ; i++)
            {
                var data = new DeviceInfo { Size = (uint)Marshal.SizeOf<DeviceInfo>() };
                if (!SetupDiEnumDeviceInfo(set, i, ref data))
                {
                    if (Marshal.GetLastWin32Error() != 259) throw new Win32Exception();
                    yield break;
                }
                if (SetupDiGetDeviceRegistryPropertyW(set, ref data, 1, out _, property, (uint)property.Length, out uint size))
                    yield return (set, data, Encoding.Unicode.GetString(property, 0, (int)size).Split('\0')[0]);
            }
        }
        finally { SetupDiDestroyDeviceInfoList(set); }
    }

    private static uint? Number(IntPtr set, ref DeviceInfo data, uint property)
    {
        byte[] value = new byte[4];
        return SetupDiGetDeviceRegistryPropertyW(set, ref data, property, out _, value, 4, out uint size) && size == 4 ? BitConverter.ToUInt32(value) : null;
    }

    public void Dispose() => key.Dispose();

    [StructLayout(LayoutKind.Sequential)] private struct DeviceInfo { public uint Size; public Guid ClassGuid; public uint DevInst; public IntPtr Reserved; }
    [StructLayout(LayoutKind.Sequential)] private struct DevPropKey { public Guid Fmtid; public uint Pid; }
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)] private static extern int CM_Locate_DevNodeW(out uint node, string id, uint flags);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)] private static extern int CM_Get_DevNode_PropertyW(uint node, ref DevPropKey key, out uint type, byte[] buffer, ref uint size, uint flags);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr SetupDiGetClassDevsW(ref Guid guid, string enumerator, IntPtr parent, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool SetupDiEnumDeviceInfo(IntPtr set, uint index, ref DeviceInfo data);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool SetupDiGetDeviceInstanceIdW(IntPtr set, ref DeviceInfo data, [Out] char[] id, uint size, out uint required);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr set, ref DeviceInfo data, uint property, out uint type, byte[] buffer, uint size, out uint required);
    [DllImport("setupapi.dll", SetLastError = true)] private static extern IntPtr SetupDiOpenDevRegKey(IntPtr set, ref DeviceInfo data, uint scope, uint profile, uint type, uint access);
    [DllImport("setupapi.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
}
