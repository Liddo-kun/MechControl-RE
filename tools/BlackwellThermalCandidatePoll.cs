using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Threading;

// Read-only transient capture of fixed candidates found by BlackwellThermalPageSweep.
// There are no address arguments and no physical-memory or write operations.
class BlackwellThermalCandidatePoll
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

    static readonly uint[] Offsets = {
        0x00AD0074, 0x00AD0104, 0x00AD013C, 0x00AD014C, 0x00AD021C,
        0x00AD0460, 0x00AD0464, 0x00AD04B8, 0x00AD04BC, 0x00AD04C0,
        0x00AD04C4, 0x00AD06EC, 0x00AD0844, 0x00AD0848,
        0x00AD0A90, 0x00AD0A94, 0x00AD0A98, 0x00AD0A9C, 0x00AD0AA0,
        0x00AD0AA4, 0x00AD0AD0, 0x00AD0AD4, 0x00AD0AD8, 0x00AD0ADC,
        0x00AD0AE0, 0x00AD0AE4, 0x00AD0B50, 0x00AD0B54, 0x00AD0B58,
        0x00AD0B5C, 0x00AD0B70, 0x00AD0B74, 0x00AD0B78, 0x00AD0B7C
    };

    static bool ReadPciBarDword(IntPtr h, uint offset, out uint value)
    {
        byte[] input = new byte[16];
        BitConverter.GetBytes(NvidiaBdf).CopyTo(input, 0);
        BitConverter.GetBytes(0x10u).CopyTo(input, 4);
        BitConverter.GetBytes(1u).CopyTo(input, 8);
        BitConverter.GetBytes(offset).CopyTo(input, 12);

        byte[] output = new byte[8];
        uint returned;
        bool ioctlOk = DeviceIoControl(h, IoctlReadPciBarDword, input, 16,
            output, 8, out returned, IntPtr.Zero);
        value = ioctlOk && returned >= 8 ? BitConverter.ToUInt32(output, 4) : 0;
        return ioctlOk && returned >= 8 && BitConverter.ToUInt32(output, 0) == 1;
    }

    static int Main(string[] args)
    {
        int seconds = 180;
        if (args.Length > 1 || (args.Length == 1 &&
            (!int.TryParse(args[0], out seconds) || seconds < 1 || seconds > 300)))
        {
            Console.Error.WriteLine("usage: BlackwellThermalCandidatePoll [seconds 1-300]");
            return 1;
        }

        IntPtr h = CreateFileW(@"\\.\cpuz165", 0, FileShareRead | FileShareWrite,
            IntPtr.Zero, OpenExisting, 0, IntPtr.Zero);
        if (h == new IntPtr(-1))
        {
            Console.Error.WriteLine("Unable to open \\.\\cpuz165: " +
                new Win32Exception(Marshal.GetLastWin32Error()).Message);
            return 1;
        }

        int errors = 0;
        DateTime start = DateTime.UtcNow;
        Console.Write("timeMs");
        foreach (uint offset in Offsets)
            Console.Write(",0x" + offset.ToString("X8"));
        Console.WriteLine();

        try
        {
            while ((DateTime.UtcNow - start).TotalSeconds < seconds)
            {
                Console.Write((long)(DateTime.UtcNow - start).TotalMilliseconds);
                foreach (uint offset in Offsets)
                {
                    uint raw;
                    bool ok = ReadPciBarDword(h, offset, out raw);
                    bool valid = ok && raw != 0xFFFFFFFF &&
                        (raw & 0x40000000) != 0 && (raw & 0xFFFF) <= 150 * 256;
                    if (!ok) errors++;
                    Console.Write(valid ? "," + ((raw & 0xFFFF) / 256.0).ToString("F4") : ",");
                }
                Console.WriteLine();
                Thread.Sleep(500);
            }
        }
        finally
        {
            CloseHandle(h);
        }

        Console.Error.WriteLine("errors=" + errors);
        return errors == 0 ? 0 : 2;
    }
}
