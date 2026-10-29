using MechControl.Driver;
using MechControl.Profile;

namespace MechControl.Features;

/// <summary>
/// Replicates the OEM GCUService EC initialization sequence for standalone operation.
///
/// On a fresh boot (no OEM service), the EC is in default/failsafe state.
/// The OEM service performs a multi-step handshake to bring the EC into
/// "custom control" mode. Without this, the EC ignores PL writes and the
/// system may enter failsafe (CPU locked at 800MHz).
///
/// Fan-curve state (0x7C5 bit 7 + 0x7C6) and the fan table itself are
/// deliberately NOT touched here — they are managed by FanControl /
/// FanSnapshot at the orchestration level so the UI can reflect real EC
/// state instead of always forcing custom mode on at boot.
///
/// Trimmed OEM init sequence:
///   1. 0x709=0x05, 0x708=0x01  — EC handshake ("OEM software present")
///   2. 0x741=0x05              — EC activation (gates PL/fan/GPU writes)
///   3. 0x748=0x81              — Feature enable register
///   4. 0x726|=0x80, 0x727|=0x40 — EC power management activation (CUME bit)
///   5. 0x7C5 |= 0x60          — Preserve bit 7, set bits 5 & 6 (OEM init bits)
///   6. 0x7C7=0x0C              — Fan curve parameter
///   7. 0x706=0x41              — BIOS_CTRL init complete signal
/// </summary>
class EcActivationControl
{
    private readonly WriteGuard _guard;
    private readonly EcDevice _device;
    private readonly MachineProfile _profile;

    // EC handshake registers (must be first)
    private const int AddrHandshake1 = 0x709;
    private const int AddrHandshake2 = 0x708;

    // EC power management activation (OEM Phase 5)
    // Writing bit 7 of 0x726 and bit 6 (CUME) of 0x727 activates
    // the EC's PECI power management loop so PL values are applied to the CPU.
    private const int AddrPowerMgmtByte9 = 0x726;
    private const int AddrPowerMgmtCume = 0x727;

    // EC activation byte. Bitfield: bit 0 = activation (DSDT APPE), bit 2 = cleared by
    // the EC when it accepts activation, bit 5 = fan error (OEM GetECFanAlert), bit 7 =
    // unknown EC-owned flag, set again by the EC after our write and kept until reboot.
    private const int AddrActivation = 0x741;
    private const byte ActivationBit = 0x01;
    private const byte ActivationPendingBit = 0x04;

    // Feature enable
    private const int AddrFeatureEnable = 0x748;

    // Fan curve control
    private const int AddrFanCurveEnable = 0x7C5;
    private const int AddrFanCurveParam = 0x7C7;

    // Init complete signal
    private const int AddrBiosCtrl = 0x706;

    public EcActivationControl(EcDevice device, WriteGuard guard, MachineProfile profile)
    {
        _device = device;
        _guard = guard;
        _profile = profile;
    }

    /// <summary>
    /// Read whether EC is unlocked (0x741 bit 0 set).
    /// </summary>
    public bool IsUnlocked()
    {
        byte val = _device.ReadByte(AddrActivation);
        return (val & ActivationBit) != 0;
    }

    /// <summary>Unlock EC writes (0x741 = 0x05).</summary>
    public void Unlock()
    {
        _guard.Write(AddrActivation, 0x05);
    }

    /// <summary>Lock EC writes (0x741 = 0x00).</summary>
    public void Lock()
    {
        _guard.Write(AddrActivation, 0x00);
    }

    /// <summary>
    /// Full EC initialization for standalone operation.
    /// Replicates the OEM GCUService startup handshake sequence.
    /// Must be called before any feature reads/writes.
    ///
    /// Each step is wrapped individually so a failure in one step
    /// doesn't prevent the remaining steps from executing.
    /// Returns a list of any errors encountered.
    /// </summary>
    public List<string> InitializeEc()
    {
        var errors = new List<string>();

        DiagnosticLog.Section("EC Initialization");

        void Step(string label, Action action)
        {
            try
            {
                action();
                DiagnosticLog.LogInitStep(label, true);
            }
            catch (Exception ex)
            {
                errors.Add($"{label}: {ex.Message}");
                DiagnosticLog.LogInitStep(label, false, ex.Message);
            }
            // 150ms per step. The OEM uses ~100ms (see notes/startup.md); we
            // add a bit of margin because the EC takes ~250ms to asynchronously
            // transform 0x741 from 0x05 -> 0x01 and we want the post-init verify
            // read to reliably catch the transformed state.
            Thread.Sleep(150);
        }

        Step("Handshake 0x709=0x05, 0x708=0x01", () =>
        {
            _guard.Write(AddrHandshake1, 0x05);
            _guard.Write(AddrHandshake2, 0x01);
        });

        Step("Activation 0x741=0x05", () =>
            _guard.Write(AddrActivation, 0x05));

        Step("Feature enable 0x748=0x81", () =>
            _guard.Write(AddrFeatureEnable, 0x81));

        Step("Power management activation 0x726|=0x80, 0x727|=0x40", () =>
        {
            byte b9 = _device.ReadByte(AddrPowerMgmtByte9);
            byte cume = _device.ReadByte(AddrPowerMgmtCume);
            DiagnosticLog.Write($"  0x726: 0x{b9:X2} -> 0x{(byte)(b9 | 0x80):X2}, 0x727: 0x{cume:X2} -> 0x{(byte)(cume | 0x40):X2}");
            _guard.Write(AddrPowerMgmtByte9, (byte)(b9 | 0x80));
            _guard.Write(AddrPowerMgmtCume, (byte)(cume | 0x40));
        });

        // Set OEM init bits 5 & 6 without touching bit 7 (custom fan curve enable).
        // Bit 7 is managed by FanControl.SetCustomFanCurveMode at the orchestration
        // level so the UI checkbox can reflect real EC state.
        Step("Fan curve init 0x7C5 |= 0x60", () =>
        {
            byte current = _device.ReadByte(AddrFanCurveEnable);
            _guard.Write(AddrFanCurveEnable, (byte)(current | 0x60));
        });

        Step("Fan curve param 0x7C7=0x0C", () =>
            _guard.Write(AddrFanCurveParam, 0x0C));

        Step("BIOS_CTRL 0x706=0x41", () =>
            _guard.Write(AddrBiosCtrl, 0x41));

        // Post-init verification: check that EC is processing activation register.
        // With 150ms inter-step sleeps, the 0x741 write happens ~900ms before
        // this read, well past the ~250ms async transform window.
        byte appe = _device.ReadByte(AddrActivation);
        if ((appe & (ActivationBit | ActivationPendingBit)) == ActivationBit)
            DiagnosticLog.Write($"EC init verified: 0x741 transformed (EC active, 0x{appe:X2})");
        else
            DiagnosticLog.Warn($"EC init: 0x741 = 0x{appe:X2} (expected bit 0 set, bit 2 cleared — EC may not be applying power limits)");

        DiagnosticLog.Write(errors.Count == 0
            ? "EC init: all steps completed successfully"
            : $"EC init: completed with {errors.Count} error(s)");

        return errors;
    }
}
