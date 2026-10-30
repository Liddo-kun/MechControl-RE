using MechControl.Hardware.Nvidia;
using System.Buffers.Binary;
using System.Reflection;
using System.Runtime.ExceptionServices;

static partial class Program
{
    private static int Main(string[] args)
    {
        (string Name, Action Test)[] tests =
        [
            ("generated BIOS discovers native limits and identity", NativeLimits),
            ("relocated different-model BIOS discovers its own limits and offsets", RelocatedModel),
            ("direct power record changes maximum only", PowerRecord),
            ("rail changes set default and maximum independently without changing other bytes", RailOverrides),
            ("board input default/maximum use discovered offsets without unrelated changes", BoardInputOverrides),
            ("unknown board input identities stay optional; recognized bad fields reject", OptionalBoardInput),
            ("original requests emit no override records", OriginalValues),
            ("invalid requests are rejected", InvalidRequests),
            ("parser owns its ROM snapshot", SnapshotIsolation),
            ("malformed ROM directories, ranges and monitor identities fail closed", Malformed),
            ("active base and extended profiles fail closed", Profiles),
            ("additional board policies fail closed", AdditionalPolicies),
            ("external override is never implicitly owned", ExternalOwnership),
            ("only patches for the saved BIOS version and offsets are owned", OwnedRecords),
            ("saved originals rebuild the original BIOS from the patched image", RestoreOriginal),
            ("legacy recovery appends board input originals and preserves ownership and stock restore", LegacyRecovery),
            ("firmware F7 resets never accumulate extra platform headroom", BudgetHeadroom),
            ("board current discovers alternate policies and preserves exact GET packet metadata", CurrentPolicyPacket),
            ("unsupported, ambiguous and out-of-range board current requests never SET; rails stay usable", CurrentPolicyRefusals),
            ("board current SET rejects missing readback and changed unselected rails", CurrentPolicyReadback),
            ("live readings discover controllers at alternate policy indices", Telemetry),
            ("unrecognized controller layouts, identities and ambiguity leave only those readings out", TelemetryFailsClosed),
        ];
        int failed = 0;
        foreach (var (name, test) in tests)
        {
            try { test(); Console.WriteLine($"PASS  {name}"); }
            catch (Exception ex) { failed++; Console.Error.WriteLine($"FAIL  {name}\n{ex}"); }
        }
        if (args.Length != 0)
        {
            try
            {
                if (args.Length != 4 || args[0] != "--captured-roms")
                    throw new ArgumentException("Optional arguments: --captured-roms ORIGINAL ACTIVE POWER_MAX_MW");
                CapturedRoms(args[1], args[2], uint.Parse(args[3], System.Globalization.CultureInfo.InvariantCulture));
                Console.WriteLine("PASS  optional captured ROM byte-for-byte equality");
            }
            catch (Exception ex) { failed++; Console.Error.WriteLine($"FAIL  optional captured ROM check\n{ex}"); }
        }
        Console.WriteLine($"{tests.Length + (args.Length == 0 ? 0 : 1) - failed}/{tests.Length + (args.Length == 0 ? 0 : 1)} tests passed; no GPU/registry handles opened.");
        return failed == 0 ? 0 : 1;
    }

    private static void BudgetHeadroom()
    {
        var original = new NvidiaBudgetSnapshot(300000, 500000);
        var raised = original.RaiseTo(320000, original);
        Equal(320000u, raised.F7Mw); Equal(520000u, raised.PlatformMw);
        var reset = raised with { F7Mw = 300000 };
        Equal(raised, reset.RaiseTo(320000, original));
        Equal(530000u, reset.RaiseTo(330000, original).PlatformMw);
        // A lower request also lowers the borrowed headroom, never below the baseline.
        Equal(original with { F7Mw = 310000, PlatformMw = 510000 }, raised.RaiseTo(310000, original));
        Equal(original, raised.RaiseTo(250000, original));
    }

