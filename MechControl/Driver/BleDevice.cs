using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

namespace MechControl.Driver;

/// <summary>
/// BLE communication with AQUARIS-compatible liquid coolers via Nordic UART Service.
/// Protocol details from Tuxedo Control Center (GPL) AQUARIS implementation.
/// </summary>
sealed class BleDevice : IDisposable
{
    // Nordic UART Service (NUS) UUIDs — standard UUIDs used by AQUARIS coolers
    static readonly Guid NusServiceUuid = Guid.Parse("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
    static readonly Guid NusTxUuid      = Guid.Parse("6e400002-b5a3-f393-e0a9-e50e24dcca9e");
    static readonly Guid NusRxUuid      = Guid.Parse("6e400003-b5a3-f393-e0a9-e50e24dcca9e");

    // Known AQUARIS device name prefixes
    static readonly string[] KnownNames = ["LCT21001", "LCT22002"];

    BluetoothLEDevice? _device;
    GattDeviceService? _service;
    GattCharacteristic? _tx;
    GattCharacteristic? _rx;
    GattWriteOption _writeOption;
    bool _disposed;

    /// <summary>True when connected to a cooler with UART TX characteristic resolved.</summary>
    public bool IsConnected => _device != null && _tx != null;

    /// <summary>BLE advertised name of connected device.</summary>
    public string? DeviceName { get; private set; }

    /// <summary>Bluetooth address of connected device (persist for auto-reconnect).</summary>
    public ulong ConnectedAddress { get; private set; }

    /// <summary>Fires when connection drops unexpectedly. May fire on a thread-pool thread.</summary>
    public event Action? ConnectionLost;

    // ── Static helpers ───────────────────────────────────────────────

    /// <summary>
    /// Check if a Bluetooth adapter is present and turned on.
    /// Returns false if no adapter, adapter disabled in Device Manager,
    /// or Bluetooth toggled off in Windows Settings.
    /// </summary>
    public static async Task<bool> IsBluetoothAvailableAsync()
    {
        try
        {
            var adapter = await BluetoothAdapter.GetDefaultAsync();
            if (adapter == null) return false;
            var radio = await adapter.GetRadioAsync();
            return radio?.State == Windows.Devices.Radios.RadioState.On;
        }
        catch { return false; }
    }

    /// <summary>
    /// Scan for AQUARIS BLE devices. Runs the advertisement watcher for
    /// <paramref name="durationMs"/> and returns discovered devices whose
    /// names match known cooler models (LCT21001, LCT22002).
    /// </summary>
    public static async Task<List<BleDeviceInfo>> ScanAsync(int durationMs = 5000, CancellationToken ct = default)
    {
        var found = new Dictionary<ulong, BleDeviceInfo>();
        var watcher = new BluetoothLEAdvertisementWatcher
        {
            ScanningMode = BluetoothLEScanningMode.Active
        };

        watcher.Received += (_, args) =>
        {
            string name = args.Advertisement.LocalName ?? "";
            if (string.IsNullOrEmpty(name)) return;
            if (!KnownNames.Any(k => name.Contains(k, StringComparison.OrdinalIgnoreCase)))
                return;

            lock (found)
                found[args.BluetoothAddress] = new BleDeviceInfo(
                    args.BluetoothAddress, name, args.RawSignalStrengthInDBm);
        };

        try { watcher.Start(); }
        catch (Exception ex)
        {
            Console.WriteLine($"[BLE] Scan start failed (Bluetooth may be off): {ex.Message}");
            return [];
        }

        try { await Task.Delay(durationMs, ct); }
        catch (TaskCanceledException) { }
        finally { watcher.Stop(); }

        lock (found)
            return [.. found.Values];
    }

    /// <summary>Format a BLE address as AA:BB:CC:DD:EE:FF.</summary>
    public static string FormatAddress(ulong address) =>
        string.Join(":", BitConverter.GetBytes(address).Take(6).Reverse().Select(b => b.ToString("X2")));

    // ── Connection ───────────────────────────────────────────────────

    /// <summary>
    /// Connect to a cooler by Bluetooth address. Resolves Nordic UART service
    /// and TX/RX characteristics. Disconnects any existing connection first.
    /// </summary>
    public async Task ConnectAsync(ulong bluetoothAddress, CancellationToken ct = default)
    {
        if (_device != null) Disconnect();

        // Retry loop: the cooler's GATT server may not be fully ready immediately
        // after a previous session, especially on app restart.
        const int maxAttempts = 3;
        const int retryDelayMs = 1500;

        for (int attempt = 1; attempt <= maxAttempts; attempt++)
        {
            ct.ThrowIfCancellationRequested();
            if (_device != null) Disconnect();

            _device = await BluetoothLEDevice.FromBluetoothAddressAsync(bluetoothAddress);
            ct.ThrowIfCancellationRequested();
            if (_device == null)
                throw new InvalidOperationException("BLE device not found or unavailable");

            _device.ConnectionStatusChanged += OnConnectionStatusChanged;

            // Resolve Nordic UART service
            var svcResult = await _device.GetGattServicesForUuidAsync(NusServiceUuid);
            ct.ThrowIfCancellationRequested();
            if (svcResult.Status != GattCommunicationStatus.Success || svcResult.Services.Count == 0)
            {
                if (attempt < maxAttempts)
                {
                    Console.WriteLine($"[BLE] NUS service not found (attempt {attempt}/{maxAttempts}), retrying...");
                    await Task.Delay(retryDelayMs, ct);
                    continue;
                }
                Disconnect();
                throw new InvalidOperationException("Nordic UART service not found on device");
            }
            _service = svcResult.Services[0];

            // TX characteristic — app writes commands here
            var txResult = await _service.GetCharacteristicsForUuidAsync(NusTxUuid);
            ct.ThrowIfCancellationRequested();
            if (txResult.Status != GattCommunicationStatus.Success || txResult.Characteristics.Count == 0)
            {
                if (attempt < maxAttempts)
                {
                    Console.WriteLine($"[BLE] TX characteristic not found (attempt {attempt}/{maxAttempts}), retrying...");
                    await Task.Delay(retryDelayMs, ct);
                    continue;
                }
                Disconnect();
                throw new InvalidOperationException("UART TX characteristic not found");
            }
            _tx = txResult.Characteristics[0];

            // Pick write mode based on what the characteristic supports
            _writeOption = _tx.CharacteristicProperties.HasFlag(GattCharacteristicProperties.Write)
                ? GattWriteOption.WriteWithResponse
                : GattWriteOption.WriteWithoutResponse;

            // RX characteristic — app receives notifications here
            var rxResult = await _service.GetCharacteristicsForUuidAsync(NusRxUuid);
            ct.ThrowIfCancellationRequested();
            if (rxResult.Status == GattCommunicationStatus.Success && rxResult.Characteristics.Count > 0)
            {
                _rx = rxResult.Characteristics[0];

                // Enable notifications persistently (matches OEM behavior).
                // The cooler expects CCCD to be enabled before it processes commits.
                try
                {
                    await _rx.WriteClientCharacteristicConfigurationDescriptorAsync(
                        GattClientCharacteristicConfigurationDescriptorValue.Notify);
                }
                catch { /* non-critical */ }
            }

            DeviceName = _device.Name;
            ConnectedAddress = bluetoothAddress;
            Console.WriteLine($"[BLE] Connected to {DeviceName} ({FormatAddress(bluetoothAddress)})");
            return;
        }
    }

    /// <summary>
    /// Disconnect and release all GATT resources.
    /// </summary>
    public void Disconnect()
    {
        _tx = null;
        _rx = null;
        DeviceName = null;
        ConnectedAddress = 0;

        if (_service != null) { _service.Dispose(); _service = null; }
        if (_device != null)
        {
            _device.ConnectionStatusChanged -= OnConnectionStatusChanged;
            _device.Dispose();
            _device = null;
        }

        Console.WriteLine("[BLE] Disconnected");
    }

    // ── Data transfer ────────────────────────────────────────────────

    /// <summary>
    /// Write a command to the cooler via the UART TX characteristic.
    /// </summary>
    public async Task WriteAsync(byte[] data)
    {
        if (_tx == null)
            throw new InvalidOperationException("Not connected");

        var writer = new DataWriter();
        writer.WriteBytes(data);
        var result = await _tx.WriteValueAsync(writer.DetachBuffer(), _writeOption);

        if (result != GattCommunicationStatus.Success)
            throw new IOException($"BLE write failed: {result}");
    }

    /// <summary>
    /// Write a command and wait for a notification response on the RX characteristic.
    /// Returns null on timeout. Used for firmware version reads.
    /// </summary>
    public async Task<byte[]?> WriteAndReceiveAsync(byte[] data, int timeoutMs = 5000)
    {
        if (_tx == null || _rx == null)
            throw new InvalidOperationException("Not connected or RX characteristic unavailable");

        var tcs = new TaskCompletionSource<byte[]>();

        void OnValue(GattCharacteristic sender, GattValueChangedEventArgs args)
        {
            var reader = DataReader.FromBuffer(args.CharacteristicValue);
            var bytes = new byte[reader.UnconsumedBufferLength];
            reader.ReadBytes(bytes);
            tcs.TrySetResult(bytes);
        }

        // Notifications are enabled persistently on connect — just attach handler
        _rx.ValueChanged += OnValue;
        try
        {
            await WriteAsync(data);
            var completed = await Task.WhenAny(tcs.Task, Task.Delay(timeoutMs));
            return completed == tcs.Task ? await tcs.Task : null;
        }
        finally
        {
            _rx.ValueChanged -= OnValue;
        }
    }

    // ── Internal ─────────────────────────────────────────────────────

    void OnConnectionStatusChanged(BluetoothLEDevice sender, object args)
    {
        if (sender.ConnectionStatus == BluetoothConnectionStatus.Disconnected)
        {
            Console.WriteLine("[BLE] Connection lost");
            _tx = null;
            _rx = null;
            ConnectionLost?.Invoke();
        }
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            _disposed = true;
            Disconnect();
        }
    }
}

/// <summary>Discovered BLE device from a scan.</summary>
record BleDeviceInfo(ulong BluetoothAddress, string Name, short Rssi);
