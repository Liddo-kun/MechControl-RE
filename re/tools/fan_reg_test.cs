// Interactive EC register tester for fan-curve control bits.
// Focused on 0x7C4 / 0x7C5 / 0x7C6. Single-key commands, live poll at 500ms,
// each write shows before / immediate readback / +500ms readback so you can
// tell rejection apart from async clearing.
//
// Compile (Git Bash):
//   /c/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe -nologo \
//     -out:fan_reg_test.exe fan_reg_test.cs
//
// Run: elevated terminal, ACPIDriver service must be running.

using System;
using System.Runtime.InteropServices;
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
    static extern bool DeviceIoControl(IntPtr hDevice, uint dwIoControlCode,
        byte[] lpInBuffer, int nInBufferSize, ref int lpOutBuffer, int nOutBufferSize,
        ref int lpBytesReturned, IntPtr lpOverlapped);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr hObject);

    const uint IOCTL_READ  = 0x9C40A488;
    const uint IOCTL_WRITE = 0x9C40A48C;

    static IntPtr hDev;
    static string _lastAction = "(none)";
    static string _lastResult = "";

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

    static string Bits(int v)
    {
        return Convert.ToString(v, 2).PadLeft(8, '0');
    }

    static void DoWrite(int addr, int val, string label)
    {
        int before = ReadEC(addr);
        WriteEC(addr, val);
        int imm = ReadEC(addr);
        Thread.Sleep(500);
        int delayed = ReadEC(addr);

        string verdict;
        if (imm == val && delayed == val)       verdict = "STUCK";
        else if (imm == val && delayed != val)  verdict = "CLEARED async";
        else if (imm != val && delayed == val)  verdict = "DELAYED accept";
        else                                    verdict = "REJECTED";

        _lastAction = label;
        _lastResult = "before=0x" + before.ToString("X2") +
                      "  wrote=0x" + val.ToString("X2") +
                      "  imm=0x"   + imm.ToString("X2") +
                      "  +500ms=0x"+ delayed.ToString("X2") +
                      "  " + verdict;
    }

    static void WriteBit(int addr, int bit, bool on, string label)
    {
        int cur = ReadEC(addr);
        int val;
        if (on) val = cur | (1 << bit);
        else    val = cur & ~(1 << bit);
        DoWrite(addr, val, label);
    }

    // Rising-edge pulse: force low, then high. Guarantees an EC-visible
    // 0->value transition even if the register was already at `value`.
    static void PulseRising(int addr, int value, string label)
    {
        int before = ReadEC(addr);
        WriteEC(addr, 0x00);
        Thread.Sleep(10);
        int low = ReadEC(addr);
        WriteEC(addr, value);
        int high = ReadEC(addr);
        Thread.Sleep(500);
        int delayed = ReadEC(addr);
        _lastAction = label;
        _lastResult = "before=0x" + before.ToString("X2") +
                      "  low=0x" + low.ToString("X2") +
                      "  high=0x" + high.ToString("X2") +
                      "  +500ms=0x" + delayed.ToString("X2");
    }

    // OEM apply pattern: high -> low -> high -> low (two rising + two falling).
    static void PulseOem(int addr, int value, string label)
    {
        int before = ReadEC(addr);
        WriteEC(addr, value); Thread.Sleep(10);
        WriteEC(addr, 0x00);  Thread.Sleep(10);
        WriteEC(addr, value); Thread.Sleep(10);
        WriteEC(addr, 0x00);
        Thread.Sleep(500);
        int delayed = ReadEC(addr);
        _lastAction = label;
        _lastResult = "before=0x" + before.ToString("X2") +
                      "  pattern=[0x" + value.ToString("X2") + ",0x00,0x" + value.ToString("X2") + ",0x00]" +
                      "  +500ms=0x" + delayed.ToString("X2");
    }

    static void WriteLineP(string s)
    {
        int width;
        try { width = Console.WindowWidth - 1; } catch { width = 79; }
        if (width < 1) width = 79;
        if (s.Length < width) s = s + new string(' ', width - s.Length);
        else if (s.Length > width) s = s.Substring(0, width);
        Console.WriteLine(s);
    }

    static void Redraw()
    {
        int v741 = ReadEC(0x741);
        int v7C4 = ReadEC(0x7C4);
        int v7C5 = ReadEC(0x7C5);
        int v7C6 = ReadEC(0x7C6);
        int v7C7 = ReadEC(0x7C7);
        int v7CC = ReadEC(0x7CC);
        int v75B = ReadEC(0x75B);
        int v75C = ReadEC(0x75C);

        try { Console.SetCursorPosition(0, 0); } catch { }

        WriteLineP("============= EC Register Tester (0x7C4 / 0x7C5 / 0x7C6) =============");
        WriteLineP("");
        WriteLineP("  0x741 = 0x" + v741.ToString("X2") + "   (activation byte)");
        WriteLineP("  0x7C4 = 0x" + v7C4.ToString("X2") + "   " + Bits(v7C4) + "   (undocumented)");
        WriteLineP("  0x7C5 = 0x" + v7C5.ToString("X2") + "   " + Bits(v7C5) +
            "   [bit7=" + ((v7C5 & 0x80) != 0 ? "ON " : "off") +
            " bit6=" + ((v7C5 & 0x40) != 0 ? "ON " : "off") +
            " bit5=" + ((v7C5 & 0x20) != 0 ? "ON " : "off") + "]");
        WriteLineP("  0x7C6 = 0x" + v7C6.ToString("X2") + "   " + Bits(v7C6) + "   (0x04 = fan curve mode active)");
        WriteLineP("  0x7C7 = 0x" + v7C7.ToString("X2") + "   (adjacent OEM byte)");
        WriteLineP("  0x7CC = 0x" + v7CC.ToString("X2") + "   (watchdog)");
        WriteLineP("");
        WriteLineP("  Fan duty:  CPU=" + (v75B / 2) + "% (0x" + v75B.ToString("X2") +
                   ")    GPU=" + (v75C / 2) + "% (0x" + v75C.ToString("X2") + ")");
        WriteLineP("");
        WriteLineP("  Last: " + _lastAction);
        WriteLineP("        " + _lastResult);
        WriteLineP("");
        // Per-bit state labels
        string b7 = (v7C5 & 0x80) != 0 ? "ON " : "off";
        string b6 = (v7C5 & 0x40) != 0 ? "ON " : "off";
        string b5 = (v7C5 & 0x20) != 0 ? "ON " : "off";
        string c4b3 = (v7C4 & 0x08) != 0 ? "ON " : "off";
        string c4b4 = (v7C4 & 0x10) != 0 ? "ON " : "off";

        // Marker for which preset matches current value
        string mA0 = v7C5 == 0xA0 ? " <-- current" : "";
        string mE0 = v7C5 == 0xE0 ? " <-- current" : "";
        string m60 = v7C5 == 0x60 ? " <-- current" : "";
        string m20 = v7C5 == 0x20 ? " <-- current" : "";
        string m7C4_20 = v7C4 == 0x20 ? " <-- current" : "";
        string m7C4_38 = v7C4 == 0x38 ? " <-- current" : "";
        string m7C6_04 = v7C6 == 0x04 ? " <-- current" : "";
        string m7C6_00 = v7C6 == 0x00 ? " <-- current" : "";

        WriteLineP("  -------- 0x7C5 (cur=0x" + v7C5.ToString("X2") + ") --------");
        WriteLineP("   q) bit7 ON    a) bit7 OFF    || bit7=" + b7);
        WriteLineP("   w) bit6 ON    s) bit6 OFF    || bit6=" + b6);
        WriteLineP("   e) bit5 ON    d) bit5 OFF    || bit5=" + b5);
        WriteLineP("   r) = 0xA0  (bits 5+7)" + mA0);
        WriteLineP("   f) = 0xE0  (bits 5+6+7)" + mE0);
        WriteLineP("   t) = 0x60  (bits 5+6)" + m60);
        WriteLineP("   g) = 0x20  (bit 5 only)" + m20);
        WriteLineP("");
        WriteLineP("  -------- 0x7C4 (cur=0x" + v7C4.ToString("X2") + ") --------");
        WriteLineP("   u) = 0x20 (OEM value)" + m7C4_20);
        WriteLineP("   j) = 0x38 (cold-boot value)" + m7C4_38);
        WriteLineP("   i) bit3 ON    k) bit3 OFF    || bit3=" + c4b3);
        WriteLineP("   o) bit4 ON    l) bit4 OFF    || bit4=" + c4b4);
        WriteLineP("");
        WriteLineP("  -------- 0x7C6 (cur=0x" + v7C6.ToString("X2") + ") --------");
        WriteLineP("   y) = 0x04" + m7C6_04);
        WriteLineP("   h) = 0x00" + m7C6_00);
        WriteLineP("   p) pulse 0->0x04 (rising edge)");
        WriteLineP("   P) OEM pulse 04,00,04,00");
        WriteLineP("");
        WriteLineP("   z) refresh      x) quit");
        WriteLineP("");
        WriteLineP("> (press a key, no Enter needed)                                    ");
    }

    static bool HandleKey(char c)
    {
        switch (c)
        {
            // 0x7C5 individual bits
            case 'q': WriteBit(0x7C5, 7, true,  "0x7C5 bit 7 ON");  break;
            case 'a': WriteBit(0x7C5, 7, false, "0x7C5 bit 7 OFF"); break;
            case 'w': WriteBit(0x7C5, 6, true,  "0x7C5 bit 6 ON");  break;
            case 's': WriteBit(0x7C5, 6, false, "0x7C5 bit 6 OFF"); break;
            case 'e': WriteBit(0x7C5, 5, true,  "0x7C5 bit 5 ON");  break;
            case 'd': WriteBit(0x7C5, 5, false, "0x7C5 bit 5 OFF"); break;
            // 0x7C5 full-byte presets
            case 'r': DoWrite(0x7C5, 0xA0, "0x7C5 = 0xA0 (bits 5+7)");    break;
            case 'f': DoWrite(0x7C5, 0xE0, "0x7C5 = 0xE0 (bits 5+6+7)");  break;
            case 't': DoWrite(0x7C5, 0x60, "0x7C5 = 0x60 (bits 5+6)");    break;
            case 'g': DoWrite(0x7C5, 0x20, "0x7C5 = 0x20 (bit 5 only)");  break;
            // 0x7C6
            case 'y': DoWrite(0x7C6, 0x04, "0x7C6 = 0x04"); break;
            case 'h': DoWrite(0x7C6, 0x00, "0x7C6 = 0x00"); break;
            case 'p': PulseRising(0x7C6, 0x04, "0x7C6 rising-edge pulse 0x00 -> 0x04"); break;
            case 'P': PulseOem(0x7C6, 0x04, "0x7C6 OEM pulse 04,00,04,00"); break;
            // 0x7C4
            case 'u': DoWrite(0x7C4, 0x20, "0x7C4 = 0x20 (OEM value)");      break;
            case 'j': DoWrite(0x7C4, 0x38, "0x7C4 = 0x38 (cold-boot value)"); break;
            case 'i': WriteBit(0x7C4, 3, true,  "0x7C4 bit 3 ON");  break;
            case 'k': WriteBit(0x7C4, 3, false, "0x7C4 bit 3 OFF"); break;
            case 'o': WriteBit(0x7C4, 4, true,  "0x7C4 bit 4 ON");  break;
            case 'l': WriteBit(0x7C4, 4, false, "0x7C4 bit 4 OFF"); break;
            // meta
            case 'z': break;
            case 'x': return false;
        }
        return true;
    }

    static void Main()
    {
        hDev = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (hDev == new IntPtr(-1))
        {
            Console.WriteLine("FAILED to open \\\\.\\ACPIDriver. Run as Administrator.");
            return;
        }

        try { Console.Clear(); } catch { }

        bool running = true;
        DateTime lastRedraw = DateTime.MinValue;

        while (running)
        {
            if ((DateTime.Now - lastRedraw).TotalMilliseconds >= 500)
            {
                Redraw();
                lastRedraw = DateTime.Now;
            }

            if (Console.KeyAvailable)
            {
                ConsoleKeyInfo key = Console.ReadKey(true);
                if (!HandleKey(key.KeyChar)) running = false;
                Redraw();
                lastRedraw = DateTime.Now;
            }

            Thread.Sleep(50);
        }

        CloseHandle(hDev);
    }
}
