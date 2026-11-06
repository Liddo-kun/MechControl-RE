using System;
using System.Runtime.InteropServices;
using System.Threading;

/// <summary>
/// Explore battery mode values and nearby undocumented registers.
/// Reads the 0x7A0-0x7BF region and tests alternate mode bit patterns.
///
/// Usage:
///   bat_mode_explore.exe              -- dump registers and current state
///   bat_mode_explore.exe scan         -- try all 8 mode values, read voltage after each
///   bat_mode_explore.exe set 3        -- set mode bits to specific value (0-7)
///   bat_mode_explore.exe restore      -- restore to High (0x08)
/// </summary>
class Program
{
    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    const uint OPEN_EXISTING = 3, FILE_SHARE_RW = 3;
    const uint IOCTL_ECREAD = 0x9C40A488, IOCTL_ECWRITE = 0x9C40A48C;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(string f, uint a, uint s, IntPtr z, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, int[] i, uint iSz, ref int o, uint oSz, out uint ret, IntPtr ov);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    static IntPtr hDev;

    static byte ReadEC(int addr)
    {
        int outBuf = 0;
        uint r;
        DeviceIoControl(hDev, IOCTL_ECREAD, new int[] { addr }, 4, ref outBuf, 4, out r, IntPtr.Zero);
        return (byte)(outBuf & 0xFF);
    }

    static void WriteEC(int addr, byte val)
    {
        int[] inBuf = new int[] { addr, val };
        int outBuf = 0;
        uint r;
        DeviceIoControl(hDev, IOCTL_ECWRITE, inBuf, 8, ref outBuf, 4, out r, IntPtr.Zero);
    }

    static int ReadEC16(int addrLo)
    {
        return ReadEC(addrLo) | (ReadEC(addrLo + 1) << 8);
    }

    static string ModeName(int bits)
    {
        switch (bits)
        {
            case 1: return "High     ";
            case 3: return "Balanced ";
            case 5: return "Stationary";
            default: return "Unknown  ";
        }
    }

    static void ShowBattery(string label)
    {
        byte mode = ReadEC(0x7A6);
        int modeBits = (mode >> 3) & 0x07;
        int voltage = ReadEC16(0x438);
        int rate = ReadEC16(0x434);
        int remaining = ReadEC16(0x436);
        byte soc = ReadEC(0x4AB);

        Console.WriteLine("  [{0}] 0x7A6=0x{1:X2} mode={2}({3})  V={4}mV  Rate={5}mW  Remain={6}mWh  SOC={7}%",
            label, mode, ModeName(modeBits), modeBits, voltage, rate, remaining, soc);
    }

    static void DumpRange(int start, int end)
    {
        Console.Write("  0x{0:X3}:", start);
        for (int addr = start; addr <= end; addr++)
        {
            if (addr > start && (addr - start) % 16 == 0)
                Console.Write("\n  0x{0:X3}:", addr);
            Console.Write(" {0:X2}", ReadEC(addr));
        }
        Console.WriteLine();
    }

