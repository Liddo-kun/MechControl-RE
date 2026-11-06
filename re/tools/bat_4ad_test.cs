using System;
using System.Runtime.InteropServices;
using System.Threading;

/// <summary>
/// Test if 0x4AD bit 5 controls charging.
/// High mode: 0x4AD=0x33 (charging), Stationary: 0x4AD=0x13 (not charging).
/// Difference is bit 5 (0x20).
///
/// Usage:
///   bat_4ad_test.exe           -- read current state
///   bat_4ad_test.exe toggle    -- toggle bit 5 of 0x4AD
///   bat_4ad_test.exe set33     -- write 0x33 to 0x4AD (charging value)
///   bat_4ad_test.exe set13     -- write 0x13 to 0x4AD (not-charging value)
/// </summary>
class Program
{
    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    const uint OPEN_EXISTING = 3, FILE_SHARE_RW = 3;
    const uint IOCTL_ECREAD = 0x9C40A488, IOCTL_ECWRITE = 0x9C40A48C;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(string f, uint a, uint s, IntPtr z, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, int[] i, uint iSz, ref int o, uint oSz, out uint ret, IntPtr ov);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    static IntPtr hDev;

    static byte ReadEC(int addr)
    {
        int outBuf = 0;
        uint r;
        DeviceIoControl(hDev, IOCTL_ECREAD, new int[] { addr }, 4, ref outBuf, 4, out r, IntPtr.Zero);
        return (byte)(outBuf & 0xFF);
    }

    static void WriteEC(int addr, byte val)
    {
        int[] inBuf = new int[] { addr, val };
        int outBuf = 0;
        uint r;
        DeviceIoControl(hDev, IOCTL_ECWRITE, inBuf, 8, ref outBuf, 4, out r, IntPtr.Zero);
    }

    static int ReadEC16(int addrLo)
    {
        return ReadEC(addrLo) | (ReadEC(addrLo + 1) << 8);
    }

    static void ShowState(string label)
    {
        byte val4AD = ReadEC(0x4AD);
        Console.WriteLine("[{0}] 0x4AD=0x{1:X2} ({2})  Rate={3}mW  V={4}mV  SOC={5}%  Mode=0x{6:X2}",
            label, val4AD, Convert.ToString(val4AD, 2).PadLeft(8, '0'),
            ReadEC16(0x434), ReadEC16(0x438), ReadEC(0x4AB), ReadEC(0x7A6));
    }

    static void Main(string[] args)
    {
        hDev = CreateFile(@"\\.\ACPIDriver", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_RW,
            IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (hDev == new IntPtr(-1))
        {
            Console.WriteLine("Cannot open ACPIDriver. Run as admin.");
            return;
        }

        try
        {
            ShowState("NOW");
            Console.WriteLine();

            if (args.Length == 0)
            {
                Console.WriteLine("Usage:");
                Console.WriteLine("  bat_4ad_test.exe toggle  -- toggle bit 5 (0x20)");
                Console.WriteLine("  bat_4ad_test.exe set33   -- write 0x33 (High/charging value)");
                Console.WriteLine("  bat_4ad_test.exe set13   -- write 0x13 (Stationary/not-charging value)");
                return;
            }

            string cmd = args[0].ToLower();
            byte cur = ReadEC(0x4AD);

            if (cmd == "toggle")
            {
                byte toggled = (byte)(cur ^ 0x20);
                Console.WriteLine("Toggling bit 5: 0x{0:X2} -> 0x{1:X2}", cur, toggled);
                WriteEC(0x4AD, toggled);
            }
            else if (cmd == "set33")
            {
                Console.WriteLine("Writing 0x4AD = 0x33 (charging value)...");
                WriteEC(0x4AD, 0x33);
            }
            else if (cmd == "set13")
            {
                Console.WriteLine("Writing 0x4AD = 0x13 (not-charging value)...");
                WriteEC(0x4AD, 0x13);
            }
            else
            {
                Console.WriteLine("Unknown command: {0}", cmd);
                return;
            }

            Thread.Sleep(200);
            ShowState("+0.2s");
            Thread.Sleep(800);
            ShowState("+1s");
            Thread.Sleep(2000);
            ShowState("+3s");
            Thread.Sleep(2000);
            ShowState("+5s");
        }
        finally
        {
            CloseHandle(hDev);
        }
    }
}
