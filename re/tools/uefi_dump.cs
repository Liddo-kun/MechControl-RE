using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;

/// <summary>
/// Dumps all UEFI firmware variables (name + GUID) and optionally reads
/// their values. Run as Administrator.
///
/// Compile: C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe -nologo uefi_dump.cs
/// Run:     uefi_dump.exe > uefi_vars_before.txt
/// </summary>
class UefiDump
{
    // --- Native APIs ---

    [DllImport("ntdll.dll")]
    static extern int NtEnumerateSystemEnvironmentValuesEx(
        uint InformationClass, IntPtr Buffer, ref uint BufferLength);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern uint GetFirmwareEnvironmentVariableW(
        string lpName, string lpGuid, IntPtr pBuffer, uint nSize);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr ProcessHandle, int Access, ref IntPtr Token);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool LookupPrivilegeValue(string sys, string name, ref LUID luid);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll,
        ref TOKEN_PRIVILEGES newState, int bufLen, IntPtr prev, IntPtr retLen);

    [DllImport("kernel32.dll")]
    static extern IntPtr GetCurrentProcess();

    [StructLayout(LayoutKind.Sequential)]
    struct LUID { public uint Lo; public int Hi; }

    [StructLayout(LayoutKind.Sequential)]
    struct LUID_AND_ATTRIBUTES { public LUID Luid; public uint Attributes; }

    [StructLayout(LayoutKind.Sequential)]
    struct TOKEN_PRIVILEGES { public uint Count; public LUID_AND_ATTRIBUTES Priv; }

    const uint SE_PRIVILEGE_ENABLED = 2;
    const int TOKEN_ADJUST_PRIVILEGES = 0x20;
    const int TOKEN_QUERY = 0x08;

    static bool EnablePrivilege(string privName)
    {
        IntPtr token = IntPtr.Zero;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, ref token))
            return false;

        LUID luid = new LUID();
        if (!LookupPrivilegeValue(null, privName, ref luid))
            return false;

        TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES();
        tp.Count = 1;
        tp.Priv.Luid = luid;
        tp.Priv.Attributes = SE_PRIVILEGE_ENABLED;
        return AdjustTokenPrivileges(token, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
    }

    struct VarEntry
    {
        public Guid Guid;
        public string Name;
    }

    static List<VarEntry> EnumerateVariables()
    {
        var list = new List<VarEntry>();

        // First call: get needed buffer size
        uint bufLen = 0;
        int status = NtEnumerateSystemEnvironmentValuesEx(1, IntPtr.Zero, ref bufLen);
        // STATUS_BUFFER_TOO_SMALL = 0xC0000023
        Console.Error.WriteLine("Enumerate pass 1: status=0x{0:X8}, bufLen={1}", status, bufLen);

        if (bufLen == 0)
        {
            // Try with a large pre-allocated buffer
            bufLen = 1024 * 1024; // 1MB
            IntPtr probe = Marshal.AllocHGlobal((int)bufLen);
            status = NtEnumerateSystemEnvironmentValuesEx(1, probe, ref bufLen);
            Console.Error.WriteLine("Enumerate pass 2 (1MB): status=0x{0:X8}, bufLen={1}", status, bufLen);
            if (status != 0)
            {
                Marshal.FreeHGlobal(probe);
                return list;
            }
            ParseBuffer(probe, bufLen, list);
            Marshal.FreeHGlobal(probe);
            return list;
        }

        IntPtr buf = Marshal.AllocHGlobal((int)bufLen);
        status = NtEnumerateSystemEnvironmentValuesEx(1, buf, ref bufLen);
        Console.Error.WriteLine("Enumerate pass 2: status=0x{0:X8}", status);

        if (status != 0)
        {
            Marshal.FreeHGlobal(buf);
            return list;
        }

        ParseBuffer(buf, bufLen, list);
        Marshal.FreeHGlobal(buf);
        return list;
    }

    static void ParseBuffer(IntPtr buf, uint bufLen, List<VarEntry> list)
    {
        uint offset = 0;
        while (offset < bufLen)
        {
            int nextOffset = Marshal.ReadInt32(buf, (int)offset);

            // GUID at offset+4 (16 bytes)
            byte[] guidBytes = new byte[16];
            Marshal.Copy(new IntPtr(buf.ToInt64() + offset + 4), guidBytes, 0, 16);
            Guid guid = new Guid(guidBytes);

            // Name at offset+20, null-terminated Unicode
            string name = Marshal.PtrToStringUni(new IntPtr(buf.ToInt64() + offset + 20));

            list.Add(new VarEntry { Guid = guid, Name = name });

            if (nextOffset == 0) break;
            offset += (uint)nextOffset;
        }
    }

    static byte[] ReadVariable(string name, string guidStr)
    {
        // Format GUID as {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
        IntPtr buf = Marshal.AllocHGlobal(4096);
        uint read = GetFirmwareEnvironmentVariableW(name, guidStr, buf, 4096);
        if (read == 0)
        {
            int err = Marshal.GetLastWin32Error();
            Marshal.FreeHGlobal(buf);
            return null;
        }

        byte[] data = new byte[read];
        Marshal.Copy(buf, data, 0, (int)read);
        Marshal.FreeHGlobal(buf);
        return data;
    }

    static void Main(string[] args)
    {
        Console.Error.WriteLine("Enabling SeSystemEnvironmentPrivilege...");
        bool ok = EnablePrivilege("SeSystemEnvironmentPrivilege");
        Console.Error.WriteLine("Privilege enabled: {0} (LastError={1})", ok, Marshal.GetLastWin32Error());

        Console.Error.WriteLine("Enumerating UEFI variables...");
        var vars = EnumerateVariables();
        Console.Error.WriteLine("Found {0} variables", vars.Count);

        // Print all variables
        Console.WriteLine("=== ALL UEFI VARIABLES ({0} total) ===", vars.Count);
        Console.WriteLine();

        // Group by GUID
        var byGuid = new Dictionary<string, List<string>>();
        foreach (var v in vars)
        {
            string key = v.Guid.ToString("B");
            if (!byGuid.ContainsKey(key))
                byGuid[key] = new List<string>();
            byGuid[key].Add(v.Name);
        }

        foreach (var kv in byGuid)
        {
            Console.WriteLine("GUID: {0}  ({1} vars)", kv.Key, kv.Value.Count);
            foreach (var name in kv.Value)
            {
                Console.Write("  {0}", name);

                // Try to read value for all variables (use --interesting for filtered mode)
                bool dumpAll = true;
                for (int ai = 0; ai < args.Length; ai++)
                    if (args[ai] == "--interesting") dumpAll = false;

                bool interesting = dumpAll
                    || name.IndexOf("Setup", StringComparison.OrdinalIgnoreCase) >= 0
                    || name.IndexOf("Tpv", StringComparison.OrdinalIgnoreCase) >= 0
                    || name.IndexOf("Oem", StringComparison.OrdinalIgnoreCase) >= 0
                    || name.IndexOf("Ap", StringComparison.OrdinalIgnoreCase) >= 0
                    || name.IndexOf("Exist", StringComparison.OrdinalIgnoreCase) >= 0
                    || name.IndexOf("Flag", StringComparison.OrdinalIgnoreCase) >= 0
                    || name.IndexOf("Nvram", StringComparison.OrdinalIgnoreCase) >= 0
                    || name.IndexOf("INOU", StringComparison.OrdinalIgnoreCase) >= 0
                    || name.IndexOf("Uniwill", StringComparison.OrdinalIgnoreCase) >= 0
                    || name.IndexOf("Gaming", StringComparison.OrdinalIgnoreCase) >= 0
                    || name.IndexOf("Control", StringComparison.OrdinalIgnoreCase) >= 0
                    || name.IndexOf("Power", StringComparison.OrdinalIgnoreCase) >= 0;

                if (interesting)
                {
                    byte[] data = ReadVariable(name, kv.Key);
                    if (data != null)
                    {
                        Console.Write("  [{0} bytes] = ", data.Length);
                        int show = Math.Min(data.Length, 256);
                        for (int i = 0; i < show; i++)
                            Console.Write("{0:X2} ", data[i]);
                        if (data.Length > show)
                            Console.Write("...");
                    }
                    else
                    {
                        Console.Write("  [read failed]");
                    }
                }

                Console.WriteLine();
            }
            Console.WriteLine();
        }

        Console.Error.WriteLine("Done.");
    }
}
