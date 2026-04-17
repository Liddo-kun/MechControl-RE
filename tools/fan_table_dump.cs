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

        string[] labels = { "CPU TempUp ", "CPU TempDn ", "CPU Duty   ", "GPU TempUp ", "GPU TempDn ", "GPU Duty   " };
        int[] bases =     {  0xF00,         0xF10,         0xF20,         0xF30,         0xF40,         0xF50 };

        for (int row = 0; row < 6; row++)
        {
            Console.Write(labels[row] + " [0x" + bases[row].ToString("X3") + "]: ");
            for (int i = 0; i < 16; i++)
            {
                int addr = bases[row] + i;
                int inBuf = addr;
                int outBuf = 0;
                int ret = 0;
                DeviceIoControl(h, 0x9C40A488, ref inBuf, 4, ref outBuf, 4, ref ret, IntPtr.Zero);
                byte val = (byte)(outBuf & 0xFF);
                Console.Write(val.ToString().PadLeft(4));
            }
            Console.WriteLine();
        }
        // Control registers
        int[] cRegs = { 0x709, 0x708, 0x741, 0x748, 0x751, 0x7C5, 0x7C6, 0x7C7, 0x706, 0x75B, 0x75C, 0x783, 0x784 };
        string[] cNames = { "Handshake1", "Handshake2", "Activation", "FeatureEn", "FanCtrlMode", "FanCurveEn", "FanCurveMode", "FanCurveParam", "BIOS_CTRL", "CpuDuty", "GpuDuty", "PL1", "PL2" };
        Console.WriteLine();
        for (int i = 0; i < cRegs.Length; i++)
        {
            int inBuf = cRegs[i]; int outBuf = 0; int ret = 0;
            DeviceIoControl(h, 0x9C40A488, ref inBuf, 4, ref outBuf, 4, ref ret, IntPtr.Zero);
            byte val = (byte)(outBuf & 0xFF);
            Console.WriteLine(cNames[i].PadRight(16) + " [0x" + cRegs[i].ToString("X3") + "]: 0x" + val.ToString("X2") + " (" + val + ")");
        }
        CloseHandle(h);
    }
}
