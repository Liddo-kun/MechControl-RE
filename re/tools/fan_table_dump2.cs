using System;
using System.Runtime.InteropServices;

class Program
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(
        IntPtr hDevice, uint dwIoControlCode,
        ref int lpInBuffer, int nInBufferSize,
        ref int lpOutBuffer, int nOutBufferSize,
        ref int lpBytesReturned, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr hObject);

    const uint GENERIC_READ = 0x80000000;
    const uint GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_READ = 0x1;
    const uint FILE_SHARE_WRITE = 0x2;
    const uint OPEN_EXISTING = 0x3;
    const uint IOCTL_ECREAD = 0x9C40A488;

    static byte ReadEc(IntPtr h, int addr)
    {
        int inBuf = addr;
        int outBuf = 0;
        int ret = 0;
        DeviceIoControl(h, IOCTL_ECREAD, ref inBuf, 4, ref outBuf, 4, ref ret, IntPtr.Zero);
        return (byte)(outBuf & 0xFF);
    }

    static void Main()
    {
        IntPtr h = CreateFile(@"\\.\ACPIDriver",
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);

        if (h == new IntPtr(-1))
        {
            int err = Marshal.GetLastWin32Error();
            Console.WriteLine("FAILED to open device. Error code: " + err + ". Run as admin.");
            return;
        }

        string[] labels = new string[] {
            "CPU TempUp  [0xF00]",
            "CPU TempDn  [0xF10]",
            "CPU Duty    [0xF20]",
            "GPU TempUp  [0xF30]",
            "GPU TempDn  [0xF40]",
            "GPU Duty    [0xF50]"
        };
        int[] bases = new int[] { 0xF00, 0xF10, 0xF20, 0xF30, 0xF40, 0xF50 };

        // Header
        Console.Write("".PadLeft(22));
        for (int i = 0; i < 16; i++)
            Console.Write(("[" + i + "]").PadLeft(5));
        Console.WriteLine();

        for (int row = 0; row < 6; row++)
        {
            Console.Write(labels[row] + ": ");
            for (int i = 0; i < 16; i++)
            {
                byte val = ReadEc(h, bases[row] + i);
                Console.Write(val.ToString().PadLeft(5));
            }
            Console.WriteLine();
        }

        CloseHandle(h);
    }
}
