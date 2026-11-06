using System;
using System.IO;
using System.Runtime.InteropServices;

/// <summary>
/// Dump the full EC memory space (0x0000-0xFFFF) via ACPIDriver.
/// Usage:
///   ec_dump.exe                  -- dump to ec_dump.bin + hex summary
///   ec_dump.exe 0x0000 0x0FFF   -- dump specific range
/// </summary>
class Program
{
    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    const uint OPEN_EXISTING = 3, FILE_SHARE_RW = 3;
    const uint IOCTL_ECREAD = 0x9C40A488;

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
            int start = 0x0000;
            int end = 0xFFFF;

            if (args.Length >= 2)
            {
                start = Convert.ToInt32(args[0], args[0].StartsWith("0x") ? 16 : 10);
                end = Convert.ToInt32(args[1], args[1].StartsWith("0x") ? 16 : 10);
            }

            int size = end - start + 1;
            Console.WriteLine("Dumping EC 0x{0:X4}-0x{1:X4} ({2} bytes)...", start, end, size);

            byte[] data = new byte[size];
            int lastPct = -1;

            for (int i = 0; i < size; i++)
            {
                data[i] = ReadEC(start + i);
                int pct = (i * 100) / size;
                if (pct != lastPct && pct % 10 == 0)
                {
                    Console.Write("\r  {0}%", pct);
                    lastPct = pct;
                }
            }
            Console.WriteLine("\r  100%  Done.");

            // Write binary dump
            string binFile = "ec_dump.bin";
            if (args.Length >= 2)
                binFile = string.Format("ec_dump_{0:X4}_{1:X4}.bin", start, end);
            File.WriteAllBytes(binFile, data);
            Console.WriteLine("Binary: {0}", binFile);

            // Write hex dump
            string hexFile = Path.ChangeExtension(binFile, ".txt");
            using (var sw = new StreamWriter(hexFile))
            {
                for (int i = 0; i < size; i += 16)
                {
                    sw.Write("  0x{0:X4}:", start + i);
                    string ascii = "";
                    for (int j = 0; j < 16 && (i + j) < size; j++)
                    {
                        sw.Write(" {0:X2}", data[i + j]);
                        char c = (char)data[i + j];
                        ascii += (c >= 0x20 && c < 0x7F) ? c : '.';
                    }
                    sw.WriteLine("  {0}", ascii);
                }
            }
            Console.WriteLine("Hex:    {0}", hexFile);

            // Summary: find non-zero regions
            Console.WriteLine("\nNon-zero regions:");
            bool inRegion = false;
            int regionStart = 0;
            int nonZeroCount = 0;
            for (int i = 0; i < size; i++)
            {
                if (data[i] != 0x00 && data[i] != 0xFF)
                {
                    if (!inRegion) { regionStart = start + i; inRegion = true; }
                    nonZeroCount++;
                }
                else if (inRegion)
                {
                    // Allow small gaps (up to 8 bytes of 00/FF)
                    bool gap = true;
                    for (int g = 1; g <= 8 && (i + g) < size; g++)
                    {
                        if (data[i + g] != 0x00 && data[i + g] != 0xFF) { gap = false; break; }
                    }
                    if (gap || i - (regionStart - start) > 256)
                    {
                        Console.WriteLine("  0x{0:X4}-0x{1:X4}  ({2} bytes)",
                            regionStart, start + i - 1, (start + i) - regionStart);
                        inRegion = false;
                    }
                }
            }
            if (inRegion)
                Console.WriteLine("  0x{0:X4}-0x{1:X4}  ({2} bytes)",
                    regionStart, end, end - regionStart + 1);

            Console.WriteLine("\nTotal non-zero/non-FF bytes: {0} of {1}", nonZeroCount, size);
        }
        finally
        {
            CloseHandle(hDev);
        }
    }
}
