// fan_curve_probe15.cs
//
// Sequential fan table modification tests.  Each test:
//   1. Neutralize (clear custom mode, wait 8 s for EC to settle)
//   2. MechControl 7-step init (EcActivationControl.InitializeEc)
//   3. SetCustomFanCurveMode(true)  (0x7C5 bit 7 + 0x7C6=0x04)
//   4. Write a surgically-modified fan table
//   5. Read-after-write verify
//   6. Poll 15 s
//   7. HIT / MISS verdict
//
// Tests:
//   0  Baseline    — exact OEM table (same as probe14, confirms init works)
//   A  duty[0]!=0  — duty[0] = 0xA0 (80%) instead of 0x00
//   B  8C spacing  — tempUp 8C apart (was 4C), 2C hysteresis kept
//   C  last-pt     — shift last active point (9) by +2C (active/padding boundary)
//   D  6C/4C hyst  — 6C spacing + 4C hysteresis (avoids overlap from wider hyst)
//
// Build:
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe -nologo fan_curve_probe15.cs
//
// Run (elevated):
//   fan_curve_probe15.exe              - run ALL tests in sequence
//   fan_curve_probe15.exe 0            - baseline only
//   fan_curve_probe15.exe a            - test A only
//   fan_curve_probe15.exe b            - test B only
//   fan_curve_probe15.exe c            - test C only
//   fan_curve_probe15.exe d            - test D only
//   fan_curve_probe15.exe --restore    - disable custom mode, exit

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
    const int Reg_Handshake1     = 0x709;
    const int Reg_Handshake2     = 0x708;
    const int Reg_Activation     = 0x741;
    const int Reg_FeatureEnable  = 0x748;
    const int Reg_PowerMgmtByte9 = 0x726;
    const int Reg_PowerMgmtCume  = 0x727;
    const int Reg_FanCurveEnable = 0x7C5;
    const int Reg_FanCurveApply  = 0x7C6;
    const int Reg_FanCurveParam  = 0x7C7;
    const int Reg_BiosCtrl       = 0x706;
    const int Reg_CpuDuty = 0x75B;
    const int Reg_GpuDuty = 0x75C;
    const int Reg_CpuTemp = 0x43E;
    const int Reg_GpuTemp = 0x44F;

    // Fan table region bases
    const int CpuTempUpBase   = 0xF00;
    const int CpuTempDownBase = 0xF10;
    const int CpuDutyBase     = 0xF20;
    const int GpuTempUpBase   = 0xF30;
    const int GpuTempDownBase = 0xF40;
    const int GpuDutyBase     = 0xF50;

    // Tuning
    const int TargetDutyPct    = 80;
    const int TargetDutyRaw    = 0xA0;  // 80%
    const int DutyTolerance    = 5;
    const int PollDurationSec  = 15;
    const int SettleWaitSec    = 8;
    const int FinalSampleCount = 6;

    // =====================================================================
    //  BASELINE OEM TABLE  (bit-for-bit from Frida capture, proven in probe13)
    // =====================================================================

    //                                      0     1     2     3     4     5     6     7     8     9    10    11    12    13    14    15
    static readonly int[] OemCpuTempUp   = { 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x55, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static readonly int[] OemCpuTempDown = {   -1, 0x2E, 0x32, 0x36, 0x3A, 0x3E, 0x42, 0x46, 0x4A, 0x4E, 0x52, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static readonly int[] OemCpuDuty     = { 0x00, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw };

    static readonly int[] OemGpuTempUp   = { 0x2E, 0x32, 0x36, 0x3A, 0x3E, 0x42, 0x46, 0x4A, 0x4E, 0x51, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static readonly int[] OemGpuTempDown = {   -1, 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static readonly int[] OemGpuDuty     = { 0x00, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw };

    // =====================================================================
    //  EC IOCTL helpers
    // =====================================================================

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
        return raw < 0 ? 0 : raw / 2;
    }

    static int[] Clone(int[] src) { return (int[])src.Clone(); }

    // =====================================================================
    //  Table write + verify  (94 bytes — tempDown[0] always skipped)
    // =====================================================================

    static void WriteTable(int[] cpuTU, int[] cpuTD, int[] cpuD,
                           int[] gpuTU, int[] gpuTD, int[] gpuD)
    {
        for (int i = 0; i < 16; i++) WriteEC(CpuTempUpBase + i, cpuTU[i]);
        for (int i = 0; i < 16; i++) if (cpuTD[i] >= 0) WriteEC(CpuTempDownBase + i, cpuTD[i]);
        for (int i = 0; i < 16; i++) WriteEC(CpuDutyBase + i, cpuD[i]);
        for (int i = 0; i < 16; i++) WriteEC(GpuTempUpBase + i, gpuTU[i]);
        for (int i = 0; i < 16; i++) if (gpuTD[i] >= 0) WriteEC(GpuTempDownBase + i, gpuTD[i]);
        for (int i = 0; i < 16; i++) WriteEC(GpuDutyBase + i, gpuD[i]);
    }

    static List<string> VerifyTable(int[] cpuTU, int[] cpuTD, int[] cpuD,
                                    int[] gpuTU, int[] gpuTD, int[] gpuD)
    {
        List<string> diffs = new List<string>();
        for (int i = 0; i < 16; i++) ChkB(diffs, CpuTempUpBase + i, cpuTU[i], "cpuTU[" + i + "]");
        for (int i = 0; i < 16; i++) if (cpuTD[i] >= 0) ChkB(diffs, CpuTempDownBase + i, cpuTD[i], "cpuTD[" + i + "]");
        for (int i = 0; i < 16; i++) ChkB(diffs, CpuDutyBase + i, cpuD[i], "cpuD[" + i + "]");
        for (int i = 0; i < 16; i++) ChkB(diffs, GpuTempUpBase + i, gpuTU[i], "gpuTU[" + i + "]");
        for (int i = 0; i < 16; i++) if (gpuTD[i] >= 0) ChkB(diffs, GpuTempDownBase + i, gpuTD[i], "gpuTD[" + i + "]");
        for (int i = 0; i < 16; i++) ChkB(diffs, GpuDutyBase + i, gpuD[i], "gpuD[" + i + "]");
        return diffs;
    }

    static void ChkB(List<string> diffs, int addr, int expected, string label)
    {
        int actual = ReadEC(addr);
        if (actual != expected)
            diffs.Add("    [0x" + addr.ToString("X3") + "] " + label
                + " exp 0x" + expected.ToString("X2") + " got 0x" + actual.ToString("X2"));
    }

    // =====================================================================
    //  Neutralize
    // =====================================================================

    static void Neutralize()
    {
        int oem5 = ReadEC(Reg_FanCurveEnable);
        if (oem5 < 0) oem5 = 0;
        WriteEC(Reg_FanCurveEnable, oem5 & 0x7F);
        WriteEC(Reg_FanCurveApply, 0x00);
    }

    // =====================================================================
    //  MechControl init (Phase A — EcActivationControl.InitializeEc, 7 steps)
    // =====================================================================

    static void MechControlInit()
    {
        // Step 1: Handshake
        WriteEC(Reg_Handshake1, 0x05);
        WriteEC(Reg_Handshake2, 0x01);
        Thread.Sleep(150);

        // Step 2: Activation
        WriteEC(Reg_Activation, 0x05);
        Thread.Sleep(150);

        // Step 3: Feature enable
        WriteEC(Reg_FeatureEnable, 0x81);
        Thread.Sleep(150);

        // Step 4: Power management (RMW)
        int b9 = ReadEC(Reg_PowerMgmtByte9);
        int cume = ReadEC(Reg_PowerMgmtCume);
        WriteEC(Reg_PowerMgmtByte9, b9 | 0x80);
        WriteEC(Reg_PowerMgmtCume, cume | 0x40);
        Thread.Sleep(150);

        // Step 5: Fan curve init bits 5+6, preserve bit 7
        int c5 = ReadEC(Reg_FanCurveEnable);
        WriteEC(Reg_FanCurveEnable, c5 | 0x60);
        Thread.Sleep(150);

        // Step 6: Fan curve param
        WriteEC(Reg_FanCurveParam, 0x0C);
        Thread.Sleep(150);

        // Step 7: BIOS_CTRL init complete
        WriteEC(Reg_BiosCtrl, 0x41);
        Thread.Sleep(150);
    }

    // =====================================================================
    //  SetCustomFanCurveMode(true) — Phase B
    // =====================================================================

    static void SetCustomMode()
    {
        int c5 = ReadEC(Reg_FanCurveEnable);
        WriteEC(Reg_FanCurveEnable, (c5 & ~0x80) | 0x80);
        WriteEC(Reg_FanCurveApply, 0x04);
    }

    // =====================================================================
    //  Poll loop
    // =====================================================================

    static void PollFor(int durationSec, out int cpuFinal, out int gpuFinal,
                        out int lastCpuTemp, out int lastGpuTemp)
    {
        int iterations = durationSec * 2;
        List<int> cpuS = new List<int>();
        List<int> gpuS = new List<int>();
        lastCpuTemp = 0; lastGpuTemp = 0;

        for (int i = 0; i < iterations; i++)
        {
            int cd = ReadDutyPct(Reg_CpuDuty);
            int gd = ReadDutyPct(Reg_GpuDuty);
            int ct = ReadEC(Reg_CpuTemp);
            int gt = ReadEC(Reg_GpuTemp);
            int o5 = ReadEC(Reg_FanCurveEnable);
            int o6 = ReadEC(Reg_FanCurveApply);
            cpuS.Add(cd); gpuS.Add(gd);
            lastCpuTemp = ct; lastGpuTemp = gt;

            bool fin = (i >= iterations - FinalSampleCount);
            Console.WriteLine("    +" + ((i + 1) * 0.5).ToString("F1").PadLeft(4) + "s"
                + (fin ? " *" : "  ")
                + "CPU " + cd.ToString().PadLeft(3) + "% (" + ct.ToString().PadLeft(2) + "C)"
                + "  |  GPU " + gd.ToString().PadLeft(3) + "% (" + gt.ToString().PadLeft(2) + "C)"
                + "  |  0x7C5=0x" + o5.ToString("X2") + " 0x7C6=0x" + o6.ToString("X2"));
            Thread.Sleep(500);
        }

        int start = Math.Max(0, cpuS.Count - FinalSampleCount);
        int cs = 0, gs = 0, n = 0;
        for (int i = start; i < cpuS.Count; i++) { cs += cpuS[i]; gs += gpuS[i]; n++; }
        cpuFinal = n > 0 ? cs / n : 0;
        gpuFinal = n > 0 ? gs / n : 0;
    }

    static bool InWindow(int val, int target, int tol)
    {
        int d = val - target; if (d < 0) d = -d;
        return d <= tol;
    }

    // =====================================================================
    //  Hex-dump a 16-element array for display
    // =====================================================================

    static string HexRow(int[] arr)
    {
        string s = "";
        for (int i = 0; i < arr.Length; i++)
        {
            if (i > 0) s += " ";
            if (arr[i] < 0) s += "??";
            else s += arr[i].ToString("X2");
        }
        return s;
    }

    // =====================================================================
    //  Test runner
    // =====================================================================

    struct Result
    {
        public string Name;
        public int CpuFinal, GpuFinal;
        public bool CpuHit, GpuHit;
    }

    static Result RunTest(string name, string desc, string[] changes,
                          int[] cpuTU, int[] cpuTD, int[] cpuD,
                          int[] gpuTU, int[] gpuTD, int[] gpuD,
                          int cpuThreshC, int gpuThreshC)
    {
        Console.WriteLine();
        Console.WriteLine("================================================================");
        Console.WriteLine("  TEST " + name + ": " + desc);
        Console.WriteLine("================================================================");
        if (changes.Length > 0)
        {
            Console.WriteLine("  Changes from OEM baseline:");
            foreach (string c in changes) Console.WriteLine("    - " + c);
        }
        else
        {
            Console.WriteLine("  (no changes — exact OEM table)");
        }

        // Show table being written
        Console.WriteLine("  CPU TU:  " + HexRow(cpuTU));
        Console.WriteLine("  CPU TD:  " + HexRow(cpuTD));
        Console.WriteLine("  CPU D :  " + HexRow(cpuD));
        Console.WriteLine("  GPU TU:  " + HexRow(gpuTU));
        Console.WriteLine("  GPU TD:  " + HexRow(gpuTD));
        Console.WriteLine("  GPU D :  " + HexRow(gpuD));

        // Pre state
        int preCpu = ReadDutyPct(Reg_CpuDuty);
        int preGpu = ReadDutyPct(Reg_GpuDuty);
        int pre5   = ReadEC(Reg_FanCurveEnable);
        int pre6   = ReadEC(Reg_FanCurveApply);
        Console.WriteLine("  pre: CPU=" + preCpu + "%  GPU=" + preGpu
            + "%  0x7C5=0x" + pre5.ToString("X2") + "  0x7C6=0x" + pre6.ToString("X2"));

        // Neutralize
        Console.WriteLine("  neutralize + wait " + SettleWaitSec + "s...");
        Neutralize();
        Thread.Sleep(SettleWaitSec * 1000);

        int midCpu = ReadDutyPct(Reg_CpuDuty);
        int mid5   = ReadEC(Reg_FanCurveEnable);
        Console.WriteLine("  settled: CPU=" + midCpu + "%  0x7C5=0x" + mid5.ToString("X2"));

        // Phase A: MechControl init
        Console.WriteLine("  Phase A: MechControl 7-step init...");
        MechControlInit();
        int post741 = ReadEC(Reg_Activation);
        Console.WriteLine("    0x741=0x" + post741.ToString("X2")
            + ((post741 & 0x01) != 0 ? " (EC active)" : " (NOT active)"));

        // Phase B: SetCustomFanCurveMode(true)
        Console.WriteLine("  Phase B: SetCustomFanCurveMode(true)...");
        SetCustomMode();

        // Phase C: Write table
        Console.WriteLine("  Phase C: Write fan table...");
        WriteTable(cpuTU, cpuTD, cpuD, gpuTU, gpuTD, gpuD);

        // Verify
        int v5 = ReadEC(Reg_FanCurveEnable);
        int v6 = ReadEC(Reg_FanCurveApply);
        Console.WriteLine("  verify: 0x7C5=0x" + v5.ToString("X2")
            + " (bit7 " + ((v5 & 0x80) != 0 ? "set" : "CLEAR") + ")"
            + "  0x7C6=0x" + v6.ToString("X2")
            + " (" + (v6 == 0x04 ? "active" : "INACTIVE") + ")");

        List<string> diffs = VerifyTable(cpuTU, cpuTD, cpuD, gpuTU, gpuTD, gpuD);
        if (diffs.Count == 0)
            Console.WriteLine("  verify: all bytes match (writes landed)");
        else
        {
            Console.WriteLine("  verify: " + diffs.Count + " MISMATCH:");
            int shown = 0;
            foreach (string d in diffs)
            {
                Console.WriteLine(d);
                if (++shown >= 10) { Console.WriteLine("    ...(" + (diffs.Count - shown) + " more)"); break; }
            }
        }

        // Poll
        Console.WriteLine("  poll " + PollDurationSec + "s (last " + FinalSampleCount + " = final)...");
        int cpuFinal, gpuFinal, lastCT, lastGT;
        PollFor(PollDurationSec, out cpuFinal, out gpuFinal, out lastCT, out lastGT);

        // Verdict
        int gpuTarget = (lastGT < gpuThreshC) ? 0 : TargetDutyPct;
        bool cpuHit = InWindow(cpuFinal, TargetDutyPct, DutyTolerance);
        bool gpuHit = InWindow(gpuFinal, gpuTarget, DutyTolerance);

        Console.WriteLine("  CPU: final=" + cpuFinal + "%  target=" + TargetDutyPct + "+-" + DutyTolerance + "  " + (cpuHit ? "HIT" : "MISS"));
        Console.WriteLine("  GPU: final=" + gpuFinal + "%  target=" + gpuTarget + "+-" + DutyTolerance
            + (lastGT < gpuThreshC ? " (temp " + lastGT + "C < thresh " + gpuThreshC + "C, expect 0%)" : "")
            + "  " + (gpuHit ? "HIT" : "MISS"));
        Console.WriteLine("  RESULT: " + (cpuHit ? ">>> HIT <<<" : "*** MISS ***") + "  (CPU primary)");

        Result r = new Result();
        r.Name = name; r.CpuFinal = cpuFinal; r.GpuFinal = gpuFinal;
        r.CpuHit = cpuHit; r.GpuHit = gpuHit;
        return r;
    }

    // =====================================================================
    //  Table builders for each test
    // =====================================================================

    // --- Test A: duty[0] = 0xA0 instead of 0x00 ---
    static void BuildTestA(out int[] cpuTU, out int[] cpuTD, out int[] cpuD,
                           out int[] gpuTU, out int[] gpuTD, out int[] gpuD)
    {
        cpuTU = Clone(OemCpuTempUp);   cpuTD = Clone(OemCpuTempDown); cpuD = Clone(OemCpuDuty);
        gpuTU = Clone(OemGpuTempUp);   gpuTD = Clone(OemGpuTempDown); gpuD = Clone(OemGpuDuty);
        cpuD[0] = TargetDutyRaw;  // 0xA0 instead of 0x00
        gpuD[0] = TargetDutyRaw;
    }

    // --- Test B: 8C spacing, 2C hysteresis ---
    //
    // CPU: 30, 38, 46, 54, 62, 70, 78, 86, 94, 102  (8C steps from 30C)
    // GPU: 28, 36, 44, 52, 60, 68, 76, 84, 92, 100  (8C steps from 28C)
    // tempDown[i] = tempUp[i-1] - 2   (same 2C hysteresis as OEM)
    // First padding tempDown = last active tempDown + 4
    //
    static void BuildTestB(out int[] cpuTU, out int[] cpuTD, out int[] cpuD,
                           out int[] gpuTU, out int[] gpuTD, out int[] gpuD)
    {
        cpuD = Clone(OemCpuDuty);  // duty unchanged (0x00 then 0xA0 x15)
        gpuD = Clone(OemGpuDuty);

        // CPU: start 30C (0x1E), step 8C
        cpuTU = new int[16]; cpuTD = new int[16];
        int cpuStart = 0x1E;
        for (int i = 0; i < 10; i++) cpuTU[i] = cpuStart + i * 8;
        for (int i = 10; i < 16; i++) cpuTU[i] = 0xFF;
        cpuTD[0] = -1;  // skip
        for (int i = 1; i < 10; i++) cpuTD[i] = cpuTU[i - 1] - 2;
        cpuTD[10] = cpuTD[9] + 4;  // first padding tempDown
        for (int i = 11; i < 16; i++) cpuTD[i] = 0xFF;

        // GPU: start 28C (0x1C), step 8C
        gpuTU = new int[16]; gpuTD = new int[16];
        int gpuStart = 0x1C;
        for (int i = 0; i < 10; i++) gpuTU[i] = gpuStart + i * 8;
        for (int i = 10; i < 16; i++) gpuTU[i] = 0xFF;
        gpuTD[0] = -1;  // skip
        for (int i = 1; i < 10; i++) gpuTD[i] = gpuTU[i - 1] - 2;
        gpuTD[10] = gpuTD[9] + 4;
        for (int i = 11; i < 16; i++) gpuTD[i] = 0xFF;
    }

    // --- Test C: shift last active point (index 9) by +2C ---
    //
    // Tests the active/padding boundary.  Only tempUp[9] changes;
    // nothing downstream depends on it (tempDown[10] = tempDown[9]+4).
    //
    // CPU: tempUp[9] 0x55 -> 0x57  (85C -> 87C)
    // GPU: tempUp[9] 0x51 -> 0x53  (81C -> 83C)
    //
    static void BuildTestC(out int[] cpuTU, out int[] cpuTD, out int[] cpuD,
                           out int[] gpuTU, out int[] gpuTD, out int[] gpuD)
    {
        cpuTU = Clone(OemCpuTempUp);   cpuTD = Clone(OemCpuTempDown); cpuD = Clone(OemCpuDuty);
        gpuTU = Clone(OemGpuTempUp);   gpuTD = Clone(OemGpuTempDown); gpuD = Clone(OemGpuDuty);

        cpuTU[9] = 0x57;      // was 0x55 (85C -> 87C)
        gpuTU[9] = 0x53;      // was 0x51 (81C -> 83C)
    }

    // --- Test D: 6C spacing + 4C hysteresis ---
    //
    // 4C hysteresis with OEM 4C spacing would overlap (tempDown[i] =
    // tempUp[i-2]).  Widen to 6C spacing so there's 2C clearance:
    //   tempDown[i] = tempUp[i-1] - 4,  tempUp spacing = 6
    //   => tempDown[i] = tempUp[i-2] + 2  (no overlap)
    //
    // CPU: 48, 54, 60, 66, 72, 78, 84, 90, 96, 102  (6C steps from 48C)
    // GPU: 46, 52, 58, 64, 70, 76, 82, 88, 94, 100  (6C steps from 46C)
    //
    static void BuildTestD(out int[] cpuTU, out int[] cpuTD, out int[] cpuD,
                           out int[] gpuTU, out int[] gpuTD, out int[] gpuD)
    {
        cpuD = Clone(OemCpuDuty);
        gpuD = Clone(OemGpuDuty);

        // CPU: start 48C (0x30), step 6C
        cpuTU = new int[16]; cpuTD = new int[16];
        int cpuStart = 0x30;
        for (int i = 0; i < 10; i++) cpuTU[i] = cpuStart + i * 6;
        for (int i = 10; i < 16; i++) cpuTU[i] = 0xFF;
        cpuTD[0] = -1;  // skip
        for (int i = 1; i < 10; i++) cpuTD[i] = cpuTU[i - 1] - 4;  // 4C hysteresis
        cpuTD[10] = cpuTD[9] + 4;  // first padding
        for (int i = 11; i < 16; i++) cpuTD[i] = 0xFF;

        // GPU: start 46C (0x2E), step 6C
        gpuTU = new int[16]; gpuTD = new int[16];
        int gpuStart = 0x2E;
        for (int i = 0; i < 10; i++) gpuTU[i] = gpuStart + i * 6;
        for (int i = 10; i < 16; i++) gpuTU[i] = 0xFF;
        gpuTD[0] = -1;  // skip
        for (int i = 1; i < 10; i++) gpuTD[i] = gpuTU[i - 1] - 4;
        gpuTD[10] = gpuTD[9] + 4;
        for (int i = 11; i < 16; i++) gpuTD[i] = 0xFF;
    }

    // =====================================================================
    //  Main
    // =====================================================================

    static void Main(string[] args)
    {
        hDev = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (hDev == new IntPtr(-1))
        {
            Console.WriteLine("FAILED to open \\\\.\\ACPIDriver - run as Administrator.");
            return;
        }

        string mode = args.Length > 0 ? args[0].ToLowerInvariant() : "all";

        if (mode == "--restore" || mode == "restore")
        {
            Console.WriteLine("Restoring EC to neutral...");
            Neutralize();
            int o5 = ReadEC(Reg_FanCurveEnable);
            int o6 = ReadEC(Reg_FanCurveApply);
            Console.WriteLine("Done. 0x7C5=0x" + o5.ToString("X2") + "  0x7C6=0x" + o6.ToString("X2"));
            CloseHandle(hDev);
            return;
        }

        if (mode == "help" || mode == "--help" || mode == "-h")
        {
            Console.WriteLine("Usage: fan_curve_probe15.exe [all|0|a|b|c|d|--restore]");
            Console.WriteLine("  all        Run all tests in sequence (default)");
            Console.WriteLine("  0          Baseline only (exact OEM table)");
            Console.WriteLine("  a          duty[0] = 0xA0 (non-zero first point)");
            Console.WriteLine("  b          8C spacing (was 4C), 2C hysteresis");
            Console.WriteLine("  c          Last active point (9) shifted +2C");
            Console.WriteLine("  d          6C spacing + 4C hysteresis (was 4C/2C)");
            Console.WriteLine("  --restore  Disable custom mode, exit");
            CloseHandle(hDev);
            return;
        }

        bool runAll = (mode == "all");

        Console.WriteLine("============================================================");
        Console.WriteLine(" fan_curve_probe15 — fan table modification tests");
        Console.WriteLine("============================================================");
        Console.WriteLine(" Init: MechControl 7-step + SetCustomFanCurveMode(true)");
        Console.WriteLine(" Each test: neutralize -> init -> write table -> poll 15s");
        Console.WriteLine(" Target CPU duty: " + TargetDutyPct + "% (0x" + TargetDutyRaw.ToString("X2") + ")  tolerance: +-" + DutyTolerance + "%");
        Console.WriteLine("============================================================");

        List<Result> results = new List<Result>();

        // ---- Test 0: Baseline ----
        if (runAll || mode == "0")
        {
            results.Add(RunTest("0", "Baseline — exact OEM table", new string[0],
                OemCpuTempUp, OemCpuTempDown, OemCpuDuty,
                OemGpuTempUp, OemGpuTempDown, OemGpuDuty,
                48, 46));
        }

        // ---- Test A: duty[0] != 0 ----
        if (runAll || mode == "a")
        {
            int[] aCTU, aCTD, aCD, aGTU, aGTD, aGD;
            BuildTestA(out aCTU, out aCTD, out aCD, out aGTU, out aGTD, out aGD);
            results.Add(RunTest("A", "duty[0] = 0xA0 (non-zero first point)",
                new string[] { "cpuDuty[0]: 0x00 -> 0xA0", "gpuDuty[0]: 0x00 -> 0xA0" },
                aCTU, aCTD, aCD, aGTU, aGTD, aGD,
                48, 46));
        }

        // ---- Test B: 8C spacing ----
        if (runAll || mode == "b")
        {
            int[] bCTU, bCTD, bCD, bGTU, bGTD, bGD;
            BuildTestB(out bCTU, out bCTD, out bCD, out bGTU, out bGTD, out bGD);
            results.Add(RunTest("B", "8C spacing (was 4C), 2C hysteresis",
                new string[] {
                    "CPU tempUp: 30,38,46,54,62,70,78,86,94,102 (8C steps from 30C)",
                    "GPU tempUp: 28,36,44,52,60,68,76,84,92,100 (8C steps from 28C)",
                    "tempDown[i] = tempUp[i-1] - 2 (hysteresis unchanged)"
                },
                bCTU, bCTD, bCD, bGTU, bGTD, bGD,
                30, 28));
        }

        // ---- Test C: single point shift ----
        if (runAll || mode == "c")
        {
            int[] cCTU, cCTD, cCD, cGTU, cGTD, cGD;
            BuildTestC(out cCTU, out cCTD, out cCD, out cGTU, out cGTD, out cGD);
            results.Add(RunTest("C", "Last active point (9) shifted +2C",
                new string[] {
                    "CPU tempUp[9]: 0x55 -> 0x57 (85C -> 87C)",
                    "GPU tempUp[9]: 0x51 -> 0x53 (81C -> 83C)",
                    "Tests active/padding boundary — nothing else changes"
                },
                cCTU, cCTD, cCD, cGTU, cGTD, cGD,
                48, 46));
        }

        // ---- Test D: 4C hysteresis ----
        if (runAll || mode == "d")
        {
            int[] dCTU, dCTD, dCD, dGTU, dGTD, dGD;
            BuildTestD(out dCTU, out dCTD, out dCD, out dGTU, out dGTD, out dGD);
            results.Add(RunTest("D", "6C spacing + 4C hysteresis (was 4C/2C)",
                new string[] {
                    "tempUp: 6C steps (was 4C) — avoids hysteresis overlap",
                    "tempDown[i] = tempUp[i-1] - 4 (was -2)",
                    "CPU: 48,54,60,66,72,78,84,90,96,102  GPU: 46,52,58,...,100"
                },
                dCTU, dCTD, dCD, dGTU, dGTD, dGD,
                48, 46));
        }

        // =====================================================================
        //  Summary
        // =====================================================================
        Console.WriteLine();
        Console.WriteLine("================================================================");
        Console.WriteLine("  SUMMARY");
        Console.WriteLine("================================================================");
        foreach (Result r in results)
        {
            Console.WriteLine("  Test " + r.Name.PadRight(2)
                + "  CPU " + r.CpuFinal.ToString().PadLeft(3) + "% " + (r.CpuHit ? "HIT " : "MISS")
                + "  GPU " + r.GpuFinal.ToString().PadLeft(3) + "% " + (r.GpuHit ? "HIT " : "MISS")
                + "  => " + (r.CpuHit ? "HIT" : "MISS"));
        }
        Console.WriteLine();

        // Final restore
        Console.WriteLine("Restoring neutral state before exit...");
        Neutralize();

        CloseHandle(hDev);
    }
}
