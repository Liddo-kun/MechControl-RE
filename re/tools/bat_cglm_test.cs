using System;
using System.Runtime.InteropServices;
using System.Threading;

/// <summary>
/// Test writing CGLM (0x7B9) charge limit with the CORRECT buffer format.
/// Uses int[2] = 8 bytes matching MechControl's EcDevice.
///
/// Usage:
///   bat_cglm_test.exe           -- read current state
///   bat_cglm_test.exe 80        -- set charge limit to 80%
///   bat_cglm_test.exe reset     -- set charge limit to 0 (disable)
/// </summary>
class Program
{
    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    const uint OPEN_EXISTING = 3, FILE_SHARE_RW = 3;
    const uint IOCTL_ECREAD = 0x9C40A488, IOCTL_ECWRITE = 0x9C40A48C;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(string f, uint a, uint s, IntPtr z, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code,
        int[] lpIn, uint inSz, ref int lpOut, uint outSz, out uint ret, IntPtr ov);
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
        byte cglm = ReadEC(0x7B9);
        byte mode = ReadEC(0x7A6);
        int voltage = ReadEC16(0x438);
        int rate = ReadEC16(0x434);
        byte soc = ReadEC(0x4AB);
        byte limitDn = ReadEC(0x7D0);

        Console.WriteLine("[{0}] CGLM=0x{1:X2}({1})  LimitDn=0x{2:X2}({2})  V={3}mV  Rate={4}mW  SOC={5}%  Mode=0x{6:X2}",
            label, cglm, limitDn, voltage, rate, soc, mode);
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
            ShowState("BEFORE");

            if (args.Length == 0)
            {
                Console.WriteLine("\nUsage:");
                Console.WriteLine("  bat_cglm_test.exe 80     -- set CGLM to 80%%");
                Console.WriteLine("  bat_cglm_test.exe reset  -- set CGLM to 0");
                return;
            }

            byte limit;
            if (args[0].ToLower() == "reset")
                limit = 0;
            else
                limit = byte.Parse(args[0]);

            if (limit > 99)
            {
                Console.WriteLine("Must be 0-99 (DSDT checks CGLM <= 0x63)");
                return;
            }

            Console.WriteLine("\nWriting CGLM (0x7B9) = {0}...", limit);
            WriteEC(0x7B9, limit);

            Thread.Sleep(100);
            ShowState("+0.1s");
            Thread.Sleep(900);
            ShowState("+1s");
            Thread.Sleep(2000);
            ShowState("+3s");
            Thread.Sleep(2000);
            ShowState("+5s");

            // Also try writing the down limit
            if (limit > 0)
            {
                byte downLimit = (byte)(limit - 10 > 0 ? limit - 10 : limit);
                Console.WriteLine("\nAlso writing 0x7D0 (limit down) = {0}...", downLimit);
                WriteEC(0x7D0, downLimit);
                Thread.Sleep(1000);
                ShowState("+6s (both set)");
            }

            Thread.Sleep(3000);
            ShowState("+9s (final)");
        }
        finally
        {
            CloseHandle(hDev);
        }
    }
}
