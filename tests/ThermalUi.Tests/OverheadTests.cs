using MechControl.Driver;
using MechControl.Features;
using MechControl.Monitoring;
using MechControl.Profile;
using MechControl.UI;

static partial class Program
{
    private static void LinkedFanTests()
    {
        var profile = MonitorProfile();
        profile.Registers["fanControlMode"] = "0x751";
        profile.Registers["fanCurveEnable"] = "0x7C5";
        profile.FanTable = new()
        {
            CpuTempUpBase = "0xF00", CpuTempDownBase = "0xF10", CpuDutyBase = "0xF20",
            GpuTempUpBase = "0xF30", GpuTempDownBase = "0xF40", GpuDutyBase = "0xF50"
        };
        UniversalDefaults.ApplyToProfile(profile);
        var device = new EcDevice();
        int cpu = profile.GetRegister("cpuTemp"), gpu = profile.GetRegister("gpuTemp");
        device.Registers[cpu] = 40;
        device.Registers[gpu] = 40;
        using var monitor = new EcMonitor(device, profile);
        var fan = new FanControl(device, new WriteGuard(device, profile), profile);
        var curve = new FanCurveModel(10);
        for (int i = 0; i < 10; i++)
        {
            curve.Points[i].TempUp = 20 + i * 8;
            curve.Points[i].DutyPercent = i == 0 ? 0 : 30;
        }
        curve.Points[^1].DutyPercent = 100;
        using var linked = new LinkedFanController(fan, monitor, () => curve, () => curve);
        var tick = typeof(EcMonitor).GetMethod("OnTick", Private)!;
        void Tick() => tick.Invoke(monitor, new object?[] { null, EventArgs.Empty });
        int samples = 0, batches = 0;
        linked.Updated += () => samples++;
        fan.CurvesApplied += (_, _) => batches++;
        linked.Enable();
        Require(linked.IsEnabled && batches == 1, "Enable did not synchronously write fresh curves");
        Require(device.Writes.Count == 97, "Initial linked apply did not use one complete guarded curve batch");
        Require(Field<System.Windows.Forms.Timer>(monitor, "_timer").Interval == 1000, "Shared control cadence changed");
        foreach (bool tray in new[] { false, true })
        foreach (bool safety in new[] { false, true })
        {
            monitor.Lightweight = tray;
            bool safetyFirst = false, fanAfterFailure = false;
            Action<int?> safetySample = t => safetyFirst = t == 40 && !device.Reads.Contains(cpu);
            Action<(int? Cpu, int? Gpu)> brokenFan = _ => throw new IOException("fan subscriber failed");
            Action<(int? Cpu, int? Gpu)> followingFan = t => fanAfterFailure = t == (40, 40);
            if (safety) monitor.GpuTemperatureSample += safetySample;
            monitor.FanTemperatureSample += brokenFan;
            monitor.FanTemperatureSample += followingFan;
            int before = samples;
            device.Reads.Clear(); device.Writes.Clear();
            Tick();
            Require(!safety || safetyFirst, "GPU safety was not sampled before CPU polling");
            Require(fanAfterFailure, "Broken subscriber suppressed a later fan subscriber");
            Require(samples == before + 1, "Linked fan control did not run on this one-second tick");
            Require(device.Reads.Count(a => a == cpu) == 1 && device.Reads.Count(a => a == gpu) == 1, "Duplicated control/telemetry sensor reads");
            Require(device.Writes.Count == 0, "Stable linked duty/ramp rewrote EC curves");
            device.ReadOverride = a => a == cpu || a == gpu ? (byte)40 : throw new IOException("telemetry failed");
            for (int i = 0; i < 6; i++) { device.Reads.Clear(); Tick(); }
            Require(samples == before + 7 && Field<System.Windows.Forms.Timer>(monitor, "_timer").Enabled,
                "Unrelated telemetry failures stopped fan control");
            device.ReadOverride = null;
            if (safety) monitor.GpuTemperatureSample -= safetySample;
            monitor.FanTemperatureSample -= brokenFan;
            monitor.FanTemperatureSample -= followingFan;
        }
        int lastBatches = batches;
        device.Registers[cpu] = 41; Tick();
        Require(batches == lastBatches, "Insignificant temperature movement rewrote curves");
        device.Writes.Clear(); device.Registers[cpu] = 45; Tick();
        Require(batches == ++lastBatches && linked.CpuTemp == 45, "Headroom crossing did not update on the next sample");
        Require(device.Writes.Count == 97, "Headroom update wrote more than one curve batch");
        device.Writes.Clear(); device.Registers[gpu] = 90; Tick();
        Require(batches == ++lastBatches && linked.LinkedDuty > 30, "GPU duty rise did not update on the next sample");
        Require(device.Writes.Count == 97, "Duty update wrote more than one curve batch");
        Require(linked.WrittenCpuCurve!.Points[^1].DutyPercent == 100 && linked.WrittenGpuCurve!.Points[^1].DutyPercent == 100,
            "Crash-safety curve tails were lost");
        foreach (byte invalid in new byte[] { 0, 128, 255 })
        {
            device.Registers[gpu] = invalid;
            device.Writes.Clear(); Tick();
            Require(device.Writes.Count == 0 && batches == lastBatches, "Invalid GPU sensor rewrote safety-ramp curves");
        }
        device.Registers[gpu] = 40;
        device.ReadOverride = a => a == cpu ? throw new IOException("CPU sensor failed") : device.Registers.GetValueOrDefault(a);
        device.Writes.Clear(); Tick();
        Require(device.Writes.Count == 0, "Failed CPU sensor rewrote curves");
        device.ReadOverride = null;
        Tick(); Require(batches == ++lastBatches, "Valid readings did not recover fan control");
        device.WriteOverride = (_, _) => throw new IOException("partial curve write failed");
        device.Registers[cpu] = 70; Tick();
        Require(batches == lastBatches, "Failed curve write was reported as complete");
        device.WriteOverride = null;
        device.Registers[cpu] = 45; Tick();
        Require(batches == ++lastBatches, "Partial curve failure was skipped when returning to previous duty");
        linked.Enable(); // Re-enable must replace, not duplicate, the subscription.
        int beforeEnable = samples;
        Tick(); Require(samples == beforeEnable + 1, "Re-enable duplicated fan callbacks");
        linked.Disable();
        device.Reads.Clear(); device.Writes.Clear(); Tick();
        Require(!device.Reads.Contains(cpu) && !device.Reads.Contains(gpu) && device.Writes.Count == 0,
            "Disabled linked mode kept tray temperature polling/writes active");
        device.Registers[cpu] = 0;
        bool refused = false;
        try { linked.Enable(); } catch (IOException) { refused = true; }
        Require(refused && !linked.IsEnabled, "Invalid initial temperature falsely enabled linked control");
        device.Registers[cpu] = 40;
        linked.Enable();
        foreach (bool readFailure in new[] { true, false })
        {
            var original = new IOException(readFailure ? "initial sensor read failed" : "initial curve write failed");
            if (readFailure) device.ReadOverride = _ => throw original;
            else device.WriteOverride = (_, _) => throw original;
            Exception? caught = null;
            try { linked.Enable(); } catch (IOException ex) { caught = ex; }
            Require(ReferenceEquals(caught, original) && !linked.IsEnabled, "Enable hid the original failure or stayed enabled");
            device.ReadOverride = null; device.WriteOverride = null;
            device.Reads.Clear(); device.Writes.Clear(); Tick();
            Require(!device.Reads.Contains(cpu) && !device.Reads.Contains(gpu) && device.Writes.Count == 0,
                "Failed re-enable retained a control subscription");
            linked.Enable();
        }
        device.Writes.Clear(); linked.Dispose();
        Require(device.Writes.Count == 0, "Disposal rewrote crash-safety curves");
        device.Reads.Clear(); Tick();
        Require(!device.Reads.Contains(cpu) && !device.Reads.Contains(gpu), "Disposed linked controller retained polling subscription");
    }

