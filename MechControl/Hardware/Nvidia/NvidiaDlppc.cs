namespace MechControl.Hardware.Nvidia;

// DLPPC (likely "Deep Learning Power/Performance Controller": its power model, DLPPM, runs on the
// GPU's neural network engine). A PMU PERF_CF controller that, under load, caps the memory clock
// (perf limit PERF_CF_CONTROLLER_DRAM_MAX) to give the core more of the power limit. On an RTX 5090
// Laptop (driver 617.14) it holds memory one step down (P2) in games. Its policy (label
// DLPPC_1X = 7) is switched by bActivate; idle downclocking is a different controller.
// PerfCfPolicyGetInfo V1 0x164C: policy mask +4, labelToIdxMap u16[16] +10, policies {type +0,
// label +8, controller mask +48} at +76 + p * 176. Get/SetControl V1 0x1548: mask +4, policies
// {type +0, bActivate +4} at +72 + p * 168. Layouts from the community nvapi.h, mapped on driver
// 617.14 (notes/nvidia_gpu_limiters.md). The driver resets the policy on a GPU restart.
internal sealed record NvidiaDlppc(bool Active, int Policy, uint ControllerMask);

internal sealed partial class NvidiaApi
{
    private const uint DlppcLabel = 7;

    // Null when this GPU/driver has no DLPPC policy. Active: the policy is requested on.
    public NvidiaDlppc? ReadDlppc()
    {
        if (FindDlppcPolicy() is not var (p, controllers)) return null;
        return new(ReadPolicyControl(p)[72 + p * 168 + 4] != 0, p, controllers);
    }

    public void SetDlppc(bool active)
    {
        var (p, _) = FindDlppcPolicy() ?? throw Unsupported("DLPPC policy");
        var control = ReadPolicyControl(p);
        control[72 + p * 168 + 4] = (byte)(active ? 1 : 0);
        Call(0x4d1c7d6e, control, "PerfCfPolicySetControl");
        if ((ReadPolicyControl(p)[72 + p * 168 + 4] != 0) != active)
            throw new InvalidOperationException("NVIDIA DLPPC policy readback did not retain the request.");
    }

    private (int Policy, uint Controllers)? FindDlppcPolicy()
    {
        var info = NewBuffer(0x164c, 0x1164c);
        Call(0x1cac3865, info, "PerfCfPolicyGetInfo(V1)");
        RequireVersion(info, 0x1164c);
        int p = BitConverter.ToUInt16(info, 10 + (int)DlppcLabel * 2);
        if (p >= 32 || (U32(info, 4) & (1u << p)) == 0) return null;
        int e = 76 + p * 176;
        if (U32(info, e + 8) != DlppcLabel || U32(info, e) != 0) throw Unsupported("PERF_CF policy identity");
        return (p, U32(info, e + 48));
    }

    // Selects only this policy, so a SET changes no other.
    private byte[] ReadPolicyControl(int p)
    {
        var control = NewBuffer(0x1548, 0x11548);
        Put(control, 4, 1u << p);
        Call(0x69de2c54, control, "PerfCfPolicyGetControl(V1)");
        RequireVersion(control, 0x11548);
        if (U32(control, 4) != 1u << p || U32(control, 72 + p * 168) != 0) throw Unsupported("PERF_CF policy control");
        return control;
    }
}
