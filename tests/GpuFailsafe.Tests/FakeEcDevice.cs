namespace MechControl.Driver;

sealed class EcDevice
{
    public Dictionary<int, byte> Registers { get; } = new();
    public List<(int Address, byte Value)> Writes { get; } = new();
    public Func<int, byte>? ReadOverride { get; set; }
    public Action<int, byte>? WriteOverride { get; set; }
    public List<int> Reads { get; } = new();

    public byte ReadByte(int address)
    {
        Reads.Add(address);
        return ReadOverride?.Invoke(address) ?? (Registers.TryGetValue(address, out byte value) ? value : (byte)0);
    }

    public void WriteByte(int address, byte value)
    {
        Writes.Add((address, value));
        WriteOverride?.Invoke(address, value);
        Registers[address] = value;
    }
}
