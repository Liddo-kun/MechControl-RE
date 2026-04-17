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

    static void Main(string[] args)
    {
        hDev = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (hDev == new IntPtr(-1)) { Console.WriteLine("FAILED - run as admin"); return; }

        if (args.Length == 0) { Console.WriteLine("Usage: ec_fan_test2.exe <test1|test2|test3>"); CloseHandle(hDev); return; }

        string mode = args[0];

        if (mode == "test1")
        {
            // Test: 0x741 + fan table + 0x7C6=4 (LEAVE it at 4, don't clear)
            Console.WriteLine("=== Test 1: 0x741=5, fan table, 0x7C6=4 (leave set) ===");
            WriteEC(0x741, 0x05);
            Console.WriteLine("0x741 = 0x" + ReadEC(0x741).ToString("X2"));

            // Enable fan curve
            int oem5 = ReadEC(0x7C5);
            WriteEC(0x7C5, oem5 | 0x80);

            // Write high-duty test curve
            WriteEC(0xF00, 30); WriteEC(0xF20, 160);
            WriteEC(0xF01, 35); WriteEC(0xF11, 28); WriteEC(0xF21, 170);
            WriteEC(0xF02, 40); WriteEC(0xF12, 33); WriteEC(0xF22, 180);
            for (int i = 3; i < 16; i++)
            {
                WriteEC(0xF00 + i, 0xFF);
                if (i > 0) WriteEC(0xF10 + i, 0xFF);
                WriteEC(0xF20 + i, 0xB4);
            }

            // Set 0x7C6=4 and LEAVE IT
            WriteEC(0x7C6, 0x04);
            Console.WriteLine("0x7C6 = 0x" + ReadEC(0x7C6).ToString("X2") + " (should be 04)");

            Console.WriteLine("\nMonitoring fan duty...");
            for (int i = 0; i < 8; i++)
            {
                Thread.Sleep(2000);
                Console.WriteLine("  +" + ((i + 1) * 2) + "s: CPU=" + (ReadEC(0x75B) / 2) + "% GPU=" + (ReadEC(0x75C) / 2) + "% 7C6=0x" + ReadEC(0x7C6).ToString("X2"));
            }
        }
        else if (mode == "test2")
        {
            // Test: just set 0x7C6=4 (fan table already written from previous test)
            Console.WriteLine("=== Test 2: just set 0x7C6=4 ===");
            WriteEC(0x7C6, 0x04);
            Console.WriteLine("0x7C6 = 0x" + ReadEC(0x7C6).ToString("X2"));

            Console.WriteLine("\nMonitoring fan duty...");
            for (int i = 0; i < 5; i++)
            {
                Thread.Sleep(2000);
                Console.WriteLine("  +" + ((i + 1) * 2) + "s: CPU=" + (ReadEC(0x75B) / 2) + "% GPU=" + (ReadEC(0x75C) / 2) + "% 7C6=0x" + ReadEC(0x7C6).ToString("X2"));
            }
        }
        else if (mode == "test3")
        {
            // Test: 0x741 only, no 0x7C6
            Console.WriteLine("=== Test 3: 0x741=5 only, fan table, NO 0x7C6 ===");
            WriteEC(0x741, 0x05);

            int oem5 = ReadEC(0x7C5);
            WriteEC(0x7C5, oem5 | 0x80);

            WriteEC(0xF00, 30); WriteEC(0xF20, 160);
            WriteEC(0xF01, 35); WriteEC(0xF11, 28); WriteEC(0xF21, 170);
            WriteEC(0xF02, 40); WriteEC(0xF12, 33); WriteEC(0xF22, 180);
            for (int i = 3; i < 16; i++)
            {
                WriteEC(0xF00 + i, 0xFF);
                if (i > 0) WriteEC(0xF10 + i, 0xFF);
                WriteEC(0xF20 + i, 0xB4);
            }

            Console.WriteLine("0x7C6 = 0x" + ReadEC(0x7C6).ToString("X2") + " (should be 00)");

            Console.WriteLine("\nMonitoring fan duty...");
            for (int i = 0; i < 8; i++)
            {
                Thread.Sleep(2000);
                Console.WriteLine("  +" + ((i + 1) * 2) + "s: CPU=" + (ReadEC(0x75B) / 2) + "% GPU=" + (ReadEC(0x75C) / 2) + "%");
            }
        }

        CloseHandle(hDev);
    }
}
