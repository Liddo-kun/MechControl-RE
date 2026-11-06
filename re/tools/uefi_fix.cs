using System;
using System.Runtime.InteropServices;

/// <summary>
/// Sets UEFI NVRAM flags needed for MechControl standalone operation.
/// Reads OemMagicVariable, sets required flags, writes it back.
/// Requires reboot to take effect.
///
/// Compile: csc.exe -nologo uefi_fix.cs
/// Run as admin: uefi_fix.exe          (apply fixes)
///               uefi_fix.exe --check  (read-only check)
///               uefi_fix.exe --clear  (reset to uninstalled state)
/// </summary>
class UefiFix
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern uint GetFirmwareEnvironmentVariableW(
        string lpName, string lpGuid, IntPtr pBuffer, uint nSize);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetFirmwareEnvironmentVariableW(
        string lpName, string lpGuid, IntPtr pValue, uint nSize);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr h, uint acc, out IntPtr tok);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool LookupPrivilegeValue(string sys, string name, out LUID luid);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool AdjustTokenPrivileges(IntPtr tok, bool disableAll,
        ref TOKEN_PRIVILEGES tp, uint len, IntPtr prev, IntPtr retLen);

    [DllImport("kernel32.dll")]
    static extern IntPtr GetCurrentProcess();

    [StructLayout(LayoutKind.Sequential)]
    struct LUID { public uint LowPart; public int HighPart; }

    [StructLayout(LayoutKind.Sequential)]
    struct LUID_AND_ATTRIBUTES { public LUID Luid; public uint Attributes; }

    [StructLayout(LayoutKind.Sequential)]
    struct TOKEN_PRIVILEGES { public uint PrivilegeCount; public LUID_AND_ATTRIBUTES Privileges; }

    // OemMagicVariable offsets (from NVRAM_STRUCT, packed sequential)
    const int OFF_POWER_MODE = 0x2F;
    const int OFF_MEM_OC_SWITCH = 0x33;
    const int OFF_AP_EXIST = 0x5D;
    const int OFF_AP_USE = 0x61;

    const string OEM_GUID = "{9f33f85c-13ca-4fd1-9c4a-96217722c593}";
    const string VAR_NAME = "OemMagicVariable";

    static readonly string[] MODE_NAMES = { "Office", "Balance", "Turbo", "Custom" };

    static bool EnablePriv()
    {
        IntPtr tok;
        if (!OpenProcessToken(GetCurrentProcess(), 0x0028, out tok)) return false;
        LUID luid;
        if (!LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out luid)) return false;
        TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES();
        tp.PrivilegeCount = 1;
        tp.Privileges.Luid = luid;
        tp.Privileges.Attributes = 2;
        AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
        return Marshal.GetLastWin32Error() == 0;
    }

    struct Fix
    {
        public int Offset;
        public string Name;
        public byte Target;
    }

    static void Main(string[] args)
    {
        bool readOnly = args.Length > 0 && args[0] == "--check";
        bool clear = args.Length > 0 && args[0] == "--clear";

        if (!EnablePriv())
        {
            Console.WriteLine("FAILED to enable SeSystemEnvironmentPrivilege. Run as Administrator.");
            return;
        }

        // Read current OemMagicVariable
        IntPtr buf = Marshal.AllocHGlobal(4096);
        uint size = GetFirmwareEnvironmentVariableW(VAR_NAME, OEM_GUID, buf, 4096);
        if (size == 0)
        {
            Console.WriteLine("FAILED to read {0} (err={1})", VAR_NAME, Marshal.GetLastWin32Error());
            Console.WriteLine("The OEM UEFI variable does not exist. Has the BIOS been set up for this platform?");
            Marshal.FreeHGlobal(buf);
            return;
        }

        byte[] data = new byte[size];
        Marshal.Copy(buf, data, 0, (int)size);
        Marshal.FreeHGlobal(buf);

        Console.WriteLine("Read {0}: {1} bytes", VAR_NAME, size);
        Console.WriteLine();

        // Define required flags
        Fix[] fixes;
        if (clear)
        {
            fixes = new Fix[] {
                new Fix { Offset = OFF_POWER_MODE,    Name = "PowerMode",           Target = 1 },
                new Fix { Offset = OFF_MEM_OC_SWITCH,  Name = "MemoryOverClockSwitch", Target = 0 },
                new Fix { Offset = OFF_AP_EXIST,       Name = "ApExistFlag",          Target = 0 },
                new Fix { Offset = OFF_AP_USE,         Name = "ApUseFlag",            Target = 0 },
            };
        }
        else
        {
            fixes = new Fix[] {
                new Fix { Offset = OFF_POWER_MODE,    Name = "PowerMode",           Target = 3 },
                new Fix { Offset = OFF_MEM_OC_SWITCH,  Name = "MemoryOverClockSwitch", Target = 1 },
                new Fix { Offset = OFF_AP_EXIST,       Name = "ApExistFlag",          Target = 1 },
                new Fix { Offset = OFF_AP_USE,         Name = "ApUseFlag",            Target = 1 },
            };
        }

        // Show current values
        Console.WriteLine("Current values:");
        foreach (var f in fixes)
        {
            byte cur = data[f.Offset];
            string extra = "";
            if (f.Name == "PowerMode" && cur < MODE_NAMES.Length)
                extra = " (" + MODE_NAMES[cur] + ")";
            Console.WriteLine("  [0x{0:X2}] {1,-24} = {2}{3}{4}",
                f.Offset, f.Name, cur, extra,
                cur == f.Target ? "" : "  <- needs fix");
        }
        Console.WriteLine();

        // Check if any fixes needed
        bool needsFix = false;
        foreach (var f in fixes)
            if (data[f.Offset] != f.Target) { needsFix = true; break; }

        if (!needsFix)
        {
            Console.WriteLine("All flags are already correct. No changes needed.");
            return;
        }

        if (readOnly)
        {
            Console.WriteLine("Flags need updating. Run without --check to apply.");
            return;
        }

        // Apply fixes
        Console.WriteLine("Writing:");
        foreach (var f in fixes)
        {
            byte cur = data[f.Offset];
            if (cur != f.Target)
            {
                data[f.Offset] = f.Target;
                Console.WriteLine("  {0}: {1} -> {2}", f.Name, cur, f.Target);
            }
        }
        Console.WriteLine();

        // Write back
        IntPtr wBuf = Marshal.AllocHGlobal((int)size);
        Marshal.Copy(data, 0, wBuf, (int)size);
        bool ok = SetFirmwareEnvironmentVariableW(VAR_NAME, OEM_GUID, wBuf, size);
        Marshal.FreeHGlobal(wBuf);

        if (!ok)
        {
            Console.WriteLine("FAILED to write {0} (err={1})", VAR_NAME, Marshal.GetLastWin32Error());
            return;
        }

        Console.WriteLine("SUCCESS. Reboot for changes to take effect.");

        // Verify
        IntPtr vBuf = Marshal.AllocHGlobal(4096);
        uint vSize = GetFirmwareEnvironmentVariableW(VAR_NAME, OEM_GUID, vBuf, 4096);
        if (vSize > 0)
        {
            byte[] verify = new byte[vSize];
            Marshal.Copy(vBuf, verify, 0, (int)vSize);
            bool verified = true;
            foreach (var f in fixes)
                if (verify[f.Offset] != f.Target) { verified = false; break; }
            Console.WriteLine("Verify: {0}", verified ? "OK" : "MISMATCH");
        }
        Marshal.FreeHGlobal(vBuf);
    }
}
