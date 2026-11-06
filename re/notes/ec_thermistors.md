# EC thermistors: looking for a VRM temperature

2026-11-05, IDY_7Y (Jon's machine), read-only. Logger and data: `C:\Temp\tandem\ec-temp-hunt\`
(`hunt.exe log <dir>` / `run`, `analyze.py`, `ts2.py`; runs `cpu1\`, `game1\`).
Firmware: X6FR5 EC image (`ec_firmware_X6FR5_findings.md`), listings in
`C:\Temp\tandem\lp3123079-review\`.

## Firmware (X6FR5 sibling, confirmed in disassembly)

- `b2_8121` stores the 8 ADC channels as 16-bit little-endian raw values at `0x600`-`0x60F`
  (ch N at `0x600 + 2N`).
- `b2_8434` converts a raw value to whole degrees C through a 101-entry NTC table at CODE
  `0x69C7` (big-endian words, 0 C = 1000 down to 100 C = 113).
- `b2_d87e`: `0x79E` = NTC(ch4), unless `0x9C0` bit 0 (then `0x9C3`, an override); `0x7A1` =
  NTC(ch1). `0x79E` feeds charging logic; nothing reads `0x7A1`.
- OEM names for `0x79E`/`0x7A1` (MyFanI min speed, ...) do not match this use.
- `0x449`/`0x44C` are not temperatures: `0x44C`, `0x701`, `0x703` are power values reset to
  20 together; `0x449` is part of the same power calculation.

## IDY_7Y measurements

ADC channels converted with the same table:

| Ch | Raw idle | Reads | Behavior |
|---|---|---|---|
| 0 | ~597 | 38 -> 52 C | **= `0x7A1` on IDY_7Y** (X6FR puts ch1 there). Largest rise. |
| 1 | ~654 | 34 -> 38 C | Not exposed as a byte. |
| 2 | 562 | constant | Not a thermistor (or fixed). |
| 3 | 962 | constant | Not a thermistor. |
| 4 | ~666 | 33 -> 38 C | = `0x79E` (matches firmware). |
| 5 | ~304 | slow drift | Different curve or not a temperature. |
| 6 | 30-160 | noisy | Tracks load; current/power sense. |
| 7 | 705 | constant | Not a thermistor. |

Game run (shared heatsink for CPU, GPU and VRMs; water cooler mainly on the GPU die area):
170 W with cooler on -> 123 W with cooler off -> cooler back on.

- ch0: +1.6 C/min at 170 W, kept rising with the cooler off, stopped rising within about a
  minute of the cooler coming back (GPU 80 -> 55 C), never fell. Coupled to the heatsink with a
  lag of minutes.
- ch1: smaller rise, flattened as soon as the cooler came back. Also coupled to the heatsink.
- ch4 and ch5: rose steadily throughout, unaffected by the cooler. Not on the heatsink
  (charger/battery/chassis).
- The first CPU-only run only reached 60 C CPU, too weak to move anything.

CPU-only run 2 (`cpu2\`, about 130 W package, CPU 82 C, water cooler on, 8 min, started warm
from the game run): **no sensor rose.** ch0 kept cooling (47.4 -> 43.8 C) at about the rate it
did before the load, and its cooling sped up when the GPU dropped from ~27 W to ~5 W
afterwards. ch1 and ch4 kept cooling too.

**Conclusion:** no thermistor sits near the CPU VRM. ch0 (`0x7A1`) responds to GPU power
(+1.6 C/min at 170 W, faster cooling when the GPU idles) and to the heatsink, with a lag of
minutes: a GPU-side board sensor, the closest thing to a GPU VRM temperature this EC has, but
not a power-stage reading.

Channel-to-location wiring differs per model (`0x7A1` = ch0 here, ch1 on X6FR), so any app
feature must take the channel or register from the profile.
