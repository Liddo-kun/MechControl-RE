namespace MechControl.Profile;

static class RegisterEncoding
{
    // PL1, PL2: direct (1 byte = 1 watt)
    public static byte EncodePL1(int watts) => (byte)Math.Clamp(watts, 10, 210);
    public static int DecodePL1(byte reg) => reg;

    // PL4: encoding depends on CPU_PL4_Double_Flag (0x727 bit 7).
    //   doubled=true  -> 1 byte = 2 watts (register stores watts/2)
    //   doubled=false -> 1 byte = 1 watt  (register stores watts directly)
    // The register-value clamp (5..210) is the same for both; only the
    // watts<->register mapping differs.
    public static byte EncodePL4(int watts, bool doubled)
    {
        int reg = doubled ? watts / 2 : watts;
        return (byte)Math.Clamp(reg, 5, 210);
    }
    public static int DecodePL4(byte reg, bool doubled) => doubled ? reg * 2 : reg;

    // TGP: offset from 95W base TDP. The model range is enforced by callers;
    // 0xFF stays reserved because it reads as an unprogrammed/unreadable register.
    public static byte EncodeTGP(int watts, int offset = 95) =>
        (byte)Math.Clamp(watts - offset, 0, 0xFE);
    public static int DecodeTGP(byte reg, int offset = 95) => reg + offset;

    // Dynamic Boost: direct (1 byte = 1 watt)
    public static byte EncodeDB(int watts) => (byte)Math.Clamp(watts, 5, 25);
    public static int DecodeDB(byte reg) => reg;

    // CPU Temp Limit (Intel, inverted_offset): bit 7 = enable, bits 0-6 = (tjMax - temp)
    // Write to 0x786, read from 0x463.
    // Safety clamp is tjMax-relative: TccOffset must stay in 0..40 (the range
    // ECSpec byte[3] declares valid). Policy clamping to the profile's
    // cpuTempLimit min/max is the caller's job.
    public static byte EncodeCpuTempLimit(int tempC, int tjMax = 105) =>
        (byte)(0x80 | (tjMax - Math.Clamp(tempC, tjMax - 40, tjMax)));

    // CPU Temp Limit (AMD, direct): bit 7 = enable/commit, bits 0-6 = absolute temp C
    // Write to 0x786, read from 0x463.
    public static byte EncodeCpuTempLimitDirect(int tempC) =>
        (byte)(0x80 | Math.Clamp(tempC, 80, 120));

    // Use on 0x463 readback, NOT 0x786. Bit 7 at 0x463 is a "new value" flag the
    // EC sets on every change and clears later, so it is masked off.
    public static int DecodeCpuTempLimit(byte reg, bool direct, int tjMax = 105) =>
        direct ? reg & 0x7F : tjMax - (reg & 0x7F);

    // Fan duty: raw/2 = percent (register 200 = 100%)
    public static byte EncodeFanDuty(int percent) =>
        (byte)Math.Clamp(percent * 2, 0, 200);
    public static int DecodeFanDuty(byte reg) => reg / 2;

    // Fan switch speed: bit 7 = enable, bits 0-6 = time in 100ms units
    public static byte EncodeFanSwitchSpeed(bool enabled, int milliseconds)
    {
        if (!enabled) return 0x00;
        return (byte)(0x80 | Math.Clamp(milliseconds / 100, 1, 127));
    }
    public static (bool enabled, int milliseconds) DecodeFanSwitchSpeed(byte reg) =>
        ((reg & 0x80) != 0, (reg & 0x7F) * 100);

    // Battery mode: bits 4-5 encode the mode (0/1/2), bit 3 is a machine-specific
    // flag. Intel (Frida-captured): bit3=0x08, values {0x08, 0x18, 0x28}.
    // AMD (observed): bit3=0x00, values {0x00, 0x10, 0x20}.
    // Callers detect bit3 at startup and pass it through.
    public static byte EncodeBatteryMode(int mode, byte bit3Flag) =>
        (byte)((Math.Clamp(mode, 0, 2) << 4) | bit3Flag);
    public static int DecodeBatteryMode(byte reg) =>
        (reg >> 4) & 0x03;
}
