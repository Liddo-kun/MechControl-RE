# NVIDIA board input current limit (mVolt+ "Board input current limit")

2026-10-29. Static analysis of mVolt+ 0.50.8 plus a read-only live probe on Jon's
RTX 5090 Laptop (driver 617.14, VBIOS 98.03.5e.00.1e). Nothing was written.

## What it is

An NVIDIA power policy of type `HW_THRESHOLD` (0x04) whose limit unit is mA. It caps
the current measured by one input sensor channel of the GPU board. It is separate from
the watt limit (TGP policy) and from the NVVDD/MSVDD regulator output limits (OCP).

On this laptop the board-power channel (ch0, `INPUT_TOTAL_BOARD`, summation) is
ch8 + ch10, both weight 1.0 (decoded from PowerMonitorGetInfo V3 relationships):

- ch8: `SENSOR_CLIENT_ALIGNED`, rail 0xDA (not named in public NVAPI headers). The
  input-current policy watches this channel.
- ch10: `PSTATE_ESTIMATION_LUT` for `INPUT_PEX3V3` at a fixed 3.3 V: 2.5 W at P8, 7 W at P0.

So TGP = ch8 power + 2.5..7 W, and the current limit acts as a second power cap:
about input volts x limit + 7 W. Idle samples gave (TGP - 2.5 W) / ch8 current =
19.9 V in all five samples. If that voltage holds under load, 11.053 A caps reported TGP
near 227 W here. Not measured under load. Another user's report of 190 W at 11.053 A
implies about 16.6 V on their machine or voltage sag under load.

TGP and the input current come from the same sensor, so a shunt mod on it lowers both
readings by the same factor. The 11.053 A threshold applies to the reported current.

## Values on Jon's machine (live, 2026-10-29 17:26)

| Item | Value |
|---|---|
| Policy | index 9, type 0x04, unit mA, channel 8 |
| INFO min / default / max | 1 / 11053 / 11053 mA |
| CONTROL request | 11053 mA |
| STATUS limitCurr / valueCurr (idle) | 11053 / ~190 mA |
| VBIOS policy-table entry 9 (original and active image) | type 3, flags 1, channel 8, 1 / 11053 / 11053 mA at +0x0D/+0x11/+0x15 |

The active image differs from the original only at MechControl's unlock patches (power
max, NVVDD/MSVDD max). The same 11.053 A was reported from an AMD machine, so the value
belongs to the GPU board/VBIOS, not the CPU platform.

Stock default = max, so at runtime the limit can only be lowered. Raising it needs a
BIOS override of the entry's maximum, the same `romOverride` mechanism as the power limit.

## Runtime interface (as used by mVolt+)

Binding (`FUN_1402387d0`, `FUN_1402377a0`, `FUN_1402373c0`): power-policy INFO,
PowerMonitorGetInfo V3 (0x30CA8), power-policy CONTROL GET and SET. Two identities:

- Native (this laptop): exactly one type 0x04 policy with unit mA on a channel of type 8,
  plus a TOTAL_GPU (0x00) policy, unit mW, on a type-1 channel with rail 0xF5. Any 0x17,
  0x18 or 0x19 policy makes this path refuse. No auxiliary request.
- Other boards: one type 0x19 policy, unit mA, on a type-8 channel (main request), plus
  a type 0x17 policy, unit mW, on a type-1/rail 0xF5 channel whose CONTROL has a sub-block
  (+0x448 = 0x80, +0x44C = 0x36, +0x454 = 0xFF000100, +0x460 = 1) with the auxiliary
  current request at +0x45C. mVolt disables the auxiliary request on RTX 50 with driver
  621.01 and later (the driver accepts it but does not apply it).

Write (`FUN_140239550` -> `FUN_140239220` -> `FUN_140237c50`): re-read INFO and
range, refuse if outside INFO min..max, GET CONTROL with only that policy selected,
refuse if the request changed since the read, set the limit at entry +0x44 (native
CONTROL layout), SET, then GET until two consecutive reads equal the request
(up to 12 tries). The pre-write value is kept for recovery.

MechControl's existing compact interfaces expose the same policy: PowerPolicyGetInfo V4
(0x67F31384, entry 0x3C + i*0x58) and PowerPolicyGetControl V1 (0x8B3E7343, entry
0x1C + i*0x28, limit at +4). PowerPolicyGetStatus (0x31B7A4CD, V10 0xB00B8, entries
0x11B8 + i*0x3768: type, limitCurr, valueCurr) gives the live current. A compact SET
(0xAFFC2279) with only this policy selected, as for the rails, is expected to work but
was not tested.

