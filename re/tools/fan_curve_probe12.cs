// fan_curve_probe12.cs
//
// EC write sequence tester for custom fan curve activation.
// Trimmed to single test only — combo 12 retained as starting point,
// to be replaced with the init-register sequence from the startup capture.
//
// Writes a "witness curve" (CPU 80% flat, GPU 40% flat), reads back
// to confirm writes landed in EC RAM, polls live fan duty for 15s,
// and reports HIT or MISS.
//
// Build:
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe -nologo fan_curve_probe12.cs
//
// Run (elevated):
//   fan_curve_probe12.exe 12           - run the test
//   fan_curve_probe12.exe --restore    - disable custom mode, return EC to neutral

using System;
using System.Collections.Generic;
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

    // --- EC register addresses ---
    const int Reg_FanCurveEnable = 0x7C5;  // bit 7 = master switch
    const int Reg_FanCurveApply  = 0x7C6;  // edge-triggered apply latch
    const int Reg_Support2       = 0x766;
    const int Reg_FanSwitchSpeed = 0x787;

    const int Reg_CpuDuty   = 0x75B;
    const int Reg_GpuDuty   = 0x75C;
    const int Reg_CpuTemp   = 0x43E;
    const int Reg_GpuTemp   = 0x44F;

    // Fan table bases — RamFan1 layout
    const int CpuTempUpBase   = 0xF00;
    const int CpuTempDownBase = 0xF10;
    const int CpuDutyBase     = 0xF20;
    const int GpuTempUpBase   = 0xF30;
    const int GpuTempDownBase = 0xF40;
    const int GpuDutyBase     = 0xF50;
    const int ActiveEntries   = 10;
    const int EntriesPerArray = 16;

    // Witness curve — flat duty across all 10 active points
    static readonly int[] WitnessTempUp   = new int[] { 5, 10, 15, 20, 25, 30, 35, 40, 45, 50 };
    static readonly int[] WitnessTempDown = new int[] { 0,  5, 10, 15, 20, 25, 30, 35, 40, 45 };
    const int CpuWitnessDutyRaw = 160;  // 160/2 = 80%
    const int GpuWitnessDutyRaw = 80;   // 80/2  = 40%

    const int CpuTargetDuty   = 80;
    const int GpuTargetDuty   = 40;
    const int DutyTolerance   = 5;
    const int PollDurationSec = 15;
    const int SettleWaitSec   = 8;
    const int FinalSampleCount = 6;

    // --- EC IOCTL helpers ---

    static int ReadEC(int addr)
    {
        int inBuf = addr, outBuf = 0, ret = 0;
        if (!DeviceIoControl(hDev, IOCTL_READ, ref inBuf, 4, ref outBuf, 4, ref ret, IntPtr.Zero))
            return -1;
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

    static int ReadDutyPct(int addr)
    {
        int raw = ReadEC(addr);
        if (raw < 0) return 0;
        return raw / 2;
    }

    // --- Read-after-write verification ---

    static void CheckByte(List<string> diffs, int addr, int expected, string label)
    {
        int actual = ReadEC(addr);
        if (actual != expected)
        {
            diffs.Add("    [0x" + addr.ToString("X3") + "] " + label
                + " expected 0x" + expected.ToString("X2")
                + " got 0x" + actual.ToString("X2"));
        }
    }

    static List<string> VerifyWitnessTable()
    {
        List<string> diffs = new List<string>();
        for (int i = 0; i < ActiveEntries; i++)
        {
            CheckByte(diffs, CpuTempUpBase   + i, WitnessTempUp[i],   "cpuTempUp[" + i + "]");
            CheckByte(diffs, CpuTempDownBase + i, WitnessTempDown[i], "cpuTempDown[" + i + "]");
            CheckByte(diffs, CpuDutyBase     + i, CpuWitnessDutyRaw,  "cpuDuty[" + i + "]");
            CheckByte(diffs, GpuTempUpBase   + i, WitnessTempUp[i],   "gpuTempUp[" + i + "]");
            CheckByte(diffs, GpuTempDownBase + i, WitnessTempDown[i], "gpuTempDown[" + i + "]");
            CheckByte(diffs, GpuDutyBase     + i, GpuWitnessDutyRaw,  "gpuDuty[" + i + "]");
        }
        for (int i = ActiveEntries; i < EntriesPerArray; i++)
        {
            CheckByte(diffs, CpuTempUpBase   + i, 0xFF,              "cpuTempUp[" + i + "]");
            CheckByte(diffs, CpuTempDownBase + i, 0xFF,              "cpuTempDown[" + i + "]");
            CheckByte(diffs, CpuDutyBase     + i, CpuWitnessDutyRaw, "cpuDuty[" + i + "]");
            CheckByte(diffs, GpuTempUpBase   + i, 0xFF,              "gpuTempUp[" + i + "]");
            CheckByte(diffs, GpuTempDownBase + i, 0xFF,              "gpuTempDown[" + i + "]");
            CheckByte(diffs, GpuDutyBase     + i, GpuWitnessDutyRaw, "gpuDuty[" + i + "]");
        }
        return diffs;
    }

    // --- Table write ---

    static void WriteWitnessTable()
    {
        for (int i = 0; i < ActiveEntries; i++)
        {
            WriteEC(CpuTempUpBase   + i, WitnessTempUp[i]);
            WriteEC(CpuTempDownBase + i, WitnessTempDown[i]);
            WriteEC(CpuDutyBase     + i, CpuWitnessDutyRaw);
            WriteEC(GpuTempUpBase   + i, WitnessTempUp[i]);
            WriteEC(GpuTempDownBase + i, WitnessTempDown[i]);
            WriteEC(GpuDutyBase     + i, GpuWitnessDutyRaw);
        }
        for (int i = ActiveEntries; i < EntriesPerArray; i++)
        {
            WriteEC(CpuTempUpBase   + i, 0xFF);
            WriteEC(CpuTempDownBase + i, 0xFF);
            WriteEC(CpuDutyBase     + i, CpuWitnessDutyRaw);
            WriteEC(GpuTempUpBase   + i, 0xFF);
            WriteEC(GpuTempDownBase + i, 0xFF);
            WriteEC(GpuDutyBase     + i, GpuWitnessDutyRaw);
        }
    }

    // --- Neutralize: disable custom curve, EC reverts to builtin ---

    static void Neutralize()
    {
        int oem5 = ReadEC(Reg_FanCurveEnable);
        if (oem5 < 0) oem5 = 0;
        WriteEC(Reg_FanCurveEnable, oem5 & 0x7F);
        WriteEC(Reg_FanCurveApply, 0x00);
    }

    // --- Poll loop ---

    struct PollResult
    {
        public int CpuMin, CpuMax, CpuFinal;
        public int GpuMin, GpuMax, GpuFinal;
        public int LastCpuTemp, LastGpuTemp;
        public int LastOem5, LastOem6;
    }

    static PollResult PollFor(int durationSec)
    {
        int iterations = durationSec * 2;
        List<int> cpuSamples = new List<int>();
        List<int> gpuSamples = new List<int>();
        int lastCpuTemp = 0, lastGpuTemp = 0, lastOem5 = 0, lastOem6 = 0;

        for (int i = 0; i < iterations; i++)
        {
            int cpuDuty = ReadDutyPct(Reg_CpuDuty);
            int gpuDuty = ReadDutyPct(Reg_GpuDuty);
            int cpuTemp = ReadEC(Reg_CpuTemp);
            int gpuTemp = ReadEC(Reg_GpuTemp);
            int oem5    = ReadEC(Reg_FanCurveEnable);
            int oem6    = ReadEC(Reg_FanCurveApply);

            cpuSamples.Add(cpuDuty);
            gpuSamples.Add(gpuDuty);
            lastCpuTemp = cpuTemp;
            lastGpuTemp = gpuTemp;
            lastOem5    = oem5;
            lastOem6    = oem6;

            bool inFinalWindow = (i >= iterations - FinalSampleCount);
            string mark = inFinalWindow ? " *" : "  ";

            Console.WriteLine("    +" + ((i + 1) * 0.5).ToString("F1").PadLeft(4) + "s" + mark
                + "CPU " + cpuDuty.ToString().PadLeft(3) + "% (" + cpuTemp.ToString().PadLeft(2) + "C)"
                + "  |  GPU " + gpuDuty.ToString().PadLeft(3) + "% (" + gpuTemp.ToString().PadLeft(2) + "C)"
                + "  |  0x7C5=0x" + oem5.ToString("X2") + " 0x7C6=0x" + oem6.ToString("X2"));

            Thread.Sleep(500);
        }

        PollResult r = new PollResult();
        r.CpuMin = cpuSamples[0]; r.CpuMax = cpuSamples[0];
        r.GpuMin = gpuSamples[0]; r.GpuMax = gpuSamples[0];
        foreach (int v in cpuSamples)
        {
            if (v < r.CpuMin) r.CpuMin = v;
            if (v > r.CpuMax) r.CpuMax = v;
        }
        foreach (int v in gpuSamples)
        {
            if (v < r.GpuMin) r.GpuMin = v;
            if (v > r.GpuMax) r.GpuMax = v;
        }
        int start = Math.Max(0, cpuSamples.Count - FinalSampleCount);
        int cpuSum = 0, gpuSum = 0, n = 0;
        for (int i = start; i < cpuSamples.Count; i++)
        {
            cpuSum += cpuSamples[i];
            gpuSum += gpuSamples[i];
            n++;
        }
        r.CpuFinal = n > 0 ? cpuSum / n : 0;
        r.GpuFinal = n > 0 ? gpuSum / n : 0;
        r.LastCpuTemp = lastCpuTemp;
        r.LastGpuTemp = lastGpuTemp;
        r.LastOem5    = lastOem5;
        r.LastOem6    = lastOem6;
        return r;
    }

    // --- Combination runner ---

    struct Result
    {
        public string Name;
        public int PreCpuDuty, PreGpuDuty;
        public int CpuMin, CpuMax, CpuFinal;
        public int GpuMin, GpuMax, GpuFinal;
        public bool CpuHit;
        public bool GpuHit;
        public bool Hit;
    }

    static bool InWindow(int value, int target, int tolerance)
    {
        int d = value - target;
        if (d < 0) d = -d;
        return d <= tolerance;
    }

    static Result RunCombination(string name, Action apply)
    {
        Console.WriteLine();
        Console.WriteLine("=== " + name + " ===");

        Console.WriteLine("  step 1: neutralize (0x7C5 bit7=0, 0x7C6=0), wait " + SettleWaitSec + "s for fans to settle...");
        Neutralize();
        Thread.Sleep(SettleWaitSec * 1000);

        int preCpu = ReadDutyPct(Reg_CpuDuty);
        int preGpu = ReadDutyPct(Reg_GpuDuty);
        int preOem5 = ReadEC(Reg_FanCurveEnable);
        int preOem6 = ReadEC(Reg_FanCurveApply);
        Console.WriteLine("  pre:  CPU=" + preCpu + "%  GPU=" + preGpu + "%  0x7C5=0x" + preOem5.ToString("X2") + "  0x7C6=0x" + preOem6.ToString("X2"));

        Console.WriteLine("  step 2: applying write sequence...");
        apply();

        Console.WriteLine("  step 2b: read-after-write verify...");
        int postOem5 = ReadEC(Reg_FanCurveEnable);
        int postOem6 = ReadEC(Reg_FanCurveApply);
        string bit7 = ((postOem5 & 0x80) != 0) ? "set" : "CLEAR";
        string c6act = (postOem6 == 0x04) ? "active(0x04)" : "INACTIVE(0x" + postOem6.ToString("X2") + ")";
        Console.WriteLine("    0x7C5 = 0x" + postOem5.ToString("X2") + "  (master bit7 " + bit7 + ")");
        Console.WriteLine("    0x7C6 = 0x" + postOem6.ToString("X2") + "  " + c6act);
        List<string> tableDiffs = VerifyWitnessTable();
        if (tableDiffs.Count == 0)
        {
            Console.WriteLine("    table: all 96 bytes match witness (writes landed)");
        }
        else
        {
            Console.WriteLine("    table: " + tableDiffs.Count + " of 96 bytes MISMATCH:");
            int shown = 0;
            foreach (string d in tableDiffs)
            {
                Console.WriteLine(d);
                shown++;
                if (shown >= 12) { Console.WriteLine("    ... (" + (tableDiffs.Count - shown) + " more)"); break; }
            }
        }

        Console.WriteLine("  step 3: polling " + PollDurationSec + "s (last " + FinalSampleCount + " samples = final window)...");
        PollResult p = PollFor(PollDurationSec);

        Result r = new Result();
        r.Name = name;
        r.PreCpuDuty = preCpu;
        r.PreGpuDuty = preGpu;
        r.CpuMin = p.CpuMin; r.CpuMax = p.CpuMax; r.CpuFinal = p.CpuFinal;
        r.GpuMin = p.GpuMin; r.GpuMax = p.GpuMax; r.GpuFinal = p.GpuFinal;
        r.CpuHit = InWindow(p.CpuFinal, CpuTargetDuty, DutyTolerance);
        r.GpuHit = InWindow(p.GpuFinal, GpuTargetDuty, DutyTolerance);
        r.Hit = r.CpuHit;

        int cpuDelta = p.CpuFinal - preCpu;
        int gpuDelta = p.GpuFinal - preGpu;
        string cpuDeltaStr = (cpuDelta >= 0 ? "+" : "") + cpuDelta;
        string gpuDeltaStr = (gpuDelta >= 0 ? "+" : "") + gpuDelta;

        Console.WriteLine("  CPU: pre=" + preCpu + "  min=" + p.CpuMin + "  max=" + p.CpuMax + "  final=" + p.CpuFinal + "  delta=" + cpuDeltaStr + "  target=" + CpuTargetDuty + "+-" + DutyTolerance + "  " + (r.CpuHit ? "HIT" : "miss"));
        Console.WriteLine("  GPU: pre=" + preGpu + "  min=" + p.GpuMin + "  max=" + p.GpuMax + "  final=" + p.GpuFinal + "  delta=" + gpuDeltaStr + "  target=" + GpuTargetDuty + "+-" + DutyTolerance + "  " + (r.GpuHit ? "HIT" : "miss"));
        Console.WriteLine("  VERDICT: " + (r.Hit ? "HIT" : "MISS") + " (CPU is primary signal)");
        return r;
    }

    // --- Combo 12: Reset + mode switch ---
    //
    // Burst 1 RESET (0x706=0x40, clear profile, 0x743=0x01) then
    // Burst 2 mode switch. To be replaced with init-register sequence
    // from the startup capture.

    static void Combo_ResetPlusModeSwitch()
    {
        // ========== BURST 1: RESET ==========
        Console.WriteLine("    [BURST 1 -- RESET: exit custom mode, clear profile]");

        Console.WriteLine("    W 0x706 = 0x40  (BIOS_CTRL: EXIT custom mode)");
        WriteEC(0x706, 0x40);
        Console.WriteLine("    W 0x7C6 = 0x00  (clear latch)");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W 0x743 = 0x01  (DB_CTRL RESET state)");
        WriteEC(0x743, 0x01);
        Console.WriteLine("    W 0x7C5 = 0x80  (master ON for reset)");
        WriteEC(Reg_FanCurveEnable, 0x80);
        Console.WriteLine("    W 0x743 = 0x01  (repeat)");
        WriteEC(0x743, 0x01);

        // Clear all mode profile registers to 0
        Console.WriteLine("    W 0x744 = 0x00  (TGP clear)");
        WriteEC(0x744, 0x00);
        Console.WriteLine("    W 0x751 = 0x00  (FAN_CTRL clear)");
        WriteEC(0x751, 0x00);
        Console.WriteLine("    W 0x727 = 0x00  (ESHUTTER clear)");
        WriteEC(0x727, 0x00);
        Console.WriteLine("    W 0x726 = 0x00  (OEM9 clear)");
        WriteEC(0x726, 0x00);
        Console.WriteLine("    W 0x783 = 0x00  (PL1 clear)");
        WriteEC(0x783, 0x00);
        Console.WriteLine("    W 0x784 = 0x00  (PL2 clear)");
        WriteEC(0x784, 0x00);
        Console.WriteLine("    W 0x785 = 0x00  (PL4 clear)");
        WriteEC(0x785, 0x00);
        Console.WriteLine("    W 0x787 = 0x00  (FAN_SWITCH_SPEED clear)");
        WriteEC(0x787, 0x00);

        Console.WriteLine("    W table (96 bytes -- default table for reset)");
        WriteWitnessTable();

        Console.WriteLine("    W 0x7C6 = 0x04  (edge activate)");
        WriteEC(Reg_FanCurveApply, 0x04);
        Thread.Sleep(32);
        Console.WriteLine("    W 0x7C6 = 0x00  (edge release)");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W 0x7C5 = 0x00  (master OFF -- reset complete)");
        WriteEC(Reg_FanCurveEnable, 0x00);

        Console.WriteLine("    sleep 1s (Burst 1->2 gap)");
        Thread.Sleep(1000);

        // ========== BURST 2: MODE SWITCH ==========
        Console.WriteLine("    [BURST 2 -- MODE SWITCH]");

        // Phase A -- pre-init (master stays OFF)
        Console.WriteLine("    [Phase A -- pre-init, master stays OFF]");
        Console.WriteLine("    W 0x706 = 0x41  (BIOS_CTRL enable custom -- 0x40->0x41 EDGE)");
        WriteEC(0x706, 0x41);
        Console.WriteLine("    W 0x7C6 = 0x00  (clear apply latch)");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W 0x743 = 0x04  (DB_CTRL initial touch)");
        WriteEC(0x743, 0x04);
        int cur7C5a = ReadEC(Reg_FanCurveEnable);
        Console.WriteLine("    R 0x7C5 = 0x" + cur7C5a.ToString("X2")
            + " -> W 0x" + cur7C5a.ToString("X2") + "  (PURE RMW, master stays 0)");
        WriteEC(Reg_FanCurveEnable, cur7C5a);
        Console.WriteLine("    W 0x743 = 0x05  (touch)");
        WriteEC(0x743, 0x05);

        // Phase B -- mode profile (SKU values hardcoded for X6FR5xxY)
        Console.WriteLine("    [Phase B -- mode profile, X6FR5xxY SKU values]");
        Console.WriteLine("    W 0x743 = 0x07  (DB_CTRL transaction open)");
        WriteEC(0x743, 0x07);
        Console.WriteLine("    W 0x727 = 0x40  (ESHUTTER)");
        WriteEC(0x727, 0x40);
        Console.WriteLine("    W 0x751 = 0x00  (FAN_CTRL normal)");
        WriteEC(0x751, 0x00);
        Console.WriteLine("    W 0x726 = 0x80  (OEM9)");
        WriteEC(0x726, 0x80);
        Console.WriteLine("    W 0x783 = 0x32  (PL1 = 50W)");
        WriteEC(0x783, 0x32);
        Console.WriteLine("    W 0x784 = 0x32  (PL2 = 50W)");
        WriteEC(0x784, 0x32);
        Console.WriteLine("    W 0x785 = 0x3C  (PL4 = 60W)");
        WriteEC(0x785, 0x3C);
        Console.WriteLine("    W 0x786 = 0xDF  (TCC_OFFSET)");
        WriteEC(0x786, 0xDF);
        Console.WriteLine("    W 0x744 = 0x28  (TGP = 40W)");
        WriteEC(0x744, 0x28);
        Console.WriteLine("    W 0x745 = 0x00  (DB_TOTAL)");
        WriteEC(0x745, 0x00);
        Console.WriteLine("    W 0x746 = 0x00  (DB_MAX clear)");
        WriteEC(0x746, 0x00);
        Console.WriteLine("    W 0x743 = 0x05  (DB_CTRL touch)");
        WriteEC(0x743, 0x05);
        Console.WriteLine("    W 0x787 = 0x00  (FAN_SWITCH_SPEED)");
        WriteEC(0x787, 0x00);
        Console.WriteLine("    W 0x746 = 0x05  (DB_MAX set)");
        WriteEC(0x746, 0x05);
        int cur728 = ReadEC(0x728);
        Console.WriteLine("    R 0x728 = 0x" + cur728.ToString("X2")
            + " -> W 0x" + cur728.ToString("X2") + "  (RMW, preserve firmware bits)");
        WriteEC(0x728, cur728);

        // Phase C -- fan table
        Console.WriteLine("    [Phase C -- fan table]");
        Console.WriteLine("    W table (96 bytes witness curve)");
        WriteWitnessTable();

        // Phase D -- iter 1: edge pulse WHILE MASTER IS 0, then set master
        Console.WriteLine("    [Phase D -- iter 1: edge pulse (master=0), then set master]");
        int v00 = ReadEC(CpuTempUpBase + 0);
        int v01 = ReadEC(CpuTempUpBase + 1);
        Console.WriteLine("    R 0xF00=0x" + v00.ToString("X2")
            + " R 0xF01=0x" + v01.ToString("X2") + "  (verify)");
        Console.WriteLine("    W 0x7C6 = 0x04  (edge activate, 0x7C5 still 0)");
        WriteEC(Reg_FanCurveApply, 0x04);
        Thread.Sleep(32);
        Console.WriteLine("    W 0x7C6 = 0x00  (edge release, 0x7C5 still 0)");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W 0x7C5 = 0x80  (master SET, first time)");
        WriteEC(Reg_FanCurveEnable, 0x80);

        // Phase E -- iter 2: edge pulse + re-master RMW
        Console.WriteLine("    [Phase E -- iter 2: edge pulse + re-master RMW]");
        Thread.Sleep(32);
        Console.WriteLine("    W 0x7C6 = 0x04  (edge activate)");
        WriteEC(Reg_FanCurveApply, 0x04);
        Thread.Sleep(32);
        Console.WriteLine("    W 0x7C6 = 0x00  (edge release)");
        WriteEC(Reg_FanCurveApply, 0x00);
        int cur7C5b = ReadEC(Reg_FanCurveEnable);
        Console.WriteLine("    R 0x7C5 = 0x" + cur7C5b.ToString("X2")
            + " -> W 0x" + cur7C5b.ToString("X2") + "  (RMW, should be 0x80)");
        WriteEC(Reg_FanCurveEnable, cur7C5b);

        // Final: hold 0x7C6 at 0x04
        Thread.Sleep(32);
        Console.WriteLine("    W 0x7C6 = 0x04  (final activate, HELD)");
        WriteEC(Reg_FanCurveApply, 0x04);
    }

    // --- Main ---

    static void Main(string[] args)
    {
        hDev = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (hDev == new IntPtr(-1))
        {
            Console.WriteLine("FAILED to open \\\\.\\ACPIDriver - run as Administrator.");
            return;
        }

        string mode = args.Length > 0 ? args[0].ToLowerInvariant() : "help";

        if (mode == "--restore" || mode == "restore")
        {
            Console.WriteLine("Restoring EC to neutral (0x7C5 bit7=0, 0x7C6=0)...");
            Neutralize();
            int o5 = ReadEC(Reg_FanCurveEnable);
            int o6 = ReadEC(Reg_FanCurveApply);
            Console.WriteLine("Done. 0x7C5=0x" + o5.ToString("X2") + "  0x7C6=0x" + o6.ToString("X2"));
            CloseHandle(hDev);
            return;
        }

        if (mode == "help" || mode == "--help" || mode == "-h")
        {
            Console.WriteLine("Usage: fan_curve_probe12.exe <12|--restore>");
            Console.WriteLine("  12         Run the test combo");
            Console.WriteLine("  --restore  Disable custom curve, exit");
            CloseHandle(hDev);
            return;
        }

        Console.WriteLine("============================================================");
        Console.WriteLine(" fan_curve_probe -- EC write sequence tester");
        Console.WriteLine("============================================================");
        Console.WriteLine(" Witness curve:  CPU=" + CpuTargetDuty + "% flat   GPU=" + GpuTargetDuty + "% flat");
        Console.WriteLine(" HIT criterion:  |final - target| <= " + DutyTolerance);
        Console.WriteLine(" Sequence:       neutralize " + SettleWaitSec + "s, apply, poll " + PollDurationSec + "s");
        Console.WriteLine("============================================================");

        List<Result> results = new List<Result>();

        int n;
        if (mode == "all" || mode == "12" || (int.TryParse(mode, out n) && n == 12))
        {
            results.Add(RunCombination("12.reset+mode-switch", Combo_ResetPlusModeSwitch));
        }
        else
        {
            Console.WriteLine("Unknown argument: " + args[0]);
            Console.WriteLine("Run with 'help' for usage.");
            CloseHandle(hDev);
            return;
        }

        Console.WriteLine();
        Console.WriteLine("Restoring neutral state before exit...");
        Neutralize();

        CloseHandle(hDev);
    }
}
