using System;
using System.Runtime.InteropServices;

/// <summary>
/// Write specific EC registers via UWACPIDriver.
/// Usage: ec_write.exe 0x7E6 0xBC [0x7E7 0x88 ...]
/// </summary>
class EcWrite
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(string path, uint access, uint share,
        IntPtr sec, uint disp, uint flags, IntPtr templ);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr dev, uint code,
        int[] inBuf, int inSize, ref int outBuf, int outSize,
        out int bytes, IntPtr overlapped);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    const uint IOCTL_ECWRITE = 0x9C40A48C;
    const uint IOCTL_ECREAD = 0x9C40A488;

    static void Main(string[] args)
    {
        if (args.Length < 2 || args.Length % 2 != 0)
        {
            Console.WriteLine("Usage: ec_write.exe <addr> <value> [<addr> <value> ...]");
            Console.WriteLine("Example: ec_write.exe 0x7E6 0xBC 0x7E7 0x88 0x7E8 0x20");
            return;
        }

        IntPtr h = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h == new IntPtr(-1))
        {
            Console.WriteLine("Failed to open ACPIDriver. Run as admin.");
            return;
        }

        for (int i = 0; i < args.Length; i += 2)
        {
            int addr = Convert.ToInt32(args[i], 16);
            int val = Convert.ToInt32(args[i + 1], 16);

            // Read before
            int inBuf = addr, outBuf = 0;
            int bytes;
            DeviceIoControl(h, IOCTL_ECREAD, new int[] { addr }, 4, ref outBuf, 4, out bytes, IntPtr.Zero);
            byte before = (byte)(outBuf & 0xFF);

            // Write
            int[] writeBuf = { addr, val };
            int dummy = 0;
            bool ok = DeviceIoControl(h, IOCTL_ECWRITE, writeBuf, 8, ref dummy, 4, out bytes, IntPtr.Zero);

            // Read after
            outBuf = 0;
            DeviceIoControl(h, IOCTL_ECREAD, new int[] { addr }, 4, ref outBuf, 4, out bytes, IntPtr.Zero);
            byte after = (byte)(outBuf & 0xFF);

            Console.WriteLine("  [0x{0:X3}] {1,3} -> {2,3} (wrote 0x{3:X2}, readback 0x{4:X2}) {5}",
                addr, before, after, val, after, ok ? "OK" : "FAIL");
        }

        CloseHandle(h);
    }
}