## BIOS override

mVolt's BIOS-override row "Board input current limit" (`FUN_140292520`, mode 2) uses
policy-table entry 9 of the 0x40/0x38/0x66 layout: type 3 (or 0x0B), flags 1, channel
< 32, min/default/max at +0x0D/+0x11/+0x15. 0.50.8 patches only the maximum, like the
power limit. mVolt 0.50.9 release notes: on RTX 50 with driver 621.01+, a raised limit
did not take effect (card stayed at stock, issue #89) until 0.50.9. That fix is not in
the analysed binary; by analogy with the rail currents it probably also patches the
default (unverified). Jon's 617.14 is before that driver branch.

## Which hardware has which layout (VBIOS survey, 2026-10-29)

mVolt's BIOS code pairs the two runtime identities with two variants of the 0x40 table
(`FUN_140292520` start): entry 2 class 1, entries 7/8 empty, entry 9 class 3 (native,
single policy) versus entries 7/8/9 classes 0x0D/0x0C/0x0B (three extra policies, matching
the runtime 0x17/0x18/0x19 set). This pairing is inferred from structure; no runtime dump
from a variant-B card has been seen. TechPowerUp images, parsed with
`C:\Temp\tandem\vbios-survey\policy_dump.py`:

| Image | Table | Entry 9 (input current) |
|---|---|---|
| Mechrevo/Tongfang 5090 Laptop 98.03.5E.00.1E (Jon's) | 0x40, native | class 3, 11.053 A |
| Mechrevo 5080 Laptop 98.03.5E.00.1D | 0x40, native | class 3, 11.053 A |
| Razer Blade 16 5090 Laptop 98.03.5C.00.CC | 0x40, native | class 3, 11.053 A |
| ASUS ProArt P16 5090 Laptop 98.03.79.00.06 | 0x40, native | class 3, 8.210 A |
| Lenovo 5070 Ti Laptop 98.05.31.00.4B | 0x40, native | class 3, 9.100 A |
| Tongfang 5060 Laptop 98.06.34.40.7A | 0x40, native | class 3, 7.263 A |
| ASUS ROG Astral 5090 desktop 98.02.2E.C0.2D | 0x40, variant B | class 0x0B, 80 A |
| ASUS TUF 5090 desktop 98.02.2E.40.7D | 0x40, variant B | class 0x0B, 60 A |
| Tongfang/Mechrevo 4070 Laptop 95.06.15.40.63, 4060 Laptop 95.07.16.80.7B | 0x30/0x32/0x53 | class 0x15, 12.434 A at +2/+6/+0xA |

Every RTX 50 laptop image uses the native layout; variant B was seen only on desktop
cards. RTX 40 laptops use the older 0x30 table, which `NvidiaBios` does not parse (so
the existing power unlock is also unavailable there); their runtime policy type has not
been read.

## MechControl implementation (2026-10-29)

A third current slider behind the existing unlock, with the same 140%-of-stock-default
ceiling as NVVDD/MSVDD (all three raised from 130%; here 1-15 A unlocked, 1-11 A locked).
The BIOS default stores the chosen current and the maximum stores the unlocked range;
live changes use the existing compact current-policy SET with only the selected policy
enabled. Recovery adds missing original offsets before writing. Details, verification
and deployed state: `nvidia_power_integration.md`.

Optional per GPU: offered only when slot 9 is class 3 / flags 1 and the driver exposes
exactly one matching policy (same min/default/max). Variant-B boards, a driver that
exposes it differently, or duplicates disable only this slider; the rest of the unlock
works. A profile's board-input value on such a GPU is skipped with a notice.

mVolt 0.50.9 notes / issue #89 (MSI 5090 desktop, driver 621.12, stuck at 80 A) give no
technical detail; "change the limit and save again" means the fix is in the saved
override, consistent with also writing the default, which MechControl does.

Hardware-free tests cover alternate policy/channel indices, exact SET metadata
preservation, failed readback, unavailable/duplicate driver policies, old recovery
files and profile/UI behavior.

## Evidence

- Decompiled: `C:\Temp\tandem\mvolt-bic\`, `C:\Temp\tandem\mvolt-bic2\`,
  `C:\Temp\tandem\mvolt-native-limits\140292520.c`.
- Probe (read-only): `C:\Temp\tandem\bic-probe\` (`run1.txt`, `out\*.bin`, `out\active.rom`).
- Earlier dumps showing the same policy: `C:\Temp\tandem\nvprobe\*.bin` (2026-10-28).