    private static void NativeLimits()
    {
        var f = new BiosFixture(); var bios = NvidiaBios.Parse(f.Rom);
        Equal(f.DeviceId, bios.DeviceId); Equal(f.Version, bios.Version);
        Equal(10000u, bios.Power.Minimum); Equal(f.PowerDefault, bios.Power.Default); Equal(f.PowerMaximum, bios.Power.Maximum);
        Equal(f.NvDefault, bios.Nvvdd.Default); Equal(f.NvDefault, bios.Nvvdd.Maximum);
        Equal(f.MsDefault, bios.Msvdd.Default); Equal(f.MsDefault, bios.Msvdd.Maximum);
        Equal(f.InputDefault, bios.BoardInput!.Default); Equal(f.InputDefault, bios.BoardInput.Maximum);
        Equal(7, bios.PatchOffsets.Length);
    }

    private static void RelocatedModel()
    {
        var f = new BiosFixture(true); var bios = NvidiaBios.Parse(f.Rom);
        Equal(f.DeviceId, bios.DeviceId); Equal(f.Version, bios.Version);
        Equal(f.PowerDefault, bios.Power.Default); Equal(f.PowerMaximum, bios.Power.Maximum);
        Equal(f.NvDefault, bios.Nvvdd.Default); Equal(f.MsDefault, bios.Msvdd.Default);
        var records = bios.BuildOverrides(new(f.PowerMaximum + 5000));
        Equal(1, records.Count); Equal((uint)(f.Entry(2) + 21), Offset(records[0]));
        Equal(f.PowerMaximum + 5000, NvidiaBios.Parse(Apply(f.Rom, records)).Power.Maximum);
        True(bios.Power.Offset != NvidiaBios.Parse(new BiosFixture().Rom).Power.Offset);
    }

