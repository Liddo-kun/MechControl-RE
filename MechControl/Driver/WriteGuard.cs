using MechControl.Profile;

namespace MechControl.Driver;

sealed class WriteGuard
{
    private readonly EcDevice _device;
    private readonly HashSet<int> _allowlist;
    private readonly HashSet<int> _skipVerify;
    private readonly Dictionary<int, VerifyRedirectInfo> _verifyRedirect;

    public WriteGuard(EcDevice device, MachineProfile profile)
    {
        _device = device;
        _allowlist = profile.ExpandAllowlist();
        _skipVerify = profile.ExpandSkipVerify();
        _verifyRedirect = profile.ExpandVerifyRedirect();
    }

    public void Write(int addr, byte value)
    {
        if (!_allowlist.Contains(addr))
            throw new InvalidOperationException($"Write to 0x{addr:X3} blocked: address not in allowlist");

        _device.WriteByte(addr, value);

        if (_skipVerify.Contains(addr))
            return;

        // Determine verify target: either the same address, or a redirect (status register)
        int verifyAddr = addr;
        int expected = value;

        if (_verifyRedirect.TryGetValue(addr, out var redirect))
        {
            verifyAddr = redirect.GetReadAddr();
            // Both CPU temp limit encodings (Intel inverted_offset, AMD direct)
            // set bit 7 as the commit/enable flag on the write register. The
            // redirect read register (0x463) holds only the 7-bit payload, so
            // strip bit 7 before comparing.
            if (redirect.Transform == "inverted_offset" || redirect.Transform == "direct")
                expected = value & 0x7F;

            // Status registers update asynchronously — EC needs time to propagate
            Thread.Sleep(500);
        }

        // Verify with one retry
        byte readback = _device.ReadByte(verifyAddr);
        if (readback == expected)
            return;

        _device.WriteByte(addr, value);
        if (verifyAddr != addr)
            Thread.Sleep(500);
        readback = _device.ReadByte(verifyAddr);

        if (readback != expected)
            throw new IOException(
                $"Write verify failed at 0x{addr:X3}: wrote 0x{value:X2}, " +
                $"verify 0x{verifyAddr:X3} = {readback} (expected {expected})");
    }

    public void WriteBitfield(int addr, byte mask, byte value)
    {
        if (!_allowlist.Contains(addr))
            throw new InvalidOperationException($"Write to 0x{addr:X3} blocked: address not in allowlist");

        byte current = _device.ReadByte(addr);
        byte newVal = (byte)((current & ~mask) | (value & mask));
        Write(addr, newVal);
    }
}
