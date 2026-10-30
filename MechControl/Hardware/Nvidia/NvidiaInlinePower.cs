using System.Buffers.Binary;
using System.Diagnostics;

namespace MechControl.Hardware.Nvidia;

// The GPU's ADC inline power controller (IPC): a hardware path that slows the clock below the
// driver controllers' caps. Thermal monitors of class ADC_IPC (5) count while it acts, at the
// utility clock (ThermMonitorsGetInfo V1 0x94C: monitor mask +4, utility clock kHz +8, class at
// +76 + m * 72; GetStatus V1 0xA48: class +72 + m * 80, u64 count +80 + m * 80; mapped on driver
// 617.14). On an RTX 5090 Laptop they count most while board input current limits, and also
// while the IPC clips power peaks (notes/nvidia_gpu_limiters.md). Read only with the live
// telemetry, so the monitors are not looked up until the Power tab is first shown.
internal sealed partial class NvidiaApi
{
    private uint inlineMask, inlineUtilityKhz;
    private bool inlineUnsupported;
    private ulong[]? inlineCounts;
    private long inlineTimestamp;

    // Share of time since the previous read (null on the first read or after more than 5 s). The
    // ADC_IPC monitors count by identical amounts or not at all; the busiest one is used.
    private double? ReadInlinePowerActivity()
    {
        if (inlineUnsupported) return null;
        try
        {
            if (inlineMask == 0)
            {
                var info = NewBuffer(0x94c, 0x1094c);
                Call(0xb2c9d666, info, "ThermMonitorsGetInfo(V1)");
                RequireVersion(info, 0x1094c);
                uint all = U32(info, 4), ipc = 0;
                for (int m = 0; m < 32; m++)
                    if ((all & (1u << m)) != 0 && U32(info, 76 + m * 72) == 5) ipc |= 1u << m;
                if (ipc == 0 || U32(info, 8) == 0) { inlineUnsupported = true; return null; }
                (inlineMask, inlineUtilityKhz) = (all, U32(info, 8));
            }
            var status = NewBuffer(0xa48, 0x10a48);
            Put(status, 4, inlineMask);
            Call(0x4b4bd039, status, "ThermMonitorsGetStatus(V1)");
            RequireVersion(status, 0x10a48);
            var counts = inlineCounts ??= new ulong[32];
            long now = Stopwatch.GetTimestamp();
            double seconds = (now - inlineTimestamp) / (double)Stopwatch.Frequency;
            bool valid = inlineTimestamp != 0 && seconds > 0 && seconds <= 5;
            ulong busiest = 0;
            for (int m = 0; m < 32; m++)
            {
                if ((inlineMask & (1u << m)) == 0 || U32(status, 72 + m * 80) != 5) continue;
                ulong count = BinaryPrimitives.ReadUInt64LittleEndian(status.AsSpan(80 + m * 80));
                if (count < counts[m]) valid = false; // Reset (GPU restart).
                else busiest = Math.Max(busiest, count - counts[m]);
                counts[m] = count;
            }
            inlineTimestamp = now;
            return valid ? busiest / (seconds * inlineUtilityKhz * 1000) : null;
        }
        catch (NotSupportedException) { inlineUnsupported = true; return null; }
        catch (InvalidOperationException) { return null; }
    }

    private void CloseInlinePower()
    {
        inlineMask = 0; inlineUnsupported = false; inlineTimestamp = 0; inlineCounts = null;
    }
}
