using MechControl.Driver;
using MechControl.Features;
using MechControl.Profile;

namespace MechControl.UI;

class LiquidCoolerTab : UserControl
{
    private readonly BleDevice _ble;
    private readonly Action _onConnectionLostHandler;
    private readonly LiquidCoolerControl _control;
    private readonly LiquidCoolerState _state;

    // Connection
    private readonly Label _lblStatus;
    private readonly Button _btnScan;
    private readonly Button _btnConnect;
    private readonly ComboBox _cboDevices;
    private readonly CheckBox _chkAutoConnect;
    private readonly Label _lblFirmware;

    // Fan
    private readonly CheckBox _chkFan;
    private readonly TrackBar _trkFan;

    // Pump
    private readonly CheckBox _chkPump;
    private readonly TrackBar _trkPump;
    private readonly ComboBox _cboPumpVoltage;

    // RGB
    private readonly CheckBox _chkRgb;
    private readonly NumericUpDown _nudR;
    private readonly NumericUpDown _nudG;
    private readonly NumericUpDown _nudB;
    private readonly ComboBox _cboRgbMode;

    // Apply buttons (stored for SetControlsEnabled)
    private readonly Button _btnFanApply;
    private readonly Button _btnPumpApply;
    private readonly Button _btnRgbApply;

    // Bluetooth unavailable overlay
    private readonly Panel _btUnavailablePanel;

    // Scan results cache
    private List<BleDeviceInfo> _scanResults = [];

    // Auto-reconnect timer (runs on thread pool, single-fire with backoff)
    private readonly System.Threading.Timer _reconnectTimer;
    private bool _reconnecting;
    private int _reconnectAttempt;

    // Backoff schedule: four fast retries (8s) for briefly-dropped connections,
    // then 20s / 40s, then cap at 5min. Avoids hammering the BLE radio for hours
    // when the cooler is absent (battery/airspace cost).
    private static readonly int[] ReconnectBackoffMs =
        [8_000, 8_000, 8_000, 8_000, 20_000, 40_000, 300_000];

