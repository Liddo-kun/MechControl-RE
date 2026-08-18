namespace MechControl.Hardware.PawnIo;

/// <summary>
/// PCI bus/device/function identity for one NVIDIA display-class candidate,
/// plus the vendor/device IDs reported by whatever enumerated it (0 = unknown,
/// e.g. when a probe tool is given a raw BDF on the command line).
/// PR #77's <c>ioctl_read_thermal_registers</c> requires the consumer to
/// supply the BDF; the module revalidates identity itself on every request.
/// </summary>
readonly struct PciBdf
{
    public byte Bus { get; }
    public byte Device { get; }
    public byte Function { get; }
    public ushort VendorId { get; }
    public ushort DeviceId { get; }

    public PciBdf(int bus, int device, int function, ushort vendorId = 0, ushort deviceId = 0)
    {
        if (bus is < 0 or > 0xFF)
            throw new ArgumentOutOfRangeException(nameof(bus));
        if (device is < 0 or > 0x1F)
            throw new ArgumentOutOfRangeException(nameof(device));
        if (function is < 0 or > 0x07)
            throw new ArgumentOutOfRangeException(nameof(function));

        Bus = (byte)bus;
        Device = (byte)device;
        Function = (byte)function;
        VendorId = vendorId;
        DeviceId = deviceId;
    }

    public override string ToString() => $"{Bus:X2}:{Device:X2}.{Function:X1}";
}
