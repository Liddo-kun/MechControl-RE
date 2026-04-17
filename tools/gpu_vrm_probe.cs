using System;
using System.Runtime.InteropServices;
using System.Text;
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
    static extern bool CloseHandle(IntPtr hObject);

    // HWiNFO shared memory
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenFileMapping(uint dwDesiredAccess, bool bInheritHandle, string lpName);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr MapViewOfFile(IntPtr hFileMappingObject, uint dwDesiredAccess,
        uint dwFileOffsetHigh, uint dwFileOffsetLow, UIntPtr dwNumberOfBytesToMap);
    [DllImport("kernel32.dll")]
    static extern bool UnmapViewOfFile(IntPtr lpBaseAddress);

    const uint FILE_MAP_READ = 0x0004;

    static IntPtr hDev;

    static byte ReadEC(int addr)
    {
        int inBuf = addr, outBuf = 0, ret = 0;
        DeviceIoControl(hDev, 0x9C40A488, ref inBuf, 4, ref outBuf, 4, ref ret, IntPtr.Zero);
        return (byte)(outBuf & 0xFF);
    }

    static double hwCpuPower = -1;
    static double hwGpuPower = -1;
    static double hwGpuHotspot = -1;
    static bool hwAvailable = false;

    static void ReadHWiNFO()
    {
        hwCpuPower = -1;
        hwGpuPower = -1;
        hwGpuHotspot = -1;
        hwAvailable = false;

        IntPtr hMap = OpenFileMapping(FILE_MAP_READ, false, "Global\\HWiNFO_SENS_SM2");
        if (hMap == IntPtr.Zero) return;

        IntPtr pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, UIntPtr.Zero);
        if (pView == IntPtr.Zero) { CloseHandle(hMap); return; }

        hwAvailable = true;

        // Header: signature(4) + version(4) + revision(4) + pollTime(8) +
        //         sensorOffset(4) + sensorSize(4) + sensorCount(4) +
        //         readingOffset(4) + readingSize(4) + readingCount(4)
        uint readingOffset = (uint)Marshal.ReadInt32(pView, 32);
        uint readingSize   = (uint)Marshal.ReadInt32(pView, 36);
        uint readingCount  = (uint)Marshal.ReadInt32(pView, 40);

        for (uint i = 0; i < readingCount; i++)
        {
            IntPtr pReading = new IntPtr(pView.ToInt64() + readingOffset + i * readingSize);
            uint readingType = (uint)Marshal.ReadInt32(pReading, 0);
            // type: 0=None, 1=Temp, 2=Volt, 3=Fan, 4=Current, 5=Power

            // Label at offset 12, 128 bytes
            byte[] labelBytes = new byte[128];
            for (int b = 0; b < 128; b++)
                labelBytes[b] = Marshal.ReadByte(pReading, 12 + b);
            string label = Encoding.ASCII.GetString(labelBytes).TrimEnd('\0');

            // Value at offset 12 + 128(orig) + 128(user) + 16(unit) = 284
            if (readingType == 5) // Power
            {
                double val = BitConverter.Int64BitsToDouble(Marshal.ReadInt64(pReading, 284));
                string labelLow = label.ToLower();
                if (labelLow.Contains("cpu package") || labelLow.Contains("cpu package power"))
                {
                    if (hwCpuPower < 0) hwCpuPower = val;
                }
                else if (labelLow.Contains("gpu power"))
                {
                    if (hwGpuPower < 0) hwGpuPower = val;
                }
            }
            else if (readingType == 1) // Temp
            {
                double val = BitConverter.Int64BitsToDouble(Marshal.ReadInt64(pReading, 284));
                string labelLow = label.ToLower();
                if (labelLow.Contains("gpu hot spot") || labelLow.Contains("gpu hotspot"))
                {
                    if (hwGpuHotspot < 0) hwGpuHotspot = val;
                }
            }
        }

        UnmapViewOfFile(pView);
        CloseHandle(hMap);
    }

    static void Main(string[] args)
    {
        int interval = 1000;
        if (args.Length > 0) int.TryParse(args[0], out interval);

        hDev = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 0x3, IntPtr.Zero, 0x3, 0, IntPtr.Zero);
        if (hDev == new IntPtr(-1)) { Console.WriteLine("FAILED - run as admin"); return; }

        Console.WriteLine("GPU VRM / Power Probe — Ctrl+C to stop");
        Console.WriteLine("Interval: " + interval + "ms");
        Console.WriteLine("Requires: HWiNFO64 running with Shared Memory enabled");
        Console.WriteLine("  (HWiNFO -> Settings -> Shared Memory Support -> check it)");
        Console.WriteLine(new string('=', 90));
        Console.WriteLine();

        byte[] prevPower = new byte[16];
        bool first = true;

        while (true)
        {
            // --- HWiNFO reference power ---
            ReadHWiNFO();

            Console.WriteLine("--- HWiNFO REFERENCE ---");
            if (!hwAvailable)
            {
                Console.WriteLine("  [HWiNFO shared memory not found - enable in HWiNFO settings]");
            }
            else
            {
                string cpuPStr = hwCpuPower >= 0 ? hwCpuPower.ToString("F1") + "W" : "N/A";
                string gpuPStr = hwGpuPower >= 0 ? hwGpuPower.ToString("F1") + "W" : "N/A";
                Console.WriteLine("  CPU Package Power: " + cpuPStr);
                Console.WriteLine("  GPU Power:         " + gpuPStr);
            }

            // --- EC temps & sensors ---
            byte cpuTemp = ReadEC(0x43E);
            byte gpuTemp = ReadEC(0x44F);
            byte acur = ReadEC(0x449);
            byte unk44B = ReadEC(0x44B);
            byte unk44C = ReadEC(0x44C);

            Console.WriteLine();
            Console.WriteLine("--- EC TEMPERATURES & SENSORS ---");
            Console.WriteLine("  CPU temp   [0x43E]: " + cpuTemp + "C");
            Console.WriteLine("  GPU temp   [0x44F]: " + gpuTemp + "C");
            Console.WriteLine("  ACUR       [0x449]: " + acur + " (adapter current, deciamps)");
            Console.WriteLine("  Unk        [0x44B]: " + unk44B + " (GPU power state flag?)");
            Console.WriteLine("  Unk        [0x44C]: " + unk44C + "  0x" + unk44C.ToString("X2") + " (sys power?)");

            // --- Power sensor 0x60C (only live one) ---
            byte pw60C = ReadEC(0x60C);
            byte pw60D = ReadEC(0x60D);
            string delta60C = first ? "-" : (pw60C == prevPower[12] ? "=" : (pw60C > prevPower[12] ? "+" + (pw60C - prevPower[12]) : "" + (pw60C - prevPower[12])));
            Console.WriteLine();
            Console.WriteLine("--- POWER SENSOR ---");
            Console.WriteLine("  0x60C  " + pw60C.ToString().PadLeft(4) + "  0x" + pw60C.ToString("X2") + "  " + delta60C.PadLeft(5) + "  | GPU/sys power (live)");
            Console.WriteLine("  0x60D  " + pw60D.ToString().PadLeft(4) + "  0x" + pw60D.ToString("X2") + "        | scale/high byte");
            prevPower[12] = pw60C;

            // --- GPU power allocation ---
            byte dbap = ReadEC(0x7D5);
            byte dbsp = ReadEC(0x7D6);
            byte ctva = ReadEC(0x744);
            byte dbCtrl = ReadEC(0x743);
            byte dbMax = ReadEC(0x746);
            byte dbTotal = ReadEC(0x745);
            byte ctwa = ReadEC(0x788);
            byte etpp = ReadEC(0x7F7);

            Console.WriteLine();
            Console.WriteLine("--- GPU POWER ALLOCATION ---");
            Console.WriteLine("  DBAP       [0x7D5]: 0x" + dbap.ToString("X2") + " (" + dbap + ") => " + (dbap * 8) + "W via NPCF");
            Console.WriteLine("  DBSP       [0x7D6]: 0x" + dbsp.ToString("X2") + " (" + dbsp + ")");
            byte cgct = ReadEC(0x7D7);
            byte cpua = ReadEC(0x7D4);
            byte dbd1 = ReadEC(0x7D0);
            byte dbd2 = ReadEC(0x7D1);
            Console.WriteLine("  CGCT       [0x7D7]: 0x" + cgct.ToString("X2") + " (" + cgct + ") GPU control/thermal");
            Console.WriteLine("  CPUA       [0x7D4]: 0x" + cpua.ToString("X2") + " (" + cpua + ")");
            Console.WriteLine("  DBD1       [0x7D0]: 0x" + dbd1.ToString("X2") + " (" + dbd1 + ") dGPU data 1");
            Console.WriteLine("  DBD2       [0x7D1]: 0x" + dbd2.ToString("X2") + " (" + dbd2 + ") dGPU data 2 / SSD temp");
            Console.WriteLine("  CTVA/TGP   [0x744]: 0x" + ctva.ToString("X2") + " (" + ctva + "W base TGP)");
            Console.WriteLine("  DB Ctrl    [0x743]: 0x" + dbCtrl.ToString("X2") + " (bits: GNEN=" + (dbCtrl & 1) + " ECDC=" + ((dbCtrl >> 1) & 1) + ")");
            Console.WriteLine("  DB Max     [0x746]: 0x" + dbMax.ToString("X2") + " (" + dbMax + "W dynamic boost max)");
            Console.WriteLine("  DB Total   [0x745]: 0x" + dbTotal.ToString("X2") + " (" + dbTotal + "W)");
            Console.WriteLine("  CTWA       [0x788]: 0x" + ctwa.ToString("X2") + " (" + ctwa + ") => " + (ctwa * 8) + "W thermal budget");
            Console.WriteLine("  ETPP       [0x7F7]: 0x" + etpp.ToString("X2") + " (" + etpp + ") => " + (etpp * 8) + "W ext thermal");

            Console.WriteLine();
            Console.WriteLine(new string('-', 90));
            Console.WriteLine("[" + DateTime.Now.ToString("HH:mm:ss") + "] Next read in " + interval + "ms...");
            Console.WriteLine();

            first = false;
            Thread.Sleep(interval);
        }
    }
}
