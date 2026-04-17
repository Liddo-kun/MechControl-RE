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
    private readonly Label _lblFanValue;

    // Pump
    private readonly CheckBox _chkPump;
    private readonly TrackBar _trkPump;
    private readonly Label _lblPumpValue;
    private readonly ComboBox _cboPumpVoltage;

    // RGB
    private readonly CheckBox _chkRgb;
    private readonly NumericUpDown _nudR;
    private readonly NumericUpDown _nudG;
    private readonly NumericUpDown _nudB;
    private readonly Panel _rgbPreview;
    private readonly ComboBox _cboRgbMode;

    // Apply buttons (stored for SetControlsEnabled)
    private readonly Button _btnFanApply;
    private readonly Button _btnPumpApply;
    private readonly Button _btnRgbApply;


    // Bluetooth unavailable overlay
    private readonly Panel _btUnavailablePanel;

    // Scan results cache
    private List<BleDeviceInfo> _scanResults = [];

    // Auto-reconnect timer (runs on thread pool)
    private readonly System.Threading.Timer _reconnectTimer;
    private bool _reconnecting;

    public LiquidCoolerTab(BleDevice ble)
    {
        _ble = ble;
        _control = new LiquidCoolerControl(ble);
        _state = LiquidCoolerState.Load();

        Dock = DockStyle.Fill;
        AutoScroll = true;

        int y = 16;

        // ── Connection section ───────────────────────────────────────
        Controls.Add(new Label
        {
            Text = "Liquid Cooler (BLE)",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Font = UiHelpers.FontHeader
        });
        y += 32;

        _lblStatus = new Label
        {
            Text = "Disconnected",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Font = UiHelpers.FontBold,
            ForeColor = System.Drawing.Color.Gray
        };
        Controls.Add(_lblStatus);
        y += 26;

        _cboDevices = new ComboBox
        {
            Location = new System.Drawing.Point(16, y),
            Size = new System.Drawing.Size(300, 24),
            DropDownStyle = ComboBoxStyle.DropDownList
        };
        Controls.Add(_cboDevices);

        _btnScan = new Button
        {
            Text = "Scan",
            Location = new System.Drawing.Point(324, y),
            Size = new System.Drawing.Size(70, 34)
        };
        _btnScan.Click += BtnScan_Click;
        Controls.Add(_btnScan);

        _btnConnect = new Button
        {
            Text = "Connect",
            Location = new System.Drawing.Point(400, y),
            Size = new System.Drawing.Size(90, 34)
        };
        _btnConnect.Click += BtnConnect_Click;
        Controls.Add(_btnConnect);
        y += 40;

        _chkAutoConnect = new CheckBox
        {
            Text = "Auto-connect on startup",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Checked = _state.AutoConnect
        };
        _chkAutoConnect.CheckedChanged += (_, _) => { _state.AutoConnect = _chkAutoConnect.Checked; _state.Save(); };
        Controls.Add(_chkAutoConnect);

        _lblFirmware = new Label
        {
            Text = "",
            Location = new System.Drawing.Point(250, y),
            AutoSize = true,
            ForeColor = System.Drawing.SystemColors.GrayText
        };
        Controls.Add(_lblFirmware);

        y += 36;

        // ── Fan section ──────────────────────────────────────────────
        Controls.Add(new Label
        {
            Text = "Radiator Fan",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Font = UiHelpers.FontBold
        });
        y += 24;

        _chkFan = new CheckBox
        {
            Text = "Fan On",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Checked = _state.FanOn
        };
        Controls.Add(_chkFan);

        _lblFanValue = new Label
        {
            Text = $"{_state.FanDuty}%",
            Location = new System.Drawing.Point(400, y),
            AutoSize = true,
            Font = UiHelpers.FontBold
        };
        Controls.Add(_lblFanValue);
        y += 26;

        _trkFan = new TrackBar
        {
            Location = new System.Drawing.Point(16, y),
            Size = new System.Drawing.Size(380, 30),
            Minimum = 0,
            Maximum = 100,
            TickFrequency = 10,
            Value = _state.FanDuty
        };
        _trkFan.ValueChanged += (_, _) => _lblFanValue.Text = $"{_trkFan.Value}%";
        Controls.Add(_trkFan);

        _btnFanApply = new Button
        {
            Text = "Apply",
            Location = new System.Drawing.Point(410, y),
            Size = new System.Drawing.Size(80, 36)
        };
        _btnFanApply.Click += BtnFanApply_Click;
        Controls.Add(_btnFanApply);
        y += 48;

        // ── Pump section ─────────────────────────────────────────────
        Controls.Add(new Label
        {
            Text = "Pump",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Font = UiHelpers.FontBold
        });
        y += 24;

        _chkPump = new CheckBox
        {
            Text = "Pump On",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Checked = _state.PumpOn
        };
        Controls.Add(_chkPump);

        Controls.Add(new Label
        {
            Text = "Voltage:",
            Location = new System.Drawing.Point(160, y),
            AutoSize = true
        });

        _cboPumpVoltage = new ComboBox
        {
            Location = new System.Drawing.Point(220, y),
            Size = new System.Drawing.Size(60, 24),
            DropDownStyle = ComboBoxStyle.DropDownList
        };
        _cboPumpVoltage.Items.AddRange(["7V", "8V", "11V", "12V"]);
        _cboPumpVoltage.SelectedIndex = VoltageToIndex(_state.PumpVoltage);
        Controls.Add(_cboPumpVoltage);

        _lblPumpValue = new Label
        {
            Text = $"{_state.PumpDuty}%",
            Location = new System.Drawing.Point(400, y),
            AutoSize = true,
            Font = UiHelpers.FontBold
        };
        Controls.Add(_lblPumpValue);
        y += 26;

        _trkPump = new TrackBar
        {
            Location = new System.Drawing.Point(16, y),
            Size = new System.Drawing.Size(380, 30),
            Minimum = 0,
            Maximum = 100,
            TickFrequency = 10,
            Value = _state.PumpDuty
        };
        _trkPump.ValueChanged += (_, _) => _lblPumpValue.Text = $"{_trkPump.Value}%";
        Controls.Add(_trkPump);

        _btnPumpApply = new Button
        {
            Text = "Apply",
            Location = new System.Drawing.Point(410, y),
            Size = new System.Drawing.Size(80, 36)
        };
        _btnPumpApply.Click += BtnPumpApply_Click;
        Controls.Add(_btnPumpApply);
        y += 48;

        // ── RGB section ──────────────────────────────────────────────
        Controls.Add(new Label
        {
            Text = "LED",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Font = UiHelpers.FontBold
        });
        y += 24;

        _chkRgb = new CheckBox
        {
            Text = "LED On",
            Location = new System.Drawing.Point(16, y),
            AutoSize = true,
            Checked = _state.RgbOn
        };
        Controls.Add(_chkRgb);

        Controls.Add(new Label { Text = "R:", Location = new System.Drawing.Point(120, y), AutoSize = true });
        _nudR = new NumericUpDown
        {
            Location = new System.Drawing.Point(138, y),
            Size = new System.Drawing.Size(68, 24),
            Minimum = 0, Maximum = 255,
            Value = _state.R
        };
        _nudR.ValueChanged += (_, _) => UpdateRgbPreview();
        Controls.Add(_nudR);

        Controls.Add(new Label { Text = "G:", Location = new System.Drawing.Point(214, y), AutoSize = true });
        _nudG = new NumericUpDown
        {
            Location = new System.Drawing.Point(232, y),
            Size = new System.Drawing.Size(68, 24),
            Minimum = 0, Maximum = 255,
            Value = _state.G
        };
        _nudG.ValueChanged += (_, _) => UpdateRgbPreview();
        Controls.Add(_nudG);

        Controls.Add(new Label { Text = "B:", Location = new System.Drawing.Point(308, y), AutoSize = true });
        _nudB = new NumericUpDown
        {
            Location = new System.Drawing.Point(326, y),
            Size = new System.Drawing.Size(68, 24),
            Minimum = 0, Maximum = 255,
            Value = _state.B
        };
        _nudB.ValueChanged += (_, _) => UpdateRgbPreview();
        Controls.Add(_nudB);

        _rgbPreview = new Panel
        {
            Location = new System.Drawing.Point(402, y),
            Size = new System.Drawing.Size(32, 22),
            BorderStyle = BorderStyle.FixedSingle
        };
        Controls.Add(_rgbPreview);
        UpdateRgbPreview();

        Controls.Add(new Label { Text = "Mode:", Location = new System.Drawing.Point(442, y), AutoSize = true });
        _cboRgbMode = new ComboBox
        {
            Location = new System.Drawing.Point(486, y),
            Size = new System.Drawing.Size(110, 24),
            DropDownStyle = ComboBoxStyle.DropDownList
        };
        _cboRgbMode.Items.AddRange(["Static", "Breathe", "Colorful", "BreatheColor"]);
        _cboRgbMode.SelectedIndex = (int)_state.RgbMode;
        Controls.Add(_cboRgbMode);
        y += 30;

        _btnRgbApply = new Button
        {
            Text = "Apply",
            Location = new System.Drawing.Point(16, y),
            Size = new System.Drawing.Size(80, 36)
        };
        _btnRgbApply.Click += BtnRgbApply_Click;
        Controls.Add(_btnRgbApply);
        y += 48;

        // ── Bluetooth unavailable overlay ────────────────────────────
        _btUnavailablePanel = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = System.Drawing.SystemColors.Control,
            Visible = false
        };
        var lblBtMsg = new Label
        {
            Text = "Bluetooth is not available.\n\nEnable Bluetooth in Windows Settings or Device Manager to use the liquid cooler.",
            Location = new System.Drawing.Point(24, 24),
            AutoSize = true,
            Font = UiHelpers.FontMedium
        };
        _btUnavailablePanel.Controls.Add(lblBtMsg);

        var btnBtRetry = new Button
        {
            Text = "Retry",
            Location = new System.Drawing.Point(24, 100),
            Size = new System.Drawing.Size(80, 36)
        };
        btnBtRetry.Click += async (_, _) => await CheckBluetoothAsync();
        _btUnavailablePanel.Controls.Add(btnBtRetry);
        Controls.Add(_btUnavailablePanel);

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

        // Bluetooth check + auto-connect is triggered by MainForm via InitAsync()
        // (not Load, which only fires when the tab is first shown).

        UiHelpers.FixZOrder(Controls);
        // Overlay must be on top after FixZOrder (which sorts by Y position)
        _btUnavailablePanel.BringToFront();
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
        _lblStatus.ForeColor = System.Drawing.Color.DarkOrange;

        try
        {
            _scanResults = await BleDevice.ScanAsync(5000);

            _cboDevices.Items.Clear();
            if (_scanResults.Count == 0)
            {
                _cboDevices.Items.Add("(no coolers found)");
                _lblStatus.Text = "No devices found";
                _lblStatus.ForeColor = System.Drawing.Color.Gray;
            }
            else
            {
                foreach (var dev in _scanResults)
                    _cboDevices.Items.Add($"{dev.Name} ({BleDevice.FormatAddress(dev.BluetoothAddress)}) [{dev.Rssi} dBm]");
                _lblStatus.Text = $"Found {_scanResults.Count} device(s)";
                _lblStatus.ForeColor = System.Drawing.Color.DarkBlue;
            }
            _cboDevices.SelectedIndex = 0;
        }
        catch (Exception ex)
        {
            _lblStatus.Text = $"Scan failed: {ex.Message}";
            _lblStatus.ForeColor = System.Drawing.Color.Red;
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
        _lblStatus.ForeColor = System.Drawing.Color.DarkOrange;

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
            _lblStatus.ForeColor = System.Drawing.Color.Red;
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
        _lblStatus.ForeColor = System.Drawing.Color.DarkGreen;
        _btnConnect.Text = "Disconnect";
        SetControlsEnabled(true);
    }

    private void OnDisconnected()
    {
        _lblStatus.Text = "Disconnected";
        _lblStatus.ForeColor = System.Drawing.Color.Gray;
        _btnConnect.Text = "Connect";
        _lblFirmware.Text = "";
        SetControlsEnabled(false);
    }

    private void OnConnectionLost()
    {
        _lblStatus.Text = "Connection lost";
        _lblStatus.ForeColor = System.Drawing.Color.Red;
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
        _reconnectTimer.Change(10000, 10000);
    }

    private void StopReconnectTimer()
    {
        _reconnectTimer.Change(Timeout.Infinite, Timeout.Infinite);
        _reconnecting = false;
    }

    private async void ReconnectTimerCallback(object? state)
    {
        if (_reconnecting || _ble.IsConnected || _state.SavedAddress == 0) return;
        _reconnecting = true;

        try
        {
            if (!await ConnectWithTimeoutAsync(_state.SavedAddress))
                return; // Silently retry next tick

            if (IsHandleCreated)
                BeginInvoke(async () => { OnConnected(); await ApplyCurrentAsync(); });
        }
        catch
        {
            // Silently retry next tick
        }
        finally
        {
            _reconnecting = false;
        }
    }

    // ── Fan / Pump / RGB apply ───────────────────────────────────────

    private async void BtnFanApply_Click(object? sender, EventArgs e)
    {
        try
        {
            if (_chkFan.Checked)
                await _control.SetFanDutyAsync(_trkFan.Value);
            else
                await _control.SetFanOffAsync();
            await _control.SendCommitAsync();

            _state.FanOn = _chkFan.Checked;
            _state.FanDuty = _trkFan.Value;
            _state.Save();
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to set fan:\n{ex.Message}", "BLE Error",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private async void BtnPumpApply_Click(object? sender, EventArgs e)
    {
        try
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
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to set pump:\n{ex.Message}", "BLE Error",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private async void BtnRgbApply_Click(object? sender, EventArgs e)
    {
        try
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
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to set LED:\n{ex.Message}", "BLE Error",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

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

    private void UpdateRgbPreview()
    {
        _rgbPreview.BackColor = System.Drawing.Color.FromArgb(
            (int)_nudR.Value, (int)_nudG.Value, (int)_nudB.Value);
    }

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
        UpdateRgbPreview();
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

        if (_state.AutoConnect && _state.SavedAddress != 0 && !_btUnavailablePanel.Visible)
            await TryConnectAsync(_state.SavedAddress);
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
