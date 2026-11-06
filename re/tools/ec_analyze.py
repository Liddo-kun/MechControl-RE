#!/usr/bin/env python3
"""
EC Binary Snapshot Analyzer
Compares two EC snapshots (0x400-0xFFF) and identifies unknown registers
with interesting values or dynamic behavior.
"""

import re
import struct
import sys

# --- Configuration ---
BASE_ADDR = 0x400
SNAP1_PATH = r"C:\Users\Jon\clean\ec_snap_1.bin"
SNAP2_PATH = r"C:\Users\Jon\clean\ec_snap_2.bin"
ECSPEC_PATH = r"C:\Users\Jon\clean\ecspec_dump.txt"

# --- Zone definitions ---
ZONES = [
    (0x400, 0x4FF, "ACPI / Battery zone"),
    (0x500, 0x6FF, "Unknown zone (0x500-0x6FF)"),
    (0x700, 0x7FF, "Control zone"),
    (0x800, 0xDFF, "Unknown zone (0x800-0xDFF)"),
    (0xE00, 0xEFF, "Unknown zone (0xE00-0xEFF)"),
    (0xF00, 0xFFF, "Fan table zone"),
]


def load_binary(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != 3072:
        print(f"WARNING: {path} is {len(data)} bytes, expected 3072")
    return data


def parse_ecspec(path):
    """Extract all hex addresses from ecspec_dump.txt that are EC register addresses (0x400+)."""
    known = {}
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            # Match lines like: UInt16 ADDR_SOMETHING = 0xNNN
            m = re.match(r'^\w+\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+)', line)
            if m:
                name = m.group(1)
                try:
                    val = int(m.group(2), 16)
                except ValueError:
                    continue
                # Only include addresses in the EC range 0x400-0xFFF
                if 0x400 <= val <= 0xFFF:
                    if val not in known:
                        known[val] = []
                    known[val].append(name)
    return known


def build_known_set(ecspec_known):
    """Combine ecspec addresses with hardcoded known ranges from app profiles."""
    known = dict(ecspec_known)  # addr -> [names]

    def add(addr, name):
        if addr not in known:
            known[addr] = []
        if name not in known[addr]:
            known[addr].append(name)

    # Battery zone extras
    battery_addrs = {
        0x402: "EC_BIF_DC_BYTE1", 0x403: "EC_BIF_DC_BYTE2",
        0x408: "EC_BIF_DV_BYTE1", 0x409: "EC_BIF_DV_BYTE2",
        0x434: "EC_BST_BPR_BYTE1", 0x435: "EC_BST_BPR_BYTE2",
        0x436: "EC_BST_BRC_BYTE1", 0x437: "EC_BST_BRC_BYTE2",
        0x438: "EC_BST_BPV_BYTE1", 0x439: "EC_BST_BPV_BYTE2",
        0x43E: "cpuTemp", 0x44F: "gpuTemp",
        0x456: "SystemID", 0x45B: "SILENTMODE_STATUS",
        0x463: "cpuTempLimitRead",
        0x464: "cpuFanRpmLo", 0x465: "cpuFanRpmHi",
        0x466: "EC_BIOS_INFO5",
        0x46B: "gpuFanRpmLo", 0x46C: "gpuFanRpmHi",
        0x476: "BIOSFuncReg",
        0x47D: "ecOEM_SUB_VER2",
        0x490: "ecPowSource", 0x494: "BATTERY_ALERT",
        0x49F: "BIOS_INFO_3",
        0x4A2: "ecBt1Temperature",
        0x4A6: "ecBt1CycleCount_1", 0x4A7: "ecBt1CycleCount_2",
        0x4AB: "ecBt1RSOC",
        0x454: "ecOEM_EC_VER",
    }
    for a, n in battery_addrs.items():
        add(a, n)

    # Control zone (0x706-0x7FB) - add specific known ones from profiles
    control_addrs = {
        0x706: "AP_BIOS_CONTROL", 0x726: "OEM_BYTE9", 0x727: "OEM_BYTE10/CPU_DOUBLE_FLAG/ESHUTTER",
        0x728: "OEM_BYTEB",
        0x730: "GAMING_PL1_DEFAULT", 0x731: "GAMING_PL2_DEFAULT",
        0x732: "GAMING_PL4_DEFAULT", 0x733: "GAMING_D_DEFAULT",
        0x734: "OFFICE_PL1_DEFAULT", 0x735: "OFFICE_PL2_DEFAULT",
        0x736: "OFFICE_PL4_DEFAULT", 0x737: "OFFICE_D_DEFAULT",
        0x738: "MYFAN3_CPU_TAU",
        0x740: "PROJECT_ID", 0x741: "AP_OEM_BYTE/FAN_ALERT",
        0x742: "SUPPORT_BYTE5", 0x743: "ConfigurableTGP_DynamicBoost_CTRL/MYFAN2_L1",
        0x744: "ConfigurableTGP_VALUE/MYFAN2_L2", 0x745: "DynamicBoost_TotalPower/MYFAN2_L3",
        0x746: "DynamicBoost_MaxTGP/MYFAN2_L4", 0x747: "MYFAN2_L5",
        0x748: "LIGHTBAR_CONTROL", 0x749: "REDBAR_CONTROL",
        0x74A: "GREENBAR_CONTROL", 0x74B: "BLUEBAR_CONTROL",
        0x74C: "OEMSERVICE_PROJECT_ID", 0x74E: "BIOS_OEM_BYTE",
        0x751: "MAFAN_CONTROL", 0x753: "CPU_VRM_CURRENT_LIMIT",
        0x754: "CPU_VRM_MAX_CURRENT_LIMIT",
        0x75B: "CPU_FAN_L_DUTY", 0x75C: "CPU_FAN_R_DUTY",
        0x75D: "TRIGGER_BYTE2",
        0x765: "SUPPORT_BYTE1", 0x766: "SUPPORT_BYTE2/Light_ChinaMode",
        0x767: "TRIGGER_BYTE", 0x768: "STATUS_BYTE",
        0x769: "RGBKB_R/LOGO_R", 0x76A: "RGBKB_G/LOGO_G", 0x76B: "RGBKB_B/LOGO_B",
        0x76C: "RGBKB_DEFAULT_R", 0x76D: "RGBKB_DEFAULT_G", 0x76E: "RGBKB_DEFAULT_B",
        0x76F: "RGBKB_MUSIC_NO", 0x771: "ROMID", 0x772: "ROMID2",
        0x782: "BIOS_OEM_BYTE2/Light_SetToChinaMode",
        0x783: "PL1_SETTING", 0x784: "PL2_SETTING", 0x785: "PL4_SETTING",
        0x786: "L1_PWM_DEFAULT_MYFAN3/TccOffset_Setting",
        0x787: "L2_PWM_DEFAULT_MYFAN3/FanSwitchSpeed",
        0x788: "L3_PWM_DEFAULT_MYFAN3",
        0x789: "L1_PWM_DEFAULT_MYFAN2/L4_PWM_DEFAULT_MYFAN3",
        0x78A: "L2_PWM_DEFAULT_MYFAN2/L5_PWM_DEFAULT_MYFAN3",
        0x78B: "L3_PWM_DEFAULT_MYFAN2/MYFAN3_GPU_SETTING",
        0x78C: "L4_PWM_DEFAULT_MYFAN2/AP_OEM_BYTE2/SINGLEKBL_ENABLE/LOGO_CONFIRM",
        0x78D: "L5_PWM_DEFAULT_MYFAN2",
        0x78E: "SINGLEKBL_SUPPORTPOWER/SUPPORT_BYTE6",
        0x79E: "MYFANI_MIN_SPEED", 0x79F: "MYFANI_MIN_TEMP",
        0x7A0: "MYFANI_EXTRA_SPEED",
        0x7A3: "BIOS_OEM_BYTE3", 0x7A4: "AP_BIOS_BYTE",
        0x7A5: "AP_OEM_BYTE3", 0x7A6: "AP_OEM_BYTE4/batteryMode",
        0x7A7: "BATTERYSAVER_PL1_DEFAULT", 0x7A8: "BATTERYSAVER_PL2_DEFAULT",
        0x7A9: "BATTERYSAVER_PL4_DEFAULT", 0x7AA: "BATTERYSAVER_D_DEFAULT",
        0x7AB: "AP_EC_LOGO/MyFanCCI_Mode_Index",
        0x7B0: "MyFanCCI_Mode_Profile1", 0x7B1: "MyFanCCI_Mode_Profile2",
        0x7B2: "MyFanCCI_Mode_Profile3",
        0x7B9: "BATTERY_CHARGE_LIMIT_UP",
        0x7C5: "AP_OEM_BYTE5/fanCurveEnable", 0x7C6: "AP_OEM_BYTE6",
        0x7C7: "AP_OEM_BYTE7", 0x7C8: "AP_OEM_BYTE8",
        0x7CA: "BIOS_OEM_BYTE8",
        0x7CC: "COMPLEX_POWER_STATUS",
        0x7D0: "BATTERY_CHARGE_LIMIT_DOWN", 0x7D1: "AP_SSDTempr",
        0x7D2: "ModuleID_GPU", 0x7D3: "ModuleID",
        0x7D8: "GAMING_TCC_OFFSET_DEFAULT", 0x7D9: "OFFICE_TCC_OFFSET_DEFAULT",
        0x7DA: "TURBO_TCC_OFFSET_DEFAULT",
        0x7E6: "LC_FAN_VALUE", 0x7E7: "LC_PUMP_VALUE",
        0x7FB: "CHANGE_PORT_ID_WKD",
    }
    for a, n in control_addrs.items():
        add(a, n)

    # System
    add(0xD00, "AP_OEM_SingleZone")
    add(0xD4E, "EC_Color_Calibration_Support")
    add(0xD4F, "AP_MISCSWITCH2")

    # Fan table zone 0xF00-0xFFF (all considered known)
    for a in range(0xF00, 0x1000):
        add(a, "FanTable")

    return known


def get_val(data, addr):
    offset = addr - BASE_ADDR
    if 0 <= offset < len(data):
        return data[offset]
    return None


def find_clusters(addrs):
    """Find clusters of consecutive addresses. Returns list of (start, end, count)."""
    if not addrs:
        return []
    sorted_addrs = sorted(addrs)
    clusters = []
    start = sorted_addrs[0]
    prev = sorted_addrs[0]
    for a in sorted_addrs[1:]:
        if a == prev + 1:
            prev = a
        else:
            clusters.append((start, prev, prev - start + 1))
            start = a
            prev = a
    clusters.append((start, prev, prev - start + 1))
    return clusters


def main():
    snap1 = load_binary(SNAP1_PATH)
    snap2 = load_binary(SNAP2_PATH)
    ecspec = parse_ecspec(ECSPEC_PATH)
    known = build_known_set(ecspec)

    # Collect stats
    all_unknown_nonzero = []
    all_unknown_changed = []
    all_unknown_interesting = []  # non-zero, non-0xFF

    print("=" * 100)
    print("EC BINARY SNAPSHOT ANALYSIS: Undocumented Register Hunt")
    print("=" * 100)
    print(f"Snap 1: {SNAP1_PATH}")
    print(f"Snap 2: {SNAP2_PATH}")
    print(f"Known registers from ecspec + profiles: {len(known)}")
    print()

    for zone_start, zone_end, zone_name in ZONES:
        zone_unknown_interesting = []
        zone_unknown_changed = []
        zone_rows = []

        for addr in range(zone_start, zone_end + 1):
            v1 = get_val(snap1, addr)
            v2 = get_val(snap2, addr)
            if v1 is None or v2 is None:
                continue

            is_known = addr in known
            changed = v1 != v2
            nonzero = (v1 != 0) or (v2 != 0)
            interesting = nonzero and (v1 != 0xFF or v2 != 0xFF)

            if not is_known:
                if nonzero:
                    all_unknown_nonzero.append(addr)
                if changed:
                    all_unknown_changed.append(addr)
                if interesting:
                    all_unknown_interesting.append(addr)

                if interesting or changed:
                    zone_unknown_interesting.append(addr)
                if changed:
                    zone_unknown_changed.append(addr)

                flags = []
                if changed:
                    flags.append("CHANGED")
                if nonzero and v1 != 0xFF and v2 != 0xFF:
                    flags.append("non-zero")
                elif v1 == 0xFF or v2 == 0xFF:
                    flags.append("0xFF")

                if interesting or changed:
                    zone_rows.append((addr, v1, v2, flags))

            else:
                # Also show known registers that changed (for context)
                if changed:
                    names = ", ".join(known[addr])
                    zone_rows.append((addr, v1, v2, [f"KNOWN-CHANGED ({names})"]))

        # Print zone
        if zone_rows:
            print("-" * 100)
            print(f"  ZONE: {zone_name} (0x{zone_start:03X}-0x{zone_end:03X})")
            print(f"  Unknown interesting: {len(zone_unknown_interesting)}, "
                  f"Unknown changed: {len(zone_unknown_changed)}")
            print("-" * 100)
            print(f"  {'Address':>10}  {'Snap1':>8}  {'Snap2':>8}  {'Delta':>8}  Flags")
            print(f"  {'-------':>10}  {'-----':>8}  {'-----':>8}  {'-----':>8}  -----")
            for addr, v1, v2, flags in zone_rows:
                delta = v2 - v1
                delta_str = f"{delta:+d}" if delta != 0 else ""
                status = "KNOWN" if addr in known else "UNK"
                flag_str = ", ".join(flags)
                print(f"  0x{addr:03X} [{status:>5}]  0x{v1:02X} ({v1:3d})  0x{v2:02X} ({v2:3d})  {delta_str:>8}  {flag_str}")
            print()

    # --- SUMMARY ---
    print()
    print("=" * 100)
    print("SUMMARY")
    print("=" * 100)

    total_regs = 0xFFF - 0x400 + 1
    total_known = sum(1 for a in range(0x400, 0x1000) if a in known)
    total_unknown = total_regs - total_known

    print(f"\nTotal EC address range:       {total_regs} registers (0x400-0xFFF)")
    print(f"Known registers:              {total_known}")
    print(f"Unknown registers:            {total_unknown}")
    print(f"Unknown with non-zero value:  {len(all_unknown_nonzero)}")
    print(f"Unknown & interesting:        {len(all_unknown_interesting)} (non-zero AND non-0xFF)")
    print(f"Unknown & CHANGED:            {len(all_unknown_changed)} (dynamic/live!)")

    # Changed registers detail
    if all_unknown_changed:
        print(f"\n--- DYNAMIC UNKNOWN REGISTERS (changed between snapshots) ---")
        print(f"These are LIVE sensors, counters, or status bits!\n")
        for addr in sorted(all_unknown_changed):
            v1 = get_val(snap1, addr)
            v2 = get_val(snap2, addr)
            delta = v2 - v1
            # Identify zone
            zone_label = "?"
            for zs, ze, zn in ZONES:
                if zs <= addr <= ze:
                    zone_label = zn
                    break
            print(f"  0x{addr:03X}:  snap1=0x{v1:02X} ({v1:3d})  snap2=0x{v2:02X} ({v2:3d})  "
                  f"delta={delta:+d}  [{zone_label}]")

    # Clusters
    if all_unknown_interesting:
        print(f"\n--- CLUSTERS of consecutive unknown registers with interesting values ---")
        print(f"(Clusters of 2+ suggest structured data or related fields)\n")
        clusters = find_clusters(all_unknown_interesting)
        # Only show clusters of size >= 2
        big_clusters = [c for c in clusters if c[2] >= 2]
        if big_clusters:
            for start, end, count in sorted(big_clusters, key=lambda x: -x[2]):
                # Show all values in the cluster
                vals1 = [get_val(snap1, a) for a in range(start, end + 1)]
                vals2 = [get_val(snap2, a) for a in range(start, end + 1)]
                any_changed = any(get_val(snap1, a) != get_val(snap2, a) for a in range(start, end + 1))
                changed_tag = " ** HAS DYNAMIC REGS **" if any_changed else ""
                print(f"  0x{start:03X}-0x{end:03X} ({count} regs){changed_tag}")
                # Print values compactly
                for a in range(start, end + 1):
                    v1 = get_val(snap1, a)
                    v2 = get_val(snap2, a)
                    ch = " CHANGED" if v1 != v2 else ""
                    print(f"    0x{a:03X}: 0x{v1:02X} ({v1:3d}) -> 0x{v2:02X} ({v2:3d}){ch}")
                print()
        else:
            print("  No clusters of 2+ found (all isolated registers)")

    # Full hex dump of unknown non-zero for pattern spotting
    print(f"\n--- ALL UNKNOWN INTERESTING REGISTERS (hex dump) ---\n")
    print(f"  {'Addr':>6}  {'S1':>4} {'S2':>4}  {'S1dec':>5} {'S2dec':>5}  {'Chg':>3}")
    print(f"  {'----':>6}  {'--':>4} {'--':>4}  {'-----':>5} {'-----':>5}  {'---':>3}")
    for addr in sorted(all_unknown_interesting):
        v1 = get_val(snap1, addr)
        v2 = get_val(snap2, addr)
        ch = " *" if v1 != v2 else ""
        print(f"  0x{addr:03X}   0x{v1:02X} 0x{v2:02X}  {v1:5d} {v2:5d} {ch}")


if __name__ == "__main__":
    main()
