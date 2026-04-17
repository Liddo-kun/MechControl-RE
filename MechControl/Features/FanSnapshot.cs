using MechControl.Driver;
using MechControl.Profile;

namespace MechControl.Features;

/// <summary>
/// One-shot snapshot of all fan-related EC state: the full fan table, the two
/// curve-control registers (0x7C5, 0x7C6), and the RamFan2 probe range. Read
/// once at startup; everything else consumes the cached values.
/// Multi-model safe: fan-table addresses come from <see cref="FanTableInfo"/>.
/// 0x7C5 / 0x7C6 are universal Uniwill registers (see register_reference.md).
/// </summary>
sealed class FanSnapshot
{
    // Universal Uniwill fan-curve control registers.
    private const int AddrFanCurveEnable = 0x7C5; // bit 7 = custom curve enable
    private const int AddrFanCurveMode   = 0x7C6; // 0x04 = custom curve mode active
    private const int AddrRamFan2Probe   = 0xF60; // diagnostic only

    public required List<int> CpuTempUp   { get; init; }
    public required List<int> CpuTempDown { get; init; }
    public required List<int> CpuDuty     { get; init; }
    public required List<int> GpuTempUp   { get; init; }
    public required List<int> GpuTempDown { get; init; }
    public required List<int> GpuDuty     { get; init; }
    public required byte[] RamFan2Probe   { get; init; } // diagnostic only, not parsed
    public required byte CurveEnableReg   { get; init; } // 0x7C5 raw
    public required byte CurveModeReg     { get; init; } // 0x7C6 raw

    /// <summary>Read the full snapshot from EC in one pass (~114 byte reads).</summary>
    public static FanSnapshot Read(EcDevice device, FanTableInfo ft)
    {
        int n = ft.EntriesPerArray;
        var ramFan2 = new byte[16];
        for (int i = 0; i < 16; i++)
        {
            try { ramFan2[i] = device.ReadByte(AddrRamFan2Probe + i); }
            catch { ramFan2[i] = 0xFF; }
        }

        return new FanSnapshot
        {
            CpuTempUp      = ReadArray(device, ft.CpuTempUpAddr,   n),
            CpuTempDown    = ReadArray(device, ft.CpuTempDownAddr, n),
            CpuDuty        = ReadArray(device, ft.CpuDutyAddr,     n),
            GpuTempUp      = ReadArray(device, ft.GpuTempUpAddr,   n),
            GpuTempDown    = ReadArray(device, ft.GpuTempDownAddr, n),
            GpuDuty        = ReadArray(device, ft.GpuDutyAddr,     n),
            RamFan2Probe   = ramFan2,
            CurveEnableReg = device.ReadByte(AddrFanCurveEnable),
            CurveModeReg   = device.ReadByte(AddrFanCurveMode),
        };
    }

    private static List<int> ReadArray(EcDevice device, int baseAddr, int count)
    {
        var values = new List<int>(count);
        for (int i = 0; i < count; i++)
            values.Add(device.ReadByte(baseAddr + i));
        return values;
    }

    /// <summary>Custom curve mode active = 0x7C5 bit 7 set AND 0x7C6 == 0x04.</summary>
    public bool IsCustomCurveModeActive()
        => (CurveEnableReg & 0x80) != 0 && CurveModeReg == 0x04;

    public int DetectCpuActive() => FanCurveModel.DetectActiveEntriesFromList(CpuTempUp);

    /// <summary>
    /// GPU fan presence with tri-state semantics:
    ///   true  → real curve data present (non-uninit bytes seen)
    ///   false → all-0xFF (OEM "absent" pattern seen on CPU-only machines)
    ///   null  → ambiguous (all-0x00 cold-boot state; cannot distinguish
    ///           "CPU-only laptop" from "GPU laptop before OEM init ran")
    /// </summary>
    public bool? DetectGpuPresent()
    {
        bool anyReal = false;
        bool anyFF = false;
        foreach (int v in GpuTempUp)
        {
            if (v == 0) continue;
            if (v == 0xFF) { anyFF = true; continue; }
            anyReal = true;
            break;
        }
        if (anyReal) return true;
        if (anyFF) return false;
        return null;
    }

    /// <summary>
    /// CPU table passes sanity: &gt;= 2 active entries, ascending tempUp, all temps &lt;= 120C.
    /// </summary>
    public bool IsValid()
    {
        int active = DetectCpuActive();
        if (active < 2) return false;
        for (int i = 0; i < active; i++)
        {
            if (CpuTempUp[i] > 120) return false;
            if (i > 0 && CpuTempUp[i] <= CpuTempUp[i - 1]) return false;
        }
        return true;
    }

    public FanCurveModel BuildCpuCurve() =>
        FanCurveModel.FromRawArrays(CpuTempUp, CpuTempDown, CpuDuty);

    public FanCurveModel BuildGpuCurve() =>
        FanCurveModel.FromRawArrays(GpuTempUp, GpuTempDown, GpuDuty);
}
