using System;
using System.Runtime.InteropServices;
using System.IO;

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

    const uint GENERIC_READ = 0x80000000;
    const uint GENERIC_WRITE = 0x40000000;
    const uint OPEN_EXISTING = 0x3;
    const uint IOCTL_GPD_ACPI_ECREAD = 0x9C40A488;

    static void Main(string[] args)
    {
        if (args.Length < 1)
        {
            Console.WriteLine("Usage: ec_snapshot.exe <snapshot|diff>");
            Console.WriteLine("  snapshot  - Read all EC registers 0x400-0xDFF, save to ec_snap_N.bin");
            Console.WriteLine("  diff      - Compare the two most recent snapshots");
            return;
        }

        string baseDir = Path.GetDirectoryName(
            System.Reflection.Assembly.GetExecutingAssembly().Location);

        if (args[0] == "snapshot")
        {
            IntPtr hDevice = CreateFile(@"\\.\ACPIDriver",
                GENERIC_READ | GENERIC_WRITE, 0x3,
                IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);

            if (hDevice == new IntPtr(-1))
            {
                Console.WriteLine("FAILED to open device. Run as Administrator.");
                return;
            }

            int startAddr = 0x400;
            int endAddr = 0xFFF;
            int count = endAddr - startAddr + 1;
            byte[] data = new byte[count];

            Console.WriteLine("Reading EC registers 0x" + startAddr.ToString("X3") +
                " - 0x" + endAddr.ToString("X3") + " (" + count + " bytes)...");

            int errors = 0;
            for (int addr = startAddr; addr <= endAddr; addr++)
            {
                int inBuf = addr;
                int outBuf = 0;
                int ret = 0;
                bool ok = DeviceIoControl(hDevice, IOCTL_GPD_ACPI_ECREAD,
                    ref inBuf, 4, ref outBuf, 4, ref ret, IntPtr.Zero);
                if (ok)
                    data[addr - startAddr] = (byte)(outBuf & 0xFF);
                else
                    errors++;
            }

            CloseHandle(hDevice);

            // Find next snapshot number
            int snapNum = 1;
            while (File.Exists(Path.Combine(baseDir, "ec_snap_" + snapNum + ".bin")))
                snapNum++;

            string snapFile = Path.Combine(baseDir, "ec_snap_" + snapNum + ".bin");
            File.WriteAllBytes(snapFile, data);
            Console.WriteLine("Saved: " + snapFile + " (" + count + " bytes, " + errors + " errors)");
            Console.WriteLine("Snapshot #" + snapNum + " done.");
            Console.WriteLine();
            Console.WriteLine("Now change a setting in the OEM app, then run: ec_snapshot.exe snapshot");
            Console.WriteLine("Then run: ec_snapshot.exe diff");
        }
        else if (args[0] == "diff")
        {
            // Find the two most recent snapshots
            int maxSnap = 0;
            while (File.Exists(Path.Combine(baseDir, "ec_snap_" + (maxSnap + 1) + ".bin")))
                maxSnap++;

            if (maxSnap < 2)
            {
                Console.WriteLine("Need at least 2 snapshots. Found " + maxSnap + ".");
                return;
            }

            string file1 = Path.Combine(baseDir, "ec_snap_" + (maxSnap - 1) + ".bin");
            string file2 = Path.Combine(baseDir, "ec_snap_" + maxSnap + ".bin");

            byte[] snap1 = File.ReadAllBytes(file1);
            byte[] snap2 = File.ReadAllBytes(file2);

            Console.WriteLine("Comparing snap_" + (maxSnap - 1) + " vs snap_" + maxSnap + ":");
            Console.WriteLine();

            int startAddr = 0x400;
            int diffCount = 0;

            for (int i = 0; i < snap1.Length && i < snap2.Length; i++)
            {
                if (snap1[i] != snap2[i])
                {
                    int addr = startAddr + i;
                    Console.WriteLine("  [0x" + addr.ToString("X3") + "]  " +
                        snap1[i] + " -> " + snap2[i] +
                        "  (0x" + snap1[i].ToString("X2") + " -> 0x" + snap2[i].ToString("X2") + ")");
                    diffCount++;
                }
            }

            if (diffCount == 0)
                Console.WriteLine("  No differences found.");
            else
                Console.WriteLine("\n" + diffCount + " register(s) changed.");
        }
    }
}
