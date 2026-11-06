using System;
using System.Runtime.InteropServices;
using System.Threading;

/// <summary>
/// Monitor battery voltage, capacity, and charge rate across mode changes.
/// Shows the real effect of battery mode on charging behavior.
/// </summary>
class Program
{
    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    const uint OPEN_EXISTING = 3, FILE_SHARE_RW = 3;
    const uint IOCTL_ECREAD = 0x9C40A488;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(string f, uint a, uint s, IntPtr z, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, byte[] i, uint iSz, byte[] o, uint oSz, out uint ret, IntPtr ov);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    static IntPtr hDev;

    static byte ReadEC(int addr)
    {
        byte[] inBuf = new byte[] { (byte)(addr & 0xFF), (byte)((addr >> 8) & 0xFF), 0, 0 };
        byte[] outBuf = new byte[4];
        uint r;
        DeviceIoControl(hDev, IOCTL_ECREAD, inBuf, 4, outBuf, 4, out r, IntPtr.Zero);
        return outBuf[0];
    }

    static int ReadEC16(int addrLo)
    {
        byte lo = ReadEC(addrLo);
        byte hi = ReadEC(addrLo + 1);
        return lo | (hi << 8);
    }

    static void ShowState()
    {
        // Battery mode
        byte mode = ReadEC(0x7A6);
        int modeBits = (mode >> 3) & 0x07;
        string modeName = modeBits == 1 ? "High" : modeBits == 3 ? "Balanced" : modeBits == 5 ? "Stationary" : "Unknown(" + modeBits + ")";

        // SOC
        byte soc = ReadEC(0x4AB);

        // Voltage (mV)
        int voltage = ReadEC16(0x438);

        // Present rate (mW) - charge or discharge
        int rate = ReadEC16(0x434);

        // Remaining capacity (mWh)
        int remaining = ReadEC16(0x436);

        // Design capacity (mWh)
        int design = ReadEC16(0x402);

        // Full charge capacity (mWh)
        int fullCharge = ReadEC16(0x404);

        // Battery status word
        int status = ReadEC16(0x432);

        // AC connected
        byte acPow = ReadEC(0x490);

        // CGLM
        byte cglm = ReadEC(0x7B9);

        // Battery temp
        byte temp = ReadEC(0x4A2);

        Console.WriteLine("{0:HH:mm:ss}  Mode={1}(0x{2:X2})  SOC={3}%  V={4}mV  Rate={5}mW  Remain={6}mWh  Full={7}mWh  Design={8}mWh  AC={9}  Status=0x{10:X4}  CGLM={11}  Temp={12}C",
            DateTime.Now, modeName, mode, soc, voltage, rate, remaining, fullCharge, design,
            acPow != 0 ? "Y" : "N", status, cglm, temp);
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
            int interval = 2;
            if (args.Length > 0)
            {
                int.TryParse(args[0], out interval);
                if (interval < 1) interval = 1;
            }

            Console.WriteLine("Battery voltage monitor. Interval: {0}s. Press Ctrl+C to stop.", interval);
            Console.WriteLine("Change battery mode in MechControl/OEM app and watch voltage change.");
            Console.WriteLine("---");
            Console.WriteLine("Time      Mode             SOC   Voltage  Rate     Remain   Full     Design   AC  Status  CGLM  Temp");

            while (true)
            {
                ShowState();
                Thread.Sleep(interval * 1000);
            }
        }
        finally
        {
            CloseHandle(hDev);
        }
    }
}