    private static void RefreshTests()
    {
        using var parent = new Panel();
        using var monitorPanel = new MonitorPanel();
        parent.Controls.Add(monitorPanel);
        var fanLabel = Field<Label>(monitorPanel, "_lblCpuFan");
        parent.Visible = false;
        monitorPanel.OnMonitorUpdated(new() { CpuFanRpm = 1234 });
        Require(fanLabel.Text == "CPU Fan: --", "Hidden monitor panel changed labels");
        parent.Visible = true;
        monitorPanel.OnMonitorUpdated(new() { CpuFanRpm = 2345 });
        Require(fanLabel.Text.Contains("2345"), "Visible monitor panel did not refresh");

        int descriptions = 0;
        bool fail = false;
        var f = new GpuTemperatureFailsafe(() => 40, _ => { if (fail) throw new IOException("write failed"); }, _ => { },
            describe: _ => { descriptions++; return "GPU minimum"; });
        f.Configure(new() { Enabled = true });
        using var tab = new FailsafeTab(f);
        tab.RefreshConfiguration(saved: true);
        int before = descriptions;
        for (int i = 0; i < 20; i++) f.OnTemperatureSample(45);
        Require(descriptions == before, "Temperature samples rebuilt action/configuration descriptions");
        Require(Field<Label>(tab, "_temperature").Text.StartsWith("45"), "Live temperature stopped refreshing");
        f.OnTemperatureSample(null);
        Require(Field<Label>(tab, "_status").Text.Contains("1/5"), "Invalid-sample status did not refresh");
        var slider = Field<TrackBar>(tab, "_threshold");
        slider.Value = 70;
        Require(tab.HasPendingEdits && descriptions > before, "Edits did not refresh configuration preview");
        before = descriptions;
        f.OnTemperatureSample(46);
        Require(descriptions == before && slider.Value == 70, "Telemetry rebuilt or replaced unapplied edits");
        tab.Visible = false;
        f.OnTemperatureSample(47);
        Require(Field<Label>(tab, "_temperature").Text.StartsWith("46"), "Hidden failsafe tab refreshed labels");
        tab.Visible = true;
        Require(Field<Label>(tab, "_temperature").Text.StartsWith("47"), "Showing tab did not refresh current temperature");
        f.TestTrip();
        before = descriptions;
        for (int i = 0; i < 20; i++) f.OnTemperatureSample(40);
        Require(descriptions == before, "Stable held maintenance rebuilt configuration/history");
        fail = true;
        for (int i = 0; i < 5; i++) f.OnTemperatureSample(40);
        Require(Field<Label>(tab, "_lastTrip").Text.Contains("NOT VERIFIED"), "History did not refresh after failed verification");
        fail = false;
        for (int i = 0; i < 5; i++) f.OnTemperatureSample(40);
        Require(!Field<Label>(tab, "_lastTrip").Text.Contains("NOT VERIFIED"), "History did not refresh after recovery");
        f.Reset();
        Require(!Field<Button>(tab, "_reset").Enabled, "Reset button stayed enabled after acknowledgement");
    }
}
