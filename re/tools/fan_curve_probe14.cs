// fan_curve_probe14.cs
//
// TEST 1: MechControl-only init + OEM fan table format
//
// Purpose: Determine if MechControl's existing 7-step EC init
// (EcActivationControl.InitializeEc) plus SetCustomFanCurveMode(true)
// plus the proven OEM fan table is sufficient for fan curves to take
// effect. No OEM mode profile, no edge pulse, no Phase 2/3/5/6 —
// just what MechControl actually does today.
//
// MechControl init sequence (from EcActivationControl.cs):
//   1. W 0x709=0x05, W 0x708=0x01   (handshake)
//   2. W 0x741=0x05                  (activation)
//   3. W 0x748=0x81                  (feature enable)
//   4. R 0x726 |= 0x80, R 0x727 |= 0x40  (power mgmt)
//   5. R 0x7C5 |= 0x60              (fan curve init — bits 5+6 only)
//   6. W 0x7C7=0x0C                 (fan curve param)
//   7. W 0x706=0x41                 (BIOS_CTRL init complete)
//
// Then simulating the user enabling custom curves + applying:
//   8. R 0x7C5 -> set bit 7          (SetCustomFanCurveMode)
//   9. W 0x7C6=0x04                  (SetCustomFanCurveMode)
//  10. Write OEM fan table (94 bytes) (ApplyCpuCurve + ApplyGpuCurve)
//
// Fan table is bit-for-bit identical to probe13's proven OEM table:
//   0xF00: 30 34 38 3C 40 44 48 4C 50 55 FF FF FF FF FF FF   CPU TEMP_UP
//   0xF10: ?? 2E 32 36 3A 3E 42 46 4A 4E 52 FF FF FF FF FF   CPU TEMP_DN (?? = not written)
//   0xF20: 00 A0 A0 A0 A0 A0 A0 A0 A0 A0 A0 A0 A0 A0 A0 A0   CPU DUTY
//   0xF30: 2E 32 36 3A 3E 42 46 4A 4E 51 FF FF FF FF FF FF   GPU TEMP_UP
//   0xF40: ?? 2C 30 34 38 3C 40 44 48 4C 50 FF FF FF FF FF   GPU TEMP_DN (?? = not written)
//   0xF50: 00 A0 A0 A0 A0 A0 A0 A0 A0 A0 A0 A0 A0 A0 A0 A0   GPU DUTY
//
// Build:
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe -nologo fan_curve_probe14.cs
//
// Run (elevated):
//   fan_curve_probe14.exe              - run test
//   fan_curve_probe14.exe --restore    - disable custom mode, return EC to neutral

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

    // MechControl init registers (EcActivationControl.cs)
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
    // OEM-exact fan table (bit-for-bit from probe13's proven working table)
    //
    // 10 active points, 4C steps, 2C hysteresis. duty[0] = 0x00.
    // tempDown[0] NOT written (94 bytes total).
    // CPU duty = 80% (0xA0), GPU duty = 80% (0xA0).
    // =========================================================================

    const int TargetDutyRaw = 0xA0;  // 80%

    //                                  act0  act1  act2  act3  act4  act5  act6  act7  act8  act9  pad   pad   pad   pad   pad   pad
    static readonly int[] CpuTempUp   = { 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x55, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static readonly int[] CpuTempDown = {   -1, 0x2E, 0x32, 0x36, 0x3A, 0x3E, 0x42, 0x46, 0x4A, 0x4E, 0x52, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };  // [0] = -1 = SKIP
    static readonly int[] CpuDuty     = { 0x00, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw };

    static readonly int[] GpuTempUp   = { 0x2E, 0x32, 0x36, 0x3A, 0x3E, 0x42, 0x46, 0x4A, 0x4E, 0x51, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static readonly int[] GpuTempDown = {   -1, 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };  // [0] = -1 = SKIP
    static readonly int[] GpuDuty     = { 0x00, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw, TargetDutyRaw };

    const int CpuTargetDuty   = 80;
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

    // --- Poll loop (reused from probe13) ---

    static void PollFor(int durationSec, out int cpuFinal, out int gpuFinal,
                        out int lastCpuTemp, out int lastGpuTemp)
    {
        int iterations = durationSec * 2;
        List<int> cpuSamples = new List<int>();
        List<int> gpuSamples = new List<int>();
        lastCpuTemp = 0;
        lastGpuTemp = 0;

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

            bool inFinalWindow = (i >= iterations - FinalSampleCount);
            string mark = inFinalWindow ? " *" : "  ";

            Console.WriteLine("    +" + ((i + 1) * 0.5).ToString("F1").PadLeft(4) + "s" + mark
                + "CPU " + cpuDuty.ToString().PadLeft(3) + "% (" + cpuTemp.ToString().PadLeft(2) + "C)"
                + "  |  GPU " + gpuDuty.ToString().PadLeft(3) + "% (" + gpuTemp.ToString().PadLeft(2) + "C)"
                + "  |  0x7C5=0x" + oem5.ToString("X2") + " 0x7C6=0x" + oem6.ToString("X2"));

            Thread.Sleep(500);
        }

        int start = Math.Max(0, cpuSamples.Count - FinalSampleCount);
        int cpuSum = 0, gpuSum = 0, n = 0;
        for (int i = start; i < cpuSamples.Count; i++)
        {
            cpuSum += cpuSamples[i];
            gpuSum += gpuSamples[i];
            n++;
        }
        cpuFinal = n > 0 ? cpuSum / n : 0;
        gpuFinal = n > 0 ? gpuSum / n : 0;
    }

    static bool InWindow(int value, int target, int tolerance)
    {
        int d = value - target;
        if (d < 0) d = -d;
        return d <= tolerance;
    }

    // =========================================================================
    // MechControl-only init + apply sequence
    //
    // This is EXACTLY what MechControl does — no more, no less:
    //   Phase A: EcActivationControl.InitializeEc() (7 steps)
    //   Phase B: FanControl.SetCustomFanCurveMode(true)
    //   Phase C: FanControl.ApplyCpuCurve + ApplyGpuCurve (OEM table format)
    // =========================================================================

    static void ApplyMechControlSequence()
    {
        // ========== PHASE A: EcActivationControl.InitializeEc() ==========
        // 7-step init, 150ms between each step (matches MechControl)

        Console.WriteLine("    [Phase A -- EcActivationControl.InitializeEc() — 7 steps]");

        // Step 1: Handshake
        Console.WriteLine("    step 1: W 0x709=0x05, W 0x708=0x01  (handshake)");
        WriteEC(Reg_Handshake1, 0x05);
        WriteEC(Reg_Handshake2, 0x01);
        Thread.Sleep(150);

        // Step 2: Activation
        Console.WriteLine("    step 2: W 0x741=0x05  (activation)");
        WriteEC(Reg_Activation, 0x05);
        Thread.Sleep(150);

        // Step 3: Feature enable
        Console.WriteLine("    step 3: W 0x748=0x81  (feature enable)");
        WriteEC(Reg_FeatureEnable, 0x81);
        Thread.Sleep(150);

        // Step 4: Power management activation (RMW)
        int b9 = ReadEC(Reg_PowerMgmtByte9);
        int cume = ReadEC(Reg_PowerMgmtCume);
        Console.WriteLine("    step 4: R 0x726=0x" + b9.ToString("X2") + " -> W 0x" + (b9 | 0x80).ToString("X2")
            + ", R 0x727=0x" + cume.ToString("X2") + " -> W 0x" + (cume | 0x40).ToString("X2")
            + "  (power mgmt)");
        WriteEC(Reg_PowerMgmtByte9, b9 | 0x80);
        WriteEC(Reg_PowerMgmtCume, cume | 0x40);
        Thread.Sleep(150);

        // Step 5: Fan curve init — set bits 5 & 6, preserve bit 7
        int cur7C5 = ReadEC(Reg_FanCurveEnable);
        Console.WriteLine("    step 5: R 0x7C5=0x" + cur7C5.ToString("X2")
            + " -> W 0x" + (cur7C5 | 0x60).ToString("X2") + "  (fan curve init |= 0x60)");
        WriteEC(Reg_FanCurveEnable, cur7C5 | 0x60);
        Thread.Sleep(150);

        // Step 6: Fan curve param
        Console.WriteLine("    step 6: W 0x7C7=0x0C  (fan curve param)");
        WriteEC(Reg_FanCurveParam, 0x0C);
        Thread.Sleep(150);

        // Step 7: BIOS_CTRL init complete
        Console.WriteLine("    step 7: W 0x706=0x41  (BIOS_CTRL init complete)");
        WriteEC(Reg_BiosCtrl, 0x41);
        Thread.Sleep(150);

        // Post-init verify (matches MechControl)
        int appe = ReadEC(Reg_Activation);
        Console.WriteLine("    verify: 0x741=0x" + appe.ToString("X2")
            + ((appe & 0x01) != 0 ? "  (EC active)" : "  (NOT active — may not apply power limits)"));

        // ========== PHASE B: SetCustomFanCurveMode(true) ==========
        // User checks "Custom Fan Curve" and clicks Apply
        // WriteBitfield(0x7C5, 0x80, 0x80) + Write(0x7C6, 0x04)

        Console.WriteLine("    [Phase B -- FanControl.SetCustomFanCurveMode(true)]");

        int pre7C5 = ReadEC(Reg_FanCurveEnable);
        int new7C5 = (pre7C5 & ~0x80) | 0x80;  // set bit 7
        Console.WriteLine("    R 0x7C5=0x" + pre7C5.ToString("X2")
            + " -> W 0x" + new7C5.ToString("X2") + "  (set bit 7 = custom enable)");
        WriteEC(Reg_FanCurveEnable, new7C5);
        Console.WriteLine("    W 0x7C6=0x04  (apply latch)");
        WriteEC(Reg_FanCurveApply, 0x04);

        // ========== PHASE C: Write OEM fan table (94 bytes) ==========
        // User clicks "Apply CPU Curve" then "Apply GPU Curve"
        // FanControl.WriteCurve() — but with the OEM table format

        Console.WriteLine("    [Phase C -- Write OEM fan table (94 bytes)]");
        WriteOemTable();
        Console.WriteLine("    table written (94 bytes, tempDown[0] skipped, duty[0]=0x00)");
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

        string mode = args.Length > 0 ? args[0].ToLowerInvariant() : "test";

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

        Console.WriteLine("============================================================");
        Console.WriteLine(" fan_curve_probe14 — TEST 1: MechControl init + OEM table");
        Console.WriteLine("============================================================");
        Console.WriteLine(" MechControl 7-step init (EcActivationControl.InitializeEc)");
        Console.WriteLine(" + SetCustomFanCurveMode(true) (0x7C5 bit7 + 0x7C6=0x04)");
        Console.WriteLine(" + OEM fan table (94 bytes, tempDown[0] skipped, duty[0]=0x00)");
        Console.WriteLine(" CPU duty=80% (0xA0)  GPU duty=80% (0xA0)");
        Console.WriteLine(" NO mode profile, NO edge pulse, NO Phase 2/3/5/6");
        Console.WriteLine("============================================================");

        // --- Pre state ---
        Console.WriteLine();
        Console.WriteLine("--- PRE STATE ---");
        int preCpu  = ReadDutyPct(Reg_CpuDuty);
        int preGpu  = ReadDutyPct(Reg_GpuDuty);
        int preCpuT = ReadEC(Reg_CpuTemp);
        int preGpuT = ReadEC(Reg_GpuTemp);
        int pre7C5  = ReadEC(Reg_FanCurveEnable);
        int pre7C6  = ReadEC(Reg_FanCurveApply);
        int pre741  = ReadEC(Reg_Activation);
        Console.WriteLine("  CPU=" + preCpu + "% (" + preCpuT + "C)  GPU=" + preGpu + "% (" + preGpuT + "C)");
        Console.WriteLine("  0x7C5=0x" + pre7C5.ToString("X2") + "  0x7C6=0x" + pre7C6.ToString("X2")
            + "  0x741=0x" + pre741.ToString("X2"));

        // --- Neutralize ---
        Console.WriteLine();
        Console.WriteLine("--- NEUTRALIZE ---");
        Console.WriteLine("  Clearing custom mode (0x7C5 bit7=0, 0x7C6=0), wait " + SettleWaitSec + "s...");
        Neutralize();
        Thread.Sleep(SettleWaitSec * 1000);

        int midCpu  = ReadDutyPct(Reg_CpuDuty);
        int midGpu  = ReadDutyPct(Reg_GpuDuty);
        int mid7C5  = ReadEC(Reg_FanCurveEnable);
        int mid7C6  = ReadEC(Reg_FanCurveApply);
        Console.WriteLine("  post-neutralize: CPU=" + midCpu + "%  GPU=" + midGpu
            + "%  0x7C5=0x" + mid7C5.ToString("X2") + "  0x7C6=0x" + mid7C6.ToString("X2"));

        // --- Apply ---
        Console.WriteLine();
        Console.WriteLine("--- APPLY (MechControl sequence) ---");
        ApplyMechControlSequence();

        // --- Verify table ---
        Console.WriteLine();
        Console.WriteLine("--- READ-AFTER-WRITE VERIFY ---");
        int postOem5 = ReadEC(Reg_FanCurveEnable);
        int postOem6 = ReadEC(Reg_FanCurveApply);
        int post741  = ReadEC(Reg_Activation);
        string bit7  = ((postOem5 & 0x80) != 0) ? "set" : "CLEAR";
        string c6act = (postOem6 == 0x04) ? "active(0x04)" : "INACTIVE(0x" + postOem6.ToString("X2") + ")";
        Console.WriteLine("  0x7C5=0x" + postOem5.ToString("X2") + " (master bit7 " + bit7 + ")");
        Console.WriteLine("  0x7C6=0x" + postOem6.ToString("X2") + " " + c6act);
        Console.WriteLine("  0x741=0x" + post741.ToString("X2")
            + ((post741 & 0x01) != 0 ? " (EC active)" : " (NOT active)"));

        List<string> tableDiffs = VerifyOemTable();
        if (tableDiffs.Count == 0)
        {
            Console.WriteLine("  table: all 94 bytes match OEM layout (writes landed)");
        }
        else
        {
            Console.WriteLine("  table: " + tableDiffs.Count + " of 94 bytes MISMATCH:");
            foreach (string d in tableDiffs)
                Console.WriteLine(d);
        }

        // --- Poll ---
        Console.WriteLine();
        Console.WriteLine("--- POLL " + PollDurationSec + "s (last " + FinalSampleCount + " samples = final window) ---");
        int cpuFinal, gpuFinal, lastCpuTemp, lastGpuTemp;
        PollFor(PollDurationSec, out cpuFinal, out gpuFinal, out lastCpuTemp, out lastGpuTemp);

        // --- Verdict ---
        int gpuTarget = (lastGpuTemp < GpuThresholdC) ? 0 : CpuTargetDuty;
        bool cpuHit = InWindow(cpuFinal, CpuTargetDuty, DutyTolerance);
        bool gpuHit = InWindow(gpuFinal, gpuTarget, DutyTolerance);

        Console.WriteLine();
        Console.WriteLine("--- VERDICT ---");
        Console.WriteLine("  CPU: final=" + cpuFinal + "%  target=" + CpuTargetDuty + "+-" + DutyTolerance + "  " + (cpuHit ? "HIT" : "MISS"));
        Console.WriteLine("  GPU: final=" + gpuFinal + "%  target=" + gpuTarget + "+-" + DutyTolerance
            + (lastGpuTemp < GpuThresholdC ? " (temp " + lastGpuTemp + "C < " + GpuThresholdC + "C, expect 0%)" : "")
            + "  " + (gpuHit ? "HIT" : "MISS"));
        Console.WriteLine("  RESULT: " + (cpuHit ? "HIT" : "MISS") + " (CPU is primary signal)");
        Console.WriteLine();
        if (cpuHit)
            Console.WriteLine("  >> MechControl init + OEM table format is SUFFICIENT.");
        else
            Console.WriteLine("  >> MechControl init alone is NOT enough. Need to test edge pulse (Test 2).");

        // --- Restore ---
        Console.WriteLine();
        Console.WriteLine("Restoring neutral state before exit...");
        Neutralize();

        CloseHandle(hDev);
    }
}
