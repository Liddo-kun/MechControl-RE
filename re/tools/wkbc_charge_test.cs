using System;
using System.Runtime.InteropServices;
using System.Threading;

/// <summary>
/// Test tool: Write charge limit via EC command interface (WKBC protocol).
/// Discovered via DSDT analysis: CGLM (7-bit) at 0x7B9 is the charge limit.
/// The EC has a command interface at 0x48A-0x48E that may be required for writes.
///
/// Usage:
///   wkbc_charge_test.exe              -- read-only: show status
///   wkbc_charge_test.exe 80           -- set charge limit to 80% via WKBC
///   wkbc_charge_test.exe read         -- read 0x7B9 via RKBC protocol
///   wkbc_charge_test.exe reset        -- set charge limit to 0 via WKBC
///   wkbc_charge_test.exe direct 80    -- direct write (old method, for comparison)
/// </summary>
class Program
{
    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    const uint OPEN_EXISTING = 3, FILE_SHARE_RW = 3;
    const uint IOCTL_ECREAD = 0x9C40A488, IOCTL_ECWRITE = 0x9C40A48C;

    // EC command interface registers (from DSDT ECXP field at offset 0x8A)
    const int ADDR_LDAT  = 0x48A;  // Address low byte
    const int ADDR_HDAT  = 0x48B;  // Address high byte
    const int ADDR_FLAGS = 0x48C;  // RFLG(0) WFLG(1) BFLG(2) CFLG(3) ... DRDY(7)
    const int ADDR_CMDL  = 0x48D;  // Data low byte
    const int ADDR_CMDH  = 0x48E;  // Data high byte

    // Flag bits in 0x48C
    const byte FLAG_RFLG = 0x01;   // bit 0: read request
    const byte FLAG_WFLG = 0x02;   // bit 1: write request
    const byte FLAG_BFLG = 0x04;   // bit 2: busy
    const byte FLAG_CFLG = 0x08;   // bit 3: command
    const byte FLAG_DRDY = 0x80;   // bit 7: data ready (EC ack)

