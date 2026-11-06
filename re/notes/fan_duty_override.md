# Fixed-duty fan override (0x759 / 0x75A)

2026-10-29. Found statically in the X6FR5 EC firmware (`ec_firmware_X6FR5_findings.md` §9),
then tested on Jon's IDY_7Y (`YAOSHI Series-X6AR55xY`, Ultra 9 275HX, watercooled) with
MechControl stopped. Tool: `C:\Temp\tandem\fanov\fanov.cs` (temporary).

## Encoding

| Register | Fan | Driven duty register |
|---|---|---|
| `0x759` | 1 (CPU) | `0x75B` |
| `0x75A` | 2 (GPU) | `0x75C` |

- Bit 7 = override on. Bits 0-6 = duty percent; values ≥ 100 mean 100 %.
- The EC sets the duty register to `percent × 2` (raw 0-200) within about 1 s.
- `0x00` (bit 7 clear) hands the fan back to the EC curve.
- Both read 0x00 in all eight archived IDY_7Y snapshots; nothing else writes them.

Firmware logic (`bank2:93aa`): if `0x6C2 == 0` (a countdown, likely after power-on/resume) and either
override bit is set, the EC writes the override duties and skips the whole curve block,
including fan boost, the boost auto-clear, the SSD minimum-duty rule and the custom-table path.
The only comparison is `value < 100` (`b2_e5b5`).

## Measured on IDY_7Y

| 0x759 | 0x75B | CPU fan RPM |
|---|---|---|
| `0x80` (0 %) | 0 | 0 (stops) |
| `0x8A` (10 %) | 20 | ~540 |
| `0x94` (20 %) | 40 | ~1,180 |
| `0xB2` (50 %) | 100 | ~2,740 |
| `0xE4` (100 %) | 200 | ~4,640 |

GPU fan via 0x75A: 50 % → ~2,700 RPM, 100 % → ~4,600 RPM, 0 % → stops.

- **No EC safety net.** Both fans held at 0 % under a 90 s all-core CPU load: duty stayed 0,
  CPU reached 77 °C (watercooled), the EC never intervened. The firmware has no temperature
  check on this path, so this would hold at any temperature short of the CPU's own throttling.
- **Fan boost is ignored** while an override is set (0x751 = 0x40: duty unchanged, 0x768
  bit 2 not set). The failsafe's "fan boost" action does nothing during an override.
- **Clearing** returns to the curve within ~5 s; the EC briefly kick-starts both fans (duty
  50 → 40 → 30 …) before settling.
- Worked with 0x741 = 0x81 (activated). Not tested: without activation, across sleep/resume,
  across reboot/shutdown.

Side result: GPU RPM is `0x46C << 8 | 0x46D`. 0x46B reads a constant 0xC3 (every reading of
0x46C:0x46B was `0x??C3`), which is why the GPU fan used to show 195 RPM while stopped.

## Implications for linked fans

`LinkedFanController` currently rewrites both 10-point fan tables (flat at the linked duty,
then a ramp to 100 %) whenever the duty or ramp start changes. With the override it would
write two bytes per change, get the exact duty, and stop depending on the custom-table
preconditions. The cost: the rewritten tables exist so that the EC still ramps the fans if
the app dies. An override has no such fallback; a crash, kill or hang leaves the fans at the
last duty, possibly 0 %, indefinitely. Any use needs app-side clearing on exit, suspend,
failsafe trip and unhandled exceptions, and a reboot/sleep persistence test first.
