using System.Text.Json;
using MechControl.Driver;
using MechControl.Features;
using MechControl.Monitoring;
using MechControl.Profile;

namespace MechControl.UI;

class MainForm : Form
{
    private readonly EcDevice _device;
    private readonly WriteGuard _guard;
    private readonly MachineProfile _profile;
    private readonly TabControl _tabs;
    private readonly EcMonitor _monitor;
    private readonly PerfCounterPublisher _perfCounters;
    private readonly StartupValidator _validator;

    private readonly PowerTab? _powerTab;
    private readonly FanTab? _fanTab;
    private readonly BatteryTab? _batteryTab;
    private readonly LightingTab _lightingTab;
    private readonly SystemInfoTab? _systemInfoTab;
    private readonly HidDevice _hidDevice;

    private readonly CpuPowerControl? _cpuControl;
    private readonly GpuPowerControl? _gpuControl;
    private readonly FanControl? _fanControl;
    private readonly FanSwitchSpeedControl? _switchSpeedControl;
    private readonly BatteryControl? _batteryControl;
    private readonly LightingControl _lightingControl;
    private readonly BleDevice? _bleDevice;
    private readonly LiquidCoolerTab? _liquidCoolerTab;
    private readonly UserProfileManager _profileManager;
    private readonly ProfileBar _profileBar;

    private readonly NotifyIcon _trayIcon;
    private readonly ContextMenuStrip _trayMenu;
    private bool _exitRequested;

    private readonly Icon _defaultTrayIcon;
    private readonly Icon[] _profileTrayIcons = new Icon[5];
    private Icon[]? _kekwaitFrames;
    private System.Threading.Timer? _trayAnimTimer;
    private int _trayAnimFrame;

