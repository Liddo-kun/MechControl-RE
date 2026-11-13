# Battery charge voltage, charge limit and charge hold

How the EC decides the charge voltage, why the OEM charge limit does nothing on most models,
and the timed hold MechControl's charge limit uses. Registers are summarized in
[`../register_reference.md`](../register_reference.md) 11.6.

Sources: the Intel X6AR EC image (EC 77.0, SHA-256 `2C90BAE2...DC13`) and the PCSpecialist
X6FR image (AMD, SHA-256 `99A329D6...E735`), read as instruction bytes. Code addresses are
`bank:address` in the Intel image unless marked. "Tested" means the hardware tests of
2026-11-12 on IDY_7Y. Both images contain every routine below, with the same registers.

## 1. The battery modes set a charge voltage, not a percentage

Once a second (B2 `C670`) the EC takes the charging voltage the pack requests (`0x30E:30F`,
big-endian) and gives the charger a lower one (`0x522`, low byte first). Cells come from
`0x491` bits 7:6 (3 = four cells, 2 = three, otherwise two). The amount taken off per cell
is the first row that any one condition reaches:

| Per cell | Age, months (`0x39D`) | Cycles (`0x4A6`) | Hours counter (`0x9C9:9CA`) | `0x7A6` bits 5:4 |
|---|---|---|---|---|
| 250 mV | >= 25 | >= 550 | > 15840 | - |
| 200 mV | >= 19 | >= 450 | > 11520 | 2 (Stationary) |
| 150 mV | >= 13 | >= 350 | > 8640 | - |
| 100 mV | >= 10 | >= 250 | > 6480 | 1 (Balanced) |
| 50 mV | >= 7 | >= 150 | > 4320 | - |

- Mode value 3 is tested nowhere and behaves like 0. There is no fourth mode.
- The EC clears the mode bits every second unless `0x741` bit 0 is set.
- The routine runs only with `0x497` bit 0 (`ISDB`) and `0x490` bit 2 (`BSOK`) set.
- The hours counter counts hours with the pack at 4.1 V per cell or more, adding 1 below
  29.9 C, 3 up to 39.9 C and 7 above. It is not readable through the driver.
- Age is the months between the pack's manufacture date (`0x314:315`, Smart Battery format;
  `0x7F2:7F3` instead when that holds a valid date) and the host-supplied `0x72D` year /
  `0x72E` month (BCD, DSDT `EYER`/`EMON`). The DSDT never writes them; on IDY_7Y they read
  the current month with no OEM service running, and what writes them is not established. `0x726` bit 1 turns age derating off, bit 2 on again,
  bit 0 saves; the EC clears each after acting. MechControl writes `0x726` masked.

A pack that has spent its life plugged in reaches the last row within about two years, after
which the three modes give the same voltage. On the test machine (4.45 V cells, 20 months,
10 cycles) the target is 16800 mV in every mode: 4.20 V per cell, still a nearly full pack.
The modes cannot lower the charge further.

## 2. The OEM charge limit `0x7B9` (`CGLM`)

B2 `CB73`, once a second: bits 6:0 are the limit, and on AC the EC sets bit 7 while the
charge is at or above it. Bit 7 stops charging. The routine runs only when `0x7C3` (`CTID`)
or `0x770` is 4, and sets `0x742` bit 2 when it does. On every other variant it clears
`0x7B9` bit 7 each second, which is why a written limit sticks and changes nothing
(IDY_7Y: `0x7C3` = 7, `0x770` = `FF`).

`0x770` = 4 is not a way in: the same test selects fan PWM and fan tables, charger
currents, thermal thresholds, keyboard handling and start-up parameters at 22 other places.

## 3. The timed charge hold `0x7F4`-`0x7F6`

B2 `B7CE` (X6FR: B2 `C23E`), once a second.

| Byte | Meaning |
|---|---|
| `0x7F6` | Target percent, 1-100. 0 or above 100 means 30. |
| `0x7F4`, `0x7F5` | Minutes left, high byte first. The EC counts them down. 0 = off. |

While minutes are left, with `BSOK` set and `0x73F` bit 7, `0x727` bit 0 and `0x97C` bit 0
clear:

- charge at or below the target, or the machine off: charging is blocked (`0x984` bit 3,
  which the charge controller treats like `0x7B9` bit 7);
- charge above the target and the machine on: GPIO B7 is driven high, the EC reports the
  adapter as absent and the machine runs from the battery until the charge reaches the target.

Tested on IDY_7Y:

- Target 100, 2 minutes: the EC counted 2, 1, 0 at 60 s steps.
- Target 97 at 100 %: within 2 s `0x490` went `07` to `06`, the battery discharged at 2 A
  and Windows saw a power source change. With the display off Windows entered battery
  standby and froze desktop processes until the minutes ran out.
- Target 100, 1 minute, while charging at 544 mA: current 0 within 1 s with AC still
  reported. Writing the minutes to 0 brought charging back within 2 s.

So target 100 makes the timer a plain "do not charge" switch, because no charge is above 100.

**How MechControl uses it** (`Controller::charge_limit`): the only target it writes is 100.
It arms the hold when the charge reaches the limit with the adapter present (`0x490` bit 0),
and writes the minutes to 0 five points below or as soon as the adapter goes. A hold left
armed on battery would stop a machine that is plugged in again while asleep from charging,
because Windows freezes the app in battery standby. The hold is a week long, renewed hourly,
so the limit survives the machine being off; it lapses by itself if the app never comes back.
Write order: target, then minutes.

**Which machines get it.** No profile or EC version is listed. Before the first write the
app checks, by reads only, that the EC's charge-voltage readings follow section 1's
arithmetic (a per-cell request of 3.5-4.6 V, lowered by 0-250 mV per cell in 50 mV steps)
and that the hold registers are idle or carry a hold of its own (target 100, up to a week).
The second never stands in for the first: on IDY_7Y the readings are the same during a hold.
That identifies the firmware family whose memory
layout both images share. The first hold of each run is then a two-minute one, which the EC
must be seen counting down; if it does not, the registers are restored and the limit is
unavailable until the app restarts.

Not tested: the hold across sleep, hibernate, shutdown and EC reset; whether it survives the
EC's shut-off when the machine is off without an adapter (B2 `85E0`); whether the app still
runs to see the adapter go during standby; holds longer than six minutes; any machine but
IDY_7Y.

## 4. Never write

- `0x727` bit 0: drives GPIO B7 high while set, a forced run from the battery (B2 `B7AC`).
- `0x73F` bit 7 with a percentage in bits 6:0: waits for 100 %, runs from the battery down
  to that percentage, recharges to 100 % and repeats (B2 `B753`).
- `0x7F6` with anything but 100 while minutes are left: discharges to that target.
