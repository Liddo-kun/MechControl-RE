using System;
using System.Runtime.InteropServices;

class Program
{
    // CreateFile
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    // DeviceIoControl
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(
        IntPtr hDevice, uint dwIoControlCode,
        ref int lpInBuffer, int nInBufferSize,
        ref int lpOutBuffer, int nOutBufferSize,
        ref int lpBytesReturned, IntPtr lpOverlapped);

    // CloseHandle
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr hObject);

    const uint GENERIC_READ = 0x80000000;
    const uint GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_READ = 0x1;
    const uint FILE_SHARE_WRITE = 0x2;
    const uint OPEN_EXISTING = 0x3;

    const uint IOCTL_GPD_ACPI_ECREAD = 0x9C40A488;

    static readonly IntPtr INVALID_HANDLE = new IntPtr(-1);

    static void Main()
    {
        Console.WriteLine("=== EC Read Test ===");
        Console.WriteLine("This tool ONLY READS. No writes. Safe to run.");
        Console.WriteLine();

        // Step 1: Open the driver
        Console.WriteLine("Opening \\\\.\\ACPIDriver ...");
        IntPtr hDevice = CreateFile(
            @"\\.\ACPIDriver",
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);

        if (hDevice == INVALID_HANDLE)
        {
            int err = Marshal.GetLastWin32Error();
            Console.WriteLine("FAILED to open device. Error code: " + err);
            if (err == 5) Console.WriteLine("  -> Access denied. Try running as Administrator.");
            if (err == 2) Console.WriteLine("  -> Device not found. Is UWACPIDriver loaded?");
            return;
        }
        Console.WriteLine("SUCCESS. Device handle obtained.");
        Console.WriteLine();

        // Step 2: Try reading Project ID (register 0x740)
        // Try the simplest possible buffer format: input = 4-byte int (address)
        Console.WriteLine("--- Test 1: Simple int buffer (addr as int32) ---");
        TryReadEC(hDevice, 0x740, "Project ID (expect 12=IDY_6Y or 13=IDY_7Y)");

        // Step 3: If that worked, read a few more known registers
        Console.WriteLine();
        Console.WriteLine("--- Test 2: Read more known registers ---");
        TryReadEC(hDevice, 0x490, "Power Source (AC=1?)");
        TryReadEC(hDevice, 0x4AB, "Battery SOC (0-100%)");
        TryReadEC(hDevice, 0x783, "PL1 Setting (expect ~210)");
        TryReadEC(hDevice, 0x784, "PL2 Setting (expect ~210)");
        TryReadEC(hDevice, 0x785, "PL4 Setting (expect ~175)");
        TryReadEC(hDevice, 0x751, "Fan Control Mode Byte");
        TryReadEC(hDevice, 0x75B, "Left Fan Duty");
        TryReadEC(hDevice, 0x75C, "Right Fan Duty");
        TryReadEC(hDevice, 0x464, "Main Fan RPM byte1");
        TryReadEC(hDevice, 0x465, "Main Fan RPM byte2");
        TryReadEC(hDevice, 0x744, "Configurable TGP Value (expect 95-150)");
        TryReadEC(hDevice, 0x745, "Dynamic Boost Value (expect 5-25)");
        TryReadEC(hDevice, 0x78C, "Keyboard Backlight Enable");
        TryReadEC(hDevice, 0x7B9, "Battery Charge Limit Up");
        TryReadEC(hDevice, 0x7D0, "Battery Charge Limit Down");
        TryReadEC(hDevice, 0x768, "Status Byte");
        TryReadEC(hDevice, 0x765, "Support Byte 1");
        TryReadEC(hDevice, 0x766, "Support Byte 2");

        // Step 4: If simple int didn't work, try alternate buffer layouts
        // (only runs if Test 1 returned 0 or nonsense)

        CloseHandle(hDevice);
        Console.WriteLine();
        Console.WriteLine("Done. Device handle closed.");
    }

    static void TryReadEC(IntPtr hDevice, int addr, string description)
    {
        int inBuffer = addr;
        int outBuffer = 0;
        int bytesReturned = 0;

        bool ok = DeviceIoControl(
            hDevice, IOCTL_GPD_ACPI_ECREAD,
            ref inBuffer, 4,
            ref outBuffer, 4,
            ref bytesReturned, IntPtr.Zero);

        if (ok)
        {
            int value = outBuffer & 0xFF; // low byte
            Console.WriteLine("  [0x" + addr.ToString("X3") + "] = " + value
                + " (0x" + value.ToString("X2") + ")"
                + "  raw32=0x" + outBuffer.ToString("X8")
                + "  bytes=" + bytesReturned
                + "  -- " + description);
        }
        else
        {
            int err = Marshal.GetLastWin32Error();
            Console.WriteLine("  [0x" + addr.ToString("X3") + "] FAILED. Error=" + err + " -- " + description);
        }
    }
}
