# EC Dark Zones: Undocumented Registers to Probe

Discovered by diffing `ec_snap_1.bin` vs `ec_snap_2.bin` (3072 bytes each, EC addresses 0x400-0xFFF) against the 380 known register addresses from OEM profiles.

**Stats:** 274 undocumented registers with interesting (non-zero, non-0xFF) values. 45 actively changed between snapshots (live sensors/counters).

---

## Hidden Temperature Sensors

| Address | Snap 1 | Snap 2 | Near | Likely Function |
|---|---|---|---|---|
| **0x449** | 51 | 37 | `cpuTemp` (0x43E) | Secondary temp sensor (VRM? chipset?) |
| **0x44C** | 78 | 71 | `gpuTemp` (0x44F) | GPU hotspot or VRM temp |
| **0x701** | 89 | 71 | Control zone | Temp mirror (moves in lockstep with 0x703) |
| **0x703** | 89 | 71 | Control zone | Another temp shadow register |

## Hidden Power/Energy Sensors (0x600-0x60C)

Structured array with value+scale byte pairs that changed between snapshots:

| Address | Snap 1 | Snap 2 | Notes |
|---|---|---|---|
| 0x600/0x601 | 44/2 | 41/2 | Value changes, scale static |
| 0x602/0x603 | 118/2 | 116/2 | Value changes, scale static |
| 0x606/0x607 | 187/3 | 191/3 | Value changes, scale static |

Likely **power consumption readings** (CPU, GPU, system rails). The scale byte is static while the value byte changes with load.

## Hidden Fan Registers

| Address | Snap 1 | Snap 2 | Likely Function |
|---|---|---|---|
| **0x468** | 1 | 0 | Fan status flag (spinning/stopped) |
| **0x46A** | 75 | 210 | Third fan RPM or aux speed byte |
| **0x46D** | 192 | 0 | Fan-related status register |
| **0x670** | 50 | 64 | Fan duty mirror/target (delta matches 0x75B) |

## Thermal History Buffer (0x6B0-0x6BF)

A 16-byte circular buffer. 10 entries all at 178 (0xB2) in snap 1, with tail entries dropping in snap 2. This is the **EC's internal thermal averaging window**.

| Address | Snap 1 | Snap 2 | Notes |
|---|---|---|---|
| 0x6B0-0x6B9 | all 178 | mostly 178 | Thermal samples |
| 0x6BA-0x6BD | 178 | ~131-132 | Tail updating first |
| 0x6BE | 10 | 2 | Index/count field |
| 0x6BF | 50 | 17 | Index/count field |

## Power Management State Machine (0x680-0x68D)

Multiple registers with small value changes — likely thermal/power throttling states:

| Address | Snap 1 | Snap 2 |
|---|---|---|
| 0x680 | 2 | 1 |
| 0x689 | 0 | 6 |
| 0x68B | 4 | 2 |
| 0x68D | 4 | 2 |

## Live GPU Power Register

**0x7D5**: 5 -> 25. Sits right next to ModuleID (0x7D3). Value 25 matches Dynamic Boost max TGP. Possibly a **live TGP reporting register** showing current GPU power target.

## Other Dynamic Registers

| Address | Snap 1 | Snap 2 | Notes |
|---|---|---|---|
| 0x460 | 0 | 1 | Near cpuTempLimitRead (0x463), thermal throttle status? |
| 0x461 | 50 | 64 | Delta +14 matches fan duty change |
| 0x46F | 145 | 173 | Accumulated thermal counter or power reading |
| 0x522 | 248 | 104 | Huge swing, near battery capacity block |
| 0x523 | 67 | 66 | Paired with 0x522 |
| 0x566 | 217 | 14 | Wildly dynamic, counter or fast sensor |
| 0x6CF | 43 | 76 | Timer or counter |
| 0x6F8-0x6FA | 24,24,35 | 220,2,30 | Very volatile, runtime timestamps? |
| 0x72B | 3 | 1 | Mode state or step counter |
| 0x763 | 45 | 0 | Pending command register, clears after exec |
| 0x764 | 45 | 0 | Same behavior as 0x763 |
| 0x7A1 | 42 | 43 | Slow increment, uptime tick? |
| 0x7D4 | 0 | 255 | Completion/status flag |
| 0x7F7 | 55 | 0 | Transient command or status |

---

## Interesting Static Clusters

### Secondary Fan Table / Thermal Config (0x532-0x544)

19 registers with repeating (30, 15) pairs = 30C up / 15C down hysteresis. Also contains capacity values 0x401F (8223) and 0x755A (23130). Likely a **secondary fan table or thermal configuration block**.

### Fan Step Levels (0x5F0-0x5F5)

Values: 1, 1, 1, 3, 3, 5. Could be **fan step levels or mode indices**.

### Battery/Power Configuration (0x518-0x523)

Contains pairs of (0xD4, 0x17) which equals 6100 as uint16 LE — exactly a common battery design capacity (6100 mAh). Runtime power/energy counters or mWh accumulators.

### LED / Keyboard Timing (0xD41-0xD47)

Values: 6, 11, 36, 1, 82, 1, 2. Near known `AP_MISCSWITCH2` (0xD4F). Likely **LED/keyboard backlight configuration or timing parameters**.

### EC Firmware Version String (0xD50-0xD60)

ASCII data: `1D05300FY22001000x1B`. Likely an **EC firmware version or build date**.

### Hardware ID / Calibration (0xEC0-0xECF)

16 registers of high-entropy static data. Could be a **hardware ID, serial hash, or calibration data**.

---

## Probe Priority

### Tier 1 — High value, easy to identify
1. **0x600-0x60C** — Power monitoring. Read under varying load to confirm they track wattage.
2. **0x449, 0x44C** — Extra temp sensors. Read alongside CPU/GPU temps to identify them.
3. **0x7D5** — Live TGP reporting. Monitor during GPU load changes.

### Tier 2 — Useful context
4. **0x6B0-0x6BF** — EC thermal averaging. Useful for understanding fan ramp behavior.
5. **0x701, 0x703** — Temp mirrors in control zone. Correlate with 0x43E.
6. **0x680-0x68D** — Power/thermal state machine. Monitor during throttling events.

### Tier 3 — Exploratory
7. **0x468, 0x46A, 0x46D** — Fan status/speed registers. Monitor during fan speed changes.
8. **0x763-0x764** — Transient command registers. Monitor during OEM app mode switches.
9. **0xD50-0xD60** — Firmware version string. Read once for identification.

---

## Tools

- `ec_snapshot.exe snapshot` — capture full EC state (0x400-0xFFF) to `ec_snap_N.bin`
- `ec_snapshot.exe diff` — compare two most recent snapshots
- `ec_analyze.py` — full analysis script comparing snapshots against known registers
