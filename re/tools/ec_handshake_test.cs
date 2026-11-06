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

        string mode = args.Length > 0 ? args[0] : "handshake";

        if (mode == "handshake" || mode == "all")
        {
            Console.WriteLine("=== EC Handshake Test ===");
            Console.WriteLine("Before:");
            Console.WriteLine("  0x708 = 0x" + ReadEC(0x708).ToString("X2"));
            Console.WriteLine("  0x709 = 0x" + ReadEC(0x709).ToString("X2"));

            Console.WriteLine("\nWriting 0x709=0x05, 0x708=0x01 (OEM handshake)...");
            WriteEC(0x709, 0x05);
            WriteEC(0x708, 0x01);

            int v708 = ReadEC(0x708);
            Console.WriteLine("\nAfter:");
            Console.WriteLine("  0x708 = 0x" + v708.ToString("X2") + (v708 == 1 ? " (OK)" : " (UNEXPECTED)"));
            Console.WriteLine("  0x709 = 0x" + ReadEC(0x709).ToString("X2"));
        }

        if (mode == "748" || mode == "all")
        {
            Console.WriteLine("\n=== 0x748 Feature Enable ===");
            Console.WriteLine("Before: 0x748 = 0x" + ReadEC(0x748).ToString("X2"));
            WriteEC(0x748, 0x81);
            Console.WriteLine("After:  0x748 = 0x" + ReadEC(0x748).ToString("X2"));
        }

        if (mode == "741" || mode == "all")
        {
            Console.WriteLine("\n=== 0x741 Activation Byte ===");
            int cur = ReadEC(0x741);
            Console.WriteLine("Before: 0x741 = 0x" + cur.ToString("X2") + " (" + cur + ")");
            // OEM reads and writes back (possibly +1)
            WriteEC(0x741, cur);
            Console.WriteLine("After:  0x741 = 0x" + ReadEC(0x741).ToString("X2"));
        }

        if (mode == "7c5" || mode == "all")
        {
            Console.WriteLine("\n=== 0x7C5 Bit 6 Test ===");
            int cur = ReadEC(0x7C5);
            Console.WriteLine("Before: 0x7C5 = 0x" + cur.ToString("X2") + " (bin: " + Convert.ToString(cur, 2).PadLeft(8, '0') + ")");
            int next = cur | 0x40; // set bit 6
            WriteEC(0x7C5, next);
            int after = ReadEC(0x7C5);
            Console.WriteLine("After:  0x7C5 = 0x" + after.ToString("X2") + " (bin: " + Convert.ToString(after, 2).PadLeft(8, '0') + ")");
        }

        if (mode == "706" || mode == "all")
        {
            Console.WriteLine("\n=== 0x706 BIOS_CTRL ===");
            Console.WriteLine("Before: 0x706 = 0x" + ReadEC(0x706).ToString("X2"));
            WriteEC(0x706, 0x41);
            Console.WriteLine("After:  0x706 = 0x" + ReadEC(0x706).ToString("X2"));
        }

        if (mode == "watchdog")
        {
            Console.WriteLine("\n=== 0x7CC Watchdog Poll ===");
            Console.WriteLine("Reading 0x7CC every 1.5s (Ctrl+C to stop)...");
            while (true)
            {
                Console.WriteLine("  [" + DateTime.Now.ToString("HH:mm:ss") + "] 0x7CC = 0x" + ReadEC(0x7CC).ToString("X2"));
                System.Threading.Thread.Sleep(1500);
            }
        }

        if (mode == "status")
        {
            Console.WriteLine("=== Status Check ===");
            int[] addrs = { 0x706, 0x708, 0x709, 0x741, 0x748, 0x749, 0x74A, 0x74B, 0x74E, 0x766, 0x7C5, 0x7C6, 0x7C7, 0x7CC };
            string[] names = { "BIOS_CTRL", "Handshake1", "Handshake0", "OEM_Byte", "FeatureEn", "Cmd1", "Cmd2", "Cmd3", "Cmd4", "Support2", "OEM5", "OEM6_APPLY", "OEM_7C7", "Watchdog" };
            for (int i = 0; i < addrs.Length; i++)
                Console.WriteLine("  [0x" + addrs[i].ToString("X3") + "] " + names[i].PadRight(14) + " = 0x" + ReadEC(addrs[i]).ToString("X2"));
        }

        Console.WriteLine("\nDone. Now test MechControl — do fan curves and power limits work?");
        CloseHandle(hDev);
    }
}
