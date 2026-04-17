// fan_curve_probe11.cs
//
// Versioned successor of fan_curve_probe.cs. Adds combo 11 ("fixed mode
// switch") — a corrected version of combo 10 that fixes the 0x7C5 master-bit
// timing bug diagnosed by comparing combo 10's live output against Burst 2
// of frida_full_capture_X6FR5xxY.log (lines 508-1235).
//
// Empirical test tool: tries 11 different write sequences against the EC to
// determine which one makes the custom fan curve actually take effect on the
// X6FR5xxY (and any other Uniwill/MECHREVO machine that's currently broken).
//
// Background (see compacted conversation summary for full detail):
//   - Our current MechControl FanControl.SetCustomFanCurveMode conflates the
//     master switch (0x7C5 bit 7) with an apply latch (0x7C6), and WriteCurve
//     never toggles 0x7C6 during table updates.
//   - Raw Frida capture of OEM GCUService shows 0x7C6 is edge-triggered: OEM
//     clears it, writes the table, rising-edges it to 0x04, then clears again.
//   - On IDY_7Y (dev), our broken sequence happens to work.
//   - On X6FR5xxY (new test machine), it doesn't — custom curves are written
//     to 0xF00-0xF5F but fans keep running the internal EC curve.
//
// This tool writes a "witness curve" (CPU 80% flat, GPU 40% flat across all
// 10 active points) using each candidate sequence, READS BACK the table to
// confirm writes landed in EC RAM, polls live fan duty for 15 seconds, and
// reports HIT or MISS. The read-after-write step distinguishes "writes are
// silently dropped" from "writes land but EC ignores the table". The first
// HIT identifies the minimum viable write sequence — drive the FanControl
// fix from that result.
//
// Safety:
//   - Writes only to registers already in IDY_7Y.json writeAllowlist
//   - Resets 0x7C5 bit 7 = 0 + 0x7C6 = 0 between tests (EC reverts to builtin)
//   - --restore flag performs the same reset and exits without running tests
//   - User should load CPU/GPU (e.g. cinebench / furmark) before running so
//     temps are above 5C (the lowest active curve threshold). Any nonzero
//     temp will trigger the curve because all 10 active points have the same
//     target duty.
//
// Build:
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe -nologo fan_curve_probe11.cs
//
// Run (elevated):
//   fan_curve_probe11.exe all          - run all 11 combinations in sequence
//   fan_curve_probe11.exe 11           - run combo 11 (fixed mode-switch, current best bet)
//   fan_curve_probe11.exe 10           - run combo 10 (full mode-switch, previous — MISS)
//   fan_curve_probe11.exe 1            - run combination 1 only
//   fan_curve_probe11.exe --restore    - disable custom mode, return EC to neutral

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

    // --- EC register addresses (shared with IDY_7Y.json profile) ---
    const int Reg_FanCurveEnable = 0x7C5;  // bit 7 = master switch
    const int Reg_FanCurveApply  = 0x7C6;  // edge-triggered apply latch
    const int Reg_Support2       = 0x766;
    const int Reg_FanSwitchSpeed = 0x787;

    const int Reg_CpuDuty   = 0x75B;
    const int Reg_GpuDuty   = 0x75C;
    const int Reg_CpuTemp   = 0x43E;
    const int Reg_GpuTemp   = 0x44F;
    // NOTE: Fan RPM reads dropped. The profile's cpuFanRpmLo/Hi and
    // gpuFanRpmLo/Hi addresses returned garbage on X6AR55xY (hi byte wildly
    // varying, lo byte stuck on a constant — not a real fan counter). Duty %
    // is the authoritative signal for whether the curve applied anyway, so we
    // simply omit RPM instead of printing fake numbers.

    // Fan table bases — RamFan1 layout (see IDY_7Y.json fanTable)
    const int CpuTempUpBase   = 0xF00;
    const int CpuTempDownBase = 0xF10;
    const int CpuDutyBase     = 0xF20;
    const int GpuTempUpBase   = 0xF30;
    const int GpuTempDownBase = 0xF40;
    const int GpuDutyBase     = 0xF50;
    const int ActiveEntries   = 10;
    const int EntriesPerArray = 16;

    // Witness curve — flat duty across all 10 active points. Lowest tempUp is
    // 5C so any real CPU temp (idle ~45C, load ~80C) triggers the curve.
    static readonly int[] WitnessTempUp   = new int[] { 5, 10, 15, 20, 25, 30, 35, 40, 45, 50 };
    static readonly int[] WitnessTempDown = new int[] { 0,  5, 10, 15, 20, 25, 30, 35, 40, 45 };
    const int CpuWitnessDutyRaw = 160;  // raw_div_2_percent: 160/2 = 80%
    const int GpuWitnessDutyRaw = 80;   // raw_div_2_percent: 80/2  = 40%

    // Verdict: HIT iff the settled duty (avg of last FinalSampleCount samples)
    // is within DutyTolerance of the target. Using the settled value — not
    // the max — distinguishes "curve ramped fan up to target and held it"
    // from "duty was already high from the previous test and decayed back to
    // the builtin curve". CPU is the primary signal; GPU is reported
    // separately (GPU curve writes may be ignored on some boards that share
    // a single physical fan, or where the profile's GPU registers don't map).
    const int CpuTargetDuty   = 80;
    const int GpuTargetDuty   = 40;
    const int DutyTolerance   = 5;   // ±%
    const int PollDurationSec = 15;  // fans observed to take ~8s to ramp
    const int SettleWaitSec   = 8;   // between tests, let fans spin down
    const int FinalSampleCount = 6;  // last 3s of 500ms-interval polls

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
        return raw / 2;  // raw_div_2_percent encoding
    }

    // --- Read-after-write verification ---
    //
    // After applying a write sequence, read every byte of the fan table and
    // 0x7C5/0x7C6, and flag any discrepancy vs what we just wrote. This is
    // the cheapest way to distinguish "writes are silently dropped on this
    // EC" from "writes land but the EC ignores the table". Pure reads — no
    // additional state is touched.

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
        // Active entries 0-9
        for (int i = 0; i < ActiveEntries; i++)
        {
            WriteEC(CpuTempUpBase   + i, WitnessTempUp[i]);
            WriteEC(CpuTempDownBase + i, WitnessTempDown[i]);
            WriteEC(CpuDutyBase     + i, CpuWitnessDutyRaw);
            WriteEC(GpuTempUpBase   + i, WitnessTempUp[i]);
            WriteEC(GpuTempDownBase + i, WitnessTempDown[i]);
            WriteEC(GpuDutyBase     + i, GpuWitnessDutyRaw);
        }
        // Padding entries 10-15 — duty pad matches active so stale lookups still hit
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
        WriteEC(Reg_FanCurveEnable, oem5 & 0x7F);   // clear bit 7
        WriteEC(Reg_FanCurveApply, 0x00);
    }

    // --- Poll loop: record duty samples and compute min/max/final stats ---

    struct PollResult
    {
        public int CpuMin, CpuMax, CpuFinal;
        public int GpuMin, GpuMax, GpuFinal;
        public int LastCpuTemp, LastGpuTemp;
        public int LastOem5, LastOem6;
    }

    // If non-zero, poll loop writes 0x7C5 = heartbeatValue before each
    // sample. Simulates the OEM's 2s-periodic WHMS refresh heartbeat.
    static int heartbeatValue = 0;

    static PollResult PollFor(int durationSec)
    {
        int iterations = durationSec * 2;  // 500ms intervals
        List<int> cpuSamples = new List<int>();
        List<int> gpuSamples = new List<int>();
        int lastCpuTemp = 0, lastGpuTemp = 0, lastOem5 = 0, lastOem6 = 0;

        for (int i = 0; i < iterations; i++)
        {
            if (heartbeatValue != 0)
                WriteEC(Reg_FanCurveEnable, heartbeatValue);

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

            // Mark the final-window samples visually so it's obvious which
            // samples feed the verdict.
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
        public bool Hit;  // overall = CpuHit (primary signal)
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

        // Reset heartbeat before every combo so previous runs don't leak.
        heartbeatValue = 0;

        // Step 1: neutralize and wait for fans to fully settle to builtin curve.
        // Observed ramp rate on X6AR55xY is ~5%/sec; 8s covers a 40% transition.
        Console.WriteLine("  step 1: neutralize (0x7C5 bit7=0, 0x7C6=0), wait " + SettleWaitSec + "s for fans to settle...");
        Neutralize();
        Thread.Sleep(SettleWaitSec * 1000);

        int preCpu = ReadDutyPct(Reg_CpuDuty);
        int preGpu = ReadDutyPct(Reg_GpuDuty);
        int preOem5 = ReadEC(Reg_FanCurveEnable);
        int preOem6 = ReadEC(Reg_FanCurveApply);
        Console.WriteLine("  pre:  CPU=" + preCpu + "%  GPU=" + preGpu + "%  0x7C5=0x" + preOem5.ToString("X2") + "  0x7C6=0x" + preOem6.ToString("X2"));

        // Step 2: apply combination
        Console.WriteLine("  step 2: applying write sequence...");
        apply();

        // Step 2b: verify table writes landed. Pure reads — no writes.
        // If writes are silently dropped, this flags it BEFORE we waste 15s
        // polling fans. Also dumps 0x7C5/0x7C6 to confirm master+activate.
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

        // Step 3: poll — rows marked with * feed the verdict
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
        r.Hit = r.CpuHit;  // overall verdict keyed off CPU — primary signal

        int cpuDelta = p.CpuFinal - preCpu;
        int gpuDelta = p.GpuFinal - preGpu;
        string cpuDeltaStr = (cpuDelta >= 0 ? "+" : "") + cpuDelta;
        string gpuDeltaStr = (gpuDelta >= 0 ? "+" : "") + gpuDelta;

        Console.WriteLine("  CPU: pre=" + preCpu + "  min=" + p.CpuMin + "  max=" + p.CpuMax + "  final=" + p.CpuFinal + "  delta=" + cpuDeltaStr + "  target=" + CpuTargetDuty + "±" + DutyTolerance + "  " + (r.CpuHit ? "HIT" : "miss"));
        Console.WriteLine("  GPU: pre=" + preGpu + "  min=" + p.GpuMin + "  max=" + p.GpuMax + "  final=" + p.GpuFinal + "  delta=" + gpuDeltaStr + "  target=" + GpuTargetDuty + "±" + DutyTolerance + "  " + (r.GpuHit ? "HIT" : "miss"));
        Console.WriteLine("  VERDICT: " + (r.Hit ? "HIT" : "MISS") + " (CPU is primary signal)");
        return r;
    }

    // --- Combinations ---
    //
    // Combos 1-4 from the previous probe run (baseline, table-then-edge,
    // clear-table-edge, clear-table-edge-clear) have already been confirmed
    // MISS on X6FR5xxY with 0x7C5 bit 7 + 0x7C6 = 0x04 both sticking for 15s.
    // The write sequence is not the bug. They have been removed so this run
    // focuses on missing OEM init writes.

    // Helper: canonical "clear-table-edge" apply tail. Always ends with
    // 0x7C6 = 0x04 so the custom curve stays active. The only thing that
    // differs between the combos below is what init writes happen BEFORE
    // this tail.
    static void ApplyClearTableEdge()
    {
        int oem5 = ReadEC(Reg_FanCurveEnable);
        Console.WriteLine("    W 0x7C5 |= 0x80  (master on)");
        WriteEC(Reg_FanCurveEnable, oem5 | 0x80);
        Console.WriteLine("    W 0x7C6 = 0x00  (clear latch)");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W table (96 bytes)");
        WriteWitnessTable();
        Console.WriteLine("    W 0x7C6 = 0x04  (activate, LEFT AT 0x04)");
        WriteEC(Reg_FanCurveApply, 0x04);
    }

    // 5. Support2 writeback + clear-table-edge
    //    OEM writes back Read(0x766) during init on X6FR5xxY (ec_snap diff
    //    shows 0x766: 0x90 -> 0x93). Hypothesis: bit 0 and bit 1 of Support2
    //    latch feature enables needed for custom curve to actually take effect.
    static void Combo_Support2PlusEdge()
    {
        int s2 = ReadEC(Reg_Support2);
        Console.WriteLine("    R 0x766 = 0x" + s2.ToString("X2") + "  (Support2)");
        Console.WriteLine("    W 0x766 = 0x" + s2.ToString("X2") + "  (writeback)");
        WriteEC(Reg_Support2, s2);
        Thread.Sleep(200);
        ApplyClearTableEdge();
    }

    // 6. Fan switch speed enable (0x787 = 0x81) + clear-table-edge
    //    OEM Phase 3 writes 0x787 = 0x81. On some Uniwill models this register
    //    gates PWM transition rates; it may also gate custom curve acceptance.
    static void Combo_787PlusEdge()
    {
        Console.WriteLine("    W 0x787 = 0x81  (fan switch speed enable)");
        WriteEC(Reg_FanSwitchSpeed, 0x81);
        Thread.Sleep(200);
        ApplyClearTableEdge();
    }

    // 7. Try to force 0x7C5 bit 6 (service managed) + clear-table-edge
    //    On the IDY_7Y dev machine, BIOS sets bit 6 of 0x7C5 by default.
    //    On X6FR5xxY it's reportedly never writable, but let's confirm
    //    empirically and see if forcing it (if it sticks) changes anything.
    static void Combo_Bit6PlusEdge()
    {
        Console.WriteLine("    W 0x7C5 = 0xE0  (force bit7+bit6+bit5)");
        WriteEC(Reg_FanCurveEnable, 0xE0);
        int after = ReadEC(Reg_FanCurveEnable);
        string bit6state = ((after & 0x40) != 0) ? "STUCK" : "rejected by EC";
        Console.WriteLine("    R 0x7C5 = 0x" + after.ToString("X2") + "  (bit6 " + bit6state + ")");
        Thread.Sleep(200);
        ApplyClearTableEdge();
    }

    // 8. All missing OEM init writes + clear-table-edge
    //    Writes every register the OEM touches on X6FR5xxY that MechControl
    //    currently doesn't, in one shot. If this HITs, bisect: which single
    //    write is the magic one? If this MISSes, the bug is elsewhere (wrong
    //    fan table address, different activation register, etc.).
    //    Targets (from ec_snap_1 -> ec_snap_2 diff on X6FR5xxY):
    //      0x766 writeback   (Support2)
    //      0x7C4 = 0x20      (OEM clears bit 4/5 of adjacent byte)
    //      0x788 = 0x28      (MyFan3 L3 PWM default)
    //      0x763 = 0x00      (cleared by OEM)
    //      0x764 = 0x00      (cleared by OEM)
    //      0x787 = 0x81      (fan switch speed enable)
    static void Combo_AllOemInitPlusEdge()
    {
        int s2 = ReadEC(Reg_Support2);
        Console.WriteLine("    W 0x766 = 0x" + s2.ToString("X2") + "  (Support2 writeback)");
        WriteEC(Reg_Support2, s2);
        Console.WriteLine("    W 0x7C4 = 0x20");
        WriteEC(0x7C4, 0x20);
        Console.WriteLine("    W 0x788 = 0x28  (MyFan3 L3 PWM)");
        WriteEC(0x788, 0x28);
        Console.WriteLine("    W 0x763 = 0x00");
        WriteEC(0x763, 0x00);
        Console.WriteLine("    W 0x764 = 0x00");
        WriteEC(0x764, 0x00);
        Console.WriteLine("    W 0x787 = 0x81  (fan switch speed enable)");
        WriteEC(Reg_FanSwitchSpeed, 0x81);
        Thread.Sleep(300);
        ApplyClearTableEdge();
    }

    // 9. Double-edge: write table twice with two full edge cycles, both
    //    ending on 0x7C6 = 0x04. Tests the hypothesis that the EC needs the
    //    table pushed twice for the write to stick, independent of the OEM's
    //    (possibly-misread) 0x00 ending.
    static void Combo_DoubleEdge()
    {
        Console.WriteLine("    (pass 1)");
        ApplyClearTableEdge();
        Console.WriteLine("    sleep 1.5s");
        Thread.Sleep(1500);
        Console.WriteLine("    (pass 2)");
        Console.WriteLine("    W 0x7C6 = 0x00");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W table");
        WriteWitnessTable();
        Console.WriteLine("    W 0x7C6 = 0x04");
        WriteEC(Reg_FanCurveApply, 0x04);
    }

    // 6. fan-adjacent sink: replay every register in 0x7xx and 0x74x/0x75x
    //    area where snap_4 (our broken active state) differs from snap_2
    //    (OEM working active state). If this HITs, bisect to find the one
    //    register that matters. If it MISSes we've ruled out this whole set.
    //
    //    Targets come directly from the snap_4 -> snap_2 filtered diff:
    //      0x741 = 0x01   (possibly a status, but write-safe per profile)
    //      0x74E = 0x01
    //      0x753 = 0x41   (was cleared between snap_1 and snap_4 somehow)
    //      0x754 = 0x78
    //      0x75E = 0x12
    //      0x763 = 0x00
    //      0x764 = 0x00
    //      0x788 = 0x28
    //      0x7A6 = 0x20
    //      0x7C4 = 0x20
    //      0x7F7 = 0x00
    //    Then: W table, W 0x7C5 = 0xA0 (bit7+bit5), W 0x7C6 = 0x04
    static void Combo_FanSink()
    {
        Console.WriteLine("    W 0x741 = 0x01  (state flag?)");
        WriteEC(0x741, 0x01);
        Console.WriteLine("    W 0x74E = 0x01");
        WriteEC(0x74E, 0x01);
        Console.WriteLine("    W 0x753 = 0x41");
        WriteEC(0x753, 0x41);
        Console.WriteLine("    W 0x754 = 0x78");
        WriteEC(0x754, 0x78);
        Console.WriteLine("    W 0x75E = 0x12");
        WriteEC(0x75E, 0x12);
        Console.WriteLine("    W 0x763 = 0x00");
        WriteEC(0x763, 0x00);
        Console.WriteLine("    W 0x764 = 0x00");
        WriteEC(0x764, 0x00);
        Console.WriteLine("    W 0x788 = 0x28  (MyFan3 L3 PWM)");
        WriteEC(0x788, 0x28);
        Console.WriteLine("    W 0x7A6 = 0x20  (batteryMode)");
        WriteEC(0x7A6, 0x20);
        Console.WriteLine("    W 0x7C4 = 0x20");
        WriteEC(0x7C4, 0x20);
        Console.WriteLine("    W 0x7F7 = 0x00");
        WriteEC(0x7F7, 0x00);
        Thread.Sleep(300);
        Console.WriteLine("    W 0x7C6 = 0x00  (clear latch)");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W table (96 bytes)");
        WriteWitnessTable();
        Console.WriteLine("    W 0x7C5 = 0xA0  (bit7 + bit5, explicit)");
        WriteEC(Reg_FanCurveEnable, 0xA0);
        Console.WriteLine("    W 0x7C6 = 0x04  (activate)");
        WriteEC(Reg_FanCurveApply, 0x04);
    }

    // 7. Minimal: force 0x7C5 = 0xA0 explicitly (bit 7 + bit 5).
    //    If this alone HITs then bit 5 of 0x7C5 is THE gate — simplest fix.
    //    Differs from ApplyClearTableEdge only in that the master byte is
    //    written as a literal 0xA0 rather than (current | 0x80).
    static void Combo_Bit5Only()
    {
        Console.WriteLine("    W 0x7C5 = 0xA0  (bit7 + bit5, literal)");
        WriteEC(Reg_FanCurveEnable, 0xA0);
        int after = ReadEC(Reg_FanCurveEnable);
        string bit5state = ((after & 0x20) != 0) ? "STUCK" : "rejected by EC";
        Console.WriteLine("    R 0x7C5 = 0x" + after.ToString("X2") + "  (bit5 " + bit5state + ")");
        Console.WriteLine("    W 0x7C6 = 0x00  (clear latch)");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W table (96 bytes)");
        WriteWitnessTable();
        Console.WriteLine("    W 0x7C5 = 0xA0  (reassert after table)");
        WriteEC(Reg_FanCurveEnable, 0xA0);
        Console.WriteLine("    W 0x7C6 = 0x04  (activate)");
        WriteEC(Reg_FanCurveApply, 0x04);
    }

    // 8. WHMS heartbeat: apply combo 7, then rewrite 0x7C5 = 0xA0 on every
    //    poll iteration (500ms cadence) to simulate the OEM's WHMS refresh.
    //    DSDT names bit 5 of 0x7C5 "WHMS" (Windows Hardware Management
    //    Service). Frida capture shows OEM reads 0x7C5 every ~2s and writes
    //    back 0xA0 periodically. Hypothesis: EC clears WHMS within ~1s if
    //    no service-side heartbeat, and invalidates the RAM fan table when
    //    WHMS clears. Continuous re-write should keep WHMS alive and let
    //    the EC actually evaluate the custom curve.
    //    RESULT: HALF-hit. WHMS stayed 0xA0 full 15s, GPU stayed flat at 36%
    //    instead of falling to 0/25, but CPU still frozen at ~builtin curve.
    static void Combo_WhmsHeartbeat()
    {
        Console.WriteLine("    W 0x7C5 = 0xA0  (bit7 + bit5)");
        WriteEC(Reg_FanCurveEnable, 0xA0);
        Console.WriteLine("    W 0x7C6 = 0x00  (clear latch)");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W table (96 bytes)");
        WriteWitnessTable();
        Console.WriteLine("    W 0x7C5 = 0xA0  (reassert)");
        WriteEC(Reg_FanCurveEnable, 0xA0);
        Console.WriteLine("    W 0x7C6 = 0x04  (activate)");
        WriteEC(Reg_FanCurveApply, 0x04);
        Console.WriteLine("    heartbeat: poll loop will rewrite 0x7C5=0xA0 every 500ms");
        heartbeatValue = 0xA0;
    }

    // 9. OEM replay: exact sequence from frida_full_capture.log lines 469-620
    //    (first curve commit in the capture). Key additions over combo 8:
    //    - 0x728 = 0x04 (mode register we've never written; adjacent to the
    //      DSDT-annotated CUME/PL4D/TB43/CMSP bits at 0x727)
    //    - 0x751 = 0x00 (fanControlMode = normal)
    //    - 0x746 = 0x05 (dbMaxTgp)
    //    - 0x7C6 edge pulse: write 0x04 then clear 0x00 ~32ms later (OEM's
    //      pattern — on X6FR5xxY this appears to commit as an edge, not
    //      stay as a level flag)
    //    - 0x7C5 re-mastered after the pulse
    //    - WHMS heartbeat continues in poll loop
    //    If this HITs, bisect to find which of 0x728/0x751/0x746/edge-pulse
    //    is the actual gate.
    static void Combo_OemReplay()
    {
        Console.WriteLine("    W 0x7C6 = 0x00  (clear latch)");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W 0x7C5 = 0xA0  (master + WHMS)");
        WriteEC(Reg_FanCurveEnable, 0xA0);
        Console.WriteLine("    W 0x751 = 0x00  (fanControlMode = normal)");
        WriteEC(0x751, 0x00);
        Console.WriteLine("    W 0x746 = 0x05  (dbMax)");
        WriteEC(0x746, 0x05);
        Console.WriteLine("    W 0x728 = 0x04  (MODE REG — candidate gate)");
        WriteEC(0x728, 0x04);
        Console.WriteLine("    W table (96 bytes)");
        WriteWitnessTable();
        Console.WriteLine("    W 0x7C6 = 0x04  (activate edge)");
        WriteEC(Reg_FanCurveApply, 0x04);
        Thread.Sleep(32);
        Console.WriteLine("    W 0x7C6 = 0x00  (release pulse)");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W 0x7C5 = 0xA0  (re-master WHMS)");
        WriteEC(Reg_FanCurveEnable, 0xA0);
        Console.WriteLine("    heartbeat: poll loop will rewrite 0x7C5=0xA0 every 500ms");
        heartbeatValue = 0xA0;
    }

    // 10. Full mode-switch replay from X6FR5xxY live Frida capture (Burst 3,
    //     lines 4114-4471 of frida_full_capture_X6FR5xxY.log).
    //
    //     Root cause of combo 9's miss (per live capture analysis): combo 9
    //     skipped ~10 mode-switch registers entirely (0x706, 0x743 state
    //     machine, 0x727, 0x726, 0x744, 0x745, 0x783-0x787) AND forced wrong
    //     values for the two firmware-preserved bit registers:
    //       - 0x7C5: combo 9 forced 0xA0. X6FR5xxY actual = 0x80. Bit 5 is
    //         firmware-set on X6AR55xY, never on X6FR5xxY. Use RMW|0x80 to
    //         preserve whatever firmware wants while guaranteeing master on.
    //       - 0x728: combo 9 forced 0x04. X6FR5xxY actual = 0x00. Bit 2 is
    //         firmware-preserved. Use pure RMW.
    //
    //     This combo replicates the full OEM template:
    //       Phase A: pre-init touch (0x706, 0x7C6, 0x743, 0x7C5 RMW, 0x743)
    //       Phase B: mode profile (0x743 state machine, ESHUTTER, OEM9, PLs,
    //               TGP, DB_TOTAL/MAX, FAN_SWITCH_SPEED, 0x728 RMW)
    //       Phase C: 96-byte witness fan table
    //       Phase D: verify reads + edge pulse + re-master + final 0x7C6=0x04
    //
    //     NO HEARTBEAT. The X6FR5xxY Frida capture showed a 4-minute window
    //     between Burst 2 and Burst 3 with zero control-register writes — the
    //     EC runs the curve autonomously after the mode-switch sequence.
    //     Combo 8's "half-hit" on WHMS heartbeat was a coincidence.
    //
    //     Values hardcoded to X6FR5xxY SKU (PL1=50, PL2=50, PL4=60, TGP=40,
    //     ESHUTTER=0x40, TCC_OFFSET=0xDF). For MechControl integration,
    //     these come from the profile JSON; see notes/fan_curve_investigation.md.
    static void Combo_FullModeSwitch()
    {
        // Phase A — pre-init touch
        Console.WriteLine("    [Phase A — pre-init]");
        Console.WriteLine("    W 0x706 = 0x41  (BIOS_CTRL enable custom mode)");
        WriteEC(0x706, 0x41);
        Console.WriteLine("    W 0x7C6 = 0x00  (clear apply latch)");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W 0x743 = 0x05  (DB_CTRL initial touch)");
        WriteEC(0x743, 0x05);
        int cur7C5a = ReadEC(Reg_FanCurveEnable);
        int new7C5a = cur7C5a | 0x80;
        Console.WriteLine("    R 0x7C5 = 0x" + cur7C5a.ToString("X2")
            + " -> W 0x" + new7C5a.ToString("X2") + "  (RMW | bit7)");
        WriteEC(Reg_FanCurveEnable, new7C5a);
        Console.WriteLine("    W 0x743 = 0x05  (touch)");
        WriteEC(0x743, 0x05);

        // Phase B — mode profile (SKU values hardcoded for X6FR5xxY)
        Console.WriteLine("    [Phase B — mode profile, X6FR5xxY SKU values]");
        Console.WriteLine("    W 0x743 = 0x07  (DB_CTRL transaction open)");
        WriteEC(0x743, 0x07);
        Console.WriteLine("    W 0x727 = 0x40  (ESHUTTER X6FR5xxY)");
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

        // Phase C — fan table (96 bytes witness curve)
        Console.WriteLine("    [Phase C — fan table]");
        Console.WriteLine("    W table (96 bytes witness curve)");
        WriteWitnessTable();

        // Phase D — verify + edge pulse + re-master + final active hold
        Console.WriteLine("    [Phase D — edge pulse + re-master]");
        int v00 = ReadEC(CpuTempUpBase + 0);
        int v01 = ReadEC(CpuTempUpBase + 1);
        Console.WriteLine("    R 0xF00=0x" + v00.ToString("X2")
            + " R 0xF01=0x" + v01.ToString("X2") + "  (verify)");
        Console.WriteLine("    W 0x7C6 = 0x04  (edge activate)");
        WriteEC(Reg_FanCurveApply, 0x04);
        Thread.Sleep(32);
        Console.WriteLine("    W 0x7C6 = 0x00  (edge release)");
        WriteEC(Reg_FanCurveApply, 0x00);
        int cur7C5b = ReadEC(Reg_FanCurveEnable);
        int new7C5b = cur7C5b | 0x80;
        Console.WriteLine("    R 0x7C5 = 0x" + cur7C5b.ToString("X2")
            + " -> W 0x" + new7C5b.ToString("X2") + "  (re-master | bit7)");
        WriteEC(Reg_FanCurveEnable, new7C5b);
        Thread.Sleep(32);
        Console.WriteLine("    W 0x7C6 = 0x04  (final activate, HELD — OEM pattern)");
        WriteEC(Reg_FanCurveApply, 0x04);
        // No heartbeat. Leave heartbeatValue at 0.
    }

    // 11. Fixed mode-switch — combo 10 with corrected 0x7C5 master-bit timing.
    //
    //     Diagnosis of combo 10 MISS (from comparing combo 10's live run
    //     output against Burst 2 of frida_full_capture_X6FR5xxY.log lines
    //     508-1235):
    //
    //     OEM Burst 2 cold-start sequence keeps 0x7C5 bit 7 OFF all the way
    //     through Phase A + Phase B + first 0x7C6 edge pulse. Master bit is
    //     only set to 0x80 AFTER the first edge (line 663: W 0x7C5 = 0x80).
    //     The EC's commit trigger is the 0x7C6 0x04->0x00 transition while
    //     bit 7 is still 0.
    //
    //     Combo 10 forced (cur | 0x80) in Phase A, so master was already ON
    //     by the time the first edge pulse fired. The EC saw the pulse as a
    //     no-op (already active) and rejected the pending mode profile.
    //
    //     Combo 11 fixes this by:
    //       1. Phase A: pure RMW on 0x7C5 (no | 0x80). On cold start from
    //          neutralize this reads 0x00 and writes 0x00 back.
    //       2. Phase A: W 0x743 = 0x04 (not 0x05) — matches Burst 2 cold
    //          start (line 512). Harmless secondary change.
    //       3. Phase D iter 1: FIRST edge pulse (0x7C6 0x04->0x00) while
    //          master is still 0, THEN W 0x7C5 = 0x80 (literal, matching
    //          OEM line 663 exactly).
    //       4. Phase E iter 2: second edge pulse + RMW 0x7C5 (now reads
    //          0x80, writes 0x80 back — matches OEM lines 761-766).
    //          OEM does 3 iterations; we do 2 to start. If combo 11 misses,
    //          escalate to 3.
    //       5. Final W 0x7C6 = 0x04 (HELD), like OEM ends Burst 2 at
    //          line 1235.
    //
    //     No heartbeat. No forcing bits. Pure sequence-timing fix.
    //
    //     SKU values hardcoded to X6FR5xxY (same as combo 10). For
    //     MechControl integration, these come from the profile JSON.
    static void Combo_FixedModeSwitch()
    {
        // Phase A — pre-init (master stays OFF, pure RMW on 0x7C5)
        Console.WriteLine("    [Phase A — pre-init, master stays OFF]");
        Console.WriteLine("    W 0x706 = 0x41  (BIOS_CTRL enable custom mode)");
        WriteEC(0x706, 0x41);
        Console.WriteLine("    W 0x7C6 = 0x00  (clear apply latch)");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W 0x743 = 0x04  (DB_CTRL initial touch — Burst 2 cold-start value)");
        WriteEC(0x743, 0x04);
        int cur7C5a = ReadEC(Reg_FanCurveEnable);
        Console.WriteLine("    R 0x7C5 = 0x" + cur7C5a.ToString("X2")
            + " -> W 0x" + cur7C5a.ToString("X2") + "  (PURE RMW, no forcing — master stays 0)");
        WriteEC(Reg_FanCurveEnable, cur7C5a);
        Console.WriteLine("    W 0x743 = 0x05  (touch)");
        WriteEC(0x743, 0x05);

        // Phase B — mode profile (SKU values hardcoded for X6FR5xxY)
        Console.WriteLine("    [Phase B — mode profile, X6FR5xxY SKU values]");
        Console.WriteLine("    W 0x743 = 0x07  (DB_CTRL transaction open)");
        WriteEC(0x743, 0x07);
        Console.WriteLine("    W 0x727 = 0x40  (ESHUTTER X6FR5xxY)");
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

        // Phase C — fan table (96 bytes witness curve)
        Console.WriteLine("    [Phase C — fan table]");
        Console.WriteLine("    W table (96 bytes witness curve)");
        WriteWitnessTable();

        // Phase D — iter 1: edge pulse WHILE MASTER IS 0, then set master
        Console.WriteLine("    [Phase D — iter 1: edge pulse (master=0), then set master]");
        int v00 = ReadEC(CpuTempUpBase + 0);
        int v01 = ReadEC(CpuTempUpBase + 1);
        Console.WriteLine("    R 0xF00=0x" + v00.ToString("X2")
            + " R 0xF01=0x" + v01.ToString("X2") + "  (verify)");
        Console.WriteLine("    W 0x7C6 = 0x04  (edge activate, 0x7C5 still 0)");
        WriteEC(Reg_FanCurveApply, 0x04);
        Thread.Sleep(32);
        Console.WriteLine("    W 0x7C6 = 0x00  (edge release, 0x7C5 still 0)");
        WriteEC(Reg_FanCurveApply, 0x00);
        Console.WriteLine("    W 0x7C5 = 0x80  (master SET, first time — OEM line 663)");
        WriteEC(Reg_FanCurveEnable, 0x80);

        // Phase E — iter 2: edge pulse + re-master RMW (OEM does 3+, we do 2)
        Console.WriteLine("    [Phase E — iter 2: edge pulse + re-master RMW]");
        Thread.Sleep(32);
        Console.WriteLine("    W 0x7C6 = 0x04  (edge activate)");
        WriteEC(Reg_FanCurveApply, 0x04);
        Thread.Sleep(32);
        Console.WriteLine("    W 0x7C6 = 0x00  (edge release)");
        WriteEC(Reg_FanCurveApply, 0x00);
        int cur7C5b = ReadEC(Reg_FanCurveEnable);
        Console.WriteLine("    R 0x7C5 = 0x" + cur7C5b.ToString("X2")
            + " -> W 0x" + cur7C5b.ToString("X2") + "  (RMW, should be 0x80 now)");
        WriteEC(Reg_FanCurveEnable, cur7C5b);

        // Final: hold 0x7C6 at 0x04 like OEM ends Burst 2 at line 1235
        Thread.Sleep(32);
        Console.WriteLine("    W 0x7C6 = 0x04  (final activate, HELD — OEM line 1235 pattern)");
        WriteEC(Reg_FanCurveApply, 0x04);
        // No heartbeat. Leave heartbeatValue at 0.
    }

    // --- Main ---

    static void PrintHeader()
    {
        Console.WriteLine("============================================================");
        Console.WriteLine(" fan_curve_probe — empirical EC write sequence tester");
        Console.WriteLine("============================================================");
        Console.WriteLine(" Witness curve:  CPU=" + CpuTargetDuty + "% flat   GPU=" + GpuTargetDuty + "% flat (all 10 active points)");
        Console.WriteLine(" HIT criterion:  |final - target| <= " + DutyTolerance + "  (final = avg of last " + FinalSampleCount + " samples)");
        Console.WriteLine(" Each combo:     neutralize " + SettleWaitSec + "s, apply, poll " + PollDurationSec + "s");
        Console.WriteLine(" Overall HIT:    keyed off CPU (primary signal); GPU reported separately");
        Console.WriteLine();
        Console.WriteLine(" NOTE: Fan RPM is NOT read — profile addresses returned garbage on");
        Console.WriteLine(" X6AR55xY. Fan duty % is the authoritative signal.");
        Console.WriteLine();
        Console.WriteLine(" PRECONDITION: CPU must be above ~5C (trivial). For cleanest results");
        Console.WriteLine(" avoid heavy load — builtin curve at high temps may approach the CPU");
        Console.WriteLine(" target and produce an ambiguous HIT.");
        Console.WriteLine("============================================================");
    }

    static void PrintSummary(List<Result> results)
    {
        Console.WriteLine();
        Console.WriteLine("============================================================");
        Console.WriteLine(" SUMMARY   (target CPU=" + CpuTargetDuty + "±" + DutyTolerance + "  GPU=" + GpuTargetDuty + "±" + DutyTolerance + ")");
        Console.WriteLine("============================================================");
        Console.WriteLine(" combination                 | pre     | final   | delta     | CPU  | GPU ");
        Console.WriteLine(" ----------------------------|---------|---------|-----------|------|-----");
        foreach (Result r in results)
        {
            int cpuDelta = r.CpuFinal - r.PreCpuDuty;
            int gpuDelta = r.GpuFinal - r.PreGpuDuty;
            string cpuD = (cpuDelta >= 0 ? "+" : "") + cpuDelta;
            string gpuD = (gpuDelta >= 0 ? "+" : "") + gpuDelta;
            string n   = r.Name.PadRight(27);
            string pre = (r.PreCpuDuty + "/" + r.PreGpuDuty).PadRight(7);
            string fin = (r.CpuFinal + "/" + r.GpuFinal).PadRight(7);
            string del = (cpuD + "/" + gpuD).PadRight(9);
            string cpuV = (r.CpuHit ? "HIT " : "miss").PadRight(4);
            string gpuV = (r.GpuHit ? "HIT " : "miss").PadRight(4);
            Console.WriteLine(" " + n + " | " + pre + " | " + fin + " | " + del + " | " + cpuV + " | " + gpuV);
        }
        Console.WriteLine();
        Console.WriteLine(" First CPU HIT = minimum viable write sequence. Use that as the");
        Console.WriteLine(" basis for the FanControl.WriteCurve fix in MechControl.");
    }

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

        if (mode == "hold")
        {
            // Apply combo 1 (support2 writeback + clear-table-edge), verify,
            // and EXIT WITHOUT NEUTRALIZING. The machine is left with:
            //   0x7C5 bit 7 = 1, 0x7C6 = 0x04, witness table in 0xF00-0xF5F
            // This is the "curve should be active but isn't" state. Take a
            // snapshot immediately and diff against snap_2 (OEM working state)
            // to find registers OEM sets that we don't.
            Console.WriteLine("============================================================");
            Console.WriteLine(" HOLD MODE: apply combo 1 and exit without neutralizing");
            Console.WriteLine("============================================================");
            Console.WriteLine();
            Console.WriteLine("Applying combo 1 (support2 writeback + clear-table-edge)...");
            Combo_Support2PlusEdge();
            Console.WriteLine();
            Console.WriteLine("Verifying writes landed...");
            int hoem5 = ReadEC(Reg_FanCurveEnable);
            int hoem6 = ReadEC(Reg_FanCurveApply);
            Console.WriteLine("  0x7C5 = 0x" + hoem5.ToString("X2"));
            Console.WriteLine("  0x7C6 = 0x" + hoem6.ToString("X2"));
            List<string> hDiffs = VerifyWitnessTable();
            if (hDiffs.Count == 0)
                Console.WriteLine("  table: all 96 bytes match witness");
            else
                Console.WriteLine("  table: " + hDiffs.Count + " MISMATCHES");
            Console.WriteLine();
            Console.WriteLine("============================================================");
            Console.WriteLine(" EC HELD IN ACTIVE-CUSTOM-CURVE STATE.");
            Console.WriteLine(" Take a snapshot NOW:");
            Console.WriteLine("   ./tools/ec_snapshot.exe snapshot");
            Console.WriteLine(" Then run ./tools/fan_curve_probe.exe --restore to return");
            Console.WriteLine(" to neutral (or just reboot).");
            Console.WriteLine("============================================================");
            CloseHandle(hDev);
            return;
        }

        if (mode == "help" || mode == "--help" || mode == "-h")
        {
            Console.WriteLine("Usage: fan_curve_probe11.exe <all|1-11|hold|--restore>");
            Console.WriteLine("  all        Run all 11 combinations in sequence");
            Console.WriteLine("  1          support2+edge     (0x766 writeback + clear-table-edge)");
            Console.WriteLine("  2          787+edge          (0x787=0x81 + clear-table-edge)");
            Console.WriteLine("  3          bit6+edge         (force 0x7C5 bit6 + clear-table-edge)");
            Console.WriteLine("  4          all-oem-init+edge (all missing OEM writes + clear-table-edge)");
            Console.WriteLine("  5          double-edge       (two full passes, both end 0x04)");
            Console.WriteLine("  6          fan-sink          (replay all non-matching 0x7xx regs + 0x7C5=0xA0)");
            Console.WriteLine("  7          bit5-only         (force 0x7C5=0xA0 literal, minimal)");
            Console.WriteLine("  8          whms-heartbeat    (bit5 + rewrite 0x7C5=0xA0 every 500ms)");
            Console.WriteLine("  9          oem-replay        (X6AR55xY frida OEM sequence + 0x728=0x04 + heartbeat)");
            Console.WriteLine("  10         full-mode-switch  (X6FR5xxY burst 3 replay + RMW 0x7C5/0x728 + no hb)");
            Console.WriteLine("  11         fixed-mode-switch (combo 10 with master-bit timing fix — current best)");
            Console.WriteLine("  hold       Apply combo 1 and exit WITHOUT neutralizing");
            Console.WriteLine("             (for snapshot-diff against OEM working state)");
            Console.WriteLine("  --restore  Disable custom curve, exit");
            Console.WriteLine();
            Console.WriteLine("Combos 1-10 all MISS on X6FR5xxY. Combo 11 is the current best bet,");
            Console.WriteLine("built from the live X6FR5xxY Frida capture Burst 2 (lines 508-1235) — fixes");
            Console.WriteLine("the 0x7C5 master-bit timing bug in combo 10 (master must stay 0 through the");
            Console.WriteLine("first edge pulse; set to 0x80 only after, per OEM line 663).");
            Console.WriteLine("If combo 11 hits, port the sequence to MechControl/Features/FanControl.cs.");
            CloseHandle(hDev);
            return;
        }

        PrintHeader();

        List<Result> results = new List<Result>();

        if (mode == "all")
        {
            results.Add(RunCombination("1.support2+edge",       Combo_Support2PlusEdge));
            results.Add(RunCombination("2.787+edge",            Combo_787PlusEdge));
            results.Add(RunCombination("3.bit6+edge",           Combo_Bit6PlusEdge));
            results.Add(RunCombination("4.all-oem-init+edge",   Combo_AllOemInitPlusEdge));
            results.Add(RunCombination("5.double-edge",         Combo_DoubleEdge));
            results.Add(RunCombination("6.fan-sink",            Combo_FanSink));
            results.Add(RunCombination("7.bit5-only",           Combo_Bit5Only));
            results.Add(RunCombination("8.whms-heartbeat",      Combo_WhmsHeartbeat));
            results.Add(RunCombination("9.oem-replay",          Combo_OemReplay));
            results.Add(RunCombination("10.full-mode-switch",   Combo_FullModeSwitch));
            results.Add(RunCombination("11.fixed-mode-switch",  Combo_FixedModeSwitch));
            PrintSummary(results);
        }
        else
        {
            int n;
            if (int.TryParse(mode, out n) && n >= 1 && n <= 11)
            {
                switch (n)
                {
                    case 1:  results.Add(RunCombination("1.support2+edge",       Combo_Support2PlusEdge)); break;
                    case 2:  results.Add(RunCombination("2.787+edge",            Combo_787PlusEdge)); break;
                    case 3:  results.Add(RunCombination("3.bit6+edge",           Combo_Bit6PlusEdge)); break;
                    case 4:  results.Add(RunCombination("4.all-oem-init+edge",   Combo_AllOemInitPlusEdge)); break;
                    case 5:  results.Add(RunCombination("5.double-edge",         Combo_DoubleEdge)); break;
                    case 6:  results.Add(RunCombination("6.fan-sink",            Combo_FanSink)); break;
                    case 7:  results.Add(RunCombination("7.bit5-only",           Combo_Bit5Only)); break;
                    case 8:  results.Add(RunCombination("8.whms-heartbeat",      Combo_WhmsHeartbeat)); break;
                    case 9:  results.Add(RunCombination("9.oem-replay",          Combo_OemReplay)); break;
                    case 10: results.Add(RunCombination("10.full-mode-switch",   Combo_FullModeSwitch)); break;
                    case 11: results.Add(RunCombination("11.fixed-mode-switch",  Combo_FixedModeSwitch)); break;
                }
                PrintSummary(results);
            }
            else
            {
                Console.WriteLine("Unknown argument: " + args[0]);
                Console.WriteLine("Run 'fan_curve_probe.exe help' for usage.");
                CloseHandle(hDev);
                return;
            }
        }

        Console.WriteLine();
        Console.WriteLine("Restoring neutral state before exit...");
        Neutralize();

        CloseHandle(hDev);
    }
}
