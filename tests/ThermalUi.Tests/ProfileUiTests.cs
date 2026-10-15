using MechControl.Driver;
using MechControl.Features;
using MechControl.Monitoring;
using MechControl.Profile;
using MechControl.UI;

static partial class Program
{
    private static void ProfileAndLinkedUiTests()
    {
        string root = Path.Combine(@"C:\Temp\tandem", "practical-ui-" + Guid.NewGuid());
        Directory.CreateDirectory(root);
        try
        {
            var model = MonitorProfile();
            model.ProjectId = "test-model";
            foreach (var (name, addr) in new[] { ("fanControlMode", "0x751"), ("fanCurveEnable", "0x7C5"),
                ("tgpDbCtrl", "0x743"), ("tgp", "0x744"), ("dbTotalPower", "0x745"), ("dbMaxTgp", "0x746") })
                model.Registers[name] = addr;
            model.Limits["TGP"] = new() { Min = 80, Max = 100, Offset = 70, Encoding = "offset" };
            model.FanTable = new()
            {
                CpuTempUpBase = "0xF00", CpuTempDownBase = "0xF10", CpuDutyBase = "0xF20",
                GpuTempUpBase = "0xF30", GpuTempDownBase = "0xF40", GpuDutyBase = "0xF50"
            };
            UniversalDefaults.ApplyToProfile(model);
            var device = new EcDevice();
            device.Registers[model.GetRegister("cpuTemp")] = 40;
            device.Registers[model.GetRegister("gpuTemp")] = 40;
            var guard = new WriteGuard(device, model);
            var gpu = new GpuPowerControl(device, guard, model);
            var fan = new FanControl(device, guard, model);
            using var monitor = new EcMonitor(device, model);
            using var fanTab = new FanTab(fan, model, monitor);
            var curve = FanCurveModel.CreateGenericDefault();
            fanTab.SeedCurves(curve, curve.Clone());
            var checkbox = Field<CheckBox>(fanTab, "_chkLinkFans");
            int batches = 0;
            fan.CurvesApplied += (_, _) => batches++;
            checkbox.Checked = true; // Real UI wrapper: exactly one intended operation.
            Require(fanTab.LinkedEnabled && batches == 1, "User checkbox did not enable exactly once");
            fanTab.SetLinkedEnabled(true);
            Require(batches == 2, "Explicit re-enable must write fresh profile curves synchronously");
            checkbox.Checked = false;
            Require(!fanTab.LinkedEnabled && batches == 3, "User disable did not restore curves exactly once");
            fanTab.SetLinkedEnabled(false);
            Require(batches == 3, "Already-disabled checkbox caused extra writes");
            fanTab.SetLinkedEnabled(true);
            var original = new IOException("original linked write failure");
            device.Writes.Clear();
            device.WriteOverride = (_, _) => throw original;
            try { fanTab.SetLinkedEnabled(true); throw new Exception("Expected failed re-enable"); }
            catch (IOException ex) { Require(ReferenceEquals(ex, original), "Linked exception identity lost"); }
            Require(!fanTab.LinkedEnabled && !checkbox.Checked, "Failed re-enable left the UI/controller enabled");
            Require(device.Writes.Count == 1, "Checkbox repair accidentally called Disable and wrote a rollback");
            device.WriteOverride = null;
            device.Writes.Clear();
            typeof(EcMonitor).GetMethod("OnTick", Private)!.Invoke(monitor, new object?[] { null, EventArgs.Empty });
            Require(device.Writes.Count == 0, "Failed re-enable retained a subscription");

            var manager = new UserProfileManager(root);
            var f = new GpuTemperatureFailsafe(() => device.ReadByte(model.GetRegister("gpuTemp")),
                _ => gpu.ApplyGpuPower(80, false, 0), manager.SaveThermalProtection);
            gpu.Failsafe = f;
            using var failsafeTab = new FailsafeTab(f);
            Require(!failsafeTab.HasConfigurationWarning, "Empty warning UI was treated as a configuration error");
            int warningChanges = 0;
            failsafeTab.ConfigurationWarningChanged += () => warningChanges++;
            failsafeTab.ShowConfigurationWarning("Requested profile failed");
            failsafeTab.ShowConfigurationWarning("Requested profile failed");
            Require(warningChanges == 1 && failsafeTab.HasConfigurationWarning, "Warning presentation did not notify exactly once for changed text");
            using var bar = new ProfileBar(manager, null, gpu, fan, null, null, new LightingControl(), null, model, failsafeTab, fanTab);
            Require(Field<Label>(failsafeTab, "_saveHint").Text == "No saved profile loaded", "Clean startup falsely claims saved-profile identity");
            int applied = 0;
            bar.ProfileApplied += () => applied++;
            string path = Path.Combine(root, "userprofile_1.json");
            void RefuseLoad()
            {
                int before = applied;
                device.Writes.Clear();
                try { bar.LoadAndApplySlot(1, automatic: true); throw new Exception("Expected profile load refusal"); }
                catch (Exception ex) when (ex is IOException or InvalidOperationException or ArgumentOutOfRangeException) { }
                Require(applied == before && bar.ActiveSlot == 0 && device.Writes.Count == 0,
                    "Failed load was reported as applied or wrote hardware");
            }
            RefuseLoad();
            File.WriteAllText(path, "{broken");
            RefuseLoad();
            Require(manager.SlotExists(1), "Missing disappearing-slot fixture");
            File.Delete(path);
            File.WriteAllText(path + ".tmp", """{"gpu":{"tgp":90},"gpuFailsafe":{"enabled":true}}""");
            RefuseLoad();
            var profile = new UserProfile { ProjectId = "wrong-model", Gpu = new() { Tgp = 90 },
                GpuFailsafe = new() { Enabled = true } };
            manager.SaveProfile(1, profile);
            RefuseLoad();
            profile.ProjectId = model.ProjectId;
            profile.Gpu.Tgp = 125;
            manager.SaveProfile(1, profile);
            RefuseLoad();
            profile.ProjectId = ""; // Moved DTO preflight coverage exercises the actual load boundary.
            manager.SaveProfile(1, profile);
            RefuseLoad();
            profile.Gpu.Tgp = 90;
            manager.SaveProfile(1, profile);
            bar.LoadAndApplySlot(1, automatic: true);
            Require(!failsafeTab.HasConfigurationWarning, "Successful legacy-ID apply retained a resolved load warning");
            profile.ProjectId = model.ProjectId;
            profile.GpuFailsafe.ThresholdC = 30;
            manager.SaveProfile(2, profile);
            bar.LoadAndApplySlot(2, automatic: true);
            Require(Field<Label>(failsafeTab, "_saveHint").Text.Contains("not saved"), "Migrated threshold falsely claimed to be saved");
            Require(failsafeTab.HasConfigurationWarning && manager.ConfigurationWarning.Length > 0, "Migration diagnostic/warning missing");
            applied = 0;
            bar.LoadAndApplySlot(1, automatic: true);
            Require(applied == 1 && bar.ActiveSlot == 1 && f.Enabled && !f.IsTripped, "Valid profile did not arm its detector");
            Require(!failsafeTab.HasConfigurationWarning && manager.ConfigurationWarning.Length > 0, "Historical diagnostics were erased or redisplayed as an active warning");

            // A failed requested load does not erase an existing hold.
            f.TestTrip();
            failsafeTab.ShowConfigurationWarning("Unresolved configuration");
            try { bar.SaveCurrentProfile(); throw new Exception("Expected held-save refusal"); }
            catch (InvalidOperationException) { }
            Require(failsafeTab.HasConfigurationWarning, "Blocked save cleared an unresolved warning");
            File.WriteAllText(Path.Combine(root, "userprofile_2.json"), "{broken");
            try { bar.LoadAndApplySlot(2, automatic: true); throw new Exception("Expected corrupt load failure"); }
            catch (IOException) { }
            Require(f.IsTripped && bar.ActiveSlot == 1 && applied == 1, "Failed load erased hold or marked requested slot applied");
            bar.LoadAndApplySlot(1); // A deliberate profile load is the one-step way out of a trip.
            Require(!f.IsTripped && applied == 2 && gpu.ReadGpuPower().TgpWatts == 90, "Deliberate profile load did not clear the trip and restore power");

            // Fresh post-fan-write GPU check still catches a new threshold crossing
            // (one reading far enough over to lift the rolling average past 50 C).
            device.WriteOverride = (a, _) => { if (a == 0xF00) device.Registers[model.GetRegister("gpuTemp")] = 100; };
            try { bar.LoadAndApplySlot(1); throw new Exception("Expected late thermal trip"); }
            catch (InvalidOperationException) { }
            Require(f.IsTripped && gpu.ReadGpuPower().TgpWatts == 80 && bar.ActiveSlot == 0 && applied == 2,
                "Slow fan work bypassed the fresh GPU check or reported a failed apply as success");
            device.WriteOverride = null;
            device.Registers[model.GetRegister("gpuTemp")] = 40;
            for (int i = 0; i < GpuTemperatureFailsafe.AverageWindow; i++) f.OnTemperatureSample(40); // Time passes; average cools.
            f.Reset();
            Require(!f.IsTripped, "Reset after cooling failed");
            bar.LoadAndApplySlot(1);

            // Save a changed runtime configuration; a later settings failure is partial.
            f.Configure(new() { Enabled = true, ThresholdC = 55 });
            using (var locked = new FileStream(Path.Combine(root, "settings.json"), FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                try { bar.SaveCurrentProfile(); throw new Exception("Expected partial save failure"); }
                catch (IOException ex) { Require(ex.Message.Contains("was saved, but"), "Partial save claimed profile was unwritten"); }
            }
            Require(manager.LoadProfile(1)!.GpuFailsafe.ThresholdC == 55 && bar.ActiveSlot == 1 && failsafeTab.HasConfigurationWarning,
                "Partial profile save lost truthful slot/warning state");
            Require(Field<GpuFailsafeSettings>(failsafeTab, "_saved").ThresholdC == 50, "Failed settings save marked everything saved");
            bar.SaveCurrentProfile();
            Require(Field<GpuFailsafeSettings>(failsafeTab, "_saved").ThresholdC == 55, "Successful save not reflected");
            Require(!failsafeTab.HasConfigurationWarning, "Successful retry retained a resolved partial-save warning");
            fanTab.SetLinkedEnabled(true);
            device.Writes.Clear();
            fanTab.Dispose();
            Require(device.Writes.Count == 0, "Disposal wrote fan curves");

            var unsupported = new GpuTemperatureFailsafe(() => throw new Exception("Unexpected sensor access"),
                _ => throw new Exception("Unexpected emergency action"), _ => { }, supported: ThermalAction.None);
            File.WriteAllText(Path.Combine(root, "settings.json.tmp"), "{broken");
            var startupManager = new UserProfileManager(root, gpuRecoverySupported: false);
            string startupWarning = startupManager.ConfigurationWarning;
            Require(startupWarning.Length > 0 && startupManager.ThermalProtection.Held == ThermalAction.None,
                "Unreadable pending settings must warn without inventing an unsupported GPU hold");
            using var unsupportedTab = new FailsafeTab(unsupported, startupWarning);
            var warningLabel = Field<Label>(unsupportedTab, "_configurationWarning");
            Require(warningLabel.Text == startupWarning, "Startup warning missing before profile operations");
            Require(!Field<CheckBox>(unsupportedTab, "_enabled").Enabled && !Field<Button>(unsupportedTab, "_test").Enabled,
                "Required controller incorrectly enabled GPU controls on unsupported hardware");
            using var unsupportedBar = new ProfileBar(startupManager, null, null, null, null, null, new LightingControl(), null, model, unsupportedTab);
            try { unsupportedBar.LoadAndApplySlot(1); throw new Exception("Expected unavailable protection refusal"); }
            catch (InvalidOperationException) { }
            Require(unsupportedBar.ActiveSlot == 0 && device.Writes.Count == 0, "Unsupported detector bypassed profile authorization");
            unsupportedTab.ShowConfigurationWarning("Requested profile failed");
            Require(warningLabel.Text == startupWarning + "\nRequested profile failed", "Operation warning hid startup diagnostics");
            var disabled = manager.LoadProfile(1)!;
            disabled.GpuFailsafe.Enabled = false;
            manager.SaveProfile(1, disabled);
            unsupportedBar.LoadAndApplySlot(1, automatic: true);
            Require(unsupportedBar.ActiveSlot == 1 && !unsupported.Enabled && !unsupported.IsTripped,
                "Unsupported controller could not apply an ordinary disabled profile");
            Require(warningLabel.Text == startupWarning, "Automatic profile load cleared startup warning or retained resolved operation warning");
            unsupportedBar.SaveCurrentProfile();
            Require(warningLabel.Text == startupWarning, "Profile save cleared startup recovery warning");
            // A restart re-evaluates disk state; persisted recovery details still warn.
            Require(new UserProfileManager(root, gpuRecoverySupported: false).ConfigurationWarning.Length > 0,
                "Restart discarded persisted recovery diagnostics");
            File.WriteAllText(Path.Combine(root, "settings.json"), "{}");
            Require(new UserProfileManager(root, gpuRecoverySupported: false).ConfigurationWarning.Length == 0,
                "Clean restart did not re-evaluate repaired settings");
        }
        finally { Directory.Delete(root, true); }
    }
}
