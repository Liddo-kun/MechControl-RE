using MechControl.Driver;
using MechControl.Profile;

namespace MechControl.Features;

/// <summary>
/// Unified controller for all 3 lighting zones (keyboard, light bar, logo).
/// Sends HID feature reports to the ITE controller, plus EC writes for logo.
/// </summary>
class LightingControl
{
    private readonly HidDevice _hid;
    private readonly EcDevice _device;
    private readonly WriteGuard _guard;
    private readonly int _addrLogoR;
    private readonly int _addrLogoG;
    private readonly int _addrLogoB;
    private readonly int _addrLogoOnOff;
    private readonly int _addrLogoTrigger;
    private readonly int _addrLogoCommit;
    private readonly int _addrFanCurveEnable;

    // Zone IDs matching ITE controller
    public const int ZoneKeyboard = 0;
    public const int ZoneLightBar = 2;
    public const int ZoneLogo = 3;

    // ZoneMask values for cmd 0x08
    private static byte GetZoneMask(int zone) => zone switch
    {
        0 => 0x02,  // keyboard
        2 => 0x22,  // light bar
        3 => 0x23,  // logo
        _ => throw new ArgumentException($"Unknown zone {zone}")
    };

    // Logo EC 0x7AB brightness encoding (one-hot bits 5-7)
    // Level: 0=off, 1=25%, 2=50%, 3=75%, 4=100%
    private static readonly byte[] LogoEcBrightness = [0x01, 0x21, 0x41, 0x61, 0x81];

    /// <summary>Map raw brightness 0-100 to logo EC brightness level 0-4.</summary>
    private static int BrightnessToLogoEcLevel(int brightness) =>
        brightness == 0 ? 0 : (brightness - 1) / 25 + 1;

    /// <summary>
    /// Scale HID RGB (0-255) to EC logo RGB (0-60).
    /// Values above 60 are invalid per hardware testing.
    /// </summary>
    private static byte ScaleRgbToEc(byte hidValue) =>
        (byte)Math.Round(hidValue * 60.0 / 255.0);

    public LightingControl(HidDevice hid, EcDevice device, WriteGuard guard, MachineProfile profile)
    {
        _hid = hid;
        _device = device;
        _guard = guard;
        _addrLogoR = profile.GetRegister("logoR");
        _addrLogoG = profile.GetRegister("logoG");
        _addrLogoB = profile.GetRegister("logoB");
        _addrLogoOnOff = profile.GetRegister("logoOnOff");
        _addrLogoTrigger = profile.GetRegister("logoTrigger");
        _addrLogoCommit = profile.GetRegister("logoCommit");
        _addrFanCurveEnable = profile.GetRegister("fanCurveEnable");
    }

    public bool IsZoneAvailable(int zone) => _hid.IsZoneAvailable(zone);

    /// <summary>
    /// Turn a zone off. Follows exact OEM sequence from Frida capture.
    /// </summary>
    public void SetZoneOff(int zone)
    {
        // Keyboard OFF additionally needs cmd 0x09 first (OEM sequence)
        if (zone == ZoneKeyboard)
            _hid.SendFeatureReport(zone, [0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]);
        // Prepare
        _hid.SendFeatureReport(zone, [0x00, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00]);
        // Stop all effects
        _hid.SendFeatureReport(zone, [0x00, 0x08, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]);
        // Set zone mode off
        _hid.SendFeatureReport(zone, [0x00, 0x08, 0x01, (byte)zone, 0x00, 0x00, 0x00, 0x00, 0x00]);
        // Power OFF + save to BIOS
        _hid.SendFeatureReport(zone, [0x00, 0x1A, (byte)zone, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01]);

        // Logo also needs EC writes — exact OEM off sequence
        if (zone == ZoneLogo)
        {
            _guard.Write(_addrLogoOnOff, LogoEcBrightness[0]); // 0x01 = off
            _guard.Write(_addrLogoCommit, 0x10);
        }
    }

    /// <summary>
    /// Turn a zone on with color and brightness (0-100 raw HID value for all zones).
    /// </summary>
    public void SetZoneOn(int zone, byte r, byte g, byte b, int brightness)
    {
        byte zoneMask = GetZoneMask(zone);
        byte hidBrightness = (byte)Math.Clamp(brightness, 0, 100);

        // Power ON + save
        _hid.SendFeatureReport(zone, [0x00, 0x1A, (byte)zone, 0x01, 0x04, 0x00, 0x00, 0x00, 0x01]);
        // Set color
        _hid.SendFeatureReport(zone, [0x00, 0x14, (byte)zone, 0x01, r, g, b, 0x00, 0x00]);
        // Apply: effect=static(0x01), with brightness and speed
        byte speed = (byte)(zone == ZoneKeyboard ? 0x08 : 0x01);
        _hid.SendFeatureReport(zone, [0x00, 0x08, zoneMask, 0x01, 0x01, hidBrightness, speed, 0x00, 0x00]);

        // Logo also needs EC writes for firmware persistence
        if (zone == ZoneLogo)
        {
            _guard.Write(_addrLogoTrigger, 0x00);
            byte curFce = _device.ReadByte(_addrFanCurveEnable);
            _guard.Write(_addrFanCurveEnable, (byte)(curFce | 0x20));
            _guard.Write(_addrLogoR, ScaleRgbToEc(r));
            _guard.Write(_addrLogoG, ScaleRgbToEc(g));
            _guard.Write(_addrLogoB, ScaleRgbToEc(b));
            _guard.Write(_addrLogoOnOff, LogoEcBrightness[BrightnessToLogoEcLevel(brightness)]);
            _guard.Write(_addrLogoCommit, 0x10);
        }
    }

