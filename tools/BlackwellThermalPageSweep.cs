using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

// Read-only diagnostic for the single Blackwell thermal MMIO page already used by HWMonitor.
// Deliberately accepts no address arguments and exposes no physical-memory or write operations.
class BlackwellThermalPageSweep
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, ExactSpelling = true)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, byte[] inBuf, uint inLen,
        byte[] outBuf, uint outLen, out uint ret, IntPtr ov);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    const uint FileShareRead = 1;
    const uint FileShareWrite = 2;
    const uint OpenExisting = 3;
    const uint IoctlReadPciBarDword = 0x9C4025D4;
    const uint NvidiaBdf = 0x80020000; // Test system: 0000:02:00.0
    const uint PageStart = 0x00AD0000;
    const uint PageEnd = 0x00AD0FFC;

    static bool ReadPciBarDword(IntPtr h, uint offset, out uint value)
    {
        if (offset < PageStart || offset > PageEnd || (offset & 3) != 0)
            throw new ArgumentOutOfRangeException("offset");

        byte[] input = new byte[16];
        BitConverter.GetBytes(NvidiaBdf).CopyTo(input, 0);
        BitConverter.GetBytes(0x10u).CopyTo(input, 4); // PCI BAR0 configuration register
        BitConverter.GetBytes(1u).CopyTo(input, 8);    // Permit a 64-bit BAR
        BitConverter.GetBytes(offset).CopyTo(input, 12);

        byte[] output = new byte[8];
        uint returned;
        bool ioctlOk = DeviceIoControl(h, IoctlReadPciBarDword, input, 16,
            output, 8, out returned, IntPtr.Zero);
        value = ioctlOk && returned >= 8 ? BitConverter.ToUInt32(output, 4) : 0;
        return ioctlOk && returned >= 8 && BitConverter.ToUInt32(output, 0) == 1;
    }

    static int Main()
    {
        IntPtr h = CreateFileW(@"\\.\cpuz165", 0, FileShareRead | FileShareWrite,
            IntPtr.Zero, OpenExisting, 0, IntPtr.Zero);
        if (h == new IntPtr(-1))
        {
            Console.Error.WriteLine("Unable to open \\.\\cpuz165: " +
                new Win32Exception(Marshal.GetLastWin32Error()).Message);
            return 1;
        }

        int reads = 0;
        int errors = 0;
        int candidates = 0;
        Console.WriteLine("# Blackwell BAR0 thermal-page sweep");
        Console.WriteLine("# UTC=" + DateTime.UtcNow.ToString("O"));
        Console.WriteLine("# BDF=0000:02:00.0 range=0xAD0000-0xAD0FFC stride=4 read-only");
        Console.WriteLine("offset,raw,tempC");

        try
        {
            for (uint offset = PageStart; offset <= PageEnd; offset += 4)
            {
                uint raw;
                bool ok = ReadPciBarDword(h, offset, out raw);
                reads++;
                if (!ok)
                {
                    errors++;
                    continue;
                }

                bool valid = raw != 0xFFFFFFFF && (raw & 0x40000000) != 0;
                double tempC = (raw & 0xFFFF) / 256.0;
                if (valid && tempC >= 0.0 && tempC <= 150.0)
                {
                    candidates++;
                    Console.WriteLine("0x" + offset.ToString("X8") + ",0x" +
                        raw.ToString("X8") + "," + tempC.ToString("F4"));
                }
            }
        }
        finally
        {
            CloseHandle(h);
        }

        Console.Error.WriteLine("reads=" + reads + " errors=" + errors +
            " plausibleCandidates=" + candidates);
        return errors == 0 ? 0 : 2;
    }
}