    public MainForm(EcDevice device, WriteGuard guard, MachineProfile profile)
    {
        _device = device;
        _guard = guard;
        _profile = profile;

        // Hardware detection + logging
        HardwareDetection? hwDetect = null;
        try { hwDetect = new HardwareDetection(device); }
        catch (Exception ex) { DiagnosticLog.Error($"Hardware detection failed: {ex.Message}"); }

        DiagnosticLog.LogHardwareDetection(hwDetect);
        if (hwDetect != null)
        {
            DiagnosticLog.LogEcSpecDecoded(hwDetect);
            DiagnosticLog.LogExtendedIdentity(hwDetect);
        }
        DiagnosticLog.LogEcIdentity(device);
        DiagnosticLog.LogProfileDetection(profile, ProfileLoader.LastDetectionMethod);

        // Enrich profile from EC hardware detection.
        // Must happen before StartupValidator so TGP offset is correct for validation.
        // Track which limits came from ECSpec — these are a stronger signal than raw
        // register spot-checks, so the validator won't disable enriched features.
        HashSet<string> enrichedLimits = new();
        if (hwDetect != null)
        {
            // ECSpec-derived limits override conservative defaults (unverified only;
            // verified profiles have tested limits that should be trusted).
            var ecSpecLimits = hwDetect.ExtractLimits();
            if (!profile.Verified)
            {
                if (ecSpecLimits != null)
                {
                    foreach (var kvp in ecSpecLimits)
                        profile.Limits[kvp.Key] = kvp.Value;
                    enrichedLimits = new HashSet<string>(ecSpecLimits.Keys);
                    DiagnosticLog.Write($"Applied {ecSpecLimits.Count} ECSpec-derived limit(s): {string.Join(", ", ecSpecLimits.Keys)}");
                }
                else
                {
                    DiagnosticLog.Write("ECSpec limits not available — using conservative defaults");
                }
            }

            // CPU temp encoding is a hardware property (Intel=offset, AMD=absolute),
            // not a profile opinion. Apply to ALL profiles including verified ones.
            if (ecSpecLimits != null && ecSpecLimits.TryGetValue("cpuTempLimit", out var ecTempLimit)
                && ecTempLimit.Encoding == "direct")
            {
                profile.Limits["cpuTempLimit"] = ecTempLimit;
                profile.VerifyRedirect["0x786"] = new VerifyRedirectInfo
                {
                    ReadAddr = "0x463",
                    Transform = "direct"
                };
                DiagnosticLog.Write($"CPU temp encoding: absolute (ECSpec byte[3]={hwDetect.EcSpecBytes[3]}, max={ecTempLimit.Max}C)");
            }
            else if (hwDetect.EcSpecDecoded)
            {
                DiagnosticLog.Write($"CPU temp encoding: inverted_offset (ECSpec byte[3]={hwDetect.EcSpecBytes[3]}, TjMax=105)");
            }

            // PL4 encoding (CPU_PL4_Double_Flag, 0x727 bit 7) is a hardware fact.
            if (profile.Limits.TryGetValue("PL4", out var pl4Limit))
                pl4Limit.Encoding = hwDetect.Pl4Doubled ? "double" : "direct";

            // Per-mode PL defaults from EC registers (all models).
            // Profile modeDefaults (hand-tuned with fan curves + GPU settings) take priority.
            if (profile.ModeDefaults == null)
            {
                var ecModes = hwDetect.ExtractModeDefaults(profile.CpuTempEncoding == "direct");
                if (ecModes != null)
                {
                    profile.ModeDefaults = ecModes;
                    DiagnosticLog.Write($"Auto-populated {ecModes.Count} mode default(s) from EC: {string.Join(", ", ecModes.Keys)}");
                }
                else
                {
                    DiagnosticLog.Write("No valid mode defaults found in EC registers");
                }
            }

            DiagnosticLog.LogModeDefaults(hwDetect, profile);
        }
        DiagnosticLog.LogEffectiveLimits(profile);

        // Titlebar
        string verifiedTag = profile.Verified ? "" : " (U)";
        string boardTag = ProfileLoader.LastBaseboard != null ? $" | {ProfileLoader.LastBaseboard}" : "";
        Text = hwDetect != null
            ? $"MechControl \u2014 {profile.ProjectId}{verifiedTag} [{hwDetect.ToTitleString()}{boardTag}]"
            : $"MechControl \u2014 {profile.ProjectId}{verifiedTag}";
        Size = new System.Drawing.Size(900, 700);
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new System.Drawing.Size(750, 550);

        // Tab control fills remaining space
        _tabs = new TabControl { Dock = DockStyle.Fill };
        Controls.Add(_tabs);

        // Monitor panel docked at bottom (reads only — always safe)
        _monitor = new EcMonitor(device, profile);
        var monitorPanel = new MonitorPanel { Dock = DockStyle.Bottom };
        _monitor.Updated += monitorPanel.OnMonitorUpdated;
        _monitor.Stopped += msg => MessageBox.Show(msg, "Monitor Error",
            MessageBoxButtons.OK, MessageBoxIcon.Warning);

        // Publish fan data to Windows Performance Counters (for MSI Afterburner overlay etc.)
        // Runs its own timer — reads only fan RPM/duty (6 EC bytes), independent of EcMonitor.
        _perfCounters = new PerfCounterPublisher(device, profile);
        _perfCounters.Start();

        // Log feature registers before init
        DiagnosticLog.LogFeatureRegisters(device, profile);

        // EC initialization
        var ecActivation = new EcActivationControl(device, guard, profile);
        var initErrors = ecActivation.InitializeEc();
        if (initErrors.Count > 0)
        {
            string errorList = string.Join("\n", initErrors);
            MessageBox.Show(
                $"EC initialization had {initErrors.Count} error(s). Some features may not work:\n\n{errorList}",
                "EC Init Warning", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        Controls.Add(monitorPanel);

        // Single fan snapshot — one read feeds detection, diagnostic dump,
        // panel seeding, and the startup custom-mode decision.
        var fanSnapshot = FanSnapshot.Read(device, profile.FanTable);
        DiagnosticLog.LogFanTable(fanSnapshot);

        // --- Hardware capability detection from the snapshot ---
        // The register map is universal but hardware capabilities differ per model.
        // Only apply detection overrides when the snapshot looks like a real curve;
        // cold-boot / wiped-EC state (all 0x00 or all 0xFF) would otherwise corrupt
        // the profile defaults with garbage.
        {
            var ft = profile.FanTable;

            DiagnosticLog.Section("Fan Table Detection");

            if (fanSnapshot.IsValid())
            {
                int detectedActive = fanSnapshot.DetectCpuActive();
                if (detectedActive != ft.ActiveEntries)
                {
                    DiagnosticLog.Write($"Active entries: detected {detectedActive} (profile default was {ft.ActiveEntries})");
                    ft.ActiveEntries = detectedActive;
                }
                else
                {
                    DiagnosticLog.Write($"Active entries: {detectedActive} (matches profile)");
                }

                // GPU presence: only meaningful when the snapshot is otherwise
                // sane. The "all 0xFF" rule below would false-negative against
                // a wiped GPU table on a real GPU laptop.
                bool? gpuDetected = fanSnapshot.DetectGpuPresent();
                if (gpuDetected.HasValue)
                {
                    ft.HasGpuFan = gpuDetected.Value;
                    DiagnosticLog.Write($"GPU fan table: {(gpuDetected.Value ? "present (has curve data)" : "not detected (all 0xFF)")}");
                }
                else
                {
                    DiagnosticLog.Write($"GPU fan table: ambiguous — keeping profile default HasGpuFan={ft.HasGpuFan}");
                }
            }
            else
            {
                DiagnosticLog.Warn($"Fan table detection skipped — snapshot is not a valid curve " +
                    $"(cold boot or wiped EC). Keeping profile defaults: activeEntries={ft.ActiveEntries}, " +
                    $"hasGpuFan={ft.HasGpuFan}");
            }

            if (ft.Format != "RamFan1")
                DiagnosticLog.Warn($"Fan table format '{ft.Format}' is not RamFan1 — fan curve editing will be disabled");
        }

        // Startup validation — determines which features are safe to enable
        _validator = new StartupValidator(device, profile,
            hwDetect?.ModuleIdGpu ?? 0xFF, profile.FanTable.HasGpuFan, enrichedLimits);
        _validator.Validate();

        // Log disabled features for unverified profiles (no popup — check diagnostic log)
        if (!profile.Verified && _validator.DisabledFeatures.Count > 0)
        {
            DiagnosticLog.Write($"Disabled features for unverified profile:");
            foreach (var kv in _validator.DisabledFeatures)
                DiagnosticLog.Write($"  - {kv.Key}: {kv.Value}");
        }

        // Power tab (CPU + GPU) — conditional on validation
        if (_validator.IsFeatureEnabled("cpuPower") || _validator.IsFeatureEnabled("gpuPower"))
        {
            if (_validator.IsFeatureEnabled("cpuPower"))
                _cpuControl = new CpuPowerControl(device, guard, profile);
            if (_validator.IsFeatureEnabled("gpuPower"))
                _gpuControl = new GpuPowerControl(device, guard, profile);
            _powerTab = new PowerTab(_cpuControl, _gpuControl, profile, _validator);
            var powerPage = new TabPage("Power");
            _powerTab.Dock = DockStyle.Fill;
            powerPage.Controls.Add(_powerTab);
            _tabs.TabPages.Add(powerPage);
        }

        // Fan Control tab — conditional on validation
        if (_validator.IsFeatureEnabled("fanControl"))
        {
            _fanControl = new FanControl(device, guard, profile);
            if (profile.Registers.ContainsKey("fanSwitchSpeed"))
                _switchSpeedControl = new FanSwitchSpeedControl(device, guard, profile);
            _fanTab = new FanTab(_fanControl, profile, device, _switchSpeedControl);
            _monitor.Updated += _fanTab.OnMonitorUpdated;
            var fanPage = new TabPage("Fan Control");
            _fanTab.Dock = DockStyle.Fill;
            fanPage.Controls.Add(_fanTab);
            _tabs.TabPages.Add(fanPage);
        }

        // Battery tab — conditional on validation
        if (_validator.IsFeatureEnabled("battery"))
        {
            _batteryControl = new BatteryControl(device, guard, profile);
            _batteryTab = new BatteryTab(_batteryControl, profile);
            var batteryPage = new TabPage("Battery");
            _batteryTab.Dock = DockStyle.Fill;
            batteryPage.Controls.Add(_batteryTab);
            _tabs.TabPages.Add(batteryPage);
        }

        // Lighting tab (Logo + Keyboard + Light Bar via HID)
        _hidDevice = new HidDevice();
        bool hidOk = false;
        try { hidOk = _hidDevice.Open(); }
        catch (Exception ex) { DiagnosticLog.Error($"HID init failed: {ex.Message}"); }
        DiagnosticLog.Write($"HID initialized: {hidOk}");
        DiagnosticLog.LogHidDevices(_hidDevice);

        _lightingControl = new LightingControl(_hidDevice, device, guard, profile);
        var dimmingControl = new LocalDimmingControl(device);
        _lightingTab = new LightingTab(_lightingControl, profile, dimmingControl);
        var lightingPage = new TabPage("Lighting");
        _lightingTab.Dock = DockStyle.Fill;
        lightingPage.Controls.Add(_lightingTab);
        _tabs.TabPages.Add(lightingPage);

        // Liquid Cooler tab — conditional on EC 0x7A4 bit 7
        if (hwDetect?.SupportsLiquidCooling == true)
        {
            _bleDevice = new BleDevice();
            _liquidCoolerTab = new LiquidCoolerTab(_bleDevice);
            var lcPage = new TabPage("Liquid Cooler");
            _liquidCoolerTab.Dock = DockStyle.Fill;
            lcPage.Controls.Add(_liquidCoolerTab);
            _tabs.TabPages.Add(lcPage);
            DiagnosticLog.Write("Liquid cooler tab enabled (0x7A4 bit 7 set)");
        }
        else
        {
            DiagnosticLog.Write("Liquid cooler tab not shown (0x7A4 bit 7 clear or HW detection failed)");
        }

        // System Info tab — read-only, driven by EcMonitor
        _systemInfoTab = new SystemInfoTab(ProfileLoader.LastBaseboard, ProfileLoader.LastCpuName);
        _monitor.Updated += _systemInfoTab.OnMonitorUpdated;
        var sysInfoPage = new TabPage("System Info");
        _systemInfoTab.Dock = DockStyle.Fill;
        sysInfoPage.Controls.Add(_systemInfoTab);
        _tabs.TabPages.Add(sysInfoPage);

        // Profile bar — docked below menu bar
        _profileManager = new UserProfileManager();
        _profileBar = new ProfileBar(
            _profileManager, _cpuControl, _gpuControl, _fanControl,
            _switchSpeedControl, _batteryControl, _lightingControl, dimmingControl, profile, _fanTab,
            _liquidCoolerTab);
        _profileBar.ProfileApplied += RefreshAllTabs;
        _profileBar.RestoreDefaultsClicked += RestoreDefaults_Click;
        _profileBar.GamingDefaultsClicked += (s, e) => ApplyModeDefaults("gaming", "Gaming");
        _profileBar.OfficeDefaultsClicked += (s, e) => ApplyModeDefaults("office", "Office");
        _profileBar.BatterySaverDefaultsClicked += (s, e) => ApplyModeDefaults("batterySaver", "Battery Saver");
        Controls.Add(_profileBar);

        // Seed panels with a fallback hierarchy so the user always has a
        // starting point to edit, even on cold-boot / wiped-EC state where
        // the snapshot is all 0x00 or all 0xFF:
        //   1. Snapshot        (normal path — EC has a real curve)
        //   2. profile default (per-model curated curve from JSON)
        //   3. Generic default (baked-in 4-point starter — last resort)
        // The non-snapshot paths get a popup warning below, but only when
        // no user profile is about to overwrite the panels anyway.
        string seedSource = "none";
        if (fanSnapshot.IsValid())
        {
            _fanTab?.SeedCurvesFromSnapshot(fanSnapshot);
            seedSource = "snapshot";
            DiagnosticLog.Write("Fan curve seed source: snapshot");
        }
        else if (_fanTab != null && profile.FanTable.Format == "RamFan1")
        {
            FanCurveModel cpuFallback;
            FanCurveModel gpuFallback;
            var dfc = profile.DefaultFanCurve;
            if (dfc?.Cpu != null && dfc?.Gpu != null)
            {
                cpuFallback = FanCurveModelFromArrays(dfc.Cpu);
                gpuFallback = FanCurveModelFromArrays(dfc.Gpu);
                seedSource = "profileDefault";
            }
            else
            {
                cpuFallback = FanCurveModel.CreateGenericDefault();
                gpuFallback = FanCurveModel.CreateGenericDefault();
                seedSource = "genericDefault";
            }
            _fanTab.SeedCurves(cpuFallback, gpuFallback);
            DiagnosticLog.Write($"Fan curve seed source: {seedSource} (snapshot invalid — cold boot or wiped EC)");
        }
        _fanTab?.SetCurveEnableCheckbox(fanSnapshot.IsCustomCurveModeActive());

        // Auto-load last user profile (overwrites panels with saved curves).
        // If no user profile: leave EC alone when snapshot looks valid, or park
        // custom curve mode off when the EC fan table is garbage.
        bool userProfileApplied = false;
        int lastSlot = _profileManager.LastUsedSlot;
        if (lastSlot > 0 && _profileManager.SlotExists(lastSlot))
        {
            try
            {
                _profileBar.LoadAndApplySlot(lastSlot);
                DiagnosticLog.Write($"Auto-loaded user profile slot {lastSlot}");
                userProfileApplied = true;
            }
            catch (Exception ex)
            {
                DiagnosticLog.Warn($"Auto-load slot {lastSlot} failed: {ex.Message}");
            }
        }
        else if (_fanControl != null && !fanSnapshot.IsValid())
        {
            // No user profile and EC fan table is garbage — park custom mode off.
            try
            {
                _fanControl.SetCustomFanCurveMode(false);
                _fanTab?.SetCurveEnableCheckbox(false);
                DiagnosticLog.Warn("EC fan table is invalid — custom fan curve mode disabled");
            }
            catch (Exception ex)
            {
                DiagnosticLog.Warn($"Failed to disable custom fan curve mode: {ex.Message}");
            }
        }

        // Post-write snapshot dump. WriteGuard already verifies fan-table bytes
        // on write; this re-read exists for two reasons:
        //   1. Diagnostic value — a post-write dump in the log alongside the
        //      pre-init dump makes it easy to see what landed on the EC.
        //   2. 0x7C6 is in SkipVerify (it's a mode flag), so this is the only
        //      way to confirm the custom-mode write actually stuck.
        if (userProfileApplied)
        {
            try
            {
                var verify = FanSnapshot.Read(device, profile.FanTable);
                DiagnosticLog.LogFanTable(verify, "Fan Table Dump (post-write)");
            }
            catch (Exception ex)
            {
                DiagnosticLog.Warn($"Fan table verify-read failed: {ex.Message}");
            }
        }
        else if (seedSource == "profileDefault" || seedSource == "genericDefault")
        {
            // Cold-boot / wiped EC, no user profile auto-loaded. Panels are
            // seeded from a fallback but nothing has been written to the EC
            // yet — tell the user so they know to review and Apply.
            string sourceText = seedSource == "profileDefault"
                ? "the profile's default fan curve"
                : "a generic starter fan curve";
            MessageBox.Show(
                "The EC fan table was in an uninitialized state (cold boot or wiped EC).\n\n" +
                $"The fan curve panels have been seeded from {sourceText}.\n" +
                "Custom fan curve mode is currently disabled.\n\n" +
                "Review the curves in the Fan Control tab, then use the Apply buttons to write them to the EC and enable custom mode.",
                "Fan Curves Seeded from Fallback",
                MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        // Tab-switch refresh: reload values when switching tabs
        _tabs.SelectedIndexChanged += (s, e) =>
        {
            RefreshActiveTab();
            // Start/stop battery telemetry timer based on active tab
            if (_batteryTab != null)
            {
                var page = _tabs.SelectedTab;
                if (page != null && page.Controls.Count > 0 && page.Controls[0] is BatteryTab)
                    _batteryTab.StartTelemetry();
                else
                    _batteryTab.StopTelemetry();
            }
        };

        // Read current values on load (monitor stays paused until window is shown)
        Load += async (s, e) =>
        {
            _powerTab?.RefreshFromEc();
            _trayIcon.Visible = true;

            // BLE auto-connect (runs regardless of which tab is active)
            if (_liquidCoolerTab != null)
                await _liquidCoolerTab.InitAsync();
        };

        FormClosing += (s, e) =>
        {
            if (!_exitRequested && e.CloseReason == CloseReason.UserClosing)
            {
                e.Cancel = true;
                HideToTray();
                return;
            }

            _trayIcon.Visible = false;
            _batteryTab?.StopTelemetry();
            _monitor.Dispose();
            _perfCounters.Dispose();
            DiagnosticLog.Close();
        };

        // System tray icon
        _trayMenu = new ContextMenuStrip();
        _trayMenu.Items.Add("Show", null, (s, e) => ShowFromTray());
        _trayMenu.Items.Add("-");
        _trayMenu.Items.Add("-");
        _trayMenu.Items.Add("Exit", null, (s, e) => { _exitRequested = true; Close(); });

        _defaultTrayIcon = Icon.ExtractAssociatedIcon(Application.ExecutablePath) ?? SystemIcons.Application;
        InitTrayProfileIcons();

        _trayIcon = new NotifyIcon
        {
            Text = "MechControl",
            Icon = _defaultTrayIcon,
            ContextMenuStrip = _trayMenu
        };
        _trayIcon.MouseClick += (s, e) =>
        {
            if (e.Button == MouseButtons.Left)
                ShowFromTray();
        };

        // Tray profile submenu — refresh on profile load, save, or rename
        _profileBar.ProfileApplied += RefreshTrayProfileMenu;
        _profileBar.ProfileListChanged += RefreshTrayProfileMenu;
        RefreshTrayProfileMenu();
        _profileBar.ProfileApplied += UpdateTrayIcon;
        UpdateTrayIcon();

        // Start hidden in tray
        WindowState = FormWindowState.Minimized;
        ShowInTaskbar = false;
    }

    private void RefreshActiveTab()
    {
        var page = _tabs.SelectedTab;
        if (page == null || page.Controls.Count == 0) return;

        var child = page.Controls[0];
        if (child is PowerTab power) power.RefreshFromEc();
        else if (child is FanTab fan) fan.RefreshFromEc();
        else if (child is BatteryTab bat) bat.RefreshFromEc();
        else if (child is LightingTab lighting) lighting.RefreshFromEc();
    }

    private void ShowFromTray()
    {
        Show();
        ShowInTaskbar = true;
        WindowState = FormWindowState.Normal;
        _monitor.Start();
        // Restart battery telemetry if battery tab is active
        if (_batteryTab != null && _tabs.SelectedTab?.Controls.Count > 0
            && _tabs.SelectedTab.Controls[0] is BatteryTab)
            _batteryTab.StartTelemetry();
        Activate();
    }

    private void HideToTray()
    {
        Hide();
        ShowInTaskbar = false;
        _monitor.Stop();
        _batteryTab?.StopTelemetry();
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        if (WindowState == FormWindowState.Minimized)
            HideToTray();
    }

    private void RefreshAllTabs()
    {
        _powerTab?.RefreshFromEc();
        _fanTab?.RefreshFromEc();
        _batteryTab?.RefreshFromEc();
        _lightingTab.RefreshFromEc();
    }

    private void RefreshTrayProfileMenu()
    {
        // Remove old profile items (between the two separators)
        while (_trayMenu.Items.Count > 4 && _trayMenu.Items[2] is not ToolStripSeparator)
            _trayMenu.Items.RemoveAt(2);

        var names = _profileManager.GetProfileNames();
        for (int i = 4; i >= 0; i--)
        {
            int slot = i + 1;
            bool exists = _profileManager.SlotExists(slot);
            var item = new ToolStripMenuItem(names[i])
            {
                Enabled = exists,
                Checked = slot == _profileBar.ActiveSlot
            };
            if (exists)
                item.Click += (s, e) =>
                {
                    try { _profileBar.LoadAndApplySlot(slot); }
                    catch (Exception ex)
                    {
                        MessageBox.Show($"Failed to load profile:\n{ex.Message}",
                            "Load Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    }
                };
            _trayMenu.Items.Insert(2, item);
        }
    }

    // ── Tray icon per-profile ────────────────────────────────────────

    private void InitTrayProfileIcons()
    {
        Color[] colors =
        {
            Color.FromArgb(76, 175, 80),   // 1: green
            Color.FromArgb(33, 150, 243),  // 2: blue
            Color.FromArgb(255, 193, 7),   // 3: amber
            Color.FromArgb(255, 87, 34)    // 4: deep orange
        };
        for (int i = 0; i < 4; i++)
            _profileTrayIcons[i] = CreateProfileIcon(colors[i], (i + 1).ToString());

        _kekwaitFrames = LoadKekwaitFrames();
        _profileTrayIcons[4] = _kekwaitFrames.Length > 0
            ? (Icon)_kekwaitFrames[^1].Clone()
            : CreateProfileIcon(Color.FromArgb(244, 67, 54), "5");
    }

    private void UpdateTrayIcon()
    {
        _trayAnimTimer?.Change(Timeout.Infinite, Timeout.Infinite);
        int slot = _profileBar.ActiveSlot;
        if (slot < 1 || slot > 5)
        {
            _trayIcon.Icon = _defaultTrayIcon;
            return;
        }
        if (slot == 5 && _kekwaitFrames != null && _kekwaitFrames.Length > 1)
        {
            _trayAnimFrame = 0;
            _trayIcon.Icon = _kekwaitFrames[0];
            _trayAnimTimer?.Dispose();
            _trayAnimTimer = new System.Threading.Timer(OnTrayAnimationTick, null, 67, 67);
        }
        else
        {
            _trayIcon.Icon = _profileTrayIcons[slot - 1];
        }
    }

    private void OnTrayAnimationTick(object? state)
    {
        int frame = Interlocked.Increment(ref _trayAnimFrame);
        if (_kekwaitFrames == null || frame >= _kekwaitFrames.Length)
        {
            _trayAnimTimer?.Change(Timeout.Infinite, Timeout.Infinite);
            return;
        }
        // Guard against a tick that's already been queued when the user switches
        // away from slot 5 — without this, the queued BeginInvoke runs after
        // UpdateTrayIcon swapped the icon and reverts it to a kekwait frame.
        try { BeginInvoke(() => {
            if (_profileBar.ActiveSlot == 5)
                _trayIcon.Icon = _kekwaitFrames[frame];
        }); }
        catch { }
    }

    private static Icon[] LoadKekwaitFrames()
    {
        var asm = System.Reflection.Assembly.GetExecutingAssembly();
        var frames = new List<Icon>();
        for (int i = 0; ; i++)
        {
            using var stream = asm.GetManifestResourceStream($"MechControl.Resources.TrayIcons.kekwait_{i:D3}.png");
            if (stream == null) break;
            using var bmp = new Bitmap(stream);
            frames.Add(IconFromBitmap(bmp));
        }
        return frames.ToArray();
    }

    private static Icon CreateProfileIcon(Color bgColor, string text)
    {
        using var bmp = new Bitmap(32, 32);
        using var g = Graphics.FromImage(bmp);
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
        g.Clear(Color.Transparent);
        using var brush = new SolidBrush(bgColor);
        g.FillEllipse(brush, 1, 1, 30, 30);
        using var font = new Font("Segoe UI", 15, FontStyle.Bold, GraphicsUnit.Pixel);
        using var sf = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };
        g.DrawString(text, font, Brushes.White, new RectangleF(0, 0, 32, 32), sf);
        return IconFromBitmap(bmp);
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool DestroyIcon(IntPtr hIcon);

    private static Icon IconFromBitmap(Bitmap bmp)
    {
        IntPtr hIcon = bmp.GetHicon();
        var tmp = Icon.FromHandle(hIcon);
        var icon = (Icon)tmp.Clone();
        tmp.Dispose();
        DestroyIcon(hIcon);
        return icon;
    }

    // ── Restore / Mode Defaults ──────────────────────────────────────

    private void RestoreDefaults_Click(object? sender, EventArgs e)
    {
        if (MessageBox.Show(
            "This will reset all EC settings to firmware defaults. Continue?",
            "Restore Firmware Defaults",
            MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
            return;

        var errors = new List<string>();

        void Write(int addr, byte value)
        {
            try { _guard.Write(addr, value); }
            catch (Exception ex) { errors.Add($"0x{addr:X3}: {ex.Message}"); }
        }
        void WriteReg(string name, byte value) => Write(_profile.GetRegister(name), value);

        // CPU/GPU power: 0 = hand control back to EC firmware
        WriteReg("PL1", 0);
        WriteReg("PL2", 0);
        WriteReg("PL4", 0);
        WriteReg("tgpDbCtrl", 0);
        WriteReg("tgp", 0);
        WriteReg("dbTotalPower", 0);
        WriteReg("dbMaxTgp", 0);

        // CPU temp limit: max allowed = no throttling
        var tl = _profile.Limits["cpuTempLimit"];
        byte tempByte = tl.Encoding == "direct"
            ? RegisterEncoding.EncodeCpuTempLimitDirect(tl.Max)
            : RegisterEncoding.EncodeCpuTempLimit(tl.Max, tl.TjMax ?? 105);
        WriteReg("cpuTempLimitWrite", tempByte);

        // Fan: boost off, custom curve disabled (EC internal fan control)
        WriteReg("fanControlMode", 0);
        if (_fanControl != null)
        {
            try { _fanControl.SetCustomFanCurveMode(false); }
            catch (Exception ex) { errors.Add($"customFanCurveMode: {ex.Message}"); }
        }
        else
        {
            _guard.WriteBitfield(_profile.GetRegister("fanCurveEnable"), 0x80, 0x00);
        }

        // Fan table: restore from profile default curve
        var dfc = _profile.DefaultFanCurve;
        if (dfc?.Cpu != null && dfc?.Gpu != null)
        {
            var ft = _profile.FanTable;
            int entries = ft.EntriesPerArray;
            void WriteFanArray(int baseAddr, List<int> values)
            {
                for (int i = 0; i < entries && i < values.Count; i++)
                    Write(baseAddr + i, (byte)values[i]);
            }
            WriteFanArray(ft.CpuTempUpAddr, dfc.Cpu.TempUp);
            WriteFanArray(ft.CpuTempDownAddr, dfc.Cpu.TempDown);
            WriteFanArray(ft.CpuDutyAddr, dfc.Cpu.Duty);
            WriteFanArray(ft.GpuTempUpAddr, dfc.Gpu.TempUp);
            WriteFanArray(ft.GpuTempDownAddr, dfc.Gpu.TempDown);
            WriteFanArray(ft.GpuDutyAddr, dfc.Gpu.Duty);
        }

        // Battery: first mode (High Capacity)
        if (_profile.BatteryModes.Count > 0)
        {
            var bf = _profile.Bitfields["batteryMode"];
            _guard.WriteBitfield(bf.GetRegisterAddr(), bf.GetMask(), _profile.BatteryModes[0].GetValue());
        }

        // Logo LED
        var dl = _profile.DefaultLogo;
        if (dl != null)
        {
            WriteReg("logoTrigger", 0x00);
            WriteReg("logoR", dl.R);
            WriteReg("logoG", dl.G);
            WriteReg("logoB", dl.B);
            _guard.WriteBitfield(_profile.GetRegister("logoOnOff"), 0x80, dl.OnOff);
        }

        RefreshAllTabs();

        if (errors.Count > 0)
            MessageBox.Show($"{errors.Count} register(s) failed:\n" + string.Join("\n", errors),
                "Restore Defaults", MessageBoxButtons.OK, MessageBoxIcon.Warning);
    }

    private void ApplyModeDefaults(string modeKey, string modeName)
    {
        // ModeDefaults is populated at startup from either static profile JSON
        // (with fan curves + GPU settings) or EC registers (PL only).
        if (_profile.ModeDefaults == null || !_profile.ModeDefaults.TryGetValue(modeKey, out var mode))
        {
            MessageBox.Show(
                $"No {modeName} defaults available.\n\nThe EC did not have valid power limits for this mode.",
                "Mode Defaults", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        bool hasCurves = mode.FanCurve != null;
        bool hasGpu = mode.CTGP.HasValue;
        string details = $"This will set power limits" +
            (hasGpu ? ", GPU TGP" : "") +
            (hasCurves ? ", and fan curves" : "") +
            $" to OEM {modeName} presets.";

        var result = MessageBox.Show(
            $"Apply {modeName} mode defaults?\n\n{details}",
            $"{modeName} Defaults",
            MessageBoxButtons.YesNo, MessageBoxIcon.Question);

        if (result != DialogResult.Yes) return;

        try
        {
            // Apply CPU power limits
            if (_cpuControl != null)
            {
                var current = _cpuControl.ReadCpuPower();
                int pl4 = mode.PL4 ?? current.PL4Watts;
                int tempLimit = mode.TccOffset ?? current.TempLimitC;
                _cpuControl.ApplyCpuPower(mode.PL1, mode.PL2, pl4, tempLimit);
            }

            // Apply GPU power (CTGP/DynamicBoost) if values provided
            if (_gpuControl != null && mode.CTGP.HasValue)
            {
                bool dbEnabled = mode.DynamicBoost.HasValue && mode.DynamicBoost.Value > 0;
                int dbWatts = mode.DynamicBoost ?? 0;
                _gpuControl.ApplyGpuPower(mode.CTGP.Value, dbEnabled, dbWatts);
            }

            // Apply fan curves if provided (only from static profile data)
            if (_fanControl != null && mode.FanCurve != null)
            {
                var cpuModel = mode.FanCurve.Cpu != null ? FanCurveModelFromArrays(mode.FanCurve.Cpu) : null;
                var gpuModel = mode.FanCurve.Gpu != null ? FanCurveModelFromArrays(mode.FanCurve.Gpu) : null;

                if (cpuModel != null)
                {
                    _fanControl.ApplyCurves(cpuModel, gpuModel, true);
                }
            }

            RefreshAllTabs();

            MessageBox.Show($"{modeName} defaults applied successfully.",
                $"{modeName} Defaults", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to apply {modeName} defaults:\n{ex.Message}",
                "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private static FanCurveModel FanCurveModelFromArrays(FanCurveArrays arrays)
    {
        var model = FanCurveModel.FromRawArrays(arrays.TempUp, arrays.TempDown, arrays.Duty);
        return model.Points.Length > 0 ? model : FanCurveModel.CreateGenericDefault();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            // BLE shutdown runs BEFORE base.Dispose so the LiquidCoolerTab's
            // reconnect timer is still live when StopReconnectTimer/SendResetAsync
            // execute. Task.Run avoids deadlock by running async work off the
            // UI SynchronizationContext. Timeout prevents hang if BLE is stuck.
            if (_liquidCoolerTab != null)
            {
                try { Task.Run(() => _liquidCoolerTab.ShutdownAsync()).Wait(3000); }
                catch { }
            }

            _trayAnimTimer?.Change(Timeout.Infinite, Timeout.Infinite);
            _trayAnimTimer?.Dispose();
            _trayIcon.Dispose();
            _trayMenu.Dispose();
            for (int i = 0; i < 5; i++)
                _profileTrayIcons[i]?.Dispose();
            if (_kekwaitFrames != null)
                foreach (var f in _kekwaitFrames)
                    f?.Dispose();
        }
        base.Dispose(disposing); // FanTab restores curves to EC via LinkedFanController.Dispose
        if (disposing)
        {
            _bleDevice?.Dispose();
            _hidDevice.Dispose();
            _device.Dispose(); // EC device must be last — child controls may write during disposal
        }
    }
}
