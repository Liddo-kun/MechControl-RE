// fan_curve_probe13.cs
//
// Tests the complete OEM-style EC init + fan curve apply sequence using
// the OEM's exact fan table layout (94 bytes, not 96 -- tempDown[0] skipped).
//
// Three sequential tests:
//   13a: OEM table + standard timing  (baseline)
//   13b: OEM table + 5s activation delay  (timing test)
//   13c: OEM table + no neutralize  (apply from active state)
//
// Fan table matches OEM structure byte-for-byte:
//   - 10 active points, 4C steps, OEM temp ranges
//   - duty[0] = 0x00 (0% below first threshold)
//   - tempDown[0] NOT written (OEM writes 94 bytes)
//   - Padding: 0xFF temps, last-active-duty
//   - CPU duty = 80% (0xA0), GPU duty = 55% (0x6E)
//
// Build:
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe -nologo fan_curve_probe13.cs
//
// Run (elevated):
//   fan_curve_probe13.exe 13           - run all three tests
//   fan_curve_probe13.exe 13a          - run only 13a
//   fan_curve_probe13.exe 13b          - run only 13b
//   fan_curve_probe13.exe 13c          - run only 13c
//   fan_curve_probe13.exe --restore    - disable custom mode, return EC to neutral

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

    // Control registers
    const int Reg_FanCurveEnable = 0x7C5;
    const int Reg_FanCurveApply  = 0x7C6;
    const int Reg_FanCurveParam  = 0x7C7;
    const int Reg_BiosCtrl       = 0x706;

    // Init registers (already in MechControl)
    const int Reg_Handshake1    = 0x709;
    const int Reg_Handshake2    = 0x708;
    const int Reg_Activation    = 0x741;
    const int Reg_FeatureEnable = 0x748;

    // Init registers (MISSING from MechControl)
    const int Reg_CmdClear1  = 0x749;
    const int Reg_CmdClear2  = 0x74A;
    const int Reg_CmdClear3  = 0x74B;
    const int Reg_CmdClear4  = 0x74E;
    const int Reg_Support2   = 0x766;
    const int Reg_Trigger    = 0x767;
    const int Reg_BattMode   = 0x7A6;

    // Monitoring
    const int Reg_CpuDuty = 0x75B;
    const int Reg_GpuDuty = 0x75C;
    const int Reg_CpuTemp = 0x43E;
    const int Reg_GpuTemp = 0x44F;

    // Fan table bases
    const int CpuTempUpBase   = 0xF00;
    const int CpuTempDownBase = 0xF10;
    const int CpuDutyBase     = 0xF20;
    const int GpuTempUpBase   = 0xF30;
    const int GpuTempDownBase = 0xF40;
    const int GpuDutyBase     = 0xF50;

    // =========================================================================
    // OEM-exact fan table (from Frida capture on X6FR5xxY)
    //
    // Layout: 10 active points, 6 padding.  tempDown[0] NOT written (94 bytes).
    // CPU duty = 80% (0xA0), GPU duty = 55% (0x6E).
    // duty[0] = 0x00 for both (OEM behavior: 0% below first threshold).
    // =========================================================================

    const int CpuDutyRaw = 0xA0;  // 80%
    const int GpuDutyRaw = 0x6E;  // 55%

    //                                  act0  act1  act2  act3  act4  act5  act6  act7  act8  act9  pad   pad   pad   pad   pad   pad
    static readonly int[] CpuTempUp   = { 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x55, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static readonly int[] CpuTempDown = {   -1, 0x2E, 0x32, 0x36, 0x3A, 0x3E, 0x42, 0x46, 0x4A, 0x4E, 0x52, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };  // [0] = -1 = SKIP
    static readonly int[] CpuDuty     = { 0x00, CpuDutyRaw, CpuDutyRaw, CpuDutyRaw, CpuDutyRaw, CpuDutyRaw, CpuDutyRaw, CpuDutyRaw, CpuDutyRaw, CpuDutyRaw, CpuDutyRaw, CpuDutyRaw, CpuDutyRaw, CpuDutyRaw, CpuDutyRaw, CpuDutyRaw };

    static readonly int[] GpuTempUp   = { 0x2E, 0x32, 0x36, 0x3A, 0x3E, 0x42, 0x46, 0x4A, 0x4E, 0x51, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static readonly int[] GpuTempDown = {   -1, 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };  // [0] = -1 = SKIP
    static readonly int[] GpuDuty     = { 0x00, GpuDutyRaw, GpuDutyRaw, GpuDutyRaw, GpuDutyRaw, GpuDutyRaw, GpuDutyRaw, GpuDutyRaw, GpuDutyRaw, GpuDutyRaw, GpuDutyRaw, GpuDutyRaw, GpuDutyRaw, GpuDutyRaw, GpuDutyRaw, GpuDutyRaw };

    const int CpuTargetDuty   = 80;   // expected at CPU temp > 48C
    const int GpuTargetDuty   = 55;   // expected at GPU temp >= 46C; 0% if GPU temp < 46C
    const int GpuThresholdC   = 46;   // first GPU tempUp (0x2E)
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

    // --- OEM fan table write (94 bytes — skips tempDown[0] for CPU and GPU) ---

    static void WriteOemTable()
    {
        for (int i = 0; i < 16; i++)
            WriteEC(CpuTempUpBase + i, CpuTempUp[i]);
        for (int i = 0; i < 16; i++)
            if (CpuTempDown[i] >= 0) WriteEC(CpuTempDownBase + i, CpuTempDown[i]);
        for (int i = 0; i < 16; i++)
            WriteEC(CpuDutyBase + i, CpuDuty[i]);
        for (int i = 0; i < 16; i++)
            WriteEC(GpuTempUpBase + i, GpuTempUp[i]);
        for (int i = 0; i < 16; i++)
            if (GpuTempDown[i] >= 0) WriteEC(GpuTempDownBase + i, GpuTempDown[i]);
        for (int i = 0; i < 16; i++)
            WriteEC(GpuDutyBase + i, GpuDuty[i]);
    }

    // --- Verify (94 bytes — skip tempDown[0]) ---

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

    static List<string> VerifyOemTable()
    {
        List<string> diffs = new List<string>();
        for (int i = 0; i < 16; i++)
            CheckByte(diffs, CpuTempUpBase + i, CpuTempUp[i], "cpuTempUp[" + i + "]");
        for (int i = 0; i < 16; i++)
            if (CpuTempDown[i] >= 0) CheckByte(diffs, CpuTempDownBase + i, CpuTempDown[i], "cpuTempDown[" + i + "]");
        for (int i = 0; i < 16; i++)
            CheckByte(diffs, CpuDutyBase + i, CpuDuty[i], "cpuDuty[" + i + "]");
        for (int i = 0; i < 16; i++)
            CheckByte(diffs, GpuTempUpBase + i, GpuTempUp[i], "gpuTempUp[" + i + "]");
        for (int i = 0; i < 16; i++)
            if (GpuTempDown[i] >= 0) CheckByte(diffs, GpuTempDownBase + i, GpuTempDown[i], "gpuTempDown[" + i + "]");
        for (int i = 0; i < 16; i++)
            CheckByte(diffs, GpuDutyBase + i, GpuDuty[i], "gpuDuty[" + i + "]");
        return diffs;
    }

    // --- Neutralize ---

    static void Neutralize()
    {
        int oem5 = ReadEC(Reg_FanCurveEnable);
        if (oem5 < 0) oem5 = 0;
        WriteEC(Reg_FanCurveEnable, oem5 & 0x7F);
        WriteEC(Reg_FanCurveApply, 0x00);
    }

    // --- Init register dump ---

    static void DumpInitRegs(string label)
    {
        Console.WriteLine("  " + label);
        int[] addrs = { 0x708, 0x709, 0x741, 0x748, 0x749, 0x74A, 0x74B, 0x74E, 0x766, 0x767, 0x7A6, 0x7C7 };
        string line = "   ";
        for (int i = 0; i < addrs.Length; i++)
        {
            int v = ReadEC(addrs[i]);
            line += " 0x" + addrs[i].ToString("X3") + "=0x" + v.ToString("X2");
            if (i % 4 == 3) { Console.WriteLine(line); line = "   "; }
        }
        if (line.Trim().Length > 0) Console.WriteLine(line);
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
        public int CpuFinal, GpuFinal;
        public bool CpuHit, GpuHit, Hit;
    }

    static bool InWindow(int value, int target, int tolerance)
    {
        int d = value - target;
        if (d < 0) d = -d;
        return d <= tolerance;
    }

    static Result RunCombination(string name, Action apply, bool doNeutralize)
    {
        Console.WriteLine();
        Console.WriteLine("=== " + name + " ===");

        DumpInitRegs("init register state (before test):");

        if (doNeutralize)
        {
            Console.WriteLine("  step 1: neutralize (0x7C5 bit7=0, 0x7C6=0), wait " + SettleWaitSec + "s...");
            Neutralize();
            Thread.Sleep(SettleWaitSec * 1000);
        }
        else
        {
            Console.WriteLine("  step 1: SKIPPED (no neutralize -- testing from current active state)");
        }

        int preCpu  = ReadDutyPct(Reg_CpuDuty);
        int preGpu  = ReadDutyPct(Reg_GpuDuty);
        int preOem5 = ReadEC(Reg_FanCurveEnable);
        int preOem6 = ReadEC(Reg_FanCurveApply);
        int pre741  = ReadEC(Reg_Activation);
        Console.WriteLine("  pre:  CPU=" + preCpu + "%  GPU=" + preGpu
            + "%  0x7C5=0x" + preOem5.ToString("X2")
            + "  0x7C6=0x" + preOem6.ToString("X2")
            + "  0x741=0x" + pre741.ToString("X2"));

        Console.WriteLine("  step 2: applying write sequence...");
        apply();

        Console.WriteLine("  step 2b: read-after-write verify...");
        int postOem5 = ReadEC(Reg_FanCurveEnable);
        int postOem6 = ReadEC(Reg_FanCurveApply);
        int post741  = ReadEC(Reg_Activation);
        string bit7  = ((postOem5 & 0x80) != 0) ? "set" : "CLEAR";
        string c6act = (postOem6 == 0x04) ? "active(0x04)" : "INACTIVE(0x" + postOem6.ToString("X2") + ")";
        Console.WriteLine("    0x7C5 = 0x" + postOem5.ToString("X2") + "  (master bit7 " + bit7 + ")");
        Console.WriteLine("    0x7C6 = 0x" + postOem6.ToString("X2") + "  " + c6act);
        Console.WriteLine("    0x741 = 0x" + post741.ToString("X2")
            + ((post741 & 0x01) != 0 ? "  (EC active)" : "  (NOT active)"));
        List<string> tableDiffs = VerifyOemTable();
        if (tableDiffs.Count == 0)
        {
            Console.WriteLine("    table: all 94 bytes match OEM layout (writes landed)");
        }
        else
        {
            Console.WriteLine("    table: " + tableDiffs.Count + " of 94 bytes MISMATCH:");
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

        // GPU target depends on temperature: 0% below first threshold, 55% above
        int gpuTarget = (p.LastGpuTemp < GpuThresholdC) ? 0 : GpuTargetDuty;

        Result r = new Result();
        r.Name = name;
        r.PreCpuDuty = preCpu;
        r.PreGpuDuty = preGpu;
        r.CpuFinal = p.CpuFinal;
        r.GpuFinal = p.GpuFinal;
        r.CpuHit = InWindow(p.CpuFinal, CpuTargetDuty, DutyTolerance);
        r.GpuHit = InWindow(p.GpuFinal, gpuTarget, DutyTolerance);
        r.Hit = r.CpuHit;  // CPU is primary signal

        int cpuDelta = p.CpuFinal - preCpu;
        int gpuDelta = p.GpuFinal - preGpu;
        string cpuDeltaStr = (cpuDelta >= 0 ? "+" : "") + cpuDelta;
        string gpuDeltaStr = (gpuDelta >= 0 ? "+" : "") + gpuDelta;

        Console.WriteLine("  CPU: pre=" + preCpu + "  final=" + p.CpuFinal + "  delta=" + cpuDeltaStr + "  target=" + CpuTargetDuty + "+-" + DutyTolerance + "  " + (r.CpuHit ? "HIT" : "miss"));
        Console.WriteLine("  GPU: pre=" + preGpu + "  final=" + p.GpuFinal + "  delta=" + gpuDeltaStr + "  target=" + gpuTarget + "+-" + DutyTolerance
            + (p.LastGpuTemp < GpuThresholdC ? " (temp " + p.LastGpuTemp + "C < " + GpuThresholdC + "C, expect duty[0]=0%)" : "")
            + "  " + (r.GpuHit ? "HIT" : "miss"));
        Console.WriteLine("  VERDICT: " + (r.Hit ? "HIT" : "MISS") + " (CPU is primary signal)");
        return r;
    }

    // =========================================================================
    // Full init + apply sequence (from GCUService startup capture)
    //
    // postActivationDelayMs: extra delay after Phase 1 activation.
    //   13a = 0ms (standard), 13b = 5000ms (timing test)
    // =========================================================================

    static void ApplyFullInit(int postActivationDelayMs)
    {
        // ========== PHASE 1: EC HANDSHAKE + ACTIVATION ==========
        Console.WriteLine("    [Phase 1 -- EC handshake + activation]");

        Console.WriteLine("    W 0x709 = 0x05  (handshake step 1)");
        WriteEC(Reg_Handshake1, 0x05);
        Console.WriteLine("    W 0x708 = 0x01  (handshake step 2)");
        WriteEC(Reg_Handshake2, 0x01);
        Thread.Sleep(150);

        Console.WriteLine("    W 0x741 = 0x05  (EC activation)");
        WriteEC(Reg_Activation, 0x05);
        Thread.Sleep(250);

        int appe = ReadEC(Reg_Activation);
        Console.WriteLine("    R 0x741 = 0x" + appe.ToString("X2")
            + ((appe & 0x01) != 0 ? "  (EC active)" : "  (pending)"));

        Console.WriteLine("    W 0x748 = 0x81  (feature enable)");
        WriteEC(Reg_FeatureEnable, 0x81);
        Thread.Sleep(150);

        if (postActivationDelayMs > 0)
        {
            Console.WriteLine("    sleep " + postActivationDelayMs + "ms (extended activation delay)");
            Thread.Sleep(postActivationDelayMs);
        }

        // ========== PHASE 2: MISSING INIT REGISTERS ==========
        Console.WriteLine("    [Phase 2 -- Init registers (missing from MechControl)]");

        int cur7A6 = ReadEC(Reg_BattMode);
        Console.WriteLine("    R 0x7A6 = 0x" + cur7A6.ToString("X2")
            + " -> W 0x" + cur7A6.ToString("X2") + "  (battery mode RMW)");
        WriteEC(Reg_BattMode, cur7A6);

        Console.WriteLine("    W 0x749 = 0x00  (cmd clear 1)");
        WriteEC(Reg_CmdClear1, 0x00);
        Console.WriteLine("    W 0x74A = 0x00  (cmd clear 2)");
        WriteEC(Reg_CmdClear2, 0x00);
        Console.WriteLine("    W 0x74B = 0x00  (cmd clear 3)");
        WriteEC(Reg_CmdClear3, 0x00);

        int cur766 = ReadEC(Reg_Support2);
        Console.WriteLine("    R 0x766 = 0x" + cur766.ToString("X2")
            + " -> W 0x" + cur766.ToString("X2") + "  (Support2 RMW)");
        WriteEC(Reg_Support2, cur766);

        Console.WriteLine("    W 0x767 = 0x00  (trigger clear)");
        WriteEC(Reg_Trigger, 0x00);
        Console.WriteLine("    W 0x74E = 0x00  (cmd clear 4)");
        WriteEC(Reg_CmdClear4, 0x00);

        Console.WriteLine("    sleep 200ms (let EC process init registers)");
        Thread.Sleep(200);

        // ========== PHASE 3: MODE PROFILE (X6FR5xxY SKU values) ==========
        Console.WriteLine("    [Phase 3 -- Mode profile (X6FR5xxY SKU values)]");

        int cur728 = ReadEC(0x728);
        Console.WriteLine("    R 0x728 = 0x" + cur728.ToString("X2")
            + " -> W 0x" + cur728.ToString("X2") + "  (RMW)");
        WriteEC(0x728, cur728);

        Console.WriteLine("    W 0x727 = 0x40  (ESHUTTER)");
        WriteEC(0x727, 0x40);
        Console.WriteLine("    W 0x751 = 0x00  (FAN_CTRL)");
        WriteEC(0x751, 0x00);
        Console.WriteLine("    W 0x726 = 0x80  (OEM9 + power mgmt bit)");
        WriteEC(0x726, 0x80);
        Console.WriteLine("    W 0x783 = 0x32  (PL1 = 50W)");
        WriteEC(0x783, 0x32);

        Console.WriteLine("    W 0x7C6 = 0x00  (clear apply latch)");
        WriteEC(Reg_FanCurveApply, 0x00);

        Console.WriteLine("    W 0x784 = 0x32  (PL2 = 50W)");
        WriteEC(0x784, 0x32);

        int cur7C5 = ReadEC(Reg_FanCurveEnable);
        Console.WriteLine("    R 0x7C5 = 0x" + cur7C5.ToString("X2")
            + " -> W 0x" + (cur7C5 | 0x80).ToString("X2") + "  (set master bit)");
        WriteEC(Reg_FanCurveEnable, cur7C5 | 0x80);

        Console.WriteLine("    W 0x785 = 0x3C  (PL4 = 60W)");
        WriteEC(0x785, 0x3C);
        Console.WriteLine("    W 0x786 = 0xDF  (TCC_OFFSET)");
        WriteEC(0x786, 0xDF);
        Console.WriteLine("    W 0x744 = 0x28  (TGP = 40W)");
        WriteEC(0x744, 0x28);
        Console.WriteLine("    W 0x745 = 0x00  (DB_TOTAL)");
        WriteEC(0x745, 0x00);
        Console.WriteLine("    W 0x746 = 0x00  (DB_MAX)");
        WriteEC(0x746, 0x00);

        int cur743 = ReadEC(0x743);
        Console.WriteLine("    R 0x743 = 0x" + cur743.ToString("X2")
            + " -> W 0x" + cur743.ToString("X2") + "  (DB_CTRL RMW touch)");
        WriteEC(0x743, cur743);

        Console.WriteLine("    W 0x787 = 0x00  (FAN_SWITCH_SPEED)");
        WriteEC(0x787, 0x00);

        // ========== PHASE 4: FAN TABLE PASS 1 (94 bytes, OEM layout) ==========
        Console.WriteLine("    [Phase 4 -- Fan table pass 1 (94 bytes, OEM layout)]");
        WriteOemTable();

        // ========== PHASE 5: FINALIZE + EDGE PULSE ==========
        Console.WriteLine("    [Phase 5 -- Finalize + edge pulse]");

        int cur7C7 = ReadEC(Reg_FanCurveParam);
        Console.WriteLine("    R 0x7C7 = 0x" + cur7C7.ToString("X2")
            + " -> W 0x" + cur7C7.ToString("X2") + "  (fan curve param RMW)");
        WriteEC(Reg_FanCurveParam, cur7C7);

        Console.WriteLine("    W 0x706 = 0x41  (BIOS_CTRL -- LATE, after table)");
        WriteEC(Reg_BiosCtrl, 0x41);

        Console.WriteLine("    W 0x7C6 = 0x04  (edge activate)");
        WriteEC(Reg_FanCurveApply, 0x04);
        Thread.Sleep(32);
        Console.WriteLine("    W 0x7C6 = 0x00  (edge release)");
        WriteEC(Reg_FanCurveApply, 0x00);

        int cur7C5b = ReadEC(Reg_FanCurveEnable);
        Console.WriteLine("    R 0x7C5 = 0x" + cur7C5b.ToString("X2")
            + " -> W 0x" + cur7C5b.ToString("X2") + "  (re-master RMW)");
        WriteEC(Reg_FanCurveEnable, cur7C5b);

        // ========== PHASE 6: SECOND PASS + HELD EDGE ==========
        Console.WriteLine("    [Phase 6 -- Second table pass + held edge]");
        WriteOemTable();
        Console.WriteLine("    W 0x7C6 = 0x04  (edge activate, HELD)");
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
            Console.WriteLine("Usage: fan_curve_probe13.exe <13|13a|13b|13c|--restore>");
            Console.WriteLine("  13         Run all three tests sequentially");
            Console.WriteLine("  13a        OEM table + standard timing");
            Console.WriteLine("  13b        OEM table + 5s activation delay");
            Console.WriteLine("  13c        OEM table + no neutralize (from active state)");
            Console.WriteLine("  --restore  Disable custom curve, exit");
            CloseHandle(hDev);
            return;
        }

        Console.WriteLine("============================================================");
        Console.WriteLine(" fan_curve_probe13 -- OEM table layout tests");
        Console.WriteLine("============================================================");
        Console.WriteLine(" OEM table: 94 bytes, tempDown[0] skipped, duty[0]=0x00");
        Console.WriteLine(" CPU duty=80% (0xA0)  GPU duty=55% (0x6E)");
        Console.WriteLine(" CPU target: 80%  GPU target: 55% (or 0% if GPU temp < 46C)");
        Console.WriteLine(" HIT criterion: |final - target| <= " + DutyTolerance + "  (CPU is primary)");
        Console.WriteLine("============================================================");

        bool runA = (mode == "13" || mode == "all" || mode == "13a");
        bool runB = (mode == "13" || mode == "all" || mode == "13b");
        bool runC = (mode == "13" || mode == "all" || mode == "13c");

        List<Result> results = new List<Result>();

        if (runA)
            results.Add(RunCombination("13a.oem-table", () => ApplyFullInit(0), true));

        if (runB)
            results.Add(RunCombination("13b.oem-table+5s-delay", () => ApplyFullInit(5000), true));

        // 13c: no neutralize — starts from whatever state 13b (or 13a) left.
        // If run after 13a/13b, EC should have 0x7C5=0x80, 0x7C6=0x04, 0x741=active.
        if (runC)
            results.Add(RunCombination("13c.oem-table+no-neutralize", () => ApplyFullInit(0), false));

        // Summary
        if (results.Count > 1)
        {
            Console.WriteLine();
            Console.WriteLine("=== SUMMARY ===");
            foreach (Result r in results)
            {
                Console.WriteLine("  " + r.Name.PadRight(38) + "  CPU " + r.CpuFinal.ToString().PadLeft(3) + "% " + (r.CpuHit ? "HIT " : "miss")
                    + "  GPU " + r.GpuFinal.ToString().PadLeft(3) + "% " + (r.GpuHit ? "HIT " : "miss")
                    + "  => " + (r.Hit ? "HIT" : "MISS"));
            }
        }

        Console.WriteLine();
        Console.WriteLine("Restoring neutral state before exit...");
        Neutralize();

        CloseHandle(hDev);
    }
}
