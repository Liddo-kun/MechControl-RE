using MechControl.Features;
using MechControl.Profile;

namespace MechControl.UI;

class ProfileBar : UserControl
{
    private readonly UserProfileManager _manager;
    private readonly Button[] _slotButtons = new Button[5];
    private readonly Button _saveButton;
    private int _activeSlot; // 1-5, 0 = none

    /// <summary>Currently highlighted profile slot (1-5, 0 = none).</summary>
    public int ActiveSlot => _activeSlot;

    // Feature controllers for save/load (nullable for unverified profiles)
    private readonly CpuPowerControl? _cpuControl;
    private readonly GpuPowerControl? _gpuControl;
    private readonly FanControl? _fanControl;
    private readonly FanSwitchSpeedControl? _switchSpeedControl;
    private readonly BatteryControl? _batteryControl;
    private readonly LightingControl _lightingControl;
    private readonly LocalDimmingControl? _dimmingControl;
    private readonly MachineProfile _profile;
    private readonly FanTab? _fanTab;
    private readonly LiquidCoolerTab? _liquidCoolerTab;
    private readonly FailsafeTab _failsafeTab;


    /// <summary>Raised after a profile is loaded and applied to EC.</summary>
    public event Action? ProfileApplied;

    /// <summary>Raised when a profile is saved or renamed (tray menu refresh).</summary>
    public event Action? ProfileListChanged;

    /// <summary>Raised when the Restore Defaults button is clicked.</summary>
    public event EventHandler? RestoreDefaultsClicked;

    /// <summary>Raised when the Gaming Defaults button is clicked.</summary>
    public event EventHandler? GamingDefaultsClicked;

    /// <summary>Raised when the Office Defaults button is clicked.</summary>
    public event EventHandler? OfficeDefaultsClicked;

    /// <summary>Raised when the Battery Saver Defaults button is clicked.</summary>
    public event EventHandler? BatterySaverDefaultsClicked;

    public ProfileBar(
        UserProfileManager manager,
        CpuPowerControl? cpuControl,
        GpuPowerControl? gpuControl,
        FanControl? fanControl,
        FanSwitchSpeedControl? switchSpeedControl,
        BatteryControl? batteryControl,
        LightingControl lightingControl,
        LocalDimmingControl? dimmingControl,
        MachineProfile profile,
        FailsafeTab failsafeTab,
        FanTab? fanTab = null,
        LiquidCoolerTab? liquidCoolerTab = null)
    {
        _manager = manager;
        _cpuControl = cpuControl;
        _gpuControl = gpuControl;
        _fanControl = fanControl;
        _switchSpeedControl = switchSpeedControl;
        _batteryControl = batteryControl;
        _lightingControl = lightingControl;
        _dimmingControl = dimmingControl;
        _profile = profile;
        _failsafeTab = failsafeTab;
        _fanTab = fanTab;
        _liquidCoolerTab = liquidCoolerTab;

        Height = 40;
        Dock = DockStyle.Top;

        // Right-aligned Defaults dropdown button
        var defaultsMenu = new ContextMenuStrip();
        defaultsMenu.Items.Add("Restore Firmware Defaults", null, (s, e) => RestoreDefaultsClicked?.Invoke(this, EventArgs.Empty));
        defaultsMenu.Items.Add(new ToolStripSeparator());
        defaultsMenu.Items.Add("Gaming Defaults", null, (s, e) => GamingDefaultsClicked?.Invoke(this, EventArgs.Empty));
        defaultsMenu.Items.Add("Office Defaults", null, (s, e) => OfficeDefaultsClicked?.Invoke(this, EventArgs.Empty));
        defaultsMenu.Items.Add("Battery Saver Defaults", null, (s, e) => BatterySaverDefaultsClicked?.Invoke(this, EventArgs.Empty));

        var defaultsButton = new Button
        {
            Text = "Defaults \u25BC",
            AutoSize = true,
            FlatStyle = FlatStyle.Flat,
            Dock = DockStyle.Right,
            Margin = new Padding(4, 2, 4, 2),
            Font = UiHelpers.FontNormal
        };
        defaultsButton.Click += (s, e) => defaultsMenu.Show(defaultsButton, 0, defaultsButton.Height);
        Controls.Add(defaultsButton);

        // Left-aligned flow: profile slots + save
        var flow = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            Padding = new Padding(4, 2, 4, 2)
        };
        Controls.Add(flow);

