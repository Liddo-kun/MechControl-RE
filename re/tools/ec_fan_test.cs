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

        string mode = args.Length > 0 ? args[0] : "status";

        if (mode == "status")
        {
            Console.WriteLine("=== Fan Curve Status ===");
            Console.WriteLine("0x741 (activation) = 0x" + ReadEC(0x741).ToString("X2"));
            Console.WriteLine("0x7C5 (OEM5)       = 0x" + ReadEC(0x7C5).ToString("X2") +
                " (bit7=" + ((ReadEC(0x7C5) & 0x80) != 0 ? "ON" : "off") +
                " bit6=" + ((ReadEC(0x7C5) & 0x40) != 0 ? "ON" : "off") + ")");
            Console.WriteLine("0x751 (fan ctrl)    = 0x" + ReadEC(0x751).ToString("X2"));
            Console.WriteLine("0x75B (CPU duty)    = " + (ReadEC(0x75B) / 2) + "%");
            Console.WriteLine("0x75C (GPU duty)    = " + (ReadEC(0x75C) / 2) + "%");
            Console.WriteLine("\nCPU fan table (first 5 points):");
            for (int i = 0; i < 5; i++)
            {
                int tu = ReadEC(0xF00 + i);
                int td = i > 0 ? ReadEC(0xF10 + i) : -1;
                int du = ReadEC(0xF20 + i);
                Console.WriteLine("  [" + i + "] up=" + tu + "C down=" + (td >= 0 ? td + "C" : "n/a") + " duty=" + (du / 2) + "%");
            }
        }
        else if (mode == "write" || mode == "write+apply")
        {
            bool doApply = mode.Contains("+apply");
            Console.WriteLine("Writing test fan curve (high duty to make it obvious)..." +
                (doApply ? " WITH OEM6_APPLY" : " NO OEM6_APPLY"));

            // Ensure 0x7C5 bit 7 is set (fan curve enable)
            int oem5 = ReadEC(0x7C5);
            if ((oem5 & 0x80) == 0)
            {
                WriteEC(0x7C5, oem5 | 0x80);
                Console.WriteLine("Enabled fan curve (0x7C5 bit 7)");
            }

            // Write a simple test curve: all points at high duty
            // Point 0: 30C, 80% duty
            WriteEC(0xF00, 30); WriteEC(0xF20, 160);
            // Point 1: 35C/28C, 85%
            WriteEC(0xF01, 35); WriteEC(0xF11, 28); WriteEC(0xF21, 170);
            // Point 2: 40C/33C, 90%
            WriteEC(0xF02, 40); WriteEC(0xF12, 33); WriteEC(0xF22, 180);
            // Pad remaining with 0xFF/0xB4
            for (int i = 3; i < 16; i++)
            {
                WriteEC(0xF00 + i, 0xFF);
                if (i > 0) WriteEC(0xF10 + i, 0xFF);
                WriteEC(0xF20 + i, 0xB4);
            }

            if (doApply)
            {
                Console.WriteLine("OEM6_APPLY: 0x7C6=4, then 0x7C6=0");
                WriteEC(0x7C6, 0x04);
                WriteEC(0x7C6, 0x00);
            }

            Console.WriteLine("\nWrote curve. Monitoring fan duty...");
            for (int i = 0; i < 8; i++)
            {
                Thread.Sleep(2000);
                int cpuDuty = ReadEC(0x75B) / 2;
                int gpuDuty = ReadEC(0x75C) / 2;
                Console.WriteLine("  +" + ((i + 1) * 2) + "s: CPU=" + cpuDuty + "% GPU=" + gpuDuty + "%");
            }
        }

        CloseHandle(hDev);
    }
}
