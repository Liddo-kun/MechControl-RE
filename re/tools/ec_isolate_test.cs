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

        if (args.Length == 0)
        {
            Console.WriteLine("Usage:");
            Console.WriteLine("  ec_isolate_test.exe clear741    -- Set 0x741=0x00 (simulate service stopped)");
            Console.WriteLine("  ec_isolate_test.exe set741      -- Set 0x741=0x05 only (no OEM6_APPLY)");
            Console.WriteLine("  ec_isolate_test.exe writepl     -- Write PL1=150W, NO OEM6_APPLY, check if it sticks");
            Console.WriteLine("  ec_isolate_test.exe writepl+apply -- Write PL1=150W WITH OEM6_APPLY, check if it sticks");
            CloseHandle(hDev);
            return;
        }

        string mode = args[0];

        if (mode == "clear741")
        {
            WriteEC(0x741, 0x00);
            Console.WriteLine("0x741 = 0x" + ReadEC(0x741).ToString("X2") + " (cleared)");
        }
        else if (mode == "set741")
        {
            WriteEC(0x741, 0x05);
            Console.WriteLine("0x741 = 0x" + ReadEC(0x741).ToString("X2") + " (set)");
        }
        else if (mode == "writepl" || mode == "writepl+apply")
        {
            bool doApply = mode.Contains("+apply");
            Console.WriteLine("0x741 = 0x" + ReadEC(0x741).ToString("X2"));
            Console.WriteLine("Writing PL1=150W (0x783=150)..." + (doApply ? " WITH OEM6_APPLY" : " NO OEM6_APPLY"));
            WriteEC(0x783, 150);
            if (doApply)
            {
                WriteEC(0x7C6, 0x04);
                WriteEC(0x7C6, 0x00);
            }
            Console.WriteLine("Immediate: PL1 = " + ReadEC(0x783) + "W");
            for (int i = 1; i <= 5; i++)
            {
                Thread.Sleep(2000);
                Console.WriteLine("+" + (i * 2) + "s:       PL1 = " + ReadEC(0x783) + "W");
            }
        }

        CloseHandle(hDev);
    }
}
