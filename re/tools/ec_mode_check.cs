using System;
using System.Runtime.InteropServices;

class Program
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr hDevice, uint dwIoControlCode,
        ref int lpInBuffer, int nInBufferSize, ref int lpOutBuffer, int nOutBufferSize,
        ref int lpBytesReturned, IntPtr lpOverlapped);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr hObject);

    const uint IOCTL_READ = 0x9C40A488;
    static IntPtr hDev;

    static int ReadEC(int addr)
    {
        int inBuf = addr, outBuf = 0, ret = 0;
        DeviceIoControl(hDev, IOCTL_READ, ref inBuf, 4, ref outBuf, 4, ref ret, IntPtr.Zero);
        return outBuf & 0xFF;
    }

    static void Main()
    {
        hDev = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (hDev == new IntPtr(-1)) { Console.WriteLine("FAILED - run as admin"); return; }

        Console.WriteLine("=== EC Mode / Fan State Registers ===");
        Console.WriteLine();

        // Known control registers
        Console.WriteLine("-- Control --");
        Console.WriteLine("  0x741 (activation)    = 0x" + ReadEC(0x741).ToString("X2"));
        Console.WriteLine("  0x751 (fan ctrl mode) = 0x" + ReadEC(0x751).ToString("X2"));
        Console.WriteLine("  0x7C5 (OEM5)          = 0x" + ReadEC(0x7C5).ToString("X2"));
        Console.WriteLine("  0x7C6 (OEM6_APPLY)    = 0x" + ReadEC(0x7C6).ToString("X2"));
        Console.WriteLine("  0x7C7 (OEM_7C7)       = 0x" + ReadEC(0x7C7).ToString("X2"));

        // Registers that might indicate mode
        Console.WriteLine();
        Console.WriteLine("-- Possible mode/state --");
        Console.WriteLine("  0x706 (BIOS_CTRL)     = 0x" + ReadEC(0x706).ToString("X2"));
        Console.WriteLine("  0x708 (handshake)     = 0x" + ReadEC(0x708).ToString("X2"));
        Console.WriteLine("  0x726 (OEM9)          = 0x" + ReadEC(0x726).ToString("X2"));
        Console.WriteLine("  0x728                 = 0x" + ReadEC(0x728).ToString("X2"));
        Console.WriteLine("  0x740 (project ID)    = 0x" + ReadEC(0x740).ToString("X2"));
        Console.WriteLine("  0x741 (OEM byte)      = 0x" + ReadEC(0x741).ToString("X2"));
        Console.WriteLine("  0x742 (Support5)      = 0x" + ReadEC(0x742).ToString("X2"));
        Console.WriteLine("  0x748 (FeatureEn)     = 0x" + ReadEC(0x748).ToString("X2"));
        Console.WriteLine("  0x749                 = 0x" + ReadEC(0x749).ToString("X2"));
        Console.WriteLine("  0x74A                 = 0x" + ReadEC(0x74A).ToString("X2"));
        Console.WriteLine("  0x74B                 = 0x" + ReadEC(0x74B).ToString("X2"));
        Console.WriteLine("  0x74C (OEM proj ID)   = 0x" + ReadEC(0x74C).ToString("X2"));
        Console.WriteLine("  0x74E                 = 0x" + ReadEC(0x74E).ToString("X2"));
        Console.WriteLine("  0x765 (Support1)      = 0x" + ReadEC(0x765).ToString("X2"));
        Console.WriteLine("  0x766 (Support2)      = 0x" + ReadEC(0x766).ToString("X2"));
        Console.WriteLine("  0x768 (Status)        = 0x" + ReadEC(0x768).ToString("X2"));

        // Scan 0x780-0x7CF for anything interesting
        Console.WriteLine();
        Console.WriteLine("-- 0x780-0x7CF dump --");
        for (int addr = 0x780; addr <= 0x7CF; addr += 4)
        {
            Console.Write("  ");
            for (int j = 0; j < 4 && addr + j <= 0x7CF; j++)
            {
                int a = addr + j;
                Console.Write("0x" + a.ToString("X3") + "=" + ReadEC(a).ToString("X2") + "  ");
            }
            Console.WriteLine();
        }

        // Fan-related
        Console.WriteLine();
        Console.WriteLine("-- Fan state --");
        Console.WriteLine("  0x75B (CPU duty)      = " + ReadEC(0x75B) + " (" + (ReadEC(0x75B) / 2) + "%)");
        Console.WriteLine("  0x75C (GPU duty)      = " + ReadEC(0x75C) + " (" + (ReadEC(0x75C) / 2) + "%)");
        Console.WriteLine("  0x787 (fan switch spd) = 0x" + ReadEC(0x787).ToString("X2"));

        CloseHandle(hDev);
    }
}