    /// <summary>
    /// Change color on an already-on zone (no power cycle).
    /// </summary>
    public void SetZoneColor(int zone, byte r, byte g, byte b, int brightness)
    {
        byte zoneMask = GetZoneMask(zone);
        byte hidBrightness = (byte)Math.Clamp(brightness, 0, 100);
        byte speed = (byte)(zone == ZoneKeyboard ? 0x08 : 0x01);

        _hid.SendFeatureReport(zone, [0x00, 0x14, (byte)zone, 0x01, r, g, b, 0x00, 0x00]);
        _hid.SendFeatureReport(zone, [0x00, 0x08, zoneMask, 0x01, 0x01, hidBrightness, speed, 0x00, 0x00]);

        if (zone == ZoneLogo)
        {
            _guard.Write(_addrLogoR, ScaleRgbToEc(r));
            _guard.Write(_addrLogoG, ScaleRgbToEc(g));
            _guard.Write(_addrLogoB, ScaleRgbToEc(b));
            _guard.Write(_addrLogoOnOff, LogoEcBrightness[BrightnessToLogoEcLevel(brightness)]);
            _guard.Write(_addrLogoCommit, 0x10);
        }
    }

    /// <summary>
    /// Save current zone settings to ITE controller flash (persists across reboots/timeouts).
    /// Keyboard and light bar only (logo persists via EC).
    /// </summary>
    public void SaveToBios(int zone, byte r, byte g, byte b, int brightness, bool enabled)
    {
        byte zoneMask = GetZoneMask(zone);
        byte hidBrightness = (byte)Math.Clamp(brightness, 0, 100);
        byte speed = (byte)(zone == ZoneKeyboard ? 0x08 : 0x01);

        if (zone == ZoneKeyboard)
        {
            // Keyboard: save startup color (byte 3 = 0x09)
            _hid.SendFeatureReport(zone, [0x00, 0x14, (byte)zone, 0x09, r, g, b, 0x00, 0x00]);
        }

        // Power ON + save (ITE controller may need zone powered on to accept writes)
        if (zone == ZoneLightBar)
            _hid.SendFeatureReport(zone, [0x00, 0x1A, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x01]);
        else
            _hid.SendFeatureReport(zone, [0x00, 0x1A, (byte)zone, 0x01, 0x04, 0x00, 0x00, 0x00, 0x01]);

        // Set color + save flag (byte 8 = 0x01)
        _hid.SendFeatureReport(zone, [0x00, 0x14, (byte)zone, 0x01, r, g, b, 0x00, 0x01]);
        // Apply + save flag (byte 8 = 0x01)
        _hid.SendFeatureReport(zone, [0x00, 0x08, zoneMask, 0x01, 0x01, hidBrightness, speed, 0x00, 0x01]);

        // If zone should be off, persist the off state after saving color/effect
        if (!enabled)
            _hid.SendFeatureReport(zone, [0x00, 0x1A, (byte)zone, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01]);
    }

    /// <summary>
    /// Read logo LED state from EC registers (HID is write-only).
    /// Returns (enabled, r, g, b, brightness 0-100).
    /// RGB values are scaled back from EC range (0-60) to HID range (0-255).
    /// Brightness snaps to nearest 25% increment (EC only has 5 levels).
    /// </summary>
    public (bool enabled, byte r, byte g, byte b, int brightnessLevel) ReadLogoState()
    {
        byte onOff = _device.ReadByte(_addrLogoOnOff);
        byte ecR = _device.ReadByte(_addrLogoR);
        byte ecG = _device.ReadByte(_addrLogoG);
        byte ecB = _device.ReadByte(_addrLogoB);

        // Scale EC (0-60) back to HID (0-255)
        byte r = (byte)Math.Clamp((int)Math.Round(ecR * 255.0 / 60.0), 0, 255);
        byte g = (byte)Math.Clamp((int)Math.Round(ecG * 255.0 / 60.0), 0, 255);
        byte b = (byte)Math.Clamp((int)Math.Round(ecB * 255.0 / 60.0), 0, 255);

        // Decode one-hot brightness to 0-100 scale
        int brightness;
        if ((onOff & 0x80) != 0) brightness = 100;     // level 4
        else if ((onOff & 0x40) != 0) brightness = 75;  // level 3
        else if ((onOff & 0x20) != 0) brightness = 50;  // level 2
        else if (onOff != 0x01) brightness = 25;         // level 1
        else brightness = 0;                              // off

        bool enabled = brightness > 0;
        return (enabled, r, g, b, brightness);
    }
}
