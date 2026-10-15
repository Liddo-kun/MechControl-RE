using MechControl.Features;
using MechControl.Profile;

namespace MechControl.UI;

class FailsafeTab : UserControl
{
    private readonly GpuTemperatureFailsafe _failsafe;
    private readonly CheckBox _enabled, _cpu, _fan, _window;
    private readonly TrackBar _threshold;
    private readonly Label _status, _temperature, _peak, _lastTrip, _preview, _saveHint;
    private readonly Label _configurationWarning;
    private readonly string _startupWarning;
    private readonly Button _reset, _test;
    private GpuFailsafeSettings? _saved;
    private bool _partial;
    private bool _loading;

    public GpuTemperatureFailsafe Controller => _failsafe;
    public bool IsTripped => _failsafe.IsTripped;
    public bool HasPendingEdits => !PendingSettings().SameAs(ReadSettings());
    public bool HasConfigurationWarning => !string.IsNullOrEmpty(_configurationWarning.Text);
    public event Action? ConfigurationWarningChanged;
    public event Action? ResetCompleted;

    public FailsafeTab(GpuTemperatureFailsafe failsafe, string startupWarning = "")
    {
        _failsafe = failsafe;
        _startupWarning = startupWarning;
        Dock = DockStyle.Fill;
        Font = UiHelpers.FontNormal;
        var stack = UiHelpers.BuildSectionStack();
        var settings = UiHelpers.BuildSection("GPU Thermal Failsafe");
        var description = Hint("For machines that depend on an external / liquid cooler to run their high-power profile. " +
            "Set the trigger above the EC GPU temperature normally reached with the cooler working. " +
            $"If the average of the last {GpuTemperatureFailsafe.AverageWindow} one-second readings reaches the trigger, the failsafe trips, " +
            "requests this model's minimum GPU power and alerts you; single noisy readings are averaged out. " +
            "Optional actions also reduce CPU power and turn on fan boost. To recover, press Reset (restores the active profile) " +
            "or load the profile that matches your cooling, e.g. an air-cooling profile.");
        settings.Controls.Add(description);
        _enabled = Check("Enable thermal failsafe", failsafe.Available);
        _enabled.Font = UiHelpers.FontBold;
        settings.Controls.Add(_enabled);
        var sliderTable = UiHelpers.BuildSliderTable();
        var slider = UiHelpers.BuildSliderRow("GPU trigger temperature", GpuFailsafeSettings.MinimumThreshold,
            GpuFailsafeSettings.MaximumThreshold, "\u00B0C", GpuFailsafeSettings.DefaultThreshold);
        slider.Slider.Enabled = failsafe.Available;
        slider.Slider.LargeChange = 1;
        _threshold = slider.Slider;
        UiHelpers.AddSliderRow(sliderTable, slider);
        settings.Controls.Add(sliderTable);
        _cpu = Check("Also drop CPU power limits (PL1 / PL2 / PL4) to minimum", failsafe.Supports(ThermalAction.Cpu) && failsafe.Available);
        _fan = Check("Turn on fan boost when tripped", failsafe.Supports(ThermalAction.Fan) && failsafe.Available);
        _window = Check("Bring up the MechControl window when tripped (safety alert, tray alert and sound remain enabled)", failsafe.Available);
        settings.Controls.Add(_cpu);
        settings.Controls.Add(_fan);
        settings.Controls.Add(_window);
        _preview = Hint("");
        settings.Controls.Add(_preview);
        var buttons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Top, WrapContents = true };
        var apply = UiHelpers.BuildApplyButton("Apply", (_, _) => UiHelpers.TryUi(() =>
        {
            _failsafe.Configure(PendingSettings());
            RefreshStatus();
        }, "configure thermal failsafe"));
        apply.Enabled = failsafe.Available;
        buttons.Controls.Add(apply);
        _saveHint = Hint("");
        _saveHint.Margin = new Padding(UiMetrics.ButtonGap, UiMetrics.RowGap + 4, 0, 0);
        buttons.Controls.Add(_saveHint);
        settings.Controls.Add(buttons);
        var saveCopy = Hint("Applies to the current profile. Apply takes effect now; use the profile bar's Save button to keep these settings.");
        settings.Controls.Add(saveCopy);
        var detailsLink = new LinkLabel { Text = "Safety details and limitations", AutoSize = true, Margin = new Padding(0, 4, 0, 0) };
        settings.Controls.Add(detailsLink);
        var details = Hint($"EC temperature checks run approximately once per second, including in the tray. A trip needs at least {GpuTemperatureFailsafe.MinimumSamples} readings " +
            $"and uses their average (up to {GpuTemperatureFailsafe.AverageWindow}), adding about 2 s of delay. Five unavailable/invalid samples trip protection. " +
            "A sleeping GPU reporting zero can cause a conservative trip.\n\n" +
            "While enabled, Reset requires the average to be below the trigger (otherwise it would trip again); loading a profile always clears the trip. " +
            "Minimum settings are verified EC requests, not proof of actual cooling or physical power enforcement. No shunt multiplier is assumed. " +
            "Fan boost is the laptop's onboard firmware mode, not the external cooler's pump or fans.\n\n" +
            "No protection while the application/OS cannot run or hardware cannot accept writes. UI-thread stalls can delay checks; this is not real-time protection. " +
            "A plausible frozen sensor cannot be detected from this register alone. No hotspot driver or BLE cooler connection is required. " +
            "Notifications and sound are best-effort; closing the independent safety alert does not reset protection.");
        details.Visible = false;
        detailsLink.LinkClicked += (_, _) => details.Visible = !details.Visible;
        settings.Controls.Add(details);
        stack.Controls.Add(settings);

