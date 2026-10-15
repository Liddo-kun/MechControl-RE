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

    public bool VerifiesSameAddress(int address) => !_skipVerify.Contains(address) && !_verifyRedirect.ContainsKey(address);

    // Redirect-verify backoff. The EC's 0x463 updates on its own polling cycle,
    // so a fresh write isn't immediately visible — empirical sampling showed
    // commits clustering at ~300/550/900ms. These delays cover all three
    // clusters with a 1500ms safety cap. Cumulative read times: 450, 650, 1000, 1500ms.
    private static readonly int[] RedirectRetryDelaysMs = [450, 200, 350, 500];

    /// <summary>
    /// Optional self-heal hook invoked when a write still fails verification
    /// after the built-in retry. The EC clears its activation byte (0x741) on
    /// sleep/resume and then silently ignores PL/fan/GPU writes — the hook is
    /// expected to re-check activation and re-run EC init if it was lost.
    /// Return true if a recovery was performed (the failed write is then
    /// retried exactly once); false to let the original error propagate.
    /// </summary>
    public Func<bool>? VerifyFailureRecovery { get; set; }

    // Recovery itself writes through this guard (EC init sequence). The
    // thread-static flag stops those nested writes from recursively invoking
    // the recovery hook. Set via EnterRecoveryScope() by the recovery routine.
    [ThreadStatic]
    private static bool _inRecovery;

    /// <summary>
    /// Marks the current thread as executing verify-failure recovery so its
    /// writes can't re-trigger <see cref="VerifyFailureRecovery"/>. Restores
    /// the previous state on dispose.
    /// </summary>
    public static RecoveryScope EnterRecoveryScope() => new();

    public readonly struct RecoveryScope : IDisposable
    {
        private readonly bool _previous;
        public RecoveryScope() { _previous = _inRecovery; _inRecovery = true; }
        public void Dispose() => _inRecovery = _previous;
    }

    public void Write(int addr, byte value)
    {
        if (!_allowlist.Contains(addr))
            throw new InvalidOperationException($"Write to 0x{addr:X3} blocked: address not in allowlist");

        try
        {
            WriteAndVerify(addr, value);
        }
        catch (IOException) when (!_inRecovery && VerifyFailureRecovery != null)
        {
            if (!VerifyFailureRecovery())
                throw; // no recovery performed — original verify error stands

            // Recovery re-activated the EC — retry once. A second failure
            // propagates (this catch does not re-enter for its own body).
            WriteAndVerify(addr, value);
        }
    }

    private void WriteAndVerify(int addr, byte value)
    {
        _device.WriteByte(addr, value);

        if (_skipVerify.Contains(addr))
            return;

        if (_verifyRedirect.TryGetValue(addr, out var redirect))
        {
            VerifyRedirect(addr, value, redirect);
            return;
        }

        VerifySameAddr(addr, value);
    }

    private void VerifySameAddr(int addr, byte value)
    {
        byte readback = _device.ReadByte(addr);
        if (readback == value)
            return;

        _device.WriteByte(addr, value);
        readback = _device.ReadByte(addr);
        if (readback == value)
            return;

        throw new IOException(
            $"Write verify failed at 0x{addr:X3}: wrote 0x{value:X2}, " +
            $"verify 0x{addr:X3} = {readback} (expected {value})");
    }

    // Compare only the payload bits at the redirect read address. For CPU temp
    // limit encodings (inverted_offset / direct), bit 7 is a commit flag on the
    // write — the read address may mirror it set (cold EC) or cleared (warm EC
    // post-commit). Either is fine; we only care that our payload landed.
    private void VerifyRedirect(int writeAddr, byte value, VerifyRedirectInfo redirect)
    {
        int verifyAddr = redirect.GetReadAddr();
        byte mask = (redirect.Transform == "inverted_offset" || redirect.Transform == "direct")
            ? (byte)0x7F
            : (byte)0xFF;
        byte expected = (byte)(value & mask);

        byte readback = 0;
        for (int i = 0; i < RedirectRetryDelaysMs.Length; i++)
        {
            Thread.Sleep(RedirectRetryDelaysMs[i]);
            readback = _device.ReadByte(verifyAddr);
            if ((readback & mask) == expected)
                return;
        }

        throw new IOException(
            $"Write verify failed at 0x{writeAddr:X3}: wrote 0x{value:X2}, " +
            $"verify 0x{verifyAddr:X3} = {readback} (expected payload 0x{expected:X2}, mask 0x{mask:X2})");
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
