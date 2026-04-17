using MechControl.Driver;
using MechControl.Profile;

namespace MechControl.Features;

struct BatteryTelemetry
{
    public bool OnAC;
    public int ChargePercent;
    public double TemperatureCelsius;
    public int CycleCount;
    public int DesignCapacityMah;
    public int FullChargeCapacityMah;
    public int RemainingCapacityMah;
    public int DesignVoltageMv;
    public int PresentVoltageMv;
    public int ChargeRateMa;
    public int HealthPercent;
}

class BatteryControl
{
    private readonly EcDevice _device;
    private readonly WriteGuard _guard;
    private readonly int _addrBatteryMode;
    private readonly byte _mask;
    private readonly byte _bit3Flag;

    // Standard ACPI battery registers — same address on all Uniwill EC models.
    // Verified via DSDT OperationRegion ECXP (SystemMemory at ECMA+0x400).
    // All 16-bit pairs are x86 little-endian: lower address = low byte.
    // DSDT _BIF Power Unit = 1 (mAh), so capacity/rate values are mAh/mA.
    // Temperature is SBS standard: 0.1 Kelvin.
    private const int ADDR_DESIGN_CAP       = 0x402;  // 16-bit LE, mAh (DSDT XIF1)
    private const int ADDR_FULL_CHARGE_CAP  = 0x404;  // 16-bit LE, mAh (DSDT XIF2) — for SOH
    private const int ADDR_DESIGN_VOLTAGE   = 0x408;  // 16-bit LE, mV  (DSDT XIF4)
    private const int ADDR_CHARGE_RATE      = 0x434;  // 16-bit LE, mA  (DSDT XST1)
    private const int ADDR_REMAINING_CAP    = 0x436;  // 16-bit LE, mAh (DSDT XST2)
    private const int ADDR_PRESENT_VOLTAGE  = 0x438;  // 16-bit LE, mV  (DSDT XST3)
    private const int ADDR_POWER_SOURCE     = 0x490;  // bit 0 = AC connected (DSDT ACIC)
    private const int ADDR_BATTERY_TEMP     = 0x4A2;  // 16-bit LE, 0.1K (SBS standard)
    private const int ADDR_CYCLE_COUNT      = 0x4A6;  // 16-bit LE (DSDT CYCN)

    public BatteryControl(EcDevice device, WriteGuard guard, MachineProfile profile)
    {
        _device = device;
        _guard = guard;
        _addrBatteryMode = profile.GetRegister("batteryMode");
        _mask = profile.Bitfields["batteryMode"].GetMask(); // 0x38

        // Detect battery mode variant: read current register, extract bit 3.
        // Intel models use bit 3 set (values 0x08/0x18/0x28).
        // AMD models observed with bit 3 clear (values 0x00/0x10/0x20).
        byte raw = _device.ReadByte(_addrBatteryMode);
        _bit3Flag = (byte)(raw & _mask & 0x08);
        DiagnosticLog.Write($"Battery mode variant: bit3={(_bit3Flag != 0 ? "set" : "clear")} " +
            $"(raw=0x{raw:X2}, values=0x{RegisterEncoding.EncodeBatteryMode(0, _bit3Flag):X2}/" +
            $"0x{RegisterEncoding.EncodeBatteryMode(1, _bit3Flag):X2}/" +
            $"0x{RegisterEncoding.EncodeBatteryMode(2, _bit3Flag):X2})");

        // Update profile BatteryModes values to match detected variant
        // so RestoreDefaults and other profile-based writes use correct encoding.
        for (int i = 0; i < profile.BatteryModes.Count && i < 3; i++)
            profile.BatteryModes[i].Value = $"0x{GetModeValue(i):X2}";
    }

    public byte GetModeValue(int mode) => RegisterEncoding.EncodeBatteryMode(mode, _bit3Flag);

    public int ReadBatteryMode()
    {
        byte raw = _device.ReadByte(_addrBatteryMode);
        return RegisterEncoding.DecodeBatteryMode(raw); // (reg >> 4) & 0x03
    }

    public void SetBatteryMode(int mode)
    {
        byte value = RegisterEncoding.EncodeBatteryMode(mode, _bit3Flag);
        _guard.WriteBitfield(_addrBatteryMode, _mask, value);
    }

    /// <summary>
    /// Read 16-bit little-endian value from consecutive EC addresses.
    /// ACPI battery registers in ECXP region are x86 LE: baseAddr = low byte.
    /// </summary>
    private int ReadWordLE(int baseAddr) =>
        _device.ReadByte(baseAddr) | (_device.ReadByte(baseAddr + 1) << 8);

    public BatteryTelemetry ReadBatteryTelemetry()
    {
        var t = new BatteryTelemetry();

        t.OnAC = (_device.ReadByte(ADDR_POWER_SOURCE) & 0x01) != 0;
        t.ChargePercent = _device.ReadByte(0x4AB);
        t.CycleCount = ReadWordLE(ADDR_CYCLE_COUNT);
        t.DesignCapacityMah = ReadWordLE(ADDR_DESIGN_CAP);
        t.FullChargeCapacityMah = ReadWordLE(ADDR_FULL_CHARGE_CAP);
        t.RemainingCapacityMah = ReadWordLE(ADDR_REMAINING_CAP);
        t.DesignVoltageMv = ReadWordLE(ADDR_DESIGN_VOLTAGE);
        t.PresentVoltageMv = ReadWordLE(ADDR_PRESENT_VOLTAGE);
        t.ChargeRateMa = ReadWordLE(ADDR_CHARGE_RATE);

        // Temperature: SBS 0.1K → Celsius
        int tempRaw = ReadWordLE(ADDR_BATTERY_TEMP);
        t.TemperatureCelsius = (tempRaw / 10.0) - 273.15;

        if (t.DesignCapacityMah > 0)
            t.HealthPercent = (int)Math.Round(100.0 * t.FullChargeCapacityMah / t.DesignCapacityMah);

        return t;
    }
}
