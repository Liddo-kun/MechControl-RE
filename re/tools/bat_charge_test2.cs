using System;
using System.Runtime.InteropServices;
using System.Threading;

class Program
{
    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    const uint OPEN_EXISTING = 3, FILE_SHARE_RW = 3;
    const uint IOCTL_ECREAD = 0x9C40A488, IOCTL_ECWRITE = 0x9C40A48C;

    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Auto)]
    static extern IntPtr CreateFile(string f, uint a, uint s, IntPtr z, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, int[] i, uint iSz, ref int o, uint oSz, out uint ret, IntPtr ov);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    static IntPtr hDev;

    static byte ReadEC(int addr) {
        int o = 0; uint r;
        DeviceIoControl(hDev, IOCTL_ECREAD, new int[] { addr }, 4, ref o, 4, out r, IntPtr.Zero);
        return (byte)(o & 0xFF);
    }
    static void WriteEC(int addr, byte val) {
        int[] i = new int[] { addr, val }; int o = 0; uint r;
        DeviceIoControl(hDev, IOCTL_ECWRITE, i, 8, ref o, 4, out r, IntPtr.Zero);
    }

    static void Dump(string label) {
        Console.WriteLine("--- {0} ---", label);
        Console.WriteLine("  0x7A6 mode     = 0x{0:X2}", ReadEC(0x7A6));
        Console.WriteLine("  0x7B9 limit up = {0}", ReadEC(0x7B9));
        Console.WriteLine("  0x7D0 limit dn = {0}", ReadEC(0x7D0));
        Console.WriteLine("  0x4AB SOC      = {0}%", ReadEC(0x4AB));
    }

    static void Main(string[] args)
    {
        hDev = CreateFile(@"\\.\ACPIDriver", GENERIC_READ|GENERIC_WRITE, FILE_SHARE_RW,
            IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (hDev == new IntPtr(-1)) { Console.WriteLine("Cannot open driver"); return; }

        try {
            Dump("BEFORE");

            Console.WriteLine("\nWriting 0x7B9=80, 0x7D0=70...");
            WriteEC(0x7B9, 80);
            WriteEC(0x7D0, 70);

            // Immediate readback
            Dump("IMMEDIATE (0ms)");

            // Delayed readbacks
            for (int delay = 100; delay <= 2000; delay += delay < 500 ? 100 : 500) {
                Thread.Sleep(delay < 500 ? 100 : 500);
                byte up = ReadEC(0x7B9);
                byte dn = ReadEC(0x7D0);
                Console.WriteLine("  +{0,4}ms: 0x7B9={1} 0x7D0={2}", delay, up, dn);
                if (up != 0 || dn != 0) {
                    Console.WriteLine("  ** VALUE APPEARED! **");
                    break;
                }
            }

            // Also try: write mode THEN limits (maybe order matters)
            Console.WriteLine("\nTrying: set mode to Balanced first, then write limits...");
            byte cur7A6 = ReadEC(0x7A6);
            WriteEC(0x7A6, (byte)((cur7A6 & ~0x38) | 0x18)); // Balanced
            Thread.Sleep(200);
            WriteEC(0x7B9, 80);
            WriteEC(0x7D0, 70);
            Thread.Sleep(500);
            Dump("AFTER mode+limits");

            // Restore mode
            WriteEC(0x7A6, cur7A6);

            Console.WriteLine("\nResetting limits to 0...");
            WriteEC(0x7B9, 0);
            WriteEC(0x7D0, 0);
        } finally { CloseHandle(hDev); }
    }
}