    public LiquidCoolerTab(BleDevice ble)
    {
        _ble = ble;
        _control = new LiquidCoolerControl(ble);
        _state = LiquidCoolerState.Load();

        Dock = DockStyle.Fill;

        var stack = UiHelpers.BuildSectionStack();

        // ── Connection section ──────────────────────────────────
        // Three sequential rows (status label / device+buttons / auto-connect+fw)
        // stacked in a 1-col TableLayoutPanel so each row inherits full width.
        var grpConn = UiHelpers.BuildSection("Liquid Cooler (BLE)");
        var connStack = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 1,
        };
        connStack.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));

        _lblStatus = new Label
        {
            Text = "Disconnected",
            AutoSize = true,
            Font = UiHelpers.FontBold,
            ForeColor = UiColors.StatusNeutral,
            Margin = new Padding(0, 4, 0, UiMetrics.RowGap),
        };
        connStack.Controls.Add(_lblStatus);

        // Row 2: devices combo (fill) + Scan + Connect
        var deviceRow = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 3,
            Margin = new Padding(0),
        };
        deviceRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));
        deviceRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        deviceRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

        _cboDevices = new ComboBox
        {
            Dock = DockStyle.Fill,
            DropDownStyle = ComboBoxStyle.DropDownList,
            Margin = new Padding(0, 0, UiMetrics.ButtonGap, 0),
        };
        deviceRow.Controls.Add(_cboDevices);

        _btnScan = new Button
        {
            Text = "Scan",
            Width = 80,
            Height = UiMetrics.ButtonHeight,
            Margin = new Padding(0, 0, UiMetrics.ButtonGap, 0),
        };
        _btnScan.Click += BtnScan_Click;
        deviceRow.Controls.Add(_btnScan);

        _btnConnect = new Button
        {
            Text = "Connect",
            Width = 100,
            Height = UiMetrics.ButtonHeight,
            Margin = new Padding(0),
        };
        _btnConnect.Click += BtnConnect_Click;
        deviceRow.Controls.Add(_btnConnect);

        connStack.Controls.Add(deviceRow);

        // Row 3: auto-connect + firmware
        var footerRow = new FlowLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            FlowDirection = FlowDirection.LeftToRight,
            Margin = new Padding(0),
        };
        _chkAutoConnect = new CheckBox
        {
            Text = "Auto-connect on startup",
            AutoSize = true,
            Checked = _state.AutoConnect,
            Margin = new Padding(0, UiMetrics.RowGap, 16, 0),
        };
        _chkAutoConnect.CheckedChanged += (_, _) => { _state.AutoConnect = _chkAutoConnect.Checked; _state.Save(); };
        _lblFirmware = new Label
        {
            Text = "",
            AutoSize = true,
            ForeColor = UiColors.Hint,
            Margin = new Padding(0, UiMetrics.RowGap + 2, 0, 0),
        };
        footerRow.Controls.Add(_chkAutoConnect);
        footerRow.Controls.Add(_lblFirmware);
        connStack.Controls.Add(footerRow);

        grpConn.Controls.Add(connStack);
        stack.Controls.Add(grpConn);

        // ── Radiator Fan section ────────────────────────────────
        var grpFan = UiHelpers.BuildSection("Radiator Fan");
        var fanLayout = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 3,
        };
        fanLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        fanLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));
        fanLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

        _chkFan = new CheckBox
        {
            Text = "Fan On",
            AutoSize = true,
            Checked = _state.FanOn,
            Margin = new Padding(0, 4, 0, UiMetrics.RowGap),
        };
        fanLayout.Controls.Add(_chkFan);
        fanLayout.SetColumnSpan(_chkFan, 3);

        var fanSlider = UiHelpers.BuildSliderRow(
            "Fan Duty", 0, 100, "%", initial: _state.FanDuty);
        _trkFan = fanSlider.Slider;
        UiHelpers.AddSliderRow(fanLayout, fanSlider);

        _btnFanApply = UiHelpers.BuildApplyButton("Apply", BtnFanApply_Click);
        fanLayout.Controls.Add(_btnFanApply);
        fanLayout.SetColumnSpan(_btnFanApply, 3);

        grpFan.Controls.Add(fanLayout);
        stack.Controls.Add(grpFan);

        // ── Pump section ────────────────────────────────────────
        var grpPump = UiHelpers.BuildSection("Pump");
        var pumpLayout = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 3,
        };
        pumpLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        pumpLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));
        pumpLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

        // Pump On + Voltage row — nested TLP for predictable AutoSize (FLP
        // with AutoSize inside a TLP cell doesn't reliably shrink vertically).
        var pumpHeader = new TableLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 3,
            RowCount = 1,
            Margin = new Padding(0, 2, 0, UiMetrics.RowGap),
        };
        pumpHeader.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        pumpHeader.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        pumpHeader.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

        _chkPump = new CheckBox
        {
            Text = "Pump On",
            AutoSize = true,
            Checked = _state.PumpOn,
            Anchor = AnchorStyles.Left,
            Margin = new Padding(0, 4, 24, 0),
        };
        pumpHeader.Controls.Add(_chkPump, 0, 0);

        pumpHeader.Controls.Add(new Label
        {
            Text = "Voltage:",
            AutoSize = true,
            Anchor = AnchorStyles.Left,
            Margin = new Padding(0, 2, 8, 0),
        }, 1, 0);

        _cboPumpVoltage = new ComboBox
        {
            Width = 70,
            DropDownStyle = ComboBoxStyle.DropDownList,
            Anchor = AnchorStyles.Left,
            Margin = new Padding(0, 0, 0, 0),
        };
        _cboPumpVoltage.Items.AddRange(["7V", "8V", "11V", "12V"]);
        _cboPumpVoltage.SelectedIndex = VoltageToIndex(_state.PumpVoltage);
        pumpHeader.Controls.Add(_cboPumpVoltage, 2, 0);

        pumpLayout.Controls.Add(pumpHeader);
        pumpLayout.SetColumnSpan(pumpHeader, 3);

        var pumpSlider = UiHelpers.BuildSliderRow(
            "Pump Duty", 0, 100, "%", initial: _state.PumpDuty);
        _trkPump = pumpSlider.Slider;
        UiHelpers.AddSliderRow(pumpLayout, pumpSlider);

        _btnPumpApply = UiHelpers.BuildApplyButton("Apply", BtnPumpApply_Click);
        pumpLayout.Controls.Add(_btnPumpApply);
        pumpLayout.SetColumnSpan(_btnPumpApply, 3);

        grpPump.Controls.Add(pumpLayout);
        stack.Controls.Add(grpPump);

        // ── LED section ─────────────────────────────────────────
        var grpLed = UiHelpers.BuildSection("LED", fitContent: true);
        var ledLayout = new TableLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 1,
        };
        ledLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

        _chkRgb = new CheckBox
        {
            Text = "LED On",
            AutoSize = true,
            Checked = _state.RgbOn,
            Margin = new Padding(0, 4, 0, UiMetrics.RowGap),
        };
        ledLayout.Controls.Add(_chkRgb);

        var rgb = UiHelpers.BuildRgbFlow(_state.R, _state.G, _state.B);
        _nudR = rgb.R;
        _nudG = rgb.G;
        _nudB = rgb.B;

        rgb.Flow.Controls.Add(UiHelpers.MakeInlineLabel("  Mode:"));

        _cboRgbMode = new ComboBox
        {
            Width = 120,
            DropDownStyle = ComboBoxStyle.DropDownList,
            Margin = new Padding(4, 4, 0, 0),
        };
        _cboRgbMode.Items.AddRange(["Static", "Breathe", "Colorful", "BreatheColor"]);
        _cboRgbMode.SelectedIndex = (int)_state.RgbMode;
        rgb.Flow.Controls.Add(_cboRgbMode);
        ledLayout.Controls.Add(rgb.Flow);

        _btnRgbApply = UiHelpers.BuildApplyButton("Apply", BtnRgbApply_Click);
        ledLayout.Controls.Add(_btnRgbApply);

        grpLed.Controls.Add(ledLayout);
        stack.Controls.Add(grpLed);

        Controls.Add(stack);

        // ── Bluetooth unavailable overlay (Dock.Fill on top) ────
        _btUnavailablePanel = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = System.Drawing.SystemColors.Control,
            Padding = new Padding(24),
            Visible = false,
        };
        var btStack = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.TopDown,
            AutoSize = true,
            WrapContents = false,
        };
        btStack.Controls.Add(new Label
        {
            Text = "Bluetooth is not available.\n\nEnable Bluetooth in Windows Settings or Device Manager to use the liquid cooler.",
            AutoSize = true,
            Font = UiHelpers.FontNormal,
            Margin = new Padding(0, 0, 0, 16),
        });
        btStack.Controls.Add(UiHelpers.BuildApplyButton(
            "Retry", async (_, _) => await CheckBluetoothAsync(),
            width: 80, margin: new Padding(0)));
        _btUnavailablePanel.Controls.Add(btStack);
        Controls.Add(_btUnavailablePanel);
        _btUnavailablePanel.BringToFront();

        // Auto-reconnect timer (10s interval)
        _reconnectTimer = new System.Threading.Timer(ReconnectTimerCallback, null,
            Timeout.Infinite, Timeout.Infinite);

        // Wire up BLE disconnect event
        _onConnectionLostHandler = () =>
        {
            if (IsHandleCreated)
                BeginInvoke(OnConnectionLost);
        };
        _ble.ConnectionLost += _onConnectionLostHandler;

        // Pre-populate combo with saved device so user can Connect without scanning
        if (_state.SavedAddress != 0)
        {
            string name = _state.SavedDeviceName ?? "Cooler";
            _scanResults = [new BleDeviceInfo(_state.SavedAddress, name, 0)];
            _cboDevices.Items.Add($"{name} ({BleDevice.FormatAddress(_state.SavedAddress)}) [saved]");
            _cboDevices.SelectedIndex = 0;
        }

        SetControlsEnabled(false);
    }

    // ── Bluetooth availability ───────────────────────────────────────

    private async Task CheckBluetoothAsync()
    {
        bool available = await BleDevice.IsBluetoothAvailableAsync();
        _btUnavailablePanel.Visible = !available;
        if (available)
            SetControlsEnabled(_ble.IsConnected);
    }

    // ── Connection ───────────────────────────────────────────────────

    private async void BtnScan_Click(object? sender, EventArgs e)
    {
        // Re-check Bluetooth availability on each scan (handles adapter re-enable)
        bool btAvailable = await BleDevice.IsBluetoothAvailableAsync();
        if (!btAvailable)
        {
            _btUnavailablePanel.Visible = true;
            return;
        }
        _btUnavailablePanel.Visible = false;

        _btnScan.Enabled = false;
        _btnScan.Text = "Scanning...";
        _cboDevices.Items.Clear();
        _lblStatus.Text = "Scanning...";
        _lblStatus.ForeColor = UiColors.StatusWarn;

        try
        {
            _scanResults = await BleDevice.ScanAsync(5000);

            _cboDevices.Items.Clear();
            if (_scanResults.Count == 0)
            {
                _cboDevices.Items.Add("(no coolers found)");
                _lblStatus.Text = "No devices found";
                _lblStatus.ForeColor = UiColors.StatusNeutral;
            }
            else
            {
                foreach (var dev in _scanResults)
                    _cboDevices.Items.Add($"{dev.Name} ({BleDevice.FormatAddress(dev.BluetoothAddress)}) [{dev.Rssi} dBm]");
                _lblStatus.Text = $"Found {_scanResults.Count} device(s)";
                _lblStatus.ForeColor = UiColors.StatusOk;
            }
            _cboDevices.SelectedIndex = 0;
        }
        catch (Exception ex)
        {
            _lblStatus.Text = $"Scan failed: {ex.Message}";
            _lblStatus.ForeColor = UiColors.StatusError;
        }
        finally
        {
            _btnScan.Text = "Scan";
            _btnScan.Enabled = true;
        }
    }

    private async void BtnConnect_Click(object? sender, EventArgs e)
    {
        if (_ble.IsConnected)
        {
            await DoDisconnectAsync();
            return;
        }

        StopReconnectTimer();

        if (_cboDevices.SelectedIndex < 0 || _cboDevices.SelectedIndex >= _scanResults.Count)
        {
            MessageBox.Show("Select a device first.", "Connect", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        var selected = _scanResults[_cboDevices.SelectedIndex];
        await TryConnectAsync(selected.BluetoothAddress);
    }

    private async Task TryConnectAsync(ulong address)
    {
        _btnConnect.Enabled = false;
        _lblStatus.Text = "Connecting...";
        _lblStatus.ForeColor = UiColors.StatusWarn;

        try
        {
            if (!await ConnectWithTimeoutAsync(address))
                throw new TimeoutException("Connection timed out — cooler may be off or out of range");
            OnConnected();

            _state.SavedAddress = address;
            _state.SavedDeviceName = _ble.DeviceName;
            _state.Save();

            // Apply current UI settings to the cooler (covers startup profile
            // apply that happened before BLE was connected, and reconnect)
            await ApplyCurrentAsync();

            // Try reading firmware version
            try
            {
                string? fw = await _control.ReadFirmwareVersionAsync();
                if (fw != null)
                    _lblFirmware.Text = $"FW: {fw}";
            }
            catch { /* non-critical */ }
        }
        catch (Exception ex)
        {
            _lblStatus.Text = $"Connection failed: {ex.Message}";
            _lblStatus.ForeColor = UiColors.StatusError;
            SetControlsEnabled(false);
            StartReconnectTimer();
        }
        finally
        {
            _btnConnect.Enabled = true;
        }
    }

    private async Task DoDisconnectAsync()
    {
        StopReconnectTimer();

        try { await _control.SendResetAsync(); }
        catch { /* device may already be gone */ }

        _ble.Disconnect();
        OnDisconnected();
    }

    private void OnConnected()
    {
        StopReconnectTimer();
        _btUnavailablePanel.Visible = false;
        _lblStatus.Text = $"Connected: {_ble.DeviceName}";
        _lblStatus.ForeColor = UiColors.StatusOk;
        _btnConnect.Text = "Disconnect";
        SetControlsEnabled(true);
    }

    private void OnDisconnected()
    {
        _lblStatus.Text = "Disconnected";
        _lblStatus.ForeColor = UiColors.StatusNeutral;
        _btnConnect.Text = "Connect";
        _lblFirmware.Text = "";
        SetControlsEnabled(false);
    }

    private void OnConnectionLost()
    {
        _lblStatus.Text = "Connection lost";
        _lblStatus.ForeColor = UiColors.StatusError;
        _btnConnect.Text = "Connect";
        SetControlsEnabled(false);

        if (_state.AutoConnect && _state.SavedAddress != 0)
            StartReconnectTimer();
    }

    /// <summary>
    /// Connect with a 15s hard timeout. WinRT BLE GATT operations can hang
    /// indefinitely when the cooler is powered off or out of range.
    /// Returns true on success, false on timeout.
    /// </summary>
    private async Task<bool> ConnectWithTimeoutAsync(ulong address)
    {
        using var cts = new CancellationTokenSource();
        var connectTask = _ble.ConnectAsync(address, cts.Token);
        if (await Task.WhenAny(connectTask, Task.Delay(15000)) != connectTask)
        {
            cts.Cancel();
            _ble.Disconnect();
            _ = connectTask.ContinueWith(static _ => { }, TaskContinuationOptions.OnlyOnFaulted);
            return false;
        }
        await connectTask;
        return true;
    }

    // ── Auto-reconnect ───────────────────────────────────────────────

    private void StartReconnectTimer()
    {
        _reconnectAttempt = 0;
        ScheduleNextReconnect();
    }

    private void StopReconnectTimer()
    {
        _reconnectTimer.Change(Timeout.Infinite, Timeout.Infinite);
        _reconnecting = false;
        _reconnectAttempt = 0;
    }

    private void ScheduleNextReconnect()
    {
        int delayMs = ReconnectBackoffMs[Math.Min(_reconnectAttempt, ReconnectBackoffMs.Length - 1)];
        _reconnectTimer.Change(delayMs, Timeout.Infinite);
    }

    private async void ReconnectTimerCallback(object? state)
    {
        if (_reconnecting || _ble.IsConnected || _state.SavedAddress == 0) return;
        _reconnecting = true;

        bool succeeded = false;
        try
        {
            if (await ConnectWithTimeoutAsync(_state.SavedAddress))
            {
                succeeded = true;
                if (IsHandleCreated)
                    BeginInvoke(async () => { OnConnected(); await ApplyCurrentAsync(); });
            }
        }
        catch
        {
            // Silently retry with backoff
        }
        finally
        {
            _reconnecting = false;
        }

        if (!succeeded)
        {
            _reconnectAttempt++;
            ScheduleNextReconnect();
        }
    }

    // ── Fan / Pump / RGB apply ───────────────────────────────────────

    private async void BtnFanApply_Click(object? sender, EventArgs e) =>
        await UiHelpers.TryUiAsync(async () =>
        {
            if (_chkFan.Checked)
                await _control.SetFanDutyAsync(_trkFan.Value);
            else
                await _control.SetFanOffAsync();
            await _control.SendCommitAsync();

            _state.FanOn = _chkFan.Checked;
            _state.FanDuty = _trkFan.Value;
            _state.Save();
        }, "set fan", "BLE Error");

    private async void BtnPumpApply_Click(object? sender, EventArgs e) =>
        await UiHelpers.TryUiAsync(async () =>
        {
            if (_chkPump.Checked)
                await _control.SetPumpAsync(_trkPump.Value, SelectedVoltage);
            else
                await _control.SetPumpOffAsync();
            await _control.SendCommitAsync();

            _state.PumpOn = _chkPump.Checked;
            _state.PumpDuty = _trkPump.Value;
            _state.PumpVoltage = SelectedVoltage;
            _state.Save();
        }, "set pump", "BLE Error");

    private async void BtnRgbApply_Click(object? sender, EventArgs e) =>
        await UiHelpers.TryUiAsync(async () =>
        {
            var mode = (CoolerRgbMode)_cboRgbMode.SelectedIndex;
            byte r = (byte)_nudR.Value, g = (byte)_nudG.Value, b = (byte)_nudB.Value;

            if (_chkRgb.Checked)
                await _control.SetRgbAsync(r, g, b, mode);
            else
                await _control.SetRgbOffAsync();
            await _control.SendCommitAsync();

            _state.RgbOn = _chkRgb.Checked;
            _state.R = r;
            _state.G = g;
            _state.B = b;
            _state.RgbMode = mode;
            _state.Save();
        }, "set LED", "BLE Error");

    // ── Helpers ──────────────────────────────────────────────────────

    // Combo order: 0=V7, 1=V8, 2=V11, 3=V12
    private static readonly PumpVoltage[] VoltageFromIndex = [PumpVoltage.V7, PumpVoltage.V8, PumpVoltage.V11, PumpVoltage.V12];

    private static int VoltageToIndex(PumpVoltage v) => v switch
    {
        PumpVoltage.V7 => 0, PumpVoltage.V8 => 1,
        PumpVoltage.V11 => 2, PumpVoltage.V12 => 3, _ => 1
    };

    private PumpVoltage SelectedVoltage =>
        _cboPumpVoltage.SelectedIndex >= 0 && _cboPumpVoltage.SelectedIndex < VoltageFromIndex.Length
            ? VoltageFromIndex[_cboPumpVoltage.SelectedIndex]
            : PumpVoltage.V8;

    private void SetControlsEnabled(bool connected)
    {
        _chkFan.Enabled = connected;
        _trkFan.Enabled = connected;
        _btnFanApply.Enabled = connected;
        _chkPump.Enabled = connected;
        _trkPump.Enabled = connected;
        _cboPumpVoltage.Enabled = connected;
        _btnPumpApply.Enabled = connected;
        _chkRgb.Enabled = connected;
        _nudR.Enabled = connected;
        _nudG.Enabled = connected;
        _nudB.Enabled = connected;
        _cboRgbMode.Enabled = connected;
        _btnRgbApply.Enabled = connected;
    }

    /// <summary>Read current UI state for user profile save.</summary>
    public LiquidCoolerProfileSettings ReadSettings() => new()
    {
        FanOn = _chkFan.Checked,
        FanDuty = _trkFan.Value,
        PumpOn = _chkPump.Checked,
        PumpDuty = _trkPump.Value,
        PumpVoltage = SelectedVoltage,
        RgbOn = _chkRgb.Checked,
        R = (byte)_nudR.Value,
        G = (byte)_nudG.Value,
        B = (byte)_nudB.Value,
        RgbMode = (CoolerRgbMode)_cboRgbMode.SelectedIndex
    };

    /// <summary>Apply saved profile settings to UI controls.</summary>
    public void ApplySettings(LiquidCoolerProfileSettings s)
    {
        _chkFan.Checked = s.FanOn;
        _trkFan.Value = Math.Clamp(s.FanDuty, 0, 100);
        _chkPump.Checked = s.PumpOn;
        _trkPump.Value = Math.Clamp(s.PumpDuty, 0, 100);
        _cboPumpVoltage.SelectedIndex = VoltageToIndex(s.PumpVoltage);
        _chkRgb.Checked = s.RgbOn;
        _nudR.Value = s.R;
        _nudG.Value = s.G;
        _nudB.Value = s.B;
        _cboRgbMode.SelectedIndex = Math.Clamp((int)s.RgbMode, 0, 3);
    }

    /// <summary>Apply current UI settings to the cooler (for profile load).</summary>
    public async Task ApplyCurrentAsync()
    {
        if (!_ble.IsConnected) return;

        try
        {
            if (_chkFan.Checked)
                await _control.SetFanDutyAsync(_trkFan.Value);
            else
                await _control.SetFanOffAsync();

            if (_chkPump.Checked)
                await _control.SetPumpAsync(_trkPump.Value, SelectedVoltage);
            else
                await _control.SetPumpOffAsync();

            var mode = (CoolerRgbMode)_cboRgbMode.SelectedIndex;
            if (_chkRgb.Checked)
                await _control.SetRgbAsync((byte)_nudR.Value, (byte)_nudG.Value, (byte)_nudB.Value, mode);
            else
                await _control.SetRgbOffAsync();

            await _control.SendCommitAsync();
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[LC] Apply failed: {ex.Message}");
        }
    }

    /// <summary>Check Bluetooth and auto-connect if enabled. Called by MainForm on startup.</summary>
    public async Task InitAsync()
    {
        // On Task Scheduler startup after reboot, the BT radio may still be
        // initializing. Retry the availability check a few times before giving up.
        for (int i = 0; i < 4; i++)
        {
            await CheckBluetoothAsync();
            if (!_btUnavailablePanel.Visible) break;
            if (i < 3) await Task.Delay(3000);
        }

        if (!_state.AutoConnect || _state.SavedAddress == 0) return;

        if (!_btUnavailablePanel.Visible)
            await TryConnectAsync(_state.SavedAddress);
        else
            StartReconnectTimer();  // BT still not up — keep trying with backoff
    }

    /// <summary>Send reset to cooler (called during app shutdown).</summary>
    public async Task ShutdownAsync()
    {
        StopReconnectTimer();
        if (_ble.IsConnected)
        {
            try { await _control.SendResetAsync(); }
            catch { }
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _ble.ConnectionLost -= _onConnectionLostHandler;
            _reconnectTimer.Dispose();
        }
        base.Dispose(disposing);
    }
}