    static void Main(string[] args)
    {
        hDev = CreateFile(@"\\.\ACPIDriver", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_RW,
            IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (hDev == new IntPtr(-1))
        {
            Console.WriteLine("Cannot open ACPIDriver. Run as admin.");
            return;
        }

        try
        {
            if (args.Length == 0)
            {
                Console.WriteLine("=== Register dump: 0x7A0-0x7BF ===");
                DumpRange(0x7A0, 0x7BF);

                Console.WriteLine();
                Console.WriteLine("=== Named registers ===");
                Console.WriteLine("  0x7A0 = 0x{0:X2}  (fan related)", ReadEC(0x7A0));
                Console.WriteLine("  0x7A1 = 0x{0:X2}  (uptime tick?)", ReadEC(0x7A1));
                Console.WriteLine("  0x7A2 = 0x{0:X2}  (undocumented)", ReadEC(0x7A2));
                Console.WriteLine("  0x7A3 = 0x{0:X2}  (undocumented)", ReadEC(0x7A3));
                Console.WriteLine("  0x7A4 = 0x{0:X2}  (GC6S bit2 per DSDT)", ReadEC(0x7A4));
                Console.WriteLine("  0x7A5 = 0x{0:X2}  (AP OEM byte 3)", ReadEC(0x7A5));
                Console.WriteLine("  0x7A6 = 0x{0:X2}  (battery mode, bits[5:3]=0x{1:X2})", ReadEC(0x7A6), ReadEC(0x7A6) & 0x38);
                Console.WriteLine("  0x7A7 = 0x{0:X2}  (batt saver PL1 default)", ReadEC(0x7A7));
                Console.WriteLine("  0x7A8 = 0x{0:X2}  (batt saver PL2 default)", ReadEC(0x7A8));
                Console.WriteLine("  0x7A9 = 0x{0:X2}  (batt saver PL4 default)", ReadEC(0x7A9));
                Console.WriteLine("  0x7AA = 0x{0:X2}  (batt saver D default)", ReadEC(0x7AA));
                Console.WriteLine("  0x7AB = 0x{0:X2}  (logo/CCI mode index)", ReadEC(0x7AB));
                Console.WriteLine("  0x7AC = 0x{0:X2}  (UNDOCUMENTED)", ReadEC(0x7AC));
                Console.WriteLine("  0x7AD = 0x{0:X2}  (UNDOCUMENTED)", ReadEC(0x7AD));
                Console.WriteLine("  0x7AE = 0x{0:X2}  (UNDOCUMENTED)", ReadEC(0x7AE));
                Console.WriteLine("  0x7AF = 0x{0:X2}  (UNDOCUMENTED)", ReadEC(0x7AF));
                Console.WriteLine("  0x7B0 = 0x{0:X2}  (CCI profile 1)", ReadEC(0x7B0));
                Console.WriteLine("  0x7B1 = 0x{0:X2}  (CCI profile 2)", ReadEC(0x7B1));
                Console.WriteLine("  0x7B2 = 0x{0:X2}  (CCI profile 3)", ReadEC(0x7B2));
                Console.WriteLine("  0x7B9 = 0x{0:X2}  (CGLM charge limit)", ReadEC(0x7B9));
                Console.WriteLine("  0x7BA = 0x{0:X4} (VBNL 16-bit)", ReadEC16(0x7BA));
                Console.WriteLine("  0x7BC = 0x{0:X4} (RBHF 16-bit)", ReadEC16(0x7BC));
                Console.WriteLine("  0x7BE = 0x{0:X4} (CMPP 16-bit)", ReadEC16(0x7BE));

                Console.WriteLine();
                ShowBattery("NOW");

                Console.WriteLine();
                Console.WriteLine("Usage:");
                Console.WriteLine("  bat_mode_explore.exe scan     -- cycle through all 8 mode values");
                Console.WriteLine("  bat_mode_explore.exe set N    -- set mode bits [5:3] to N (0-7)");
                Console.WriteLine("  bat_mode_explore.exe restore  -- restore to High (1)");
                return;
            }

            string cmd = args[0].ToLower();

            if (cmd == "scan")
            {
                byte orig7A6 = ReadEC(0x7A6);
                int origMode = (orig7A6 >> 3) & 0x07;
                Console.WriteLine("Original 0x7A6=0x{0:X2} (mode bits={1})", orig7A6, origMode);
                Console.WriteLine();

                Console.WriteLine("=== Scanning all 8 mode values ===");
                Console.WriteLine("Will set each mode, wait 3s, read voltage, then move to next.");
                Console.WriteLine("Known: 1=High, 3=Balanced, 5=Stationary. Others=unknown.");
                Console.WriteLine();

                for (int m = 0; m < 8; m++)
                {
                    byte newVal = (byte)((orig7A6 & ~0x38) | (m << 3));
                    WriteEC(0x7A6, newVal);
                    Thread.Sleep(500);

                    // Verify write stuck
                    byte readback = ReadEC(0x7A6);
                    int rbMode = (readback >> 3) & 0x07;

                    if (rbMode != m)
                    {
                        Console.WriteLine("  mode={0}: WRITE REJECTED (wrote 0x{1:X2}, read 0x{2:X2})", m, newVal, readback);
                        continue;
                    }

                    // Wait for EC to react
                    Thread.Sleep(2500);

                    int voltage = ReadEC16(0x438);
                    int rate = ReadEC16(0x434);
                    byte soc = ReadEC(0x4AB);
                    Console.WriteLine("  mode={0} ({1}): 0x7A6=0x{2:X2}  V={3}mV  Rate={4}mW  SOC={5}%",
                        m, ModeName(m), readback, voltage, rate, soc);
                }

                // Restore
                Console.WriteLine();
                Console.WriteLine("Restoring original mode {0}...", origMode);
                WriteEC(0x7A6, orig7A6);
                Thread.Sleep(500);
                ShowBattery("RESTORED");
            }
            else if (cmd == "set" && args.Length > 1)
            {
                int m = int.Parse(args[1]);
                if (m < 0 || m > 7)
                {
                    Console.WriteLine("Mode must be 0-7.");
                    return;
                }

                byte orig = ReadEC(0x7A6);
                byte newVal = (byte)((orig & ~0x38) | (m << 3));
                Console.WriteLine("Setting 0x7A6 mode bits to {0}: 0x{1:X2} -> 0x{2:X2}", m, orig, newVal);
                WriteEC(0x7A6, newVal);
                Thread.Sleep(1000);
                ShowBattery("AFTER");
            }
            else if (cmd == "restore")
            {
                byte orig = ReadEC(0x7A6);
                byte newVal = (byte)((orig & ~0x38) | 0x08); // High = mode 1
                Console.WriteLine("Restoring to High mode: 0x{0:X2} -> 0x{1:X2}", orig, newVal);
                WriteEC(0x7A6, newVal);
                Thread.Sleep(1000);
                ShowBattery("RESTORED");
            }
        }
        finally
        {
            CloseHandle(hDev);
        }
    }
}
