using System.Buffers.Binary;
using System.Text;

// Generated structural input, not firmware: only the supported parser structures
// are populated. No bytes from an OEM ROM or mVolt recovery file are included.
sealed class BiosFixture
{
    public byte[] Rom { get; }
    public int LegacyLength { get; }
    public int EfiLength { get; }
    public int Bit { get; }
    public int PowerToken { get; }
    public int Table { get; }
    public int Profile { get; }
    public ushort DeviceId { get; }
    public uint PowerDefault { get; }
    public uint PowerMaximum { get; }
    public uint NvDefault { get; }
    public uint MsDefault { get; }
    public string Version { get; }
    public int Entry(int index) => Table + 56 + index * 102;

    public BiosFixture(bool relocated = false)
    {
        LegacyLength = relocated ? 0x1800 : 0x1000;
        EfiLength = relocated ? 0x600 : 0x400;
        Bit = relocated ? 0x300 : 0x100;
        PowerToken = Bit + 0x100;
        int logicalTable = LegacyLength + (relocated ? 0x300 : 0x100);
        int logicalProfile = LegacyLength + 0x1000;
        Table = logicalTable + EfiLength;
        Profile = logicalProfile + EfiLength;
        DeviceId = relocated ? (ushort)0x5678 : (ushort)0x1234;
        PowerDefault = relocated ? 70000u : 130000u;
        PowerMaximum = relocated ? 120000u : 220000u;
        NvDefault = relocated ? 110000u : 150000u;
        MsDefault = relocated ? 25000u : 40000u;
        Version = relocated ? "96.05.06.07.08" : "97.01.02.03.04";
        Rom = new byte[LegacyLength + EfiLength + 0x3000];
        Image(0, LegacyLength, 0, false);
        Image(LegacyLength, EfiLength, 3, false);
        Image(LegacyLength + EfiLength, 0x3000, 0xe0, true);

        Rom[Bit] = 0xff; Rom[Bit + 1] = 0xb8; Text(Bit + 2, "BIT\0");
        U16(Bit + 6, 0x100); Rom[Bit + 8] = 12; Rom[Bit + 9] = 6; Rom[Bit + 10] = 2;
        Rom[Bit + 11] = unchecked((byte)-Rom.AsSpan(Bit, 11).ToArray().Sum(v => (int)v));
        Token(Bit + 12, 'B', 5, Bit + 0x80);
        Token(Bit + 18, 'P', 0xf8, PowerToken);
        byte[] version = relocated ? [7, 6, 5, 0x96, 8] : [3, 2, 1, 0x97, 4];
        version.CopyTo(Rom, Bit + 0x80);
        U32(PowerToken + 0x2c, (uint)logicalTable);
        U32(PowerToken + 0xf4, (uint)logicalProfile);
        new byte[] { 0x40, 0x38, 0x66, 20 }.CopyTo(Rom, Table);
        Policy(2, 1, 2, 0, 10000, PowerDefault, PowerMaximum);
        byte nvChannel = relocated ? (byte)9 : (byte)3;
        byte msChannel = relocated ? (byte)10 : (byte)4;
        Policy(11, 6, 1, nvChannel, 1, 1000000, 1000000);
        Policy(12, 6, 1, msChannel, 1, 1000000, 1000000);
        Policy(13, 7, 1, nvChannel, 1000, NvDefault, NvDefault);
        Policy(14, 7, 1, msChannel, 1000, MsDefault, MsDefault);
        new byte[] { 0x10, 0x0a, 5, 4, 7, 8, 1, 0x10, 9, 8 }.CopyTo(Rom, Profile);
        FixChecksums();
    }

    public void U16(int p, ushort value) => BinaryPrimitives.WriteUInt16LittleEndian(Rom.AsSpan(p), value);
    public void U32(int p, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(Rom.AsSpan(p), value);
    private void Text(int p, string value) => Encoding.ASCII.GetBytes(value).CopyTo(Rom, p);
    private void Token(int p, char id, ushort size, int offset)
    {
        Rom[p] = (byte)id; Rom[p + 1] = 2; U16(p + 2, size); U16(p + 4, (ushort)offset);
    }
    private void Image(int p, int length, byte type, bool last)
    {
        U16(p, 0xaa55); U16(p + 0x18, 0x40);
        Text(p + 0x40, "PCIR"); U16(p + 0x44, 0x10de); U16(p + 0x46, DeviceId);
        U16(p + 0x4a, 0x18); U16(p + 0x50, (ushort)(length / 512)); Rom[p + 0x54] = type;
        Text(p + 0x60, "NPDE"); U16(p + 0x64, 0x101); U16(p + 0x66, 12);
        U16(p + 0x68, (ushort)(length / 512)); Rom[p + 0x6a] = last ? (byte)0x80 : (byte)0;
    }
    private void Policy(int index, byte type, uint flags, byte channel, uint min, uint def, uint max)
    {
        int p = Entry(index); Rom[p] = type; U32(p + 1, flags); Rom[p + 5] = channel;
        U32(p + 13, min); U32(p + 17, def); U32(p + 21, max);
    }
    public void FixChecksums()
    {
        foreach (var (p, size) in new[] { (0, LegacyLength), (LegacyLength, EfiLength) })
        {
            Rom[p + size - 1] = 0;
            Rom[p + size - 1] = unchecked((byte)-Rom.AsSpan(p, size).ToArray().Sum(v => (int)v));
        }
    }
}
