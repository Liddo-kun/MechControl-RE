using System.Buffers.Binary;

namespace MechControl.Hardware.Nvidia;

internal sealed record NvidiaBiosField(int Offset, uint Minimum, uint Default, uint Maximum);
internal sealed record NvidiaRailOverride(uint DefaultMa, uint MaximumMa);
internal sealed record NvidiaOverrideRequest(uint? PowerMaximumMw = null, NvidiaRailOverride? Nvvdd = null, NvidiaRailOverride? Msvdd = null);
internal sealed record NvidiaOverrideRecord(string Name, byte[] Data);

/// <summary>Validated BIT P power-policy layout. Offsets are image coordinates, not logical BIT addresses.</summary>
internal sealed class NvidiaBios
{
    private readonly byte[] image;
    private readonly byte[] version;
    public ushort DeviceId { get; }
    public string Version => string.Join(".", version.Select(b => b.ToString("X2")));
    public NvidiaBiosField Power { get; }
    public NvidiaBiosField Nvvdd { get; }
    public NvidiaBiosField Msvdd { get; }
    public byte[] GetImage() => (byte[])image.Clone();
    // Every place BuildOverrides can patch: the power maximum and both rails' default/maximum.
    public int[] PatchOffsets => [Power.Offset + 8, Nvvdd.Offset + 4, Nvvdd.Offset + 8, Msvdd.Offset + 4, Msvdd.Offset + 8];
    public uint ValueAt(int offset) => BinaryPrimitives.ReadUInt32LittleEndian(image.AsSpan(offset));

    private NvidiaBios(byte[] image, byte[] version, ushort deviceId, NvidiaBiosField power,
        NvidiaBiosField nvvdd, NvidiaBiosField msvdd)
    {
        this.image = image; this.version = version; DeviceId = deviceId;
        Power = power; Nvvdd = nvvdd; Msvdd = msvdd;
    }

