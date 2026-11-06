using System;
using System.Runtime.InteropServices;
using System.Threading;

/// <summary>
/// Clear BPST (0x490 bit 1) via direct EC write to stop charging.
/// Usage:
///   bat_bpst_test.exe           -- clear BPST every 2.5s until Ctrl+C
///   bat_bpst_test.exe once      -- single clear, watch for 10s
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

    static void ShowState()
    {
        byte ps = ReadEC(0x490);
        int rate = ReadEC16(0x434);
        int voltage = ReadEC16(0x438);
        byte soc = ReadEC(0x4AB);
        int temp = ReadEC16(0x4A2);
        double tempC = (temp / 10.0) - 273.15;
        Console.WriteLine("  0x490=0x{0:X2} BPST={1}  Rate={2}mA  V={3}mV  SOC={4}%  Temp={5:F1}C",
            ps, (ps >> 1) & 1, rate, voltage, soc, tempC);
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
            Console.WriteLine("=== BPST (0x490 bit 1) direct EC write test ===");
            ShowState();

            if (args.Length > 0 && args[0].ToLower() == "once")
            {
                byte cur = ReadEC(0x490);
                byte cleared = (byte)(cur & ~0x02);
                Console.WriteLine("\nClearing BPST: 0x{0:X2} -> 0x{1:X2}", cur, cleared);
                WriteEC(0x490, cleared);
                for (int i = 0; i < 10; i++)
                {
                    Thread.Sleep(1000);
                    Console.Write("[+{0}s]", i + 1);
                    ShowState();
                }
            }
            else
            {
                Console.WriteLine("\nClearing BPST every 2.5s. Press Ctrl+C to stop.");
                Console.WriteLine();
                bool running = true;
                Console.CancelKeyPress += (s, e) => { e.Cancel = true; running = false; };
                int count = 0;
                while (running)
                {
                    byte cur = ReadEC(0x490);
                    byte cleared = (byte)(cur & ~0x02);
                    WriteEC(0x490, cleared);
                    count++;
                    Console.Write("[{0}]", count);
                    ShowState();
                    Thread.Sleep(2500);
                }
                Console.WriteLine("\nStopped. Final state:");
                ShowState();
            }
        }
        finally
        {
            CloseHandle(hDev);
        }
    }
}