    private static void PowerRecord()
    {
        var f = new BiosFixture(); var bios = NvidiaBios.Parse(f.Rom);
        var records = bios.BuildOverrides(new(f.PowerMaximum + 10000));
        Equal(1, records.Count); Equal("romOverride00", records[0].Name);
        byte[] data = records[0].Data;
        Equal(14, data.Length); True(data.AsSpan(0, 5).SequenceEqual(new byte[] { 0x97, 1, 2, 3, 4 }));
        Equal((byte)4, data[9]); Equal((uint)(f.Entry(2) + 21), Offset(records[0]));
        Equal(f.PowerMaximum + 10000, BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(10)));
        byte[] expected = (byte[])f.Rom.Clone(); BinaryPrimitives.WriteUInt32LittleEndian(expected.AsSpan(f.Entry(2) + 21), f.PowerMaximum + 10000);
        True(expected.AsSpan().SequenceEqual(Apply(f.Rom, records)));
    }

    private static void RailOverrides()
    {
        foreach (bool relocated in new[] { false, true })
        {
            var f = new BiosFixture(relocated); var bios = NvidiaBios.Parse(f.Rom);
            uint nv = f.NvDefault + 7000, nvMax = f.NvDefault * 2, ms = f.MsDefault + 3000;
            var records = bios.BuildOverrides(new(null, new(nv, nvMax), new(ms, ms)));
            Equal(4, records.Count);
            int[] offsets = [f.Entry(13) + 17, f.Entry(13) + 21, f.Entry(14) + 17, f.Entry(14) + 21];
            uint[] values = [nv, nvMax, ms, ms];
            byte[] expected = (byte[])f.Rom.Clone();
            for (int i = 0; i < records.Count; i++)
            {
                Equal($"romOverride{i:X2}", records[i].Name); Equal((uint)offsets[i], Offset(records[i]));
                BinaryPrimitives.WriteUInt32LittleEndian(expected.AsSpan(offsets[i]), values[i]);
            }
            byte[] target = Apply(f.Rom, records); True(target.AsSpan().SequenceEqual(expected));
            var parsed = NvidiaBios.Parse(target);
            Equal(nv, parsed.Nvvdd.Default); Equal(nvMax, parsed.Nvvdd.Maximum);
            Equal(ms, parsed.Msvdd.Default); Equal(ms, parsed.Msvdd.Maximum);
            Equal(f.PowerMaximum, parsed.Power.Maximum);
            // A maximum alone (default unchanged) patches one field.
            var maximumOnly = bios.BuildOverrides(new(null, new(f.NvDefault, nvMax)));
            Equal(1, maximumOnly.Count); Equal((uint)(f.Entry(13) + 21), Offset(maximumOnly[0]));
        }
    }

    private static void OriginalValues()
    {
        var f = new BiosFixture(); var bios = NvidiaBios.Parse(f.Rom);
        Equal(0, bios.BuildOverrides(new()).Count);
        Equal(0, bios.BuildOverrides(new(f.PowerMaximum, new(f.NvDefault, f.NvDefault), new(f.MsDefault, f.MsDefault), new(f.InputDefault, f.InputDefault))).Count);
    }

    private static void BoardInputOverrides()
    {
        foreach (bool relocated in new[] { false, true })
        {
            var f = new BiosFixture(relocated); var bios = NvidiaBios.Parse(f.Rom);
            uint def = f.InputDefault + 1000, max = f.InputDefault + 4000;
            var records = bios.BuildOverrides(new(BoardInput: new(def, max)));
            Equal(2, records.Count);
            Equal((uint)(f.Entry(9) + 17), Offset(records[0])); Equal((uint)(f.Entry(9) + 21), Offset(records[1]));
            byte[] expected = (byte[])f.Rom.Clone();
            BinaryPrimitives.WriteUInt32LittleEndian(expected.AsSpan(f.Entry(9) + 17), def);
            BinaryPrimitives.WriteUInt32LittleEndian(expected.AsSpan(f.Entry(9) + 21), max);
            byte[] target = Apply(f.Rom, records); True(target.AsSpan().SequenceEqual(expected));
            var parsed = NvidiaBios.Parse(target);
            Equal(def, parsed.BoardInput!.Default); Equal(max, parsed.BoardInput.Maximum);
            var maximumOnly = bios.BuildOverrides(new(BoardInput: new(f.InputDefault, max)));
            Equal(1, maximumOnly.Count); Equal((uint)(f.Entry(9) + 21), Offset(maximumOnly[0]));
            foreach (var invalid in new NvidiaRailOverride[] { new(0, max), new(def, def - 1), new(def, uint.MaxValue) })
                Throws<ArgumentOutOfRangeException>(() => bios.BuildOverrides(new(BoardInput: invalid)));
        }
    }

    private static void OptionalBoardInput()
    {
        Action<BiosFixture>[] unknown =
        [
            f => f.Rom[f.Entry(9)] = 0,
            f => f.Rom[f.Entry(9)] = 0x0b,
            f => f.U32(f.Entry(9) + 1, 0x10001),
        ];
        foreach (var mutation in unknown)
        {
            var f = new BiosFixture(); mutation(f);
            // Unknown fields are not range-validated or writable.
            f.U32(f.Entry(9) + 21, 0);
            var bios = NvidiaBios.Parse(f.Rom); True(bios.BoardInput == null); Equal(5, bios.PatchOffsets.Length);
            var records = bios.BuildOverrides(new(f.PowerMaximum + 1000));
            byte[] expected = (byte[])f.Rom.Clone(); f.U32(f.Entry(2) + 21, f.PowerMaximum + 1000);
            True(Apply(expected, records).AsSpan().SequenceEqual(f.Rom));
            Throws<InvalidOperationException>(() => bios.BuildOverrides(new(BoardInput: new(1000, 1000))));
        }
        Action<BiosFixture>[] malformed =
        [
            f => f.Rom[f.Entry(9) + 5] = 32,
            f => f.U32(f.Entry(9) + 13, f.InputDefault + 1),
            f => f.U32(f.Entry(9) + 21, f.InputDefault - 1),
            f => { f.U32(f.Entry(9) + 13, 0); f.U32(f.Entry(9) + 17, 0); f.U32(f.Entry(9) + 21, 0); },
            f => f.U32(f.Entry(9) + 21, uint.MaxValue),
        ];
        foreach (var mutation in malformed)
        {
            var f = new BiosFixture(); mutation(f);
            Throws<InvalidDataException>(() => NvidiaBios.Parse(f.Rom));
        }
    }

    private static void InvalidRequests()
    {
        var bios = NvidiaBios.Parse(new BiosFixture().Rom);
        uint nv = bios.Nvvdd.Default;
        foreach (var request in new NvidiaOverrideRequest[] { new(0), new(bios.Power.Default - 1), new(uint.MaxValue), new(null, new(0, nv)),
            new(null, null, new(999, bios.Msvdd.Default)), new(null, new(nv, uint.MaxValue)), new(null, new(nv + 1, nv)) })
            Throws<ArgumentOutOfRangeException>(() => bios.BuildOverrides(request));
    }

    private static void SnapshotIsolation()
    {
        var f = new BiosFixture(); var bios = NvidiaBios.Parse(f.Rom); byte[] before = bios.GetImage();
        f.Rom[f.Entry(2) + 21] ^= 1;
        byte[] exported = bios.GetImage(); exported[0] = 0;
        True(before.AsSpan().SequenceEqual(bios.GetImage())); Equal(f.PowerMaximum, bios.Power.Maximum);
    }

    private static void Malformed()
    {
        Action<BiosFixture>[] mutations =
        [
            f => f.Rom[0] = 0,
            f => f.Rom[0x40] = 0,
            f => f.U16(0x68, 0),
            f => f.Rom[f.Bit + 11] ^= 1,
            f => f.Rom[f.Bit + 18] = (byte)'B',
            f => f.Rom[f.Bit + 19] = 1,
            f => f.U32(f.PowerToken + 0x2c, uint.MaxValue),
            f => f.U32(f.PowerToken + 0x2c, (uint)f.LegacyLength),
            f => f.Rom[f.Table] = 0x41,
            f => f.Rom[f.Table + 1]++,
            f => f.Rom[f.Table + 2]++,
            f => f.Rom[f.Table + 3]--,
            f => f.Rom[f.Entry(2)] = 7,
            f => f.U32(f.Entry(2) + 17, f.PowerMaximum + 1),
            f => f.U32(f.Entry(13) + 21, 0),
            f => f.Rom[f.Entry(13) + 5] = 32,
            f => f.Rom[f.Entry(11) + 5]++,
            f => { f.Rom[f.Entry(14) + 5] = f.Rom[f.Entry(13) + 5]; f.Rom[f.Entry(12) + 5] = f.Rom[f.Entry(13) + 5]; },
        ];
        foreach (var mutation in mutations)
        {
            var f = new BiosFixture(); mutation(f); f.FixChecksums();
            Throws<InvalidDataException>(() => NvidiaBios.Parse(f.Rom));
        }
        var complete = new BiosFixture();
        Throws<InvalidDataException>(() => NvidiaBios.Parse(complete.Rom[..400]));
        Throws<InvalidDataException>(() => NvidiaBios.Parse(complete.Rom[..(complete.Table + 100)]));
        var checksum = new BiosFixture(); checksum.Rom[checksum.LegacyLength - 1]++;
        Throws<InvalidDataException>(() => NvidiaBios.Parse(checksum.Rom));
    }

    private static void Profiles()
    {
        foreach (int index in Enumerable.Range(0, 4))
        {
            var f = new BiosFixture(); f.Rom[f.Profile + 10 + index * 0x3d] = 1;
            Throws<InvalidDataException>(() => NvidiaBios.Parse(f.Rom));
        }
        foreach (int index in Enumerable.Range(0, 16))
        {
            var f = new BiosFixture(); f.Rom[f.Profile + 0xfe + index * 0x49] = 1;
            Throws<InvalidDataException>(() => NvidiaBios.Parse(f.Rom));
        }
        var badHeader = new BiosFixture(); badHeader.Rom[badHeader.Profile + 8]++;
        Throws<InvalidDataException>(() => NvidiaBios.Parse(badHeader.Rom));
        var noProfiles = new BiosFixture(); noProfiles.U32(noProfiles.PowerToken + 0xf4, 0); noProfiles.FixChecksums();
        Equal(noProfiles.PowerMaximum, NvidiaBios.Parse(noProfiles.Rom).Power.Maximum);
    }

    private static void AdditionalPolicies()
    {
        foreach (int index in new[] { 7, 8 })
        {
            var f = new BiosFixture(); f.U32(f.Entry(index) + 21, 90000);
            Throws<InvalidDataException>(() => NvidiaBios.Parse(f.Rom));
        }
    }

    // Reflection is confined to the store's pure helpers. No store constructor,
    // SetupAPI call, registry key or recovery file is used.
    private static object? StoreMethod(string name, params object?[] args)
    {
        var method = typeof(NvidiaOverrideStore).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static)
            ?? throw new Exception($"Missing pure store method {name}.");
        try { return method.Invoke(null, args); }
        catch (TargetInvocationException ex) when (ex.InnerException != null)
        { ExceptionDispatchInfo.Capture(ex.InnerException).Throw(); throw; }
    }
    private static NvidiaOverrideRecovery Recovery(NvidiaBios bios) => new(bios.Version, bios.PatchOffsets.ToDictionary(o => o, bios.ValueAt));
    private static Dictionary<string, byte[]> Registry(IEnumerable<NvidiaOverrideRecord> records) =>
        records.ToDictionary(r => r.Name, r => r.Data, StringComparer.OrdinalIgnoreCase);

    private static void ExternalOwnership()
    {
        StoreMethod("RequireOwned", Registry([]), null);
        var f = new BiosFixture(); var records = NvidiaBios.Parse(f.Rom).BuildOverrides(new(f.PowerMaximum + 1));
        Throws<InvalidOperationException>(() => StoreMethod("RequireOwned", Registry(records), null));
    }

    private static void OwnedRecords()
    {
        foreach (bool relocated in new[] { false, true })
        {
            var f = new BiosFixture(relocated); var bios = NvidiaBios.Parse(f.Rom); var recovery = Recovery(bios);
            var records = bios.BuildOverrides(new(f.PowerMaximum + 1000, new(f.NvDefault + 7000, f.NvDefault + 7000), new(f.MsDefault + 3000, f.MsDefault + 3000)));
            Equal(5, records.Count);
            StoreMethod("RequireOwned", Registry(records), recovery);
            StoreMethod("RequireOwned", Registry(records.Take(2)), recovery);
            void Reject(Action<byte[]> change)
            {
                byte[] data = (byte[])records[0].Data.Clone(); change(data);
                Throws<InvalidOperationException>(() => StoreMethod("RequireOwned", Registry([records[1], new(records[0].Name, data)]), recovery));
            }
            Reject(v => v[0] ^= 1); Reject(v => v[9] = 8);
            Reject(v => BinaryPrimitives.WriteUInt32LittleEndian(v.AsSpan(5), (uint)f.Table));
            Throws<InvalidOperationException>(() => StoreMethod("RequireOwned", Registry([new("romOverride00", records[0].Data[..13])]), recovery));
        }
    }

    private static void RestoreOriginal()
    {
        foreach (bool relocated in new[] { false, true })
        {
            var f = new BiosFixture(relocated); var bios = NvidiaBios.Parse(f.Rom);
            byte[] patched = Apply(f.Rom, bios.BuildOverrides(new(f.PowerMaximum + 1000, new(f.NvDefault + 7000, f.NvDefault * 2))));
            var restored = (NvidiaBios)StoreMethod("Restore", patched, Recovery(bios))!;
            True(restored.GetImage().AsSpan().SequenceEqual(f.Rom));
            Equal(f.PowerMaximum, restored.Power.Maximum); Equal(f.NvDefault, restored.Nvvdd.Default);
        }
    }

    private static void CapturedRoms(string originalPath, string activePath, uint target)
    {
        byte[] original = File.ReadAllBytes(originalPath), active = File.ReadAllBytes(activePath);
        var bios = NvidiaBios.Parse(original);
        True(Apply(original, bios.BuildOverrides(new(target))).AsSpan().SequenceEqual(active));
        Equal(target, NvidiaBios.Parse(active).Power.Maximum);
    }

    private static void LegacyRecovery()
    {
        foreach (bool relocated in new[] { false, true })
        {
            var f = new BiosFixture(relocated); var stock = NvidiaBios.Parse(f.Rom);
            var legacy = new NvidiaOverrideRecovery(stock.Version, stock.PatchOffsets.Take(5).ToDictionary(o => o, stock.ValueAt));
            var oldRecords = stock.BuildOverrides(new(f.PowerMaximum + 1000, new(f.NvDefault + 1000, f.NvDefault + 1000), new(f.MsDefault + 1000, f.MsDefault + 1000)));
            Equal(5, oldRecords.Count);
            StoreMethod("RequireOwned", Registry(oldRecords), legacy);
            byte[] active = Apply(f.Rom, oldRecords);
            var recovered = (NvidiaBios)StoreMethod("Restore", active, legacy)!;
            var complete = (NvidiaOverrideRecovery)StoreMethod("CompleteRecovery", recovered, legacy)!;
            Equal(7, complete.Originals.Count); Equal(5, legacy.Originals.Count);
            foreach (var (offset, value) in legacy.Originals) Equal(value, complete.Originals[offset]);
            Equal(f.InputDefault, complete.Originals[f.Entry(9) + 17]);
            Equal(f.InputDefault, complete.Originals[f.Entry(9) + 21]);
            StoreMethod("RequireOwned", Registry(oldRecords), complete);
            var newRecords = recovered.BuildOverrides(new(f.PowerMaximum + 1000, new(f.NvDefault + 1000, f.NvDefault + 1000),
                new(f.MsDefault + 1000, f.MsDefault + 1000), new(f.InputDefault + 1000, f.InputDefault + 1000)));
            Equal(7, newRecords.Count);
            Throws<InvalidOperationException>(() => StoreMethod("RequireOwned", Registry(newRecords), legacy));
            StoreMethod("RequireOwned", Registry(newRecords), complete);
            var restored = (NvidiaBios)StoreMethod("Restore", Apply(f.Rom, newRecords), complete)!;
            True(restored.GetImage().AsSpan().SequenceEqual(f.Rom));
            // Even a conflicting caller snapshot must never replace saved values.
            var preserved = (NvidiaOverrideRecovery)StoreMethod("CompleteRecovery", NvidiaBios.Parse(active), legacy)!;
            foreach (var (offset, value) in legacy.Originals) Equal(value, preserved.Originals[offset]);
            var again = (NvidiaOverrideRecovery)StoreMethod("CompleteRecovery", restored, complete)!;
            Equal(7, again.Originals.Count);
            foreach (var (offset, value) in complete.Originals) Equal(value, again.Originals[offset]);
            var fresh = (NvidiaOverrideRecovery)StoreMethod("CompleteRecovery", stock, null)!;
            Equal(7, fresh.Originals.Count);
        }
    }
    private static uint Offset(NvidiaOverrideRecord record) => BinaryPrimitives.ReadUInt32LittleEndian(record.Data.AsSpan(5));
    private static byte[] Apply(byte[] original, IReadOnlyList<NvidiaOverrideRecord> records)
    {
        byte[] target = (byte[])original.Clone();
        foreach (var record in records) record.Data.AsSpan(10).CopyTo(target.AsSpan(checked((int)Offset(record))));
        return target;
    }
    private static void Equal<T>(T expected, T actual)
    { if (!EqualityComparer<T>.Default.Equals(expected, actual)) throw new Exception($"Expected {expected}, got {actual}."); }
    private static void True(bool value) { if (!value) throw new Exception("Assertion failed."); }
    private static void Throws<T>(Action action) where T : Exception
    {
        try { action(); } catch (T) { return; }
        throw new Exception($"Expected {typeof(T).Name}.");
    }
}