    // Evidence: mVolt 0.50.8 FUN_14028cad0/14028c970/140291fb0/140292520/140293950.
    // Deliberately supports only the evidenced 0x40/0x38/0x66 policy layout, with no
    // active BIOS profile copies. A different layout/profile is not a writable guess.
    public static NvidiaBios Parse(byte[] rom)
    {
        ArgumentNullException.ThrowIfNull(rom);
        if (rom.Length < 512 || rom.Length > 0x1000000) throw Invalid("ROM size");
        byte[] b = (byte[])rom.Clone();
        void Span(int p, int n) { if (p < 0 || n < 0 || p > b.Length - n) throw Invalid("truncated ROM"); }
        ushort U16(int p) { Span(p, 2); return BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(p)); }
        uint U32(int p) { Span(p, 4); return BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(p)); }
        bool Sig(int p, string s) { Span(p, s.Length); return b.AsSpan(p, s.Length).SequenceEqual(System.Text.Encoding.ASCII.GetBytes(s)); }
        int legacy = 0, efi = 0, pcir = 0, pos = 0;
        bool ended = false;
        for (int i = 0; i < 16; i++)
        {
            Span(pos, 0x1a);
            ushort signature = U16(pos);
            if (signature != 0xaa55 && signature != 0x4e56 && signature != 0xbb77) throw Invalid("image signature");
            int pci = checked(pos + U16(pos + 0x18));
            if (!Sig(pci, "PCIR") && !Sig(pci, "NPDS") && !Sig(pci, "RGIS")) throw Invalid("PCI directory");
            Span(pci, 0x18);
            int pciLength = U16(pci + 10);
            if (pciLength < 0x18) throw Invalid("PCI directory length");
            int npde = checked((pci + pciLength + 15) & ~15);
            if (!Sig(npde, "NPDE") || (U16(npde + 4) != 0x100 && U16(npde + 4) != 0x101)
                || U16(npde + 6) < 12) throw Invalid("NPDE directory");
            Span(npde, U16(npde + 6));
            int length = U16(npde + 8) * 512, pciSize = U16(pci + 0x10) * 512;
            if (length == 0 || pciSize == 0 || pciSize > length || npde + U16(npde + 6) > pos + length)
                throw Invalid("image length");
            byte type = b[pci + 0x14];
            if (i == 0)
            {
                if (type != 0 || length != pciSize || U16(pci + 4) != 0x10de) throw Invalid("NVIDIA legacy image");
                legacy = length; pcir = pci;
            }
            else if (i == 1 && type == 3)
            {
                if (length != pciSize) throw Invalid("EFI image length");
                efi = length;
            }
            else if (type != 0xe0) throw Invalid("image ordering");
            if (type == 0 || type == 3)
            {
                Span(pos, length);
                int sum = 0; for (int j = pos; j < pos + length; j++) sum += b[j];
                if ((sum & 255) != 0) throw Invalid("option ROM checksum");
            }
            // NVAPI may return a truncated final non-legacy image (as in the capture).
            if (pos + length > b.Length && (i < 2 || type != 0xe0)) throw Invalid("truncated image");
            ended = (b[npde + 10] & 0x80) != 0;
            pos = checked(pos + length);
            if (ended) break;
            if (pos >= b.Length) throw Invalid("unterminated image chain");
        }
        if (!ended) throw Invalid("image count");
        int Map(uint logical, int count)
        {
            if (logical == 0 || logical == legacy || logical > int.MaxValue) throw Invalid("logical pointer");
            int p = checked((int)logical + (logical > legacy ? efi : 0));
            Span(p, count); return p;
        }
        int bit = -1;
        for (int p = 0; p <= legacy - 12; p++)
            if (b[p] == 0xff && b[p + 1] == 0xb8 && Sig(p + 2, "BIT\0"))
            { if (bit >= 0) throw Invalid("ambiguous BIT directory"); bit = p; }
        if (bit < 0 || U16(bit + 6) != 0x100 || b[bit + 8] != 12 || b[bit + 9] != 6)
            throw Invalid("BIT header");
        if ((b.AsSpan(bit, 12).ToArray().Sum(v => (int)v) & 255) != 0) throw Invalid("BIT checksum");
        int tokens = b[bit + 10];
        if (bit + 12 + tokens * 6 > legacy) throw Invalid("BIT entries");
        var entries = new Dictionary<byte, (byte Version, int Offset, int Size)>();
        for (int i = 0; i < tokens; i++)
        {
            int t = bit + 12 + 6 * i, size = U16(t + 2), ptr = U16(t + 4);
            if (entries.ContainsKey(b[t])) throw Invalid("duplicate BIT token");
            if (size == 0) continue;
            entries.Add(b[t], (b[t + 1], Map((uint)ptr, size), size));
        }
        if (!entries.TryGetValue((byte)'B', out var bios) || bios.Version != 2 || bios.Size < 5
            || !entries.TryGetValue((byte)'P', out var power) || power.Version != 2 || power.Size < 0xf8)
            throw Invalid("BIT B/P token");
        byte[] ver = { b[bios.Offset + 3], b[bios.Offset + 2], b[bios.Offset + 1], b[bios.Offset], b[bios.Offset + 4] };
        int table = Map(U32(power.Offset + 0x2c), 56 + 20 * 102);
        if (b[table] != 0x40 || b[table + 1] != 0x38 || b[table + 2] != 0x66 || b[table + 3] != 20)
            throw Invalid("power policy layout");
        int Entry(int index) => table + 56 + index * 102;
        NvidiaBiosField Field(int index, byte type, uint flags)
        {
            int e = Entry(index);
            if (b[e] != type || U32(e + 1) != flags || b[e + 5] >= 32) throw Invalid("policy identity");
            uint min = U32(e + 13), def = U32(e + 17), max = U32(e + 21);
            if (def < min || max < def || max == 0 || max > int.MaxValue) throw Invalid("policy range");
            return new(e + 13, min, def, max);
        }
        var board = Field(2, 1, 2);
        // mVolt aggregates board ceilings from slots 2/7/8. The captured layout
        // disables 7/8; do not silently ignore extra active board policies.
        foreach (int index in new[] { 7, 8 })
        {
            int e = Entry(index);
            if (b[e] != 0 || U32(e + 1) != 0 || U32(e + 13) != 0 || U32(e + 17) != 0 || U32(e + 21) != 0)
                throw Invalid("additional board-power policies are unsupported");
        }
        var nv = Field(13, 7, 1); var ms = Field(14, 7, 1);
        // Bind each current monitor to its corresponding REL companion policy,
        // not to this laptop's channel numbers (0x1b/0x1c).
        if (b[Entry(11)] != 6 || b[Entry(12)] != 6 || U32(Entry(11) + 1) != 1 || U32(Entry(12) + 1) != 1
            || b[Entry(11) + 5] != b[Entry(13) + 5] || b[Entry(12) + 5] != b[Entry(14) + 5]
            || b[Entry(13) + 5] == b[Entry(14) + 5]) throw Invalid("rail monitor identity");
        uint profilePtr = U32(power.Offset + 0xf4);
        if (profilePtr != 0)
        {
            int profile = Map(profilePtr, 0x58e);
            byte[] header = { 0x10, 0x0a, 5, 4, 7, 8, 1, 0x10, 9, 8 };
            if (!b.AsSpan(profile, 10).SequenceEqual(header)) throw Invalid("BIOS profile header");
            for (int i = 0; i < 4; i++)
                if ((b[profile + 10 + i * 0x3d] & 15) != 0) throw Invalid("active base BIOS profiles are unsupported");
            for (int i = 0; i < 16; i++)
                if ((b[profile + 0xfe + i * 0x49] & 15) != 0) throw Invalid("active extended BIOS profiles are unsupported");
        }
        return new(b, ver, U16(pcir + 6), board, nv, ms);
    }

    public IReadOnlyList<NvidiaOverrideRecord> BuildOverrides(NvidiaOverrideRequest request)
    {
        var patches = new SortedDictionary<int, uint>();
        void Add(NvidiaBiosField f, uint def, uint max)
        {
            if (def < f.Minimum || def == 0 || max > int.MaxValue) throw new ArgumentOutOfRangeException(nameof(request));
            if (def > max) throw new ArgumentOutOfRangeException(nameof(request), "BIOS default is above its maximum.");
            if (def != f.Default) patches.Add(f.Offset + 4, def);
            if (max != f.Maximum) patches.Add(f.Offset + 8, max);
        }
        if (request.PowerMaximumMw is uint power) Add(Power, Power.Default, power);
        if (request.Nvvdd is { } nv) Add(Nvvdd, nv.DefaultMa, nv.MaximumMa);
        if (request.Msvdd is { } ms) Add(Msvdd, ms.DefaultMa, ms.MaximumMa);
        return patches.Select((patch, i) =>
        {
            byte[] data = new byte[14]; version.CopyTo(data, 0);
            BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(5), (uint)patch.Key);
            data[9] = 4; BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(10), patch.Value);
            return new NvidiaOverrideRecord($"romOverride{i:X2}", data);
        }).ToArray();
    }

    private static InvalidDataException Invalid(string detail) => new($"Unsupported NVIDIA BIOS: {detail}.");
}
