using System;
using System.Runtime.InteropServices;

/// <summary>
/// Reads OEM UEFI variables fully and dumps hex + decoded NVRAM_STRUCT fields.
/// Compile: csc.exe -nologo uefi_read_oem.cs
/// </summary>
class UefiReadOem
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern uint GetFirmwareEnvironmentVariableW(
        string lpName, string lpGuid, IntPtr pBuffer, uint nSize);

    // Privilege APIs
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
    struct TOKEN_PRIVILEGES
    {
        public uint PrivilegeCount;
        public LUID_AND_ATTRIBUTES Privileges;
    }

    static bool EnablePriv()
    {
        IntPtr tok;
        if (!OpenProcessToken(GetCurrentProcess(), 0x0020 | 0x0008, out tok))
        {
            Console.WriteLine("OpenProcessToken failed: {0}", Marshal.GetLastWin32Error());
            return false;
        }

        LUID luid;
        if (!LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out luid))
        {
            Console.WriteLine("LookupPrivilegeValue failed: {0}", Marshal.GetLastWin32Error());
            return false;
        }

        TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES();
        tp.PrivilegeCount = 1;
        tp.Privileges.Luid = luid;
        tp.Privileges.Attributes = 0x00000002; // SE_PRIVILEGE_ENABLED

        if (!AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero))
        {
            Console.WriteLine("AdjustTokenPrivileges failed: {0}", Marshal.GetLastWin32Error());
            return false;
        }

        int lastErr = Marshal.GetLastWin32Error();
        if (lastErr != 0)
        {
            Console.WriteLine("AdjustTokenPrivileges partial: lastError={0}", lastErr);
            return false;
        }

        return true;
    }

    static byte[] ReadVar(string name, string guid)
    {
        IntPtr buf = Marshal.AllocHGlobal(4096);
        uint read = GetFirmwareEnvironmentVariableW(name, guid, buf, 4096);
        if (read == 0)
        {
            int err = Marshal.GetLastWin32Error();
            Marshal.FreeHGlobal(buf);
            Console.WriteLine("  {0}: READ FAILED (err={1})", name, err);
            return null;
        }
        byte[] data = new byte[read];
        Marshal.Copy(buf, data, 0, (int)read);
        Marshal.FreeHGlobal(buf);
        return data;
    }

    static void DumpHex(string label, byte[] data)
    {
        Console.WriteLine("{0}  [{1} bytes]:", label, data.Length);
        for (int i = 0; i < data.Length; i++)
        {
            if (i % 16 == 0) Console.Write("  {0:X4}: ", i);
            Console.Write("{0:X2} ", data[i]);
            if (i % 16 == 15) Console.WriteLine();
        }
        if (data.Length % 16 != 0) Console.WriteLine();
        Console.WriteLine();
    }

    static void Main()
    {
        Console.WriteLine("Enabling SeSystemEnvironmentPrivilege...");
        bool ok = EnablePriv();
        Console.WriteLine("Privilege: {0}", ok ? "OK" : "FAILED");
        Console.WriteLine();

        string oemGuid = "{9f33f85c-13ca-4fd1-9c4a-96217722c593}";
        string tpvGuid = "{1c3483d5-1e7e-4450-9806-dede002c974b}";

        // Read all OEM variables
        string[] oemVars = { "OemMagicDoor", "OemLightbarPresent", "OemLightbarReset", "OemEdpReset", "OemMagicVariable" };
        foreach (var name in oemVars)
        {
            byte[] data = ReadVar(name, oemGuid);
            if (data != null) DumpHex(name, data);
        }

        // TpvSetup
        byte[] tpv = ReadVar("TpvSetup", tpvGuid);
        if (tpv != null) DumpHex("TpvSetup", tpv);

        // Decode NVRAM_STRUCT from OemMagicVariable
        byte[] magic = ReadVar("OemMagicVariable", oemGuid);
        if (magic != null && magic.Length >= 100)
        {
            Console.WriteLine("=== NVRAM_STRUCT Decode (packed, estimated offsets) ===");
            Console.WriteLine();

            int o = 0;
            DecodeField(magic, ref o, 4, "OemBoardSsid");
            DecodeField(magic, ref o, 2, "SupportByte");
            DecodeField(magic, ref o, 1, "ProjectID");
            DecodeField(magic, ref o, 1, "KeyboardType");
            DecodeField(magic, ref o, 1, "CustomerList");
            DecodeField(magic, ref o, 1, "RGBLightbarMode");
            DecodeField(magic, ref o, 1, "RGBLightbarMode_R");
            DecodeField(magic, ref o, 1, "RGBLightbarMode_G");
            DecodeField(magic, ref o, 1, "RGBLightbarMode_B");
            DecodeField(magic, ref o, 1, "ColorCalibrationSupport");
            DecodeField(magic, ref o, 1, "TDRdata");
            DecodeArray(magic, ref o, 8, "RGBKeyboard1A");
            DecodeArray(magic, ref o, 8, "RGBKeyboard08");
            DecodeArray(magic, ref o, 8, "SmartLightbar1A");
            DecodeArray(magic, ref o, 8, "SmartLightbar08");
            DecodeField(magic, ref o, 1, "PowerMode");
            DecodeField(magic, ref o, 1, "BatteryLimitation");
            DecodeField(magic, ref o, 1, "ChargeMaximumLimit");
            DecodeField(magic, ref o, 1, "ChargeMinimumLimit");
            DecodeField(magic, ref o, 1, "MemoryOverClockSwitch");
            DecodeField(magic, ref o, 2, "ICpuCoreVoltageValue");
            DecodeField(magic, ref o, 2, "ICpuCoreVoltageMaximum");
            DecodeField(magic, ref o, 2, "ICpuCoreVoltageMinimum");
            DecodeField(magic, ref o, 2, "ICpuCoreVoltageOffsetValue");
            DecodeField(magic, ref o, 2, "ICpuCoreVoltageOffsetMaximum");
            DecodeField(magic, ref o, 2, "ICpuCoreVoltageOffsetMinimum");
            DecodeField(magic, ref o, 2, "ICpuTauValue");
            DecodeField(magic, ref o, 1, "ACpuOverClockSupport");
            DecodeField(magic, ref o, 1, "APVersionCheck");
            DecodeField(magic, ref o, 4, "ACpuFreqValue");
            DecodeField(magic, ref o, 4, "ACpuFreqValueMaximum");
            DecodeField(magic, ref o, 4, "ACpuFreqValueMinimum");
            DecodeField(magic, ref o, 4, "ACpuVoltageValue");
            DecodeField(magic, ref o, 4, "ACpuVoltageValueMaximum");
            DecodeField(magic, ref o, 4, "ACpuVoltageValueMinimum");
            DecodeField(magic, ref o, 1, "OverClockRecoveryFlag");
            DecodeField(magic, ref o, 1, "*** ApExistFlag ***");
            DecodeField(magic, ref o, 1, "ACRecoverySupport");
            DecodeField(magic, ref o, 1, "ACRecoveryStatus");
            DecodeField(magic, ref o, 1, "MemoryOverClockSupport");
            DecodeField(magic, ref o, 1, "*** ApUseFlag ***");
            DecodeField(magic, ref o, 1, "OemDisplayMode");
            DecodeField(magic, ref o, 2, "ICpuCoreVoltageOffsetNegativeValue");
            DecodeField(magic, ref o, 1, "ICpuCoreVoltageOffsetRangeType");
            DecodeField(magic, ref o, 1, "FnKeyStatus");
            DecodeField(magic, ref o, 4, "AudioCodecID");
            DecodeField(magic, ref o, 1, "PowerPhase");
            DecodeField(magic, ref o, 1, "ICpuCoreVoltageOffsetSupport");
            DecodeField(magic, ref o, 1, "CPUPerformanceAndOverClockMenuSupport");
            DecodeField(magic, ref o, 1, "CPUPerformanceAndOverClockMenuSwitch");
            Console.WriteLine("  Remaining bytes from offset 0x{0:X2}: {1}", o, magic.Length - o);
        }
    }

    static void DecodeField(byte[] data, ref int offset, int size, string name)
    {
        if (offset + size > data.Length)
        {
            Console.WriteLine("  [0x{0:X2}] {1}: OUT OF BOUNDS", offset, name);
            offset += size;
            return;
        }

        uint val = 0;
        for (int i = 0; i < size && i < 4; i++)
            val |= (uint)data[offset + i] << (i * 8);

        Console.WriteLine("  [0x{0:X2}] {1} = 0x{2:X} ({2})", offset, name, val);
        offset += size;
    }

    static void DecodeArray(byte[] data, ref int offset, int size, string name)
    {
        if (offset + size > data.Length)
        {
            Console.WriteLine("  [0x{0:X2}] {1}: OUT OF BOUNDS", offset, name);
            offset += size;
            return;
        }

        Console.Write("  [0x{0:X2}] {1} = ", offset, name);
        for (int i = 0; i < size; i++)
            Console.Write("{0:X2} ", data[offset + i]);
        Console.WriteLine();
        offset += size;
    }
}
