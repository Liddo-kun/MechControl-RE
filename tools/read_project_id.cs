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

    const uint IOCTL_GPD_ACPI_ECREAD = 0x9C40A488;

    static byte ReadEC(IntPtr h, int addr)
    {
        int inBuf = addr, outBuf = 0, ret = 0;
        DeviceIoControl(h, IOCTL_GPD_ACPI_ECREAD, ref inBuf, 4, ref outBuf, 4, ref ret, IntPtr.Zero);
        return (byte)(outBuf & 0xFF);
    }

    static void Main()
    {
        IntPtr h = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h == new IntPtr(-1)) { Console.WriteLine("Failed to open driver. Run as admin."); return; }

        Console.WriteLine("=== Project ID Investigation ===");
        Console.WriteLine();

        // Primary project ID register
        byte raw740 = ReadEC(h, 0x740);
        Console.WriteLine("0x740 (Project ID)     = " + raw740 + " (0x" + raw740.ToString("X2") + ") binary: " + Convert.ToString(raw740, 2).PadLeft(8, '0'));

        // Related registers
        byte r741 = ReadEC(h, 0x741);
        Console.WriteLine("0x741 (unknown)        = " + r741 + " (0x" + r741.ToString("X2") + ") binary: " + Convert.ToString(r741, 2).PadLeft(8, '0'));

        byte r742 = ReadEC(h, 0x742);
        Console.WriteLine("0x742 (Support Byte 5) = " + r742 + " (0x" + r742.ToString("X2") + ") binary: " + Convert.ToString(r742, 2).PadLeft(8, '0'));

        byte r74C = ReadEC(h, 0x74C);
        Console.WriteLine("0x74C (OEM Project ID) = " + r74C + " (0x" + r74C.ToString("X2") + ") binary: " + Convert.ToString(r74C, 2).PadLeft(8, '0'));

        byte r74D = ReadEC(h, 0x74D);
        Console.WriteLine("0x74D (unknown)        = " + r74D + " (0x" + r74D.ToString("X2") + ") binary: " + Convert.ToString(r74D, 2).PadLeft(8, '0'));

        byte r74E = ReadEC(h, 0x74E);
        Console.WriteLine("0x74E (BIOS OEM Byte)  = " + r74E + " (0x" + r74E.ToString("X2") + ") binary: " + Convert.ToString(r74E, 2).PadLeft(8, '0'));

        byte r765 = ReadEC(h, 0x765);
        Console.WriteLine("0x765 (Support Byte 1) = " + r765 + " (0x" + r765.ToString("X2") + ") binary: " + Convert.ToString(r765, 2).PadLeft(8, '0'));

        byte r766 = ReadEC(h, 0x766);
        Console.WriteLine("0x766 (Support Byte 2) = " + r766 + " (0x" + r766.ToString("X2") + ") binary: " + Convert.ToString(r766, 2).PadLeft(8, '0'));

        byte r78E = ReadEC(h, 0x78E);
        Console.WriteLine("0x78E (Support Byte 6) = " + r78E + " (0x" + r78E.ToString("X2") + ") binary: " + Convert.ToString(r78E, 2).PadLeft(8, '0'));

        // Module IDs
        byte r7D2 = ReadEC(h, 0x7D2);
        Console.WriteLine("0x7D2 (Module ID GPU)  = " + r7D2 + " (0x" + r7D2.ToString("X2") + ") binary: " + Convert.ToString(r7D2, 2).PadLeft(8, '0'));

        byte r7D3 = ReadEC(h, 0x7D3);
        Console.WriteLine("0x7D3 (Module ID CPU)  = " + r7D3 + " (0x" + r7D3.ToString("X2") + ") binary: " + Convert.ToString(r7D3, 2).PadLeft(8, '0'));

        // ROM IDs
        byte r771 = ReadEC(h, 0x771);
        Console.WriteLine("0x771 (ROM ID 1)       = " + r771 + " (0x" + r771.ToString("X2") + ")");

        byte r772 = ReadEC(h, 0x772);
        Console.WriteLine("0x772 (ROM ID 2)       = " + r772 + " (0x" + r772.ToString("X2") + ")");

        // EC version
        byte r454 = ReadEC(h, 0x454);
        Console.WriteLine("0x454 (EC Version)     = " + r454 + " (0x" + r454.ToString("X2") + ")");

        // System ID
        byte r456 = ReadEC(h, 0x456);
        Console.WriteLine("0x456 (System ID)      = " + r456 + " (0x" + r456.ToString("X2") + ")");

        Console.WriteLine();
        Console.WriteLine("=== 6-bit Transform Analysis ===");
        Console.WriteLine("Raw 0x740 = " + raw740 + " (0x" + raw740.ToString("X2") + ")");
        Console.WriteLine("Low 6 bits (& 0x3F)    = " + (raw740 & 0x3F));
        Console.WriteLine("High 2 bits (>> 6)     = " + (raw740 >> 6));
        Console.WriteLine("Shifted right 1 (>> 1) = " + (raw740 >> 1));
        Console.WriteLine("Shifted right 2 (>> 2) = " + (raw740 >> 2));
        Console.WriteLine("XOR 0x3F               = " + (raw740 ^ 0x3F));
        Console.WriteLine("Subtract 14            = " + (raw740 - 14));
        Console.WriteLine("raw740 & 0x1F          = " + (raw740 & 0x1F));
        Console.WriteLine();

        // Check if 0x74C combined with 0x740 gives anything
        Console.WriteLine("=== Combined register analysis ===");
        int combined = (r74C << 8) | raw740;
        Console.WriteLine("(0x74C << 8) | 0x740   = " + combined + " (0x" + combined.ToString("X4") + ")");
        combined = (raw740 << 8) | r74C;
        Console.WriteLine("(0x740 << 8) | 0x74C   = " + combined + " (0x" + combined.ToString("X4") + ")");

        CloseHandle(h);
    }
}