        var statusSection = UiHelpers.BuildSection("Status", fitContent: true);
        var table = UiHelpers.BuildKeyValueTable();
        _status = UiHelpers.AddKeyValueRow(table, "State:");
        _temperature = UiHelpers.AddKeyValueRow(table, "GPU temperature (EC):");
        _peak = UiHelpers.AddKeyValueRow(table, "Peak since armed:");
        _lastTrip = UiHelpers.AddKeyValueRow(table, "Last trip:");
        _configurationWarning = UiHelpers.AddKeyValueRow(table, "Configuration:", startupWarning);
        _configurationWarning.ForeColor = UiColors.StatusWarn;
        _configurationWarning.TextChanged += (_, _) => ConfigurationWarningChanged?.Invoke();
        statusSection.Controls.Add(table);
        var actions = new FlowLayoutPanel { AutoSize = true, WrapContents = true, Margin = new Padding(0) };
        _reset = UiHelpers.BuildApplyButton("Reset", (_, _) =>
        {
            if (MessageBox.Show(FindForm(), "Check cooling, then reset the failsafe and reapply the active profile's power and fan settings?",
                "Reset Thermal Failsafe", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK) return;
            bool reset = false;
            UiHelpers.TryUi(() => { _failsafe.Reset(); reset = true; }, "reset thermal failsafe");
            if (reset) ResetCompleted?.Invoke();
        });
        _test = UiHelpers.BuildApplyButton("Test Failsafe", (_, _) =>
        {
            var applied = ReadSettings();
            string text = _failsafe.DescribeActions(applied) +
                "\n\nThis really applies emergency settings and latches protection. " +
                (HasPendingEdits ? "Unapplied control changes are NOT part of this test. " : "") + "Continue?";
            if (MessageBox.Show(FindForm(), text, "Test Failsafe", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.Yes)
                UiHelpers.TryUi(() =>
                {
                    if (!applied.SameAs(ReadSettings())) throw new InvalidOperationException("Applied settings changed during confirmation. Review the actions and test again.");
                    _failsafe.TestTrip();
                }, "test thermal failsafe");
        }, width: UiMetrics.ButtonReadWidth);
        _test.Margin = new Padding(UiMetrics.ButtonGap, UiMetrics.RowGap, 0, 0);
        actions.Controls.Add(_reset);
        actions.Controls.Add(_test);
        statusSection.Controls.Add(actions);
        var resetCopy = Hint("Reset reapplies the active profile. Loading any profile also clears a trip.");
        statusSection.Controls.Add(resetCopy);
        stack.Controls.Add(statusSection);
        Controls.Add(stack);

        // Wrap against the available client width, rather than screenshot-sized
        // coordinates; long diagnostics grow vertically into the scrollable stack.
        void Wrap()
        {
            int width = Math.Max(150, ClientSize.Width - (int)(64 * DeviceDpi / 96f));
            foreach (var control in new Control[] { description, _preview, saveCopy, details })
                control.MaximumSize = new Size(width, 0);
            foreach (var check in new[] { _window, _cpu, _fan })
            {
                // CheckBox.AutoSize with MaximumSize clips rather than wrapping
                // at native DPI. Measure the text and give the control its rows.
                check.AutoSize = false;
                check.Width = width;
                check.CheckAlign = ContentAlignment.TopLeft;
                check.TextAlign = ContentAlignment.TopLeft;
                check.Height = Math.Max(SystemInformation.MenuCheckSize.Height,
                    TextRenderer.MeasureText(check.Text, check.Font,
                        new Size(Math.Max(50, check.ClientSize.Width - SystemInformation.MenuCheckSize.Width - 12), 0),
                        TextFormatFlags.WordBreak).Height) + 4;
            }
            int captionWidth = table.Controls[0].PreferredSize.Width;
            for (int i = 2; i < table.Controls.Count; i += 2) captionWidth = Math.Max(captionWidth, table.Controls[i].PreferredSize.Width);
            foreach (var label in new[] { _status, _lastTrip, _configurationWarning }) label.MaximumSize = new Size(Math.Max(120, width - captionWidth - 32), 0);
            resetCopy.MaximumSize = new Size(width, 0);
        }
        // TableLayoutPanel can constrain child bounds during the parent's resize.
        // Measure after that layout finishes, not against the previous cell size.
        void WrapAfterLayout(object? sender, EventArgs e)
        {
            if (IsHandleCreated) BeginInvoke((Action)(() => { if (!IsDisposed) Wrap(); }));
            else Wrap();
        }
        SizeChanged += WrapAfterLayout;
        DpiChangedAfterParent += WrapAfterLayout;
        _threshold.ValueChanged += (_, _) => EditsChanged();
        foreach (var check in new[] { _enabled, _cpu, _fan, _window }) check.CheckedChanged += (_, _) => EditsChanged();
        failsafe.Changed += OnChanged;
        failsafe.TemperatureChanged += OnTemperatureChanged;
        VisibleChanged += (_, _) => { if (Visible) RefreshStatus(); };
        RefreshConfiguration();
        Wrap();
    }

    private static Label Hint(string text) => new()
    {
        Text = text, AutoSize = true, Font = UiHelpers.FontNormal, ForeColor = UiColors.Hint,
        Margin = new Padding(0, UiMetrics.RowGap / 2, 0, UiMetrics.RowGap / 2)
    };

    private static CheckBox Check(string text, bool enabled) => new()
    {
        Text = text, AutoSize = true, Enabled = enabled, Font = UiHelpers.FontNormal,
        Margin = new Padding(0, UiMetrics.RowGap / 3, 0, UiMetrics.RowGap / 3)
    };

    private GpuFailsafeSettings PendingSettings() => new()
    {
        Enabled = _enabled.Checked, ThresholdC = _threshold.Value,
        ReduceCpuPower = _cpu.Checked, FanBoost = _fan.Checked, ShowMainWindow = _window.Checked
    };

    // Profile success does not establish that startup recovery issues are resolved.
    public void ShowConfigurationWarning(string message) => _configurationWarning.Text =
        _startupWarning + (_startupWarning.Length > 0 && message.Length > 0 ? "\n" : "") + message;

    public void RefreshConfiguration(bool saved = false, bool partial = false)
    {
        var settings = ReadSettings();
        _loading = true;
        _enabled.Checked = settings.Enabled;
        _threshold.Value = settings.ThresholdC;
        _cpu.Checked = settings.ReduceCpuPower;
        _fan.Checked = settings.FanBoost;
        _window.Checked = settings.ShowMainWindow;
        _loading = false;
        _partial = partial;
        if (saved) _saved = settings.Copy();
        RefreshStatus();
    }

    public void MarkSaved() { _saved = ReadSettings(); _partial = false; RefreshStatus(); }
    public GpuFailsafeSettings ReadSettings() => _failsafe.GetSettings();
    private void EditsChanged() { if (!_loading) RefreshStatus(); }
    private void OnChanged() { if (Visible) RefreshStatus(); }
    private void OnTemperatureChanged() { if (Visible) RefreshTelemetry(); }

    private void RefreshStatus()
    {
        var pending = PendingSettings();
        var applied = ReadSettings();
        bool hasEdits = !pending.SameAs(applied);
        bool unsaved = _partial || _saved == null || !applied.SameAs(_saved);
        _preview.Text = _failsafe.Available ? _failsafe.DescribeActions(pending) : "Unavailable: GPU presence, power registers, temperature register or model-specific minimum could not be validated.";
        _saveHint.Text = hasEdits ? "Changes not applied" : _saved == null && !_partial ? "No saved profile loaded" : unsaved
            ? "Applied - not saved to profile" : "Settings match saved profile";
        _saveHint.ForeColor = hasEdits || unsaved ? UiColors.StatusWarn : UiColors.Hint;
        _lastTrip.Text = _failsafe.LastTripText;
        _reset.Enabled = IsTripped;
        _test.Enabled = _failsafe.Available && !IsTripped;
        RefreshTelemetry();
    }

    private void RefreshTelemetry()
    {
        _status.Text = IsTripped ? _failsafe.Status : _failsafe.StateText;
        _status.ForeColor = IsTripped || _failsafe.ProtectionIncomplete ? UiColors.StatusError
            : _failsafe.Enabled && _failsafe.TemperatureC == null ? UiColors.StatusWarn
            : _failsafe.Enabled ? UiColors.StatusOk : UiColors.StatusNeutral;
        _status.Font = IsTripped || _failsafe.ProtectionIncomplete ? UiHelpers.FontBold : UiHelpers.FontNormal;
        _temperature.Text = _failsafe.TemperatureC is int current ? $"{current}\u00B0C" +
            (_failsafe.AverageTemperatureC is double average ? $" ({GpuTemperatureFailsafe.AverageWindow} s average {average:0.#}\u00B0C)" : "") : "--";
        _peak.Text = _failsafe.PeakTemperatureC is int peak ? $"{peak}\u00B0C" : "--";
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _failsafe.Changed -= OnChanged;
            _failsafe.TemperatureChanged -= OnTemperatureChanged;
        }
        base.Dispose(disposing);
    }
}
