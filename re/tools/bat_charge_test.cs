using System;
using System.Runtime.InteropServices;

/// <summary>
/// Test tool: read/write battery charge limit registers 0x7B9 and 0x7D0.
/// Usage:
///   bat_charge_test.exe              -- read-only, show current values
///   bat_charge_test.exe 80           -- write 80 to 0x7B9 (charge limit up)
///   bat_charge_test.exe 80 70        -- write 80 to 0x7B9, 70 to 0x7D0
///   bat_charge_test.exe reset        -- write 0 to both (restore default)
/// </summary>
class Program
{
    const uint GENERIC_READ  = 0x80000000;
    const uint GENERIC_WRITE = 0x40000000;
    const uint OPEN_EXISTING = 3;
    const uint FILE_SHARE_READ  = 1;
    const uint FILE_SHARE_WRITE = 2;
    const uint IOCTL_ECREAD  = 0x9C40A488;
    const uint IOCTL_ECWRITE = 0x9C40A48C;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(string lpFileName, uint dwDesiredAccess,
        uint dwShareMode, IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr hDevice, uint dwIoControlCode,
        int[] lpInBuffer, uint nInBufferSize, ref int lpOutBuffer,
        uint nOutBufferSize, out uint lpBytesReturned, IntPtr lpOverlapped);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr hObject);

    static IntPtr hDevice;

    static byte ReadEC(int addr)
    {
        int outBuf = 0;
        uint ret;
        if (!DeviceIoControl(hDevice, IOCTL_ECREAD, new int[] { addr }, 4, ref outBuf, 4, out ret, IntPtr.Zero))
            throw new Exception("ECREAD failed for 0x" + addr.ToString("X3"));
        return (byte)(outBuf & 0xFF);
    }

    static void WriteEC(int addr, byte value)
    {
        int[] inBuf = new int[] { addr, value };
        int outBuf = 0;
        uint ret;
        if (!DeviceIoControl(hDevice, IOCTL_ECWRITE, inBuf, 8, ref outBuf, 4, out ret, IntPtr.Zero))
            throw new Exception("ECWRITE failed for 0x" + addr.ToString("X3"));
    }

    static void Main(string[] args)
    {
        hDevice = CreateFile(@"\\.\ACPIDriver",
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);

        if (hDevice == new IntPtr(-1))
        {
            Console.WriteLine("ERROR: Cannot open ACPIDriver. Run as admin.");
            return;
        }

        try
        {
            // Always read current state first
            byte limitUp   = ReadEC(0x7B9);
            byte limitDown = ReadEC(0x7D0);
            byte mode7A6   = ReadEC(0x7A6);
            byte soc       = ReadEC(0x4AB);

            Console.WriteLine("=== Current State ===");
            Console.WriteLine("  0x7A6 (battery mode)     = 0x{0:X2}  (mode bits: 0x{1:X2})",
                mode7A6, mode7A6 & 0x38);
            Console.WriteLine("  0x7B9 (charge limit up)  = {0}  (0x{0:X2})", limitUp);
            Console.WriteLine("  0x7D0 (charge limit down)= {0}  (0x{0:X2})", limitDown);
            Console.WriteLine("  0x4AB (battery SOC)      = {0}%", soc);
            Console.WriteLine();

            if (args.Length == 0)
            {
                Console.WriteLine("Read-only mode. To write:");
                Console.WriteLine("  bat_charge_test.exe 80       -- set upper limit to 80%");
                Console.WriteLine("  bat_charge_test.exe 80 70    -- set upper=80%, lower=70%");
                Console.WriteLine("  bat_charge_test.exe reset    -- write 0 to both");
                Console.WriteLine("  bat_charge_test.exe 100      -- set upper limit to 100%");
                return;
            }

            if (args[0].ToLower() == "reset")
            {
                Console.WriteLine("Resetting both to 0...");
                WriteEC(0x7B9, 0);
                WriteEC(0x7D0, 0);
            }
            else
            {
                byte newUp = byte.Parse(args[0]);
                if (newUp > 100)
                {
                    Console.WriteLine("ERROR: Value must be 0-100 (percentage).");
                    return;
                }

                Console.WriteLine("Writing 0x7B9 = {0} (charge limit up)...", newUp);
                WriteEC(0x7B9, newUp);

                if (args.Length > 1)
                {
                    byte newDown = byte.Parse(args[1]);
                    if (newDown > 100)
                    {
                        Console.WriteLine("ERROR: Value must be 0-100 (percentage).");
                        return;
                    }
                    Console.WriteLine("Writing 0x7D0 = {0} (charge limit down)...", newDown);
                    WriteEC(0x7D0, newDown);
                }
            }

            // Read back
            Console.WriteLine();
            Console.WriteLine("=== After Write ===");
            byte newLimitUp   = ReadEC(0x7B9);
            byte newLimitDown = ReadEC(0x7D0);
            Console.WriteLine("  0x7B9 (charge limit up)  = {0}  (0x{0:X2})", newLimitUp);
            Console.WriteLine("  0x7D0 (charge limit down)= {0}  (0x{0:X2})", newLimitDown);

            bool upStuck   = newLimitUp   == (args[0].ToLower() == "reset" ? 0 : byte.Parse(args[0]));
            Console.WriteLine();
            Console.WriteLine(upStuck
                ? "Write verified -- value stuck. Now monitor if charging behavior changes."
                : "WARNING: Value did not stick! EC may not support this register.");
        }
        finally
        {
            CloseHandle(hDevice);
        }
    }
}
