using System;
using System.Runtime.InteropServices;
using System.Threading;

/// <summary>
/// Test write to 0x7A6 using the CORRECT buffer format (matching MechControl's EcDevice).
/// MechControl uses int[2] = 8 bytes: [addr_as_int32, value_as_int32]
/// Our other tools used byte[5] = [addr_lo, addr_hi, value, 0, 0] which may be wrong.
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

    // Also keep the byte[] overload for the old format
    [DllImport("kernel32.dll", SetLastError = true, EntryPoint = "DeviceIoControl")]
    static extern bool DeviceIoControlBytes(IntPtr h, uint code,
        byte[] lpIn, uint inSz, byte[] lpOut, uint outSz, out uint ret, IntPtr ov);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    static IntPtr hDev;

    // NEW format: matches MechControl's EcDevice.WriteByteInternal
    static byte ReadNew(int addr)
    {
        int inBuf = addr;
        int outBuf = 0;
        uint r;
        DeviceIoControl(hDev, IOCTL_ECREAD, new int[] { inBuf }, 4, ref outBuf, 4, out r, IntPtr.Zero);
        return (byte)(outBuf & 0xFF);
    }

    static void WriteNew(int addr, byte val)
    {
        int[] inBuf = new int[] { addr, val };
        int outBuf = 0;
        uint r;
        DeviceIoControl(hDev, IOCTL_ECWRITE, inBuf, 8, ref outBuf, 4, out r, IntPtr.Zero);
    }

    // OLD format: what our test tools have been using
    static byte ReadOld(int addr)
    {
        byte[] inBuf = new byte[] { (byte)(addr & 0xFF), (byte)((addr >> 8) & 0xFF), 0, 0 };
        byte[] outBuf = new byte[4];
        uint r;
        DeviceIoControlBytes(hDev, IOCTL_ECREAD, inBuf, 4, outBuf, 4, out r, IntPtr.Zero);
        return outBuf[0];
    }

    static void WriteOld(int addr, byte val)
    {
        byte[] inBuf = new byte[] { (byte)(addr & 0xFF), (byte)((addr >> 8) & 0xFF), val, 0, 0 };
        byte[] outBuf = new byte[4];
        uint r;
        DeviceIoControlBytes(hDev, IOCTL_ECWRITE, inBuf, 5, outBuf, 4, out r, IntPtr.Zero);
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
            byte cur = ReadNew(0x7A6);
            Console.WriteLine("Current 0x7A6 = 0x{0:X2} (mode bits: {1})", cur, (cur >> 3) & 7);
            Console.WriteLine();

            // Read-modify-write: set mode to High (1) preserving other bits
            byte target = (byte)((cur & ~0x38) | 0x08);

            // Test 1: OLD format (byte[5])
            Console.WriteLine("=== OLD format (byte[5]) ===");
            WriteOld(0x7A6, target);
            Thread.Sleep(10);
            byte rb1 = ReadNew(0x7A6);
            Console.WriteLine("  Wrote 0x{0:X2}, readback = 0x{1:X2}  {2}", target, rb1,
                rb1 == target ? "STUCK" : "REJECTED");

            // Restore
            if (rb1 == target)
            {
                WriteOld(0x7A6, cur);
                Thread.Sleep(100);
            }

            // Test 2: NEW format (int[2], 8 bytes) - matches MechControl
            Console.WriteLine();
            Console.WriteLine("=== NEW format (int[2], 8 bytes - matches MechControl) ===");
            WriteNew(0x7A6, target);
            Thread.Sleep(10);
            byte rb2 = ReadNew(0x7A6);
            Console.WriteLine("  Wrote 0x{0:X2}, readback = 0x{1:X2}  {2}", target, rb2,
                rb2 == target ? "STUCK" : "REJECTED");

            // Restore
            if (rb2 == target)
            {
                WriteNew(0x7A6, cur);
                Thread.Sleep(100);
                Console.WriteLine("  Restored to 0x{0:X2}", ReadNew(0x7A6));
            }

            Console.WriteLine();
            Console.WriteLine("Final 0x7A6 = 0x{0:X2}", ReadNew(0x7A6));
        }
        finally
        {
            CloseHandle(hDev);
        }
    }
}
