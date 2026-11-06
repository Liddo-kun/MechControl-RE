#!/usr/bin/env python3
"""
Add defaultFanCurve data to MechControl app profile JSONs,
sourced from OEM M1T1.json fan table files.

Conversion rules:
  tempUp[i]  = OEM CPU[i].UpT  (as-is, 255 for padding)
  tempDown[0] = 0  (always; EC ignores index 0)
  tempDown[i>0]: if UpT==255 -> 255 (padding), else max(0, UpT[i] - hysteresis)
  duty[i]:      if UpT==255 -> paddingDuty,     else OEM Duty * 2 (percent -> raw EC)
  Same for GPU.
"""

import json
import os
import sys
from collections import OrderedDict

OEM_BASE = r"C:\Users\Jon\clean\UserFanTables-OEM"
PROFILE_BASE = r"C:\Users\Jon\clean\MechControl\Profiles\MECHREVO"

MODELS = [
    "PH4AQE3", "PH4AQxx", "PH4ARxx", "PH4AUxf", "PH4AUxx", "PH4AXxx",
    "PH4PGx1", "PH4PGx2", "PH4PRxx", "PH4PUxx", "PH4TQx1", "PH4TRX1",
    "PH4TUX1", "PH6AQxx", "PH6ARxx", "PH6PG0x", "PH6PG0x150W", "PH6PG3x",
    "PH6PG3x150W", "PH6PG7x", "PH6PG7x150W", "PH6PGEx", "PH6PRxx", "PH6TRX1",
]


def convert_fan_array(oem_entries, padding_duty, hysteresis):
    """Convert one OEM fan array (CPU or GPU) to defaultFanCurve format."""
    temp_up = []
    temp_down = []
    duty = []

    for i, entry in enumerate(oem_entries):
        up_t = entry["UpT"]
        temp_up.append(up_t)

        # tempDown
        if i == 0:
            temp_down.append(0)
        elif up_t == 255:
            temp_down.append(255)
        else:
            temp_down.append(max(0, up_t - hysteresis))

        # duty
        if up_t == 255:
            duty.append(padding_duty)
        else:
            duty.append(entry["Duty"] * 2)

    return {
        "tempUp": temp_up,
        "tempDown": temp_down,
        "duty": duty,
    }


def insert_key_before(d, new_key, new_val, before_key):
    """Return a new OrderedDict with new_key inserted before before_key."""
    result = OrderedDict()
    inserted = False
    for k, v in d.items():
        if k == before_key and not inserted:
            result[new_key] = new_val
            inserted = True
        result[k] = v
    if not inserted:
        # before_key not found -- just append
        result[new_key] = new_val
    return result


def process_model(model):
    """Process one model. Returns (status_str, was_updated)."""
    oem_path = os.path.join(OEM_BASE, model, "M1T1.json")
    profile_path = os.path.join(PROFILE_BASE, f"{model}.json")

    if not os.path.isfile(profile_path):
        return f"  SKIP  {model} -- no app profile found", False

    if not os.path.isfile(oem_path):
        return f"  SKIP  {model} -- no OEM M1T1.json found", False

    # Read OEM fan table
    with open(oem_path, "r", encoding="utf-8") as f:
        oem = json.load(f)

    # Read existing profile (preserve key order)
    with open(profile_path, "r", encoding="utf-8") as f:
        profile = json.load(f, object_pairs_hook=OrderedDict)

    # Already has defaultFanCurve?
    if "defaultFanCurve" in profile:
        return f"  SKIP  {model} -- defaultFanCurve already exists", False

    # Read paddingDuty and hysteresis from profile
    fan_table = profile.get("fanTable", {})
    padding_duty = fan_table.get("paddingDuty", 180)
    hysteresis = fan_table.get("hysteresis", 6)

    cpu_entries = oem["CPU"]
    gpu_entries = oem["GPU"]

    default_fan_curve = OrderedDict()
    default_fan_curve["cpu"] = convert_fan_array(cpu_entries, padding_duty, hysteresis)
    default_fan_curve["gpu"] = convert_fan_array(gpu_entries, padding_duty, hysteresis)

    # Insert before skipVerify if present, otherwise after batteryModes
    if "skipVerify" in profile:
        profile = insert_key_before(profile, "defaultFanCurve", default_fan_curve, "skipVerify")
    elif "batteryModes" in profile:
        # Insert after batteryModes by rebuilding
        new_profile = OrderedDict()
        for k, v in profile.items():
            new_profile[k] = v
            if k == "batteryModes":
                new_profile["defaultFanCurve"] = default_fan_curve
        profile = new_profile
    else:
        profile["defaultFanCurve"] = default_fan_curve

    # Write back
    with open(profile_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(profile, f, indent=2)
        f.write("\n")

    return f"  OK    {model} (paddingDuty={padding_duty}, hysteresis={hysteresis})", True


def main():
    updated = 0
    skipped = 0

    print("Adding defaultFanCurve to app profiles...\n")

    for model in MODELS:
        msg, was_updated = process_model(model)
        print(msg)
        if was_updated:
            updated += 1
        else:
            skipped += 1

    print(f"\nDone. Updated: {updated}, Skipped: {skipped}")


if __name__ == "__main__":
    main()
