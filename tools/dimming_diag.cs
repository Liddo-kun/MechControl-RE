// Diagnostic tool: read critical EC registers before/after WMI dimming call
// Compile: C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe -nologo dimming_diag.cs
// Run elevated (admin)
using System;
using System.Runtime.InteropServices;
using System.Management;

class DimmingDiag
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFile(string name, uint access, uint share,
        IntPtr sec, uint disp, uint flags, IntPtr tmpl);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code,
        ref int inBuf, int inSz, ref int outBuf, int outSz,
        out int ret, IntPtr ovl);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    static IntPtr hDev;

    static byte ReadEC(int addr)
    {
        int inBuf = addr, outBuf = 0;
        int bytesRet;
        if (!DeviceIoControl(hDev, 0x9C40A488u, ref inBuf, 4, ref outBuf, 4, out bytesRet, IntPtr.Zero))
            throw new Exception("EC read failed at 0x" + addr.ToString("X3") + " err=" + Marshal.GetLastWin32Error());
        return (byte)(outBuf & 0xFF);
    }

    static void DumpRegisters(string label)
    {
        Console.WriteLine("\n=== " + label + " ===");
        byte gfid = ReadEC(0x7D2);
        byte csms_byte = ReadEC(0x72B);
        byte cume_byte = ReadEC(0x727);
        byte aptc_byte = ReadEC(0x786);
        byte lcse_byte = ReadEC(0x7C7);
        byte tb43_byte = ReadEC(0x728);
        byte moid = ReadEC(0x7D3); // MOID might be near GFID

        Console.WriteLine("  GFID     (0x7D2) = 0x{0:X2}  ({0})  {1}", gfid,
            gfid == 0 ? "<-- ZERO! PMSF will skip!" : "OK (non-zero)");
        Console.WriteLine("  CSMS byte(0x72B) = 0x{0:X2}  bit1={1}  {2}", csms_byte,
            (csms_byte >> 1) & 1,
            ((csms_byte >> 1) & 1) == 1 ? "(dimming ON)" : "(dimming OFF)");
        Console.WriteLine("  CUME byte(0x727) = 0x{0:X2}  bit6={1}  {2}", cume_byte,
            (cume_byte >> 6) & 1,
            ((cume_byte >> 6) & 1) == 1 ? "(custom mode)" : "(normal mode)");
        Console.WriteLine("  APTC/APTN(0x786) = 0x{0:X2}  APTC={1} APTN={2}", aptc_byte,
            aptc_byte & 0x7F, (aptc_byte >> 7) & 1);
        Console.WriteLine("  LCSE byte(0x7C7) = 0x{0:X2}  bit1={1}", lcse_byte,
            (lcse_byte >> 1) & 1);
        Console.WriteLine("  TB43 byte(0x728) = 0x{0:X2}  bit0={1}", tb43_byte,
            tb43_byte & 1);
    }

    static byte WmiCall(ulong data)
    {
        var obj = new ManagementObject("root\\WMI",
            "AcpiTest_MULong.InstanceName='ACPI\\PNP0C14\\1_1'", null);
        var p = obj.GetMethodParameters("GetSetULong");
        p["Data"] = data;
        var r = obj.InvokeMethod("GetSetULong", p, null);
        return (byte)(uint)r["Return"];
    }

    static void Main()
    {
        Console.WriteLine("=== Dimming Diagnostic Tool ===");
        Console.WriteLine("Opening \\\\.\\.\\ACPIDriver...");

        hDev = CreateFile("\\\\.\\ACPIDriver", 0xC0000000u, 3u,
            IntPtr.Zero, 3u, 0u, IntPtr.Zero);
        if (hDev.ToInt64() == -1)
        {
            Console.WriteLine("FAILED to open ACPIDriver! Error: " + Marshal.GetLastWin32Error());
            Console.WriteLine("Are you running as admin? Is UWACPIDriver installed?");
            return;
        }
        Console.WriteLine("ACPIDriver opened OK.");

        // Also read a wider range around GFID for context
        Console.WriteLine("\n--- EC registers 0x7D0-0x7D5 (around GFID) ---");
        for (int a = 0x7D0; a <= 0x7D5; a++)
        {
            byte v = ReadEC(a);
            Console.WriteLine("  0x{0:X3} = 0x{1:X2}  ({1})", a, v);
        }

        // Also read MOID area - check the DSDT for MOID location
        Console.WriteLine("\n--- EC registers 0x724-0x72C (CUME/CSMS area) ---");
        for (int a = 0x724; a <= 0x72C; a++)
        {
            byte v = ReadEC(a);
            Console.WriteLine("  0x{0:X3} = 0x{1:X2}  ({1})", a, v);
        }

        DumpRegisters("BEFORE any WMI call");

        // Send dimming OFF
        Console.WriteLine("\nSending WMI dimming OFF (0x0000050000000001)...");
        byte ret1 = WmiCall(0x0000050000000001UL);
        Console.WriteLine("  Return = 0x{0:X2} ({0})  {1}", ret1,
            ret1 == 0xAA ? "OK (expected 0xAA)" : "UNEXPECTED!");

        DumpRegisters("AFTER dimming OFF");

        Console.Write("\nPress Enter to send dimming ON...");
        Console.ReadLine();

        // Send dimming ON
        Console.WriteLine("Sending WMI dimming ON (0x0000050000000002)...");
        byte ret2 = WmiCall(0x0000050000000002UL);
        Console.WriteLine("  Return = 0x{0:X2} ({0})  {1}", ret2,
            ret2 == 0xBB ? "OK (expected 0xBB)" : "UNEXPECTED!");

        DumpRegisters("AFTER dimming ON");

        // Also try GPIO commands
        Console.Write("\nTest GPIO commands too? (y/n): ");
        string ans = Console.ReadLine();
        if (ans != null && ans.Trim().ToLower() == "y")
        {
            Console.WriteLine("\nSending GPIO LOW (SA00=0x0D): 0x000005000000000D...");
            byte r3 = WmiCall(0x000005000000000DUL);
            Console.WriteLine("  Return = 0x{0:X2} ({0})", r3);
            DumpRegisters("AFTER GPIO LOW");

            Console.Write("Press Enter to send GPIO HIGH...");
            Console.ReadLine();

            Console.WriteLine("Sending GPIO HIGH (SA00=0x0E): 0x000005000000000E...");
            byte r4 = WmiCall(0x000005000000000EUL);
            Console.WriteLine("  Return = 0x{0:X2} ({0})", r4);
            DumpRegisters("AFTER GPIO HIGH");
        }

        // Try FMFG commands
        Console.Write("\nTest FMFG commands too? (y/n): ");
        ans = Console.ReadLine();
        if (ans != null && ans.Trim().ToLower() == "y")
        {
            Console.WriteLine("\nSending FMFG=0 (SA00=0x10): 0x0000050000000010...");
            byte r5 = WmiCall(0x0000050000000010UL);
            Console.WriteLine("  Return = 0x{0:X2} ({0})", r5);

            Console.Write("Press Enter to send FMFG=1...");
            Console.ReadLine();

            Console.WriteLine("Sending FMFG=1 (SA00=0x11): 0x0000050000000011...");
            byte r6 = WmiCall(0x0000050000000011UL);
            Console.WriteLine("  Return = 0x{0:X2} ({0})", r6);
        }

        CloseHandle(hDev);
        Console.WriteLine("\nDone. Press Enter to exit.");
        Console.ReadLine();
    }
}