    // Target register
    const int ADDR_CGLM = 0x7B9;   // Charge limit (7-bit, 0-99)

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
        if (!DeviceIoControl(hDev, IOCTL_ECREAD, new int[] { addr }, 4, ref outBuf, 4, out r, IntPtr.Zero))
            throw new Exception("ECREAD failed for 0x" + addr.ToString("X3"));
        return (byte)(outBuf & 0xFF);
    }

    static void WriteEC(int addr, byte val)
    {
        int[] inBuf = new int[] { addr, val };
        int outBuf = 0;
        uint r;
        if (!DeviceIoControl(hDev, IOCTL_ECWRITE, inBuf, 8, ref outBuf, 4, out r, IntPtr.Zero))
            throw new Exception("ECWRITE failed for 0x" + addr.ToString("X3"));
    }

    static void ShowStatus(string label)
    {
        byte flags = ReadEC(ADDR_FLAGS);
        byte ldat = ReadEC(ADDR_LDAT);
        byte hdat = ReadEC(ADDR_HDAT);
        byte cmdl = ReadEC(ADDR_CMDL);
        byte cmdh = ReadEC(ADDR_CMDH);
        byte cglm = ReadEC(ADDR_CGLM);
        byte soc  = ReadEC(0x4AB);
        byte mode = ReadEC(0x7A6);

        Console.WriteLine("--- {0} ---", label);
        Console.WriteLine("  Command interface (0x48A-0x48E):");
        Console.WriteLine("    LDAT=0x{0:X2}  HDAT=0x{1:X2}  CMDL=0x{2:X2}  CMDH=0x{3:X2}",
            ldat, hdat, cmdl, cmdh);
        Console.WriteLine("    FLAGS=0x{0:X2}  RFLG={1} WFLG={2} BFLG={3} CFLG={4} DRDY={5}",
            flags,
            (flags & FLAG_RFLG) != 0 ? 1 : 0,
            (flags & FLAG_WFLG) != 0 ? 1 : 0,
            (flags & FLAG_BFLG) != 0 ? 1 : 0,
            (flags & FLAG_CFLG) != 0 ? 1 : 0,
            (flags & FLAG_DRDY) != 0 ? 1 : 0);
        Console.WriteLine("  CGLM (0x7B9) = {0} (0x{0:X2})  [7-bit charge limit, 1-99 = active]", cglm);
        Console.WriteLine("  Battery SOC  = {0}%", soc);
        Console.WriteLine("  Mode (0x7A6) = 0x{0:X2}  (mode bits: 0x{1:X2})", mode, mode & 0x38);
    }

    /// <summary>
    /// WKBC protocol: write via the EC command interface.
    /// Mirrors the DSDT WKBC method.
    /// </summary>
    static bool WkbcWrite(int targetAddr, byte value)
    {
        byte addrLo = (byte)(targetAddr & 0xFF);
        byte addrHi = (byte)((targetAddr >> 8) & 0xFF);

        // Step 1: Check BFLG is clear
        byte flags = ReadEC(ADDR_FLAGS);
        if ((flags & FLAG_BFLG) != 0)
        {
            Console.WriteLine("  ERROR: BFLG set (busy). Aborting.");
            return false;
        }

        // Step 2: Set BFLG, clear DRDY
        flags = (byte)((flags | FLAG_BFLG) & ~FLAG_DRDY);
        WriteEC(ADDR_FLAGS, flags);
        Console.WriteLine("  [1] Set BFLG=1, DRDY=0 (flags=0x{0:X2})", flags);

        // Step 3: Clear DRDY via re-read (match DSDT sequence exactly)
        flags = ReadEC(ADDR_FLAGS);
        flags = (byte)(flags & ~FLAG_DRDY);
        WriteEC(ADDR_FLAGS, flags);

        // Step 4: Write address and data
        WriteEC(ADDR_LDAT, addrLo);
        WriteEC(ADDR_HDAT, addrHi);
        WriteEC(ADDR_CMDL, value);
        WriteEC(ADDR_CMDH, 0x00);
        Console.WriteLine("  [2] LDAT=0x{0:X2} HDAT=0x{1:X2} CMDL=0x{2:X2} CMDH=0x00",
            addrLo, addrHi, value);

        // Step 5: Set WFLG to trigger write
        flags = ReadEC(ADDR_FLAGS);
        flags = (byte)(flags | FLAG_WFLG);
        WriteEC(ADDR_FLAGS, flags);
        Console.WriteLine("  [3] WFLG=1 (flags=0x{0:X2}) -- write triggered", flags);

        // Step 6: Sleep 10ms (per DSDT)
        Thread.Sleep(10);

        // Step 7: Poll DRDY (20 x 50ms = 1000ms timeout)
        bool gotAck = false;
        for (int i = 0; i < 20; i++)
        {
            Thread.Sleep(50);
            flags = ReadEC(ADDR_FLAGS);
            if ((flags & FLAG_DRDY) != 0)
            {
                Console.WriteLine("  [4] DRDY ack after {0}ms (flags=0x{1:X2})", (i + 1) * 50 + 10, flags);
                gotAck = true;
                break;
            }
        }

        if (!gotAck)
            Console.WriteLine("  [4] WARNING: No DRDY ack after 1010ms");

        // Step 8: Clear DRDY and BFLG
        flags = ReadEC(ADDR_FLAGS);
        flags = (byte)(flags & ~(FLAG_DRDY | FLAG_BFLG));
        WriteEC(ADDR_FLAGS, flags);
        Console.WriteLine("  [5] Cleanup (flags=0x{0:X2})", flags);

        return gotAck;
    }

    /// <summary>
    /// RKBC protocol: read via the EC command interface.
    /// </summary>
    static bool RkbcRead(int targetAddr, out byte valueLo, out byte valueHi)
    {
        valueLo = 0;
        valueHi = 0;
        byte addrLo = (byte)(targetAddr & 0xFF);
        byte addrHi = (byte)((targetAddr >> 8) & 0xFF);

        // Check BFLG
        byte flags = ReadEC(ADDR_FLAGS);
        if ((flags & FLAG_BFLG) != 0)
        {
            Console.WriteLine("  ERROR: BFLG set (busy). Aborting.");
            return false;
        }

        // Set BFLG, clear DRDY
        flags = (byte)((flags | FLAG_BFLG) & ~FLAG_DRDY);
        WriteEC(ADDR_FLAGS, flags);

        // Clear DRDY again (match DSDT)
        flags = ReadEC(ADDR_FLAGS);
        flags = (byte)(flags & ~FLAG_DRDY);
        WriteEC(ADDR_FLAGS, flags);

        // Write address
        WriteEC(ADDR_LDAT, addrLo);
        WriteEC(ADDR_HDAT, addrHi);
        Console.WriteLine("  RKBC: target=0x{0:X2}{1:X2}", addrHi, addrLo);

        // Set RFLG
        flags = ReadEC(ADDR_FLAGS);
        flags = (byte)(flags | FLAG_RFLG);
        WriteEC(ADDR_FLAGS, flags);
        Console.WriteLine("  RFLG=1 (flags=0x{0:X2})", flags);

        // Poll DRDY
        bool gotAck = false;
        for (int i = 0; i < 20; i++)
        {
            Thread.Sleep(50);
            flags = ReadEC(ADDR_FLAGS);
            if ((flags & FLAG_DRDY) != 0)
            {
                Console.WriteLine("  DRDY ack after {0}ms", (i + 1) * 50);
                gotAck = true;
                break;
            }
        }

        if (gotAck)
        {
            valueLo = ReadEC(ADDR_CMDL);
            valueHi = ReadEC(ADDR_CMDH);
            Console.WriteLine("  Result: CMDL=0x{0:X2} ({0})  CMDH=0x{1:X2} ({1})", valueLo, valueHi);
        }
        else
        {
            Console.WriteLine("  WARNING: No DRDY ack after 1000ms");
        }

        // Cleanup
        flags = ReadEC(ADDR_FLAGS);
        flags = (byte)(flags & ~(FLAG_DRDY | FLAG_BFLG));
        WriteEC(ADDR_FLAGS, flags);

        return gotAck;
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
            ShowStatus("CURRENT STATE");
            Console.WriteLine();

            if (args.Length == 0)
            {
                Console.WriteLine("Usage:");
                Console.WriteLine("  wkbc_charge_test.exe                  -- show status only");
                Console.WriteLine("  wkbc_charge_test.exe 80               -- set CGLM to 80 via WKBC");
                Console.WriteLine("  wkbc_charge_test.exe reset            -- set CGLM to 0 via WKBC");
                Console.WriteLine("  wkbc_charge_test.exe read             -- read 0x7B9 via RKBC");
                Console.WriteLine("  wkbc_charge_test.exe read 0x7A6       -- read any addr via RKBC");
                Console.WriteLine("  wkbc_charge_test.exe w 0x7A6 0x28     -- write any addr via WKBC");
                Console.WriteLine("  wkbc_charge_test.exe mode 5           -- set battery mode (0-7) via WKBC");
                Console.WriteLine("  wkbc_charge_test.exe combo 80         -- CGLM + mode via WKBC");
                Console.WriteLine("  wkbc_charge_test.exe direct 80        -- direct write CGLM (no WKBC)");
                return;
            }

            string cmd = args[0].ToLower();

            if (cmd == "read")
            {
                int readAddr = ADDR_CGLM;
                if (args.Length > 1)
                    readAddr = args[1].StartsWith("0x") ? Convert.ToInt32(args[1], 16) : int.Parse(args[1]);
                Console.WriteLine("=== RKBC Read of 0x{0:X3} ===", readAddr);
                byte lo, hi;
                if (RkbcRead(readAddr, out lo, out hi))
                {
                    Console.WriteLine("  RKBC value = 0x{0:X2} ({0})  hi=0x{1:X2}", lo, hi);
                    Console.WriteLine("  Direct read = 0x{0:X2} ({0})", ReadEC(readAddr));
                }
                Console.WriteLine();
                ShowStatus("AFTER RKBC READ");
            }
            else if (cmd == "w" && args.Length > 2)
            {
                int wAddr = args[1].StartsWith("0x") ? Convert.ToInt32(args[1], 16) : int.Parse(args[1]);
                byte wVal = (byte)(args[2].StartsWith("0x") ? Convert.ToInt32(args[2], 16) : int.Parse(args[2]));
                byte before = ReadEC(wAddr);
                Console.WriteLine("=== WKBC Write: 0x{0:X3} = 0x{1:X2} (was 0x{2:X2}) ===", wAddr, wVal, before);
                bool ok = WkbcWrite(wAddr, wVal);
                Thread.Sleep(500);
                byte after = ReadEC(wAddr);
                Console.WriteLine("  Readback: 0x{0:X2}  {1}", after, after == wVal ? "STUCK" : "DID NOT STICK");
                Console.WriteLine();
                ShowStatus("AFTER WKBC WRITE");
            }
            else if (cmd == "mode" && args.Length > 1)
            {
                int m = int.Parse(args[1]);
                if (m < 0 || m > 7) { Console.WriteLine("Mode must be 0-7"); return; }
                byte cur7A6 = ReadEC(0x7A6);
                byte newVal = (byte)((cur7A6 & ~0x38) | (m << 3));
                Console.WriteLine("=== WKBC Mode: 0x7A6 = 0x{0:X2} -> 0x{1:X2} (mode {2}) ===", cur7A6, newVal, m);
                bool ok = WkbcWrite(0x7A6, newVal);
                Thread.Sleep(1000);
                byte after = ReadEC(0x7A6);
                Console.WriteLine("  Readback: 0x{0:X2}  {1}", after, after == newVal ? "STUCK" : "DID NOT STICK");
                Console.WriteLine();
                ShowStatus("AFTER WKBC MODE");
            }
            else if (cmd == "combo" && args.Length > 1)
            {
                byte limit = byte.Parse(args[1]);
                byte cur7A6 = ReadEC(0x7A6);
                // Set CGLM via WKBC
                Console.WriteLine("=== COMBO: CGLM={0} + Stationary mode via WKBC ===", limit);
                Console.WriteLine("Step 1: WKBC write CGLM (0x7B9) = {0}", limit);
                WkbcWrite(ADDR_CGLM, limit);
                Thread.Sleep(500);
                // Set mode to Stationary via WKBC
                byte statVal = (byte)((cur7A6 & ~0x38) | 0x28);
                Console.WriteLine("Step 2: WKBC write mode (0x7A6) = 0x{0:X2} (Stationary)", statVal);
                WkbcWrite(0x7A6, statVal);
                Thread.Sleep(1000);
                Console.WriteLine();
                ShowStatus("AFTER COMBO");
                Console.WriteLine("  CGLM = {0}, SOC = {1}%", ReadEC(ADDR_CGLM), ReadEC(0x4AB));
            }
            else if (cmd == "direct" && args.Length > 1)
            {
                byte val = byte.Parse(args[1]);
                Console.WriteLine("=== Direct ECWRITE to 0x7B9 = {0} ===", val);
                WriteEC(ADDR_CGLM, val);
                Thread.Sleep(100);
                byte rb = ReadEC(ADDR_CGLM);
                Console.WriteLine("  Readback: {0} (0x{0:X2})  {1}", rb,
                    rb == val ? "STUCK" : "DID NOT STICK");
                Console.WriteLine();
                ShowStatus("AFTER DIRECT WRITE");
            }
            else
            {
                byte limit;
                if (cmd == "reset")
                    limit = 0;
                else
                    limit = byte.Parse(args[0]);

                if (limit > 99)
                {
                    Console.WriteLine("ERROR: Charge limit must be 0-99.");
                    return;
                }

                Console.WriteLine("=== WKBC Write: CGLM (0x7B9) = {0} ===", limit);
                bool ok = WkbcWrite(ADDR_CGLM, limit);

                Thread.Sleep(200);
                Console.WriteLine();
                ShowStatus("AFTER WKBC WRITE");

                byte cglm = ReadEC(ADDR_CGLM);
                Console.WriteLine();
                if (cglm == limit)
                    Console.WriteLine("** SUCCESS: CGLM = {0}. Charge limit set! **", cglm);
                else
                    Console.WriteLine("** FAILED: CGLM = {0} (expected {1}). **", cglm, limit);
            }
        }
        finally
        {
            CloseHandle(hDev);
        }
    }
}
