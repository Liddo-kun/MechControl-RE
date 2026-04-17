using MechControl.Driver;

namespace MechControl.Features;

/// <summary>
/// Command layer for the AQUARIS BLE liquid cooler.
/// Builds 8-byte protocol frames and sends them via BleDevice.
/// Protocol: [0xFE, CMD, ON/OFF, P1, P2, P3, P4, 0xEF]
/// </summary>
class LiquidCoolerControl
{
    private readonly BleDevice _ble;

    // Frame delimiters
    const byte FRAME_START = 0xFE;
    const byte FRAME_END   = 0xEF;

    // Command bytes
    const byte CMD_RESET  = 0x19;
    const byte CMD_FAN    = 0x1B;
    const byte CMD_PUMP   = 0x1C;
    const byte CMD_RGB    = 0x1E;
    const byte CMD_COMMIT = 0x33;

    // Firmware version request (no framing — standalone 2-byte command)
    static readonly byte[] FW_VERSION_CMD = [0x73, 0x77];

    public LiquidCoolerControl(BleDevice ble)
    {
        _ble = ble;
    }

    // ── Fan ──────────────────────────────────────────────────────────

    /// <summary>Set radiator fan duty cycle (0-100%).</summary>
    public async Task SetFanDutyAsync(int percent)
    {
        byte duty = (byte)Math.Clamp(percent, 0, 100);
        await _ble.WriteAsync([FRAME_START, CMD_FAN, 0x01, duty, 0x00, 0x00, 0x00, FRAME_END]);
    }

    public async Task SetFanOffAsync() =>
        await _ble.WriteAsync([FRAME_START, CMD_FAN, 0x00, 0x00, 0x00, 0x00, 0x00, FRAME_END]);

    // ── Pump ─────────────────────────────────────────────────────────

    /// <summary>
    /// Set pump duty cycle (0-100%) and voltage.
    /// Default voltage is V8 (matches OEM default PumpV=8).
    /// </summary>
    public async Task SetPumpAsync(int percent, PumpVoltage voltage = PumpVoltage.V8)
    {
        byte duty = (byte)Math.Clamp(percent, 0, 100);
        await _ble.WriteAsync([FRAME_START, CMD_PUMP, 0x01, duty, (byte)voltage, 0x00, 0x00, FRAME_END]);
    }

    public async Task SetPumpOffAsync() =>
        await _ble.WriteAsync([FRAME_START, CMD_PUMP, 0x00, 0x00, 0x00, 0x00, 0x00, FRAME_END]);

    // ── RGB LED ──────────────────────────────────────────────────────

    /// <summary>Set LED color and mode.</summary>
    public async Task SetRgbAsync(byte r, byte g, byte b, CoolerRgbMode mode = CoolerRgbMode.Static) =>
        await _ble.WriteAsync([FRAME_START, CMD_RGB, 0x01, r, g, b, (byte)mode, FRAME_END]);

    public async Task SetRgbOffAsync() =>
        await _ble.WriteAsync([FRAME_START, CMD_RGB, 0x00, 0x00, 0x00, 0x00, 0x00, FRAME_END]);

    // ── Commit ────────────────────────────────────────────────────────

    /// <summary>
    /// Send commit command after a batch of settings. Observed in OEM app traffic —
    /// sent after every settings change (discovered via ETW BLE capture, CMD 0x33).
    /// </summary>
    public async Task SendCommitAsync() =>
        await _ble.WriteAsync([FRAME_START, CMD_COMMIT, 0x00, 0x00, 0x00, 0x00, 0x00, FRAME_END]);

    // ── Reset ────────────────────────────────────────────────────────

    /// <summary>
    /// Send reset command. Called on disconnect to return cooler to default state.
    /// </summary>
    public async Task SendResetAsync() =>
        await _ble.WriteAsync([FRAME_START, CMD_RESET, 0x00, 0x01, 0x00, 0x00, 0x00, FRAME_END]);

    // ── Firmware version ─────────────────────────────────────────────

    /// <summary>
    /// Read firmware version from the cooler. Returns version string or null on timeout.
    /// </summary>
    public async Task<string?> ReadFirmwareVersionAsync()
    {
        var response = await _ble.WriteAndReceiveAsync(FW_VERSION_CMD);
        if (response == null || response.Length == 0) return null;
        return System.Text.Encoding.UTF8.GetString(response).TrimEnd('\0');
    }

}

/// <summary>Pump voltage options.</summary>
enum PumpVoltage : byte
{
    V11 = 0x00,
    V12 = 0x01,
    V7  = 0x02,
    V8  = 0x03,
}

/// <summary>Cooler LED animation modes.</summary>
enum CoolerRgbMode : byte
{
    Static       = 0x00,
    Breathe      = 0x01,
    Colorful     = 0x02,
    BreatheColor = 0x03,
}
