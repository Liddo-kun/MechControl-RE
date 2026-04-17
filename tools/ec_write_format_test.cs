using System;
using System.Runtime.InteropServices;

class Program
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    // Overload 1: ref int in, ref int out
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(
        IntPtr hDevice, uint dwIoControlCode,
        ref int lpInBuffer, int nInBufferSize,
        ref int lpOutBuffer, int nOutBufferSize,
        ref int lpBytesReturned, IntPtr lpOverlapped);

    // Overload 2: byte[] in, ref int out (for variable-size input)
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(
        IntPtr hDevice, uint dwIoControlCode,
        byte[] lpInBuffer, int nInBufferSize,
        ref int lpOutBuffer, int nOutBufferSize,
        ref int lpBytesReturned, IntPtr lpOverlapped);

    // Overload 3: IntPtr in, ref int out (for precise control)
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(
        IntPtr hDevice, uint dwIoControlCode,
        IntPtr lpInBuffer, int nInBufferSize,
        ref int lpOutBuffer, int nOutBufferSize,
        ref int lpBytesReturned, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr hObject);

    const uint GENERIC_READ  = 0x80000000;
    const uint GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_READ  = 0x1;
    const uint FILE_SHARE_WRITE = 0x2;
    const uint OPEN_EXISTING = 0x3;

    const uint IOCTL_GPD_ACPI_ECREAD  = 0x9C40A488;
    const uint IOCTL_GPD_ACPI_ECWRITE = 0x9C40A48C;

    static readonly IntPtr INVALID_HANDLE = new IntPtr(-1);

    static int ReadEC(IntPtr hDevice, int addr)
    {
        int inBuf = addr;
        int outBuf = 0;
        int bytesRet = 0;
        bool ok = DeviceIoControl(hDevice, IOCTL_GPD_ACPI_ECREAD,
            ref inBuf, 4, ref outBuf, 4, ref bytesRet, IntPtr.Zero);
        if (!ok)
        {
            int err = Marshal.GetLastWin32Error();
            Console.WriteLine("  READ FAILED addr=0x{0:X3} err={1}", addr, err);
            return -1;
        }
        return outBuf & 0xFF;
    }

    static void Main()
    {
        Console.WriteLine("=== EC Write Buffer Format Test ===");
        Console.WriteLine("Tests different write buffer layouts using SAFE no-op writes");
        Console.WriteLine("(writes the current value back, so EC state is unchanged)");
        Console.WriteLine();

        IntPtr hDevice = CreateFile(@"\\.\ACPIDriver",
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);

        if (hDevice == INVALID_HANDLE)
        {
            int err = Marshal.GetLastWin32Error();
            Console.WriteLine("FAILED to open device. Error={0}", err);
            if (err == 5) Console.WriteLine("  -> Run as Administrator.");
            return;
        }
        Console.WriteLine("Driver opened successfully.");
        Console.WriteLine();

        // Step 1: Read current PL1 value as baseline
        int testAddr = 0x783; // PL1 register
        int currentVal = ReadEC(hDevice, testAddr);
        if (currentVal < 0) { CloseHandle(hDevice); return; }
        Console.WriteLine("Baseline: PL1 [0x783] = {0} (0x{0:X2})", currentVal);
        Console.WriteLine();

        // Also read a second register for cross-check
        int testAddr2 = 0x78C; // Keyboard backlight enable
        int currentVal2 = ReadEC(hDevice, testAddr2);
        Console.WriteLine("Baseline: KBD_BL [0x78C] = {0} (0x{0:X2})", currentVal2);
        Console.WriteLine();

        // Step 2: Test Format A — 8 bytes: int32 addr + int32 value (most likely)
        Console.WriteLine("=== Format A: 8-byte buffer [int32 addr][int32 value] ===");
        {
            byte[] buf = new byte[8];
            buf[0] = (byte)(testAddr & 0xFF);
            buf[1] = (byte)((testAddr >> 8) & 0xFF);
            buf[2] = (byte)((testAddr >> 16) & 0xFF);
            buf[3] = (byte)((testAddr >> 24) & 0xFF);
            buf[4] = (byte)(currentVal & 0xFF);
            buf[5] = 0;
            buf[6] = 0;
            buf[7] = 0;

            Console.Write("  Input buffer hex: ");
            for (int i = 0; i < buf.Length; i++) Console.Write("{0:X2} ", buf[i]);
            Console.WriteLine();

            int outBuf = 0;
            int bytesRet = 0;
            bool ok = DeviceIoControl(hDevice, IOCTL_GPD_ACPI_ECWRITE,
                buf, 8, ref outBuf, 4, ref bytesRet, IntPtr.Zero);
            int err = ok ? 0 : Marshal.GetLastWin32Error();
            Console.WriteLine("  Result: {0}  error={1}  bytesReturned={2}  outBuf=0x{3:X8}",
                ok ? "SUCCESS" : "FAILED", err, bytesRet, outBuf);

            // Verify value unchanged
            int verify = ReadEC(hDevice, testAddr);
            Console.WriteLine("  Verify read-back: PL1 = {0} (was {1}) {2}",
                verify, currentVal, verify == currentVal ? "OK" : "CHANGED!");
            Console.WriteLine();
        }

        // Step 3: Test Format B — 8 bytes but with 0-size output buffer
        Console.WriteLine("=== Format B: 8-byte input, 0-byte output ===");
        {
            byte[] buf = new byte[8];
            buf[0] = (byte)(testAddr & 0xFF);
            buf[1] = (byte)((testAddr >> 8) & 0xFF);
            buf[2] = (byte)((testAddr >> 16) & 0xFF);
            buf[3] = (byte)((testAddr >> 24) & 0xFF);
            buf[4] = (byte)(currentVal & 0xFF);
            buf[5] = 0;
            buf[6] = 0;
            buf[7] = 0;

            Console.Write("  Input buffer hex: ");
            for (int i = 0; i < buf.Length; i++) Console.Write("{0:X2} ", buf[i]);
            Console.WriteLine();

            int outBuf = 0;
            int bytesRet = 0;
            bool ok = DeviceIoControl(hDevice, IOCTL_GPD_ACPI_ECWRITE,
                buf, 8, ref outBuf, 0, ref bytesRet, IntPtr.Zero);
            int err = ok ? 0 : Marshal.GetLastWin32Error();
            Console.WriteLine("  Result: {0}  error={1}  bytesReturned={2}",
                ok ? "SUCCESS" : "FAILED", err, bytesRet);
            Console.WriteLine();
        }

        // Step 4: Test Format C — 4-byte input (packed: high word=addr, low word=value)
        Console.WriteLine("=== Format C: 4-byte packed [hiword=addr, loword=value] ===");
        {
            int packed = (testAddr << 16) | (currentVal & 0xFFFF);
            Console.WriteLine("  Packed int32 = 0x{0:X8}", packed);

            int outBuf = 0;
            int bytesRet = 0;
            bool ok = DeviceIoControl(hDevice, IOCTL_GPD_ACPI_ECWRITE,
                ref packed, 4, ref outBuf, 4, ref bytesRet, IntPtr.Zero);
            int err = ok ? 0 : Marshal.GetLastWin32Error();
            Console.WriteLine("  Result: {0}  error={1}  bytesReturned={2}  outBuf=0x{3:X8}",
                ok ? "SUCCESS" : "FAILED", err, bytesRet, outBuf);
            Console.WriteLine();
        }

        // Step 5: Test Format D — 4-byte packed reversed [loword=addr, hiword=value]
        Console.WriteLine("=== Format D: 4-byte packed [loword=addr, hiword=value] ===");
        {
            int packed = (currentVal << 16) | (testAddr & 0xFFFF);
            Console.WriteLine("  Packed int32 = 0x{0:X8}", packed);

            int outBuf = 0;
            int bytesRet = 0;
            bool ok = DeviceIoControl(hDevice, IOCTL_GPD_ACPI_ECWRITE,
                ref packed, 4, ref outBuf, 4, ref bytesRet, IntPtr.Zero);
            int err = ok ? 0 : Marshal.GetLastWin32Error();
            Console.WriteLine("  Result: {0}  error={1}  bytesReturned={2}  outBuf=0x{3:X8}",
                ok ? "SUCCESS" : "FAILED", err, bytesRet, outBuf);
            Console.WriteLine();
        }

        // Step 6: Test with the decompiled WriteACPI signature for clues
        // WriteACPI(uint ioctrl, int addr, int data) takes two int params
        // This strongly suggests Format A (8 bytes: addr + data)
        // But let's also try matching the AcpiCtrl.Write signature:
        // Write(string ReadName, ushort Addr, byte Data)
        // The string becomes an IOCTL code, addr is ushort, data is byte
        // So the actual call is likely: DeviceIoControl(handle, IOCTL_CODE, inBuf, inSize, ...)
        // where inBuf contains the addr and data packed somehow

        // Step 7: Final verification — read both test registers again
        Console.WriteLine("=== Final Verification ===");
        int finalPL1 = ReadEC(hDevice, testAddr);
        int finalKBD = ReadEC(hDevice, testAddr2);
        Console.WriteLine("  PL1  [0x783] = {0} (was {1}) {2}",
            finalPL1, currentVal, finalPL1 == currentVal ? "UNCHANGED" : "CHANGED!");
        Console.WriteLine("  KBD  [0x78C] = {0} (was {1}) {2}",
            finalKBD, currentVal2, finalKBD == currentVal2 ? "UNCHANGED" : "CHANGED!");

        CloseHandle(hDevice);
        Console.WriteLine();
        Console.WriteLine("Done. Driver handle closed.");
    }
}
