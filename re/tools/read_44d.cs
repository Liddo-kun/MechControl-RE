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
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr hObject);

    static void Main()
    {
        IntPtr h = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 0x3, IntPtr.Zero, 0x3, 0, IntPtr.Zero);
        if (h == new IntPtr(-1)) { Console.WriteLine("FAILED - run as admin"); return; }

        // Read 0x44C through 0x44F to see the full picture
        for (int addr = 0x449; addr <= 0x44F; addr++)
        {
            int inBuf = addr, outBuf = 0, ret = 0;
            DeviceIoControl(h, 0x9C40A488, ref inBuf, 4, ref outBuf, 4, ref ret, IntPtr.Zero);
            byte val = (byte)(outBuf & 0xFF);
            string label = "";
            if (addr == 0x449) label = " ACUR (adapter current)";
            else if (addr == 0x44C) label = " GPU power low byte?";
            else if (addr == 0x44D) label = " GPU power high byte?";
            else if (addr == 0x44E) label = " (gap end)";
            else if (addr == 0x44F) label = " VGAT (GPU temp)";
            Console.WriteLine("0x" + addr.ToString("X3") + ": " + val.ToString().PadLeft(4) + "  (0x" + val.ToString("X2") + ")" + label);
        }
        CloseHandle(h);
    }
}
