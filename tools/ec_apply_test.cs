using System;
using System.Runtime.InteropServices;
using System.Threading;

class Program
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr hDevice, uint dwIoControlCode,
        ref int lpInBuffer, int nInBufferSize, ref int lpOutBuffer, int nOutBufferSize,
        ref int lpBytesReturned, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr hDevice, uint dwIoControlCode,
        byte[] lpInBuffer, int nInBufferSize, ref int lpOutBuffer, int nOutBufferSize,
        ref int lpBytesReturned, IntPtr lpOverlapped);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr hObject);

    const uint IOCTL_READ = 0x9C40A488;
    const uint IOCTL_WRITE = 0x9C40A48C;
    static IntPtr hDev;

    static int ReadEC(int addr)
    {
        int inBuf = addr, outBuf = 0, ret = 0;
        DeviceIoControl(hDev, IOCTL_READ, ref inBuf, 4, ref outBuf, 4, ref ret, IntPtr.Zero);
        return outBuf & 0xFF;
    }

    static void WriteEC(int addr, int val)
    {
        byte[] inBuf = new byte[8];
        inBuf[0] = (byte)(addr & 0xFF);
        inBuf[1] = (byte)((addr >> 8) & 0xFF);
        inBuf[4] = (byte)(val & 0xFF);
        int outBuf = 0, ret = 0;
        DeviceIoControl(hDev, IOCTL_WRITE, inBuf, 8, ref outBuf, 4, ref ret, IntPtr.Zero);
    }

    static void Main()
    {
        hDev = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (hDev == new IntPtr(-1)) { Console.WriteLine("FAILED - run as admin"); return; }

        // Ensure 0x741 is set (activation byte)
        Console.WriteLine("Setting 0x741 = 0x05 (activation)...");
        WriteEC(0x741, 0x05);

        // Read current values
        Console.WriteLine("\n=== Before ===");
        Console.WriteLine("  PL1 (0x783) = " + ReadEC(0x783) + "W");
        Console.WriteLine("  PL2 (0x784) = " + ReadEC(0x784) + "W");
        Console.WriteLine("  PL4 (0x785) = " + (ReadEC(0x785) * 2) + "W (raw=" + ReadEC(0x785) + ")");
        Console.WriteLine("  TCC (0x786) = 0x" + ReadEC(0x786).ToString("X2"));
        Console.WriteLine("  7C6 (APPLY) = 0x" + ReadEC(0x7C6).ToString("X2"));

        // Write PL group (same as OEM: 210/210/346W, 85C)
        Console.WriteLine("\n=== Writing PL1=210, PL2=210, PL4=346, TCC=85C ===");
        WriteEC(0x783, 210);  // PL1 = 210W
        WriteEC(0x784, 210);  // PL2 = 210W
        WriteEC(0x785, 173);  // PL4 = 173*2 = 346W
        WriteEC(0x786, 0x94); // TCC = 0x80 | (105-85) = 0x94

        Console.WriteLine("  PL1 immediate readback = " + ReadEC(0x783) + "W");

        // OEM6_APPLY sequence
        Console.WriteLine("\n=== OEM6_APPLY: 0x7C6 = 0x04, then 0x00 ===");
        WriteEC(0x7C6, 0x04);
        int applyRead = ReadEC(0x7C6);
        Console.WriteLine("  0x7C6 after write = 0x" + applyRead.ToString("X2"));
        WriteEC(0x7C6, 0x00);

        // Wait a moment for EC to process
        Thread.Sleep(500);

        // Read back
        Console.WriteLine("\n=== After (500ms later) ===");
        Console.WriteLine("  PL1 (0x783) = " + ReadEC(0x783) + "W");
        Console.WriteLine("  PL2 (0x784) = " + ReadEC(0x784) + "W");
        Console.WriteLine("  PL4 (0x785) = " + (ReadEC(0x785) * 2) + "W (raw=" + ReadEC(0x785) + ")");
        Console.WriteLine("  TCC (0x463) = " + (105 - ReadEC(0x463)) + "C (raw=" + ReadEC(0x463) + ")");

        // Wait longer and check again
        Thread.Sleep(2000);
        Console.WriteLine("\n=== After (2.5s later) ===");
        Console.WriteLine("  PL1 (0x783) = " + ReadEC(0x783) + "W");
        Console.WriteLine("  PL2 (0x784) = " + ReadEC(0x784) + "W");

        CloseHandle(hDev);
    }
}