        var names = _manager.GetProfileNames();
        for (int i = 0; i < 5; i++)
        {
            int slot = i + 1;
            var btn = new Button
            {
                Text = names[i],
                Size = new System.Drawing.Size(120, 32),
                FlatStyle = FlatStyle.Flat,
                Font = UiHelpers.FontNormal
            };

            if (!_manager.SlotExists(slot))
                btn.ForeColor = UiColors.Disabled;

            btn.Click += (s, e) => OnSlotClick(slot);
            btn.MouseUp += (s, e) =>
            {
                if (e.Button == MouseButtons.Right)
                    OnSlotRightClick(slot);
            };

            _slotButtons[i] = btn;
            flow.Controls.Add(btn);
        }

        _saveButton = new Button
        {
            Text = "Save",
            Size = new System.Drawing.Size(80, 32),
            FlatStyle = FlatStyle.Flat,
            Font = UiHelpers.FontBold
        };
        _saveButton.Click += (s, e) => OnSaveClick();
        flow.Controls.Add(_saveButton);
    }

    /// <summary>
    /// Load and apply a profile slot. Called from MainForm for auto-load on startup.
    /// </summary>
    public void LoadAndApplySlot(int slot, bool preserveFailsafeSettings = false, bool automatic = false)
    {
        var profile = _manager.LoadProfile(slot);
        if (profile == null) return;

        var failsafe = _failsafeTab.Controller;
        var destination = preserveFailsafeSettings ? _failsafeTab.ReadSettings() : profile.GpuFailsafe ?? new();
        _failsafeTab.ValidateSettings(destination);
        HighlightSlot(0); // Failed/partial applies must not keep a misleading old highlight.
        void Apply(GpuTemperatureFailsafe.Restoration? permit)
        {
            ApplyProfile(profile, permit);
            _manager.LastUsedSlot = slot;
        }
        try
        {
            if (failsafe != null) failsafe.ApplyProfile(destination, ThermalAction.All, Apply, !automatic);
            else Apply(null);
        }
        catch { _failsafeTab.RefreshConfiguration(partial: true); ProfileApplied?.Invoke(); throw; }
        _failsafeTab.RefreshConfiguration(saved: !preserveFailsafeSettings);
        HighlightSlot(slot);
        ProfileApplied?.Invoke();
    }

    private void OnSlotClick(int slot)
    {
        if (!_manager.SlotExists(slot))
        {
            // Empty slot — select as save target but don't load
            HighlightSlot(slot);
            return;
        }

        try
        {
            LoadAndApplySlot(slot);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to load profile:\n{ex.Message}",
                "Load Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void OnSaveClick()
    {
        try
        {
            _failsafeTab.Controller?.EnsureCanSaveProfile();
            // A blocked save must not change the selected/highlighted slot.
            if (_activeSlot == 0) HighlightSlot(1);
            string currentName = _manager.GetProfileNames()[_activeSlot - 1];
            var profile = BuildProfileFromCurrentState(currentName);
            _manager.SaveProfile(_activeSlot, profile);
            _failsafeTab.MarkSaved();

            HighlightSlot(_activeSlot);
            ProfileListChanged?.Invoke();

            DiagnosticLog.Write($"[UserProfile] Saved to slot {_activeSlot}");
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to save profile:\n{ex.Message}",
                "Save Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void OnSlotRightClick(int slot)
    {
        string currentName = _manager.GetProfileNames()[slot - 1];

        using var dialog = new Form
        {
            Text = $"Rename Profile {slot}",
            Size = new System.Drawing.Size(300, 130),
            StartPosition = FormStartPosition.CenterParent,
            FormBorderStyle = FormBorderStyle.FixedDialog,
            MaximizeBox = false,
            MinimizeBox = false
        };
        var textBox = new TextBox
        {
            Text = currentName,
            Location = new System.Drawing.Point(12, 12),
            Size = new System.Drawing.Size(260, 24)
        };
        dialog.Controls.Add(textBox);

        var okBtn = new Button
        {
            Text = "OK",
            DialogResult = DialogResult.OK,
            Location = new System.Drawing.Point(116, 50),
            Size = new System.Drawing.Size(80, 28)
        };
        dialog.Controls.Add(okBtn);
        dialog.AcceptButton = okBtn;

        if (dialog.ShowDialog(FindForm()) == DialogResult.OK
            && !string.IsNullOrWhiteSpace(textBox.Text))
        {
            string newName = textBox.Text.Trim();
            _manager.RenameProfile(slot, newName);
            _slotButtons[slot - 1].Text = newName;
            ProfileListChanged?.Invoke();
        }
    }

    private void HighlightSlot(int slot)
    {
        _activeSlot = slot;
        for (int i = 0; i < 5; i++)
        {
            bool isActive = (i + 1) == slot;
            _slotButtons[i].Font = isActive ? UiHelpers.FontBold : UiHelpers.FontNormal;
            _slotButtons[i].BackColor = isActive
                ? System.Drawing.SystemColors.Highlight
                : System.Drawing.SystemColors.Control;
            _slotButtons[i].ForeColor = isActive
                ? System.Drawing.SystemColors.HighlightText
                : (_manager.SlotExists(i + 1)
                    ? System.Drawing.SystemColors.ControlText
                    : UiColors.Disabled);
        }
    }

    // ── Apply profile to EC ──────────────────────────────────────────

    private void ApplyProfile(UserProfile profile, GpuTemperatureFailsafe.Restoration? restoration)
    {
        // 1-4. Fan (if available)
        if (_fanControl != null)
        {
            var cpuCurve = FanCurveFromData(profile.Fan.CpuCurve);
            var gpuCurve = FanCurveFromData(profile.Fan.GpuCurve);
            if (_fanTab != null)
            {
                // Panels are source of truth — load there, FanTab writes to EC
                _fanTab.LoadCurves(cpuCurve, gpuCurve, profile.Fan.CurveEnabled);
            }
            else
            {
                _fanControl.ApplyCurves(cpuCurve, gpuCurve, profile.Fan.CurveEnabled);
            }
            _fanTab?.SetLinkedEnabled(profile.Fan.LinkedEnabled);
        }

        // 5. CPU power (if available)
        _cpuControl?.ApplyCpuPower(
            profile.Cpu.PL1, profile.Cpu.PL2,
            profile.Cpu.PL4, profile.Cpu.TempLimit, restoration);

        // 6. GPU power (if available)
        _gpuControl?.ApplyGpuPower(
                profile.Gpu.Tgp,
                profile.Gpu.DynamicBoostEnabled,
                profile.Gpu.DynamicBoostWatts, restoration);

        // 7. Fan boost (if available)
        _fanControl?.SetFanBoost(profile.Fan.BoostEnabled, restoration);

        // 8. Fan switch speed
        _switchSpeedControl?.Apply(profile.Fan.SwitchSpeedEnabled, profile.Fan.SwitchSpeedMs);

        // 9. Battery mode (if available)
        _batteryControl?.SetBatteryMode(profile.Battery.Mode);

        // 10. Lighting
        ApplyLighting(profile.Lighting);

        // 11. Liquid cooler (if tab exists and profile has LC settings)
        if (_liquidCoolerTab != null && profile.LiquidCooler != null)
        {
            _liquidCoolerTab.ApplySettings(profile.LiquidCooler);
            _ = _liquidCoolerTab.ApplyCurrentAsync(); // fire-and-forget BLE writes
        }
    }

    private void ApplyLighting(LightingProfileSettings lighting)
    {
        // Apply each lighting zone to hardware
        ApplyLightingZone(LightingControl.ZoneLogo,
            lighting.LogoEnabled, lighting.LogoR, lighting.LogoG, lighting.LogoB, lighting.LogoBrightness);
        ApplyLightingZone(LightingControl.ZoneKeyboard,
            lighting.KeyboardEnabled, lighting.KeyboardR, lighting.KeyboardG, lighting.KeyboardB, lighting.KeyboardBrightness);
        ApplyLightingZone(LightingControl.ZoneLightBar,
            lighting.LightBarEnabled, lighting.LightBarR, lighting.LightBarG, lighting.LightBarB, lighting.LightBarBrightness);

        // Local dimming
        if (_dimmingControl != null)
        {
            try { _dimmingControl.SetState(lighting.LocalDimmingEnabled); }
            catch (Exception ex) { DiagnosticLog.Warn($"[UserProfile] Local dimming apply failed: {ex.Message}"); }
        }

        // Update LightingState file so LightingTab stays in sync
        var state = new LightingState();
        CopyToZoneState(state.Logo, lighting.LogoEnabled, lighting.LogoR, lighting.LogoG, lighting.LogoB, lighting.LogoBrightness);
        CopyToZoneState(state.Keyboard, lighting.KeyboardEnabled, lighting.KeyboardR, lighting.KeyboardG, lighting.KeyboardB, lighting.KeyboardBrightness);
        CopyToZoneState(state.LightBar, lighting.LightBarEnabled, lighting.LightBarR, lighting.LightBarG, lighting.LightBarB, lighting.LightBarBrightness);
        state.IdleTimerSeconds = lighting.IdleTimerSeconds;
        state.Save();
    }

    private void ApplyLightingZone(int zone, bool enabled, byte r, byte g, byte b, int brightness)
    {
        try
        {
            if (!_lightingControl.IsZoneAvailable(zone)) return;
            if (enabled && brightness > 0)
                _lightingControl.SetZoneOn(zone, r, g, b, brightness);
            else
                _lightingControl.SetZoneOff(zone);
        }
        catch (Exception ex) { DiagnosticLog.Warn($"[UserProfile] Zone {zone} apply failed: {ex.Message}"); }
    }

    private static void CopyToZoneState(ZoneState zone, bool enabled, byte r, byte g, byte b, int brightness)
    {
        zone.Enabled = enabled;
        zone.R = r;
        zone.G = g;
        zone.B = b;
        zone.Brightness = brightness;
    }

    // ── Save current EC state to profile ─────────────────────────────

    private UserProfile BuildProfileFromCurrentState(string name)
    {
        // 1. CPU (defaults if controller not available)
        var cpu = _cpuControl?.ReadCpuPower() ?? new CpuPowerValues { PL1Watts = 45, PL2Watts = 65, PL4Watts = 90, TempLimitC = 95 };

        // 2. GPU (defaults if controller not available)
        var gpu = _gpuControl?.ReadGpuPower() ?? new GpuPowerValues { TgpWatts = 115, DynamicBoostEnabled = false, DbWatts = 0 };

        // 3-6. Fan — curves from panels (source of truth), state from EC
        bool fanBoost = _fanControl?.ReadFanBoost() ?? false;
        bool fanCurveEnabled = _fanControl?.ReadCustomFanCurveMode() ?? false;
        bool linkedEnabled = _fanTab?.LinkedEnabled ?? false;
        var cpuCurve = _fanTab?.CpuCurve ?? _fanControl?.ReadCpuCurve() ?? new FanCurveModel(1);
        var gpuCurve = _fanTab?.GpuCurve ?? _fanControl?.ReadGpuCurve() ?? new FanCurveModel(1);

        // 7. Fan switch speed
        bool switchEnabled = false;
        int switchMs = 100;
        if (_switchSpeedControl != null)
        {
            var (en, ms) = _switchSpeedControl.ReadState();
            switchEnabled = en;
            switchMs = ms;
        }

        // 8. Battery (default to mode 0 if controller not available)
        int batteryMode = _batteryControl?.ReadBatteryMode() ?? 0;

        // 9. Logo (from EC)
        var (logoOn, logoR, logoG, logoB, logoBright) = _lightingControl.ReadLogoState();

        // 10. Keyboard + light bar (from LightingState — HID has no readback)
        var lightState = LightingState.Load();

        // 11. Local dimming
        bool dimmingOn = false;
        if (_dimmingControl != null)
        {
            try { dimmingOn = _dimmingControl.ReadState(); }
            catch (Exception ex) { DiagnosticLog.Warn($"[UserProfile] Dimming read failed: {ex.Message}"); }
        }

        return new UserProfile
        {
            Name = name,
            ProjectId = _profile.ProjectId,
            SavedAt = DateTime.Now,
            GpuFailsafe = _failsafeTab.ReadSettings(),
            Cpu = new CpuProfileSettings
            {
                PL1 = cpu.PL1Watts,
                PL2 = cpu.PL2Watts,
                PL4 = cpu.PL4Watts,
                TempLimit = cpu.TempLimitC
            },
            Gpu = new GpuProfileSettings
            {
                Tgp = gpu.TgpWatts,
                DynamicBoostEnabled = gpu.DynamicBoostEnabled,
                DynamicBoostWatts = gpu.DbWatts
            },
            Fan = new FanProfileSettings
            {
                BoostEnabled = fanBoost,
                CurveEnabled = fanCurveEnabled,
                LinkedEnabled = linkedEnabled,
                SwitchSpeedEnabled = switchEnabled,
                SwitchSpeedMs = switchMs,
                CpuCurve = FanCurveToData(cpuCurve),
                GpuCurve = FanCurveToData(gpuCurve)
            },
            Battery = new BatteryProfileSettings { Mode = batteryMode },
            Lighting = new LightingProfileSettings
            {
                LogoEnabled = logoOn,
                LogoR = logoR,
                LogoG = logoG,
                LogoB = logoB,
                LogoBrightness = logoBright,
                KeyboardEnabled = lightState.Keyboard.Enabled,
                KeyboardR = lightState.Keyboard.R,
                KeyboardG = lightState.Keyboard.G,
                KeyboardB = lightState.Keyboard.B,
                KeyboardBrightness = lightState.Keyboard.Brightness,
                LightBarEnabled = lightState.LightBar.Enabled,
                LightBarR = lightState.LightBar.R,
                LightBarG = lightState.LightBar.G,
                LightBarB = lightState.LightBar.B,
                LightBarBrightness = lightState.LightBar.Brightness,
                IdleTimerSeconds = lightState.IdleTimerSeconds,
                LocalDimmingEnabled = dimmingOn
            },
            LiquidCooler = _liquidCoolerTab?.ReadSettings()
        };
    }

    // ── Fan curve conversion ─────────────────────────────────────────

    /// <summary>
    /// Convert FanCurveModel (active entries, DutyPercent 0-100) to
    /// FanCurveArrays (16 padded entries, raw duty 0-200).
    /// </summary>
    private FanCurveArrays FanCurveToData(FanCurveModel model)
    {
        int entriesPerArray = _profile.FanTable.EntriesPerArray;
        int paddingTemp = _profile.FanTable.PaddingTemp;
        // Recompute TempDown from TempUp before serializing — matches
        // what FanControl.WriteCurve pushes to the EC.
        model.EnforceRange();
        // Padding duty = last active entry's raw duty (matches EC write behavior).
        int paddingDuty = model.ActiveEntries > 0
            ? model.Points[model.ActiveEntries - 1].DutyPercent * 2
            : 0;

        var data = new FanCurveArrays
        {
            TempUp = new List<int>(entriesPerArray),
            TempDown = new List<int>(entriesPerArray),
            Duty = new List<int>(entriesPerArray)
        };
        for (int i = 0; i < entriesPerArray; i++)
        {
            if (i < model.ActiveEntries)
            {
                data.TempUp.Add(model.Points[i].TempUp);
                data.TempDown.Add(model.Points[i].TempDown);
                data.Duty.Add(model.Points[i].DutyPercent * 2); // percent → raw
            }
            else
            {
                data.TempUp.Add(paddingTemp);
                data.TempDown.Add(paddingTemp);
                data.Duty.Add(paddingDuty);
            }
        }
        return data;
    }

    private static FanCurveModel FanCurveFromData(FanCurveArrays data)
    {
        if (data.TempUp.Count == 0)
            return FanCurveModel.CreateGenericDefault();
        var model = FanCurveModel.FromRawArrays(data.TempUp, data.TempDown, data.Duty);
        return model.Points.Length > 0 ? model : FanCurveModel.CreateGenericDefault();
    }
}
