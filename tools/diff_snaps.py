#!/usr/bin/env python3
"""Multi-snapshot differ for ec_snap_*.bin files.

Compares snap1->snap2, snap2->snap3, snap1->snap3 and annotates each
changed byte with its meaning from IDY_7Y.json where known.

File offset 0 = EC address 0x400.
Files are 3072 bytes covering 0x400..0xFFF.
"""
import os
import sys

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CLEAN_DIR = os.path.abspath(os.path.join(BASE_DIR, ".."))
START_ADDR = 0x400

# Register annotations from IDY_7Y.json and Uniwill RE notes
ANNOTATIONS = {
    0x43E: "cpuTemp",
    0x44F: "gpuTemp",
    0x463: "cpuTempLimitRead (7-bit payload)",
    0x464: "cpuFanRpmLo?",
    0x465: "cpuFanRpmHi?",
    0x46B: "gpuFanRpmLo?",
    0x46C: "gpuFanRpmHi?",
    0x4AB: "batterySoc",
    0x740: "projectId",
    0x741: "?  (writeAllowlist+skipVerify)",
    0x743: "tgpDbCtrl (bit mask 0x07 = dynBoostEn)",
    0x744: "tgp",
    0x745: "dbTotalPower",
    0x746: "dbMaxTgp",
    0x748: "? (writeAllowlist+skipVerify)",
    0x751: "fanControlMode (0x40=boost, 0x00=normal)",
    0x75B: "cpuFanDuty (live)",
    0x75C: "gpuFanDuty (live)",
    0x767: "logoTrigger",
    0x768: "statusByte",
    0x769: "logoR",
    0x76A: "logoG",
    0x76B: "logoB",
    0x783: "PL1",
    0x784: "PL2",
    0x785: "PL4",
    0x786: "cpuTempLimitWrite",
    0x787: "fanSwitchSpeed",
    0x78C: "logoCommit",
    0x7A6: "batteryMode",
    0x7AB: "logoOnOff",
    0x7C5: "fanCurveEnable (bit7 = master)",
    0x7C6: "fanCurveApply/Mode (Uniwill level flag)",
    0x7C7: "? (writeAllowlist+skipVerify)",
}

# Fan table addresses
for i in range(16):
    ANNOTATIONS[0xF00 + i] = f"cpuTempUp[{i}]"
    ANNOTATIONS[0xF10 + i] = f"cpuTempDown[{i}]"
    ANNOTATIONS[0xF20 + i] = f"cpuDuty[{i}]  (raw/2=%)"
    ANNOTATIONS[0xF30 + i] = f"gpuTempUp[{i}]"
    ANNOTATIONS[0xF40 + i] = f"gpuTempDown[{i}]"
    ANNOTATIONS[0xF50 + i] = f"gpuDuty[{i}]  (raw/2=%)"


def note(addr):
    return ANNOTATIONS.get(addr, "")


def load(n):
    path = os.path.join(CLEAN_DIR, f"ec_snap_{n}.bin")
    with open(path, "rb") as f:
        data = f.read()
    print(f"loaded {path} ({len(data)} bytes)")
    return data


def diff(a, b, label_a, label_b, show_volatile=True):
    print()
    print("=" * 78)
    print(f"DIFF  {label_a}  ->  {label_b}")
    print("=" * 78)
    count = 0
    # Volatile registers that change moment-to-moment — show but mark
    volatile = {0x43E, 0x44F, 0x463, 0x464, 0x465, 0x46B, 0x46C,
                0x4AB, 0x75B, 0x75C, 0x768}
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            addr = START_ADDR + i
            ann = note(addr)
            tag = " (live)" if addr in volatile else ""
            print(f"  [0x{addr:03X}]  0x{a[i]:02X} -> 0x{b[i]:02X}  "
                  f"({a[i]:3d} -> {b[i]:3d}){tag}  {ann}")
            count += 1
    print(f"  --- {count} register(s) changed ---")
    return count


def fan_table(data, label):
    """Render fan table as grid."""
    print()
    print(f"--- Fan table in {label} ---")
    cpu_up = [data[0xF00 - START_ADDR + i] for i in range(16)]
    cpu_dn = [data[0xF10 - START_ADDR + i] for i in range(16)]
    cpu_dt = [data[0xF20 - START_ADDR + i] for i in range(16)]
    gpu_up = [data[0xF30 - START_ADDR + i] for i in range(16)]
    gpu_dn = [data[0xF40 - START_ADDR + i] for i in range(16)]
    gpu_dt = [data[0xF50 - START_ADDR + i] for i in range(16)]

    header = " idx |" + "|".join(f"{i:4d}" for i in range(16))
    print(header)
    print("-" * len(header))
    def row(name, values):
        print(f"{name:>4} |" + "|".join(
            "  FF" if v == 0xFF else f"{v:4d}" for v in values))
    row("CPUu", cpu_up)
    row("CPUd", cpu_dn)
    row("CPU%", cpu_dt)
    print(f"       raw/2=%: " + " ".join(
        "  -" if v == 0xFF else f"{v//2:3d}" for v in cpu_dt))
    row("GPUu", gpu_up)
    row("GPUd", gpu_dn)
    row("GPU%", gpu_dt)
    print(f"       raw/2=%: " + " ".join(
        "  -" if v == 0xFF else f"{v//2:3d}" for v in gpu_dt))


def diff_filtered(a, b, label_a, label_b):
    """Diff excluding fan-table body (0xF00-0xF5F) and known-volatile regs."""
    print()
    print("=" * 78)
    print(f"FILTERED DIFF  {label_a}  ->  {label_b}")
    print("  (excludes fan-table body 0xF00-0xF5F and volatile sensors)")
    print("=" * 78)
    volatile = {0x43E, 0x44F, 0x463, 0x464, 0x465, 0x46B, 0x46C,
                0x4AB, 0x75B, 0x75C, 0x768,
                # known OEM scratch/cache pages
                }
    # Exclude the whole 0x600-0x6FF block (appears to be GCUService scratch)
    # and the fan-table body 0xF00-0xF5F.
    count = 0
    for i in range(min(len(a), len(b))):
        addr = START_ADDR + i
        if 0xF00 <= addr <= 0xF5F:
            continue
        if 0x600 <= addr <= 0x6FF:
            continue
        if addr in volatile:
            continue
        if a[i] != b[i]:
            ann = note(addr)
            print(f"  [0x{addr:03X}]  0x{a[i]:02X} -> 0x{b[i]:02X}  "
                  f"({a[i]:3d} -> {b[i]:3d})  {ann}")
            count += 1
    print(f"  --- {count} register(s) changed ---")
    return count


def main():
    s1 = load(1)
    s2 = load(2)
    s3 = load(3)
    s4 = load(4)

    fan_table(s4, "snap_4 (probe HOLD state, witness curve, curve should be active)")

    # Key diff: what does OEM set in its working active state that we don't?
    diff_filtered(s4, s2,
                  "snap_4 (probe active, BROKEN)",
                  "snap_2 (OEM active, WORKING)")

    # Supporting diff: what's different between OEM default curve state and
    # OEM user-modified state? (already have this but filtered version helps)
    diff_filtered(s2, s3,
                  "snap_2 (OEM default curve)",
                  "snap_3 (OEM user curve)")

    # Full picture: fresh boot to probe hold state
    diff_filtered(s1, s4,
                  "snap_1 (fresh boot)",
                  "snap_4 (probe hold)")


if __name__ == "__main__":
    main()
