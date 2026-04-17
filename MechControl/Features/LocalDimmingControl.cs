using System.Management;
using MechControl.Driver;

namespace MechControl.Features;

/// <summary>
/// Controls the mini-LED panel's local dimming via WMI/ACPI GPIO.
/// Two WMI commands per toggle: EC state flag (SA00=1/2) + GPIO signal (SA00=0x0D/0x0E).
/// State verified by reading EC register 0x72B bit 1 (CSMS).
/// </summary>
class LocalDimmingControl
{
    private readonly EcDevice _device;
    private const int AddrCsms = 0x72B;
    private const int CsmsBit = 1; // bit 1

    // WMI command constants
    private const ulong CmdEcOn  = 0x0000050000000002UL; // SA00=2: CTAF(1)
    private const ulong CmdEcOff = 0x0000050000000001UL; // SA00=1: CTAF(0)
    private const ulong CmdGpioHigh = 0x000005000000000EUL; // SA00=0x0E: GPIO HIGH (panel on)
    private const ulong CmdGpioLow  = 0x000005000000000DUL; // SA00=0x0D: GPIO LOW (panel off)

    // Expected WMI return values
    private const byte RetEcOn  = 0xBB;
    private const byte RetEcOff = 0xAA;
    private const byte RetGpioHigh = 0x0E;
    private const byte RetGpioLow  = 0x0D;

    public LocalDimmingControl(EcDevice device)
    {
        _device = device;
    }

    /// <summary>Read current dimming state from EC register 0x72B bit 1.</summary>
    public bool ReadState()
    {
        byte val = _device.ReadByte(AddrCsms);
        return (val & (1 << CsmsBit)) != 0;
    }

    /// <summary>
    /// Toggle local dimming on or off. Sends both EC state and GPIO commands,
    /// then verifies EC state matches expected value.
    /// </summary>
    public void SetState(bool enabled)
    {
        if (enabled)
        {
            byte r1 = WmiCall(CmdEcOn);
            if (r1 != RetEcOn)
                throw new InvalidOperationException(
                    $"EC state ON returned 0x{r1:X2}, expected 0x{RetEcOn:X2}");

            byte r2 = WmiCall(CmdGpioHigh);
            if (r2 != RetGpioHigh)
                throw new InvalidOperationException(
                    $"GPIO HIGH returned 0x{r2:X2}, expected 0x{RetGpioHigh:X2}");
        }
        else
        {
            byte r1 = WmiCall(CmdEcOff);
            if (r1 != RetEcOff)
                throw new InvalidOperationException(
                    $"EC state OFF returned 0x{r1:X2}, expected 0x{RetEcOff:X2}");

            byte r2 = WmiCall(CmdGpioLow);
            if (r2 != RetGpioLow)
                throw new InvalidOperationException(
                    $"GPIO LOW returned 0x{r2:X2}, expected 0x{RetGpioLow:X2}");
        }

        // Verify EC state matches
        bool actual = ReadState();
        if (actual != enabled)
            throw new InvalidOperationException(
                $"Local dimming verify failed: wrote {(enabled ? "ON" : "OFF")} but EC reads {(actual ? "ON" : "OFF")}");
    }

    private static byte WmiCall(ulong data)
    {
        using var obj = new ManagementObject("root\\WMI",
            "AcpiTest_MULong.InstanceName='ACPI\\PNP0C14\\1_1'", null);
        using var p = obj.GetMethodParameters("GetSetULong");
        p["Data"] = data;
        using var result = obj.InvokeMethod("GetSetULong", p, null);
        return (byte)(uint)result["Return"];
    }
}
