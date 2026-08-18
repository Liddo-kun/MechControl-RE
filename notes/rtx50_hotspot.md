# RTX 50 Hotspot and Possible VRM Sensor Investigation

## Purpose

This is the single canonical handoff for continuing RTX 50-series thermal sensor experiments. It consolidates the former `C:\Temp\tandem\handoff.md` and the earlier version of this note.

The immediate goal was to determine what the removed HWMonitor 1.65 `[HotSpot #2]` reading represents, whether it is independent of adjacent channels, and whether any evidence supports a GPU VRM temperature interpretation. No production MechControl monitoring or UI code has been changed.

**STATUS 2026-07-18:** Hotspot identity is concluded: `0xAD0AE0` (old map) and `0xAD0AA0` (1.65.1 map) are hardware hotspot-max aggregates. A later constrained sweep of the full `0xAD0000` thermal page found two non-die-like values, `0xAD021C` and `0xAD06EC`. Their identities remain unconfirmed, but a VRM interpretation is now considered unlikely because their ~37 C water-cooled idle values are physically inconsistent with the ~24 C shared HSF and ~27 C expected idle VRM temperature. See Current Conclusions and runs 5-6.

Read this document before running another experiment. Append new conditions, captures, analysis, and conclusions here rather than creating another handoff.

## Current Conclusions

Confirmed:

1. The usual NVAPI thermal call does not expose RTX 50 hotspot data on this system.
2. HWMonitor obtains the data through read-only PCI BAR0 MMIO using its proprietary `cpuz165` kernel driver.
3. The temperature values are 8.8 fixed-point Celsius.
4. HWMonitor 1.65 directly exposes `0xAD0A90` as `Hot Spot` and labels `0xAD0AE0` as `[HotSpot #2]`.
5. HWMonitor 1.65.1 removes `0xAD0AE0` and the other old high-offset reads. It instead reads six contiguous channels and exposes their per-poll maximum as `Hot Spot`.
6. The removed second-hotspot register is `0xAD0AE0`, not `0xAD0A9C` or `0xAD0AA0`.
7. **`0xAD0AE0` is a hardware MAXIMUM/hotspot aggregate register** (run3, 2026-07-18): it equals max(other exposed channels) and follows the argmax through regime changes (tracks `0xAD0ADC` at idle because `ADC` is hottest at idle, snaps to `0xAD0A90` under load). Its idle equality with `ADC` was coincidence, not aliasing.
8. **`0xAD0AA0` in the 1.65.1 map is likewise a hardware max aggregate** (run4: equals max of the other five in 135/135 samples, zero deviation), possibly the same hardware max exposed at a different offset. `0xAD0A9C` is an individual die channel that is hottest at idle.
9. `0xAD0AD4`/`0xAD0A94` and `0xAD0AD8`/`0xAD0A98` are duplicate reads of the same sensors (within 0.25 C through a 24 C load swing).
10. **The VRM hypothesis is rejected for the known `0xAD0A90`-`0xAD0AE4` die/hotspot block.** At ~166 W actual on air and ~221 W actual on water, those channels behaved as die sensors or mirrors/aggregates.
11. Fixed-point decode, architecture gate, and driver IOCTL mechanism are all fully understood (sections below).
12. A constrained, read-only sweep of aligned dwords in `0xAD0000`-`0xAD0FFC` completed twice (idle and loaded) with 1,024/1,024 successful reads each, no TDR, and no invalid IOCTL completion.
13. The sweep found many additional die-like channels/mirrors plus **two unusual candidates outside the known block: `0xAD021C` and `0xAD06EC`**. In a 180-second water-cooled pulse capture at ~223 W actual:
    - `0xAD021C`: 37.06 C baseline, 39.44 C terminal-load mean, +2.38 C; 40.23 C peak.
    - `0xAD06EC`: 37.02 C baseline, 39.92 C terminal-load mean, +2.90 C; 40.30 C peak.
    - Direct die channel `0xAD0A90`: 26.78 C baseline, 53.17 C terminal-load mean, +26.39 C.
    - Both values rose promptly but were strongly attenuated versus die temperature and cooled gradually. They use full 8.8 fractional resolution (1/256 C values), unlike the exposed die channels' usual 1/32 C steps. This supports a different signal or processing path but does not establish that either is a physical temperature.
14. HWiNFO was checked by Jon and exposes no GPU VRM temperature. That does not exclude an undocumented NVIDIA internal register; it only rules out an already-labeled HWiNFO sensor on this system.

Still open:

- Identity of `0xAD021C` and `0xAD06EC`: likely possibilities are a remote PCB/internal-air sensor not touching the HSF, filtered/derived thermal values, or control/estimate registers that merely use the same 8.8 encoding. A GPU-package `Tcase` estimate and VRM temperature are disfavored: with the die near 26-27 C, shared HSF near 24 C, low idle VRM losses, and good VRM-to-HSF contact, a real idle value near 37 C is not credible for either. Workload separation or physical correlation would be needed to establish any physical identity.
- `0xAD021C` alternates between two close bands under load, and `0xAD06EC` only passed the bit-30/plausibility filter in 196/346 poll rows despite zero IOCTL errors. These update/validity semantics need explanation.
- Whether the hardware max includes internal channels beyond the exposed ones (`0xAD0AE0` occasionally read up to 0.34 C above max-of-six in run3). The page sweep found additional die-like channels, but a single snapshot cannot prove which feed the max tree.
- No user-confirmed naming or UI decision exists for exposing hotspot in MechControl.
- A production access mechanism has not been chosen (see Production Integration Constraints).
- Possible additional avenue for board/VRM temperature: unidentified EC temperature registers (see `ec_trace_decoded.md`), readable through MechControl's existing safe path.

## Test System and Cooling Context

Current test system:

- GPU: NVIDIA GeForce RTX 5090 Laptop GPU
- PCI vendor/device: `10DE:2C18`
- PCI BDF: `0000:02:00.0`
- NVIDIA driver: 595.97
- GPU is shunt-modded
- GPU operation can reach approximately 230 W per the user
- Current HWMonitor process: 1.65.1 from `hwmonitor_1.65.1\HWMonitor_x64.exe`
- HWMonitor 1.65 is retained under `C:\Temp\tandem\hwmonitor_1.65_official_inspection`

Cooling information supplied by the user:

- The laptop supports water cooling; the final page-sweep experiments were run water-cooled at the 234 W configured limit.
- The GPU VRMs have good, close coverage from the heatsink.
- Experiments may be performed with or without water cooling if useful.

User hypothesis, explicitly unverified:

- At low load, the VRMs may be cooler than the GPU die.
- At high load, the VRMs may become hotter because there are relatively few power stages carrying a high load at approximately 230 W GPU power.

This is a reasonable hypothesis to test, not evidence that any current channel is VRM. Because the die and VRMs share a closely coupled heatsink, water cooling may move both temperatures in the same direction and can make identification harder if only absolute temperature is considered. Transient response, workload sensitivity, and matched air-versus-water deltas are more informative.

The shunt modification may make NVIDIA-reported `power.draw` differ from actual board input. Treat software power as a relative signal within the same hardware configuration. Record external power measurements if available, but do not assume they exist.

## Existing MechControl Behavior

MechControl currently has no NVAPI, NVML, or direct GPU MMIO integration. Existing GPU temperature is read from the model profile's canonical `gpuTemp` EC register, normally `0x44F` on this machine.

No production MechControl source, profiles, EC register definitions, write paths, or UI have been changed for this investigation. The work consists of reverse-engineering notes, read-only captures, and standalone tools.

Any future production implementation must remain model-compatible:

- Do not hardcode this machine's PCI device ID or bus number.
- Detect NVIDIA BDF and Blackwell architecture at runtime.
- Probe support and hide unavailable sensors.
- Preserve EC `gpuTemp` as the existing general GPU temperature.
- Handle a sleeping/power-gated laptop dGPU without waking it unnecessarily.

## NVAPI Result

`NvAPI_GPU_GetThermalSettings` interface ID `0xE3640A56` returns `NVAPI_INVALID_ARGUMENT` for tested structure versions 1 through 5 and sensor-array sizes 1 through 16.

Other NVAPI functions work in the same probe:

- initialization
- physical GPU enumeration
- full GPU name
- current P-state

This isolates the failure to the old thermal path rather than NVAPI setup.

The NVAPI driver on this system only exports `nvapi_QueryInterface` and `nvapi_Direct_GetMethod`; initialization and other functions are obtained through QueryInterface. HWMonitor dynamically requests 69 valid NVAPI function IDs, but its RTX 50 hotspot path is not one of those calls.

## CPUID Driver and MMIO Path

HWMonitor installs or loads:

- Service/device family: `cpuz165`
- User-mode path: `\\.\cpuz165`
- Driver file observed during one run: `C:\ProgramData\CPUID Software\hwmonitor\Y8GcP54Oezn`

The random driver filename may change on reinstall, so recalculate its hash when needed.

Ghidra decompilation of the approximately 43 KB driver confirms the relevant read-only PCI BAR IOCTL:

```text
IOCTL 0x9C4025D4

Input, four uint32 values:
  [0] encoded PCI BDF
  [1] PCI BAR configuration offset
  [2] allow 64-bit BAR
  [3] BAR-relative register offset

Output, two uint32 values:
  [0] success flag (1)
  [1] register value
```

Driver helper `FUN_00011940` at image address `0x11940`:

1. Decodes bus/device/function from input dword 0.
2. Calls `HalGetBusDataByOffset` for the BAR selected by input dword 1.
3. Handles a 64-bit BAR when input dword 2 is 1.
4. Adds input dword 3 as the BAR-relative register offset.
5. Reads one dword through `MmMapIoSpace` or an existing cached mapping.

Input used for this GPU:

```text
encoded BDF       = 0x80020000  # bus 2, device 0, function 0
BAR config offset = 0x10        # BAR0
allow 64-bit BAR  = 1
offset            = selected thermal register
```

`0x80020000` is the encoded PCI BDF. It is not physical BAR0 plus `0x20000`.

## Architecture Gate and Encoding

Both HWMonitor versions gate the direct reads on this Blackwell architecture value:

```text
(gpuArchitecture & 0xFFFFF000) == 0x010D0000
```

HWMonitor 1.65.1 decoding:

```text
valid = (raw & 0x40000000) != 0
temperatureC = (raw & 0xFFFF) / 256.0
```

Also reject `0xFFFFFFFF`. A power-gated laptop dGPU can return all ones, which falsely satisfies the bit-30 validity test. A plausible-temperature range check is also required.

HWMonitor 1.65 uses an equivalent 8.8 decode:

```text
integer = (raw >> 8) & 0xFF
fraction = raw & 0xFF
valid = integer != 0x00 && integer != 0xFF
temperatureC = integer + fraction / 256.0
```

Version 1.65 does not use the 1.65.1 bit-30 check, but its `0xFF` integer-byte rejection also rejects an all-ones value.

## HWMonitor 1.65.1 Behavior

Binary baseline:

- File/product version: `1.6.5.1`
- x64 executable size: 3,582,456 bytes
- x64 SHA-256: `C9C3B20F8B8CFBDCDDE852E03CBD42934E7E946660AC7877057F3143059211D3`
- Authenticode status: valid
- Signer: CPUID

Thermal function `FUN_1400a8b80` maps:

| Sensor ID | BAR0 offset | Hidden label |
|---:|---:|---|
| `0x1D` | `0xAD0A90` | `[TMP #1]` |
| `0x1E` | `0xAD0A94` | `[TMP #2]` |
| `0x1F` | `0xAD0A98` | `[TMP #3]` |
| `0x20` | `0xAD0A9C` | `[TMP #4]` |
| `0x21` | `0xAD0AA0` | `[TMP #5]` |
| `0x22` | `0xAD0AA4` | `[TMP #6]` |

The visible `Hot Spot` is not direct `0xAD0A90`. Assembly for `FUN_1400fe720` shows a separate aggregate object. After each successful hidden-sensor read, HWMonitor compares that temperature with the aggregate's current value and writes the larger value through `FUN_140063194`. The aggregate is reset at the start of the next successful polling batch.

Therefore:

```text
HWMonitor 1.65.1 Hot Spot = max(TMP #1 ... TMP #6)
```

Useful symbols and addresses:

- `FUN_1400a8b80`: six-register Blackwell mapping and fixed-point decode
- `FUN_1400fe720`: six hidden sensors and visible maximum aggregation
- `FUN_140063194`: writes scaled current value and optionally updates extrema
- `NVIDIA_DirectIO_DisplayAPI::vftable`: `0x140212328`
- Thermal function is the vtable `+0x40` slot

## HWMonitor 1.65 Behavior

The exact official portable ZIP remains available at:

`https://download.cpuid.com/hwmonitor/hwmonitor_1.65.zip`

Verified archive and executable:

- ZIP path: `C:\Temp\tandem\hwmonitor_1.65.zip`
- ZIP size: 2,820,347 bytes
- ZIP SHA-256: `F6A835D2A9690CD13832C79C686D8DC7CB8639C14552978ED21D194B8504A23F`
- ZIP hash matches CPUID's published checksum
- Extracted x64 version: `1.6.5.0`
- Extracted x64 size: 3,581,432 bytes
- Extracted x64 SHA-256: `47C18254CE92D8561D982C6F1CC35657A39FC0BDD3EEE3873624D77EC7207D44`
- Authenticode status: valid
- Signer: CPUID
- Archive contents: `HWMonitor_x64.exe`, `HWMonitor_x32.exe`, and `hwm_readme.pdf`; no DLLs or extra loaders

Static mapping from thermal function `FUN_1400a88b0` and sensor creation function `FUN_1400fe4c0`:

| Sensor ID | BAR0 offset | HWMonitor 1.65 label |
|---:|---:|---|
| `0x02` | `0xAD0A90` | visible `Hot Spot` |
| `0x22` | `0xAD0AE0` | `[HotSpot #2]` |
| `0x1D` | `0xAD0A94` | `[TMP #9]` |
| `0x1E` | `0xAD0A98` | `[TMP #10]` |
| `0x1F` | `0xAD0AD4` | `[TMP #11]` |
| `0x20` | `0xAD0AD8` | `[TMP #12]` |
| `0x21` | `0xAD0ADC` | `[TMP #13]` |

Version 1.65.1 removes reads at `0xAD0AD4`, `0xAD0AD8`, `0xAD0ADC`, and `0xAD0AE0`. It adds contiguous `0xAD0A9C`, `0xAD0AA0`, and `0xAD0AA4`, and changes the visible result from direct `0xAD0A90` to the six-channel maximum.

The code-level comparison conclusively identifies the removed second reading as `0xAD0AE0`.

## Existing Live Baselines

### HWMonitor 1.65.1 Idle

A focused 12-second capture recorded four samples per channel. Public NVIDIA telemetry at the end was 52 C, approximately 20 W, and 0% GPU utilization.

| BAR0 offset | Average | Range |
|---:|---:|---:|
| `0xAD0A90` | 53.273 C | 53.125-53.500 C |
| `0xAD0A94` | 51.273 C | 51.188-51.469 C |
| `0xAD0A98` | 51.938 C | 51.844-52.125 C |
| `0xAD0A9C` | 54.938 C | 54.844-55.125 C |
| `0xAD0AA0` | 54.938 C | 54.844-55.125 C |
| `0xAD0AA4` | 52.273 C | 52.188-52.500 C |

`0xAD0A9C` and `0xAD0AA0` were identical throughout both 1.65.1 captures. They supplied the aggregate `Hot Spot` at idle. They are not the second reading removed from 1.65.

### HWMonitor 1.65 Idle

A read-only 15-second capture recorded five samples per channel:

| BAR0 offset | Label | Average | Range |
|---:|---|---:|---:|
| `0xAD0A90` | `Hot Spot` | 52.413 C | 52.281-52.688 C |
| `0xAD0AE0` | `[HotSpot #2]` | 54.138 C | 54.000-54.375 C |
| `0xAD0A94` | `[TMP #9]` | 50.425 C | 50.250-50.594 C |
| `0xAD0A98` | `[TMP #10]` | 51.169 C | 51.000-51.375 C |
| `0xAD0AD4` | `[TMP #11]` | 50.456 C | 50.250-50.719 C |
| `0xAD0AD8` | `[TMP #12]` | 51.150 C | 51.000-51.375 C |
| `0xAD0ADC` | `[TMP #13]` | 54.150 C | 54.000-54.375 C |

Important idle relationships:

- `0xAD0AE0` and `0xAD0ADC` were equal in four of five samples.
- They differed by only 0.0625 C in the fifth sample.
- `0xAD0AE0` averaged approximately 1.7 C above direct `0xAD0A90`.
- `0xAD0A94` approximately tracked `0xAD0AD4`.
- `0xAD0A98` approximately tracked `0xAD0AD8`.

The repeated pairings make aliasing or redundant views plausible, but do not prove it. Closely coupled physical sensors can also track each other at idle.

## Why 1.65.1 Probably Changed the Mapping

There is no direct CPUID statement identifying the engineering reason. The following is evidence-based interpretation, not confirmed fact.

The change looks like a register-map and semantics correction rather than removal of one independently validated VRM sensor:

- 1.65 uses the first three contiguous offsets and then jumps from `0xAD0A98` to `0xAD0AD4`.
- The later 1.65 channels form near-duplicate pairs at idle.
- 1.65 labels `0xAD0AE0` `[HotSpot #2]` without a confirmed hardware identity.
- 1.65.1 removes the entire old high-offset group, not only `0xAD0AE0`.
- 1.65.1 adopts a contiguous six-register block, a bit-30 validity check, and a conservative maximum aggregate.

The most likely interpretation is that CPUID no longer considered the old high-offset mapping or `[HotSpot #2]` label reliable enough. This does not prove that `0xAD0AE0` is invalid; it explains why experiments are still required.

## Experimental Questions

The original hotspot questions were answered on 2026-07-18 (see Experiment Log). The later full-page sweep reopened only the broader VRM/board-sensor question:

1. Is `0xAD0AE0` independent from `0xAD0ADC`, or are they aliases/redundant views?
   **ANSWERED: neither.** `0xAD0AE0` is a max aggregate; it equals `ADC` only while `ADC` is the hottest channel (idle), and switches to `A90` under load.
2. Does `0xAD0AE0` respond primarily to GPU die activity, board power, memory activity, or heatsink temperature?
   **ANSWERED:** die activity — it is the maximum of the die thermal channels.
3. Does any response pattern support a VRM interpretation?
   **ANSWERED for the known HWMonitor block: no. The full-page values do not reverse this:** `0xAD021C` and `0xAD06EC` have a strongly attenuated, different-resolution response, but their ~37 C water-cooled idle values conflict with the shared-HSF thermal geometry. They are more likely derived/control values than VRM temperatures.
4. What are the identities or relationships of 1.65.1 channels `0xAD0A9C` and `0xAD0AA0`?
   **ANSWERED:** `A9C` is an individual die channel (hottest at idle); `AA0` is a hardware max aggregate (135/135 exact). They are equal only at idle.
5. Does water cooling affect the candidates differently from direct `0xAD0A90`?
   **ANSWERED for the known HWMonitor channels:** no differential behavior; those channels scale together. Water dropped the hotspot peak ~11 C despite ~55 W more actual power (54.4 C at ~221 W water vs 65.3 C at ~166 W air). `0xAD021C` and `0xAD06EC` were discovered only during water-cooled testing, so they do not yet have a matched air comparison.

New primary question after runs 5-6:

6. What are `0xAD021C` and `0xAD06EC`?
   **OPEN, but VRM is unlikely.** Their absolute idle values, full-resolution fractions, alternating/update behavior, and intermittent validity point toward derived/control quantities. Required discriminators: matched-power workload separation and, ideally, physical correlation. Do not name either register as a physical temperature based on the current data.

The original working-hypothesis table is retained below for methodology reference.

Working hypotheses to test:

| Hypothesis | Expected observations |
|---|---|
| Alias/redundant register | Candidate stays equal or within one quantization step of another channel across every workload, transient, cooldown, and cooling mode. |
| GPU die thermal channel | Rapid rise with core-heavy load, strong correlation with direct/aggregate hotspot, rapid fall when load stops. |
| VRM or board-power sensor | Stronger relationship to sustained board power/current than to core utilization alone; potentially slower rise and cooldown; may continue rising briefly after load begins or ends. |
| Memory-related sensor | Disproportionate response to memory-heavy load relative to core-heavy load at similar total power. |
| Heatsink or aggregate sensor | Slow response, strong sensitivity to fan/water state, and broad correlation across workload types. |

None of these signatures alone is definitive because the die, VRMs, and memory are thermally coupled through the shared heatsink.

## Experimental Controls and Safety

All current MMIO work is read-only. Prefer the verified HWMonitor path and Frida capture. The approved full-page work uses purpose-built tools restricted to aligned PCI BAR0 reads in `0xAD0000`-`0xAD0FFC` or a fixed candidate list; it does not use the generic physical-memory IOCTL or accept arbitrary addresses.

Controls:

- Run only one HWMonitor version at a time because the driver/device can be exclusively held.
- Start capture before the load and continue through cooldown.
- Keep fan mode or fan curve constant between matched runs.
- Record whether water is on or off.
- Record ambient temperature and coolant inlet temperature if available.
- Allow a stable baseline before each run.
- Use the same workload settings, duration, display mode, and power configuration for matched air/water comparisons.
- Record NVIDIA P-state, temperature, power, GPU utilization, memory utilization, and clocks concurrently.
- Treat reported power as relative because of the shunt modification.
- Do not infer identity from a single peak temperature.

Safety:

- Do not start a sustained high-power or stress workload without Jon choosing or approving it.
- Escalate load in steps rather than beginning with a long 230 W run.
- Stop on instability, telemetry loss, an unexpected temperature rise, cooling malfunction, or violation of the machine's already-established operating limits.
- Do not invent a new temperature or power limit for this experiment.
- Only switch water state while running if the cooling hardware's normal operating procedure permits it safely. Otherwise use separate fully stopped air and water runs.
- Do not redistribute or depend on CPUID's proprietary driver in production code.

## Proposed Experiment Matrix

Prioritize HWMonitor 1.65 for the first experiments because it reads direct `0xAD0A90`, `0xAD0ADC`, and target `0xAD0AE0` in the same polling batch.

### Phase 0: Capture Validation

1. Confirm the running process path and version.
2. Attach `frida_blackwell_thermal.js` for 5 to 10 seconds at idle.
3. Confirm seven 1.65 offsets appear, with no errors, all-ones values, or invalid samples.
4. Confirm synchronized NVIDIA telemetry is being written.

### Phase 1: Air-Cooled Baseline

The laptop is currently air cooled, so establish this baseline first.

1. Hold the same fan state for the entire series.
2. Record at least several minutes of idle after temperatures stabilize.
3. Record ambient temperature if available.
4. Calculate baseline mean, range, and pairwise differences, especially:
   - `0xAD0AE0 - 0xAD0ADC`
   - `0xAD0AE0 - 0xAD0A90`
   - `0xAD0AD4 - 0xAD0A94`
   - `0xAD0AD8 - 0xAD0A98`

### Phase 2: Short Load Pulses

Use short, repeatable pulses before sustained high power:

1. Core-heavy pulse.
2. Memory-heavy pulse.
3. High-board-power pulse chosen by Jon.
4. Include baseline before each pulse and enough cooldown afterward to observe decay.

Short pulses help separate a fast die response from a slower heatsink or VRM response without requiring immediate long-duration high-power operation.

### Phase 3: Power Steps

If the short tests are stable, repeat one workload at increasing, controlled power levels available through the existing setup. Do not assume software-reported watts equal actual watts after the shunt modification.

For each step:

1. Hold the workload and cooling state constant.
2. Reach a stable or clearly trending temperature.
3. Record the rise slope, peak or terminal temperature, and cooldown.
4. Compare sensor delta versus relative power.

The user's VRM hypothesis predicts that a true VRM channel might separate upward from the die-related channel as load approaches the approximately 230 W operating region. A failure to separate does not disprove VRM because heatsink coupling may dominate.

### Phase 4: Water-Cooling A/B

Repeat the same workload sequence with water cooling enabled after an adequate stabilization period.

Best comparison:

- Same workload settings
- Same power configuration
- Same fan behavior
- Similar ambient condition
- Air run and water run recorded separately

If the hardware can safely engage water while a constant load is running, an air-to-water transition at fixed workload may be especially informative. Compare onset delay and cooling slope for `0xAD0A90`, `0xAD0ADC`, and `0xAD0AE0`. If live switching is not part of the hardware's normal safe procedure, do not do it; use separate runs.

Interpret water results cautiously:

- Strong water sensitivity supports physical coupling to the cooled heatsink, not a unique VRM identity.
- A true VRM sensor with good heatsink coverage may cool substantially with water.
- The GPU die will also cool substantially, so relative timing and normalized deltas matter more than direction alone.

### Phase 5: HWMonitor 1.65.1 Matched Runs

After the 1.65 target-channel experiments, repeat selected matched runs with 1.65.1 to classify its contiguous channels and aggregate behavior.

The two versions cannot provide every old and new offset in the same polling batch. Compare matched conditions rather than treating separately timed values as simultaneous.

## Capture Procedure

Run commands from `C:\Users\Jon\code\Mechcontrol`.

### Switch to Verified HWMonitor 1.65

Stop the current HWMonitor process, then launch the verified old binary:

```powershell
Get-Process HWMonitor_x64 -ErrorAction SilentlyContinue | Stop-Process
Start-Process 'C:\Temp\tandem\hwmonitor_1.65_official_inspection\HWMonitor_x64.exe'
```

Confirm the path before applying load:

```powershell
Get-Process HWMonitor_x64 | Select-Object Id, Path, Responding
```

### Capture Thermal IOCTLs

Use a descriptive filename containing version, cooling mode, workload, and run number:

```powershell
$p = Get-Process HWMonitor_x64 | Select-Object -First 1
python tools\frida_attach.py $p.Id 180 tools\frida_blackwell_thermal.js captures\blackwell-1.65-air-WORKLOAD-run1.jsonl
```

The capture script recognizes the known offsets from both versions and records:

- timestamp in milliseconds
- offset
- NT status
- driver success value
- raw register value
- all-ones state
- validity
- decoded Celsius

### Capture NVIDIA Telemetry

Run concurrently in another terminal and stop it after the thermal capture finishes:

```powershell
nvidia-smi --query-gpu=timestamp,name,pstate,temperature.gpu,power.draw,utilization.gpu,utilization.memory,clocks.gr,clocks.mem --format=csv -l 1 -f captures\nvidia-1.65-air-WORKLOAD-run1.csv
```

The full query above was validated on this system with driver 595.97. Recheck it if the NVIDIA driver or `nvidia-smi` version changes.

### Restore HWMonitor 1.65.1

After the old-version experiment:

```powershell
Get-Process HWMonitor_x64 -ErrorAction SilentlyContinue | Stop-Process
Start-Process 'C:\Users\Jon\code\Mechcontrol\hwmonitor_1.65.1\HWMonitor_x64.exe'
```

Verify the restored process path and optionally attach for a short idle capture to confirm six-channel polling.

## Analysis Plan

Align Frida and NVIDIA telemetry by timestamp. For each sensor and each experimental phase, calculate:

- stable baseline mean and standard deviation
- delta from baseline
- initial rise slope
- time to a fixed fraction of the total rise, if the run is long enough
- peak or terminal temperature
- time of peak relative to workload start/stop
- initial cooldown slope
- cooldown half-time
- correlation with reported power
- correlation with GPU utilization
- correlation with memory utilization
- response difference between air and water

Candidate-specific comparisons:

1. `0xAD0AE0` versus `0xAD0ADC`
   - Count exact matches.
   - Measure absolute error at every sample.
   - Check whether one consistently leads the other during rapid transitions.
2. `0xAD0AE0` versus direct `0xAD0A90`
   - Compare rise latency, slope, peak delta, and cooldown.
3. Old duplicated-looking pairs
   - `0xAD0A94` versus `0xAD0AD4`
   - `0xAD0A98` versus `0xAD0AD8`
4. New 1.65.1 pair
   - `0xAD0A9C` versus `0xAD0AA0`
   - Determine whether equality persists under every load and cooling mode.
5. Latest aggregate
   - Reconstruct `max(0xAD0A90 ... 0xAD0AA4)` and confirm the contributing channel over time.

Evidence that would support aliasing:

- exact equality or fixed quantized difference across idle, multiple workloads, transitions, cooldown, and air/water modes
- no independent lag, overshoot, or workload sensitivity

Evidence that would support, but not prove, a VRM interpretation:

- stronger scaling with sustained board power than with core utilization alone
- behavior that diverges from die hotspot under matched-power core-heavy and memory-heavy loads
- a distinct thermal time constant
- a reproducible response to water cooling consistent with the VRM-to-heatsink path
- agreement with an external VRM-area thermocouple or other physical measurement, if one is later available

External physical correlation would be more conclusive than software behavior alone. A thermocouple near the VRM/heatsink interface or another safe localized measurement could validate the software channel, but no such measurement is assumed or required for the next run.

## Experiment Record Template

Append one block per run:

```text
Date/time:
HWMonitor version and executable path:
Cooling: air / water
Water transition during run: no / yes, with timestamp
Fan mode or fixed behavior:
Ambient temperature, if available:
Coolant inlet temperature, if available:
Workload and exact settings:
Power configuration:
Reported NVIDIA power range:
External power, if available:
Baseline duration:
Load duration:
Cooldown duration:
Thermal capture file:
NVIDIA telemetry file:
Observed anomalies:
Initial interpretation:
Confidence and unresolved alternatives:
```

## Experiment Log

### Shunt-Mod Power Calibration (2026-07-18, from Jon)

- Reported 175 W corresponds to approximately 234 W actual: scale factor approximately 1.337.
- Air-cooled runs are limited by Jon to 165 W actual, which should read as roughly 120-125 W in `nvidia-smi`.
- Apply this factor when correlating sensor response with power; continue to treat software power as relative.

### Run: Phase 0 Capture Validation (2026-07-18)

```text
Date/time: 2026-07-18
HWMonitor version and executable path: 1.65, C:\Temp\tandem\hwmonitor_1.65_official_inspection\HWMonitor_x64.exe (PID 15664)
Cooling: air
Water transition during run: no
Fan mode or fixed behavior: auto (stock EC behavior)
Workload and exact settings: none (idle)
Reported NVIDIA power range: ~21 W at 52 C, P2, 0% util (pre-capture check)
Load duration: n/a (10 s idle validation)
Thermal capture file: captures\blackwell-1.65-air-phase0-validation.jsonl
Observed anomalies: none
Initial interpretation: all seven 1.65 offsets polling, 4 valid samples each, no all-ones. 0xAD0AE0 == 0xAD0ADC in all 4 samples (54.84 C avg both).
```

### Run: Phase 1 Air Idle Baseline, run1 (2026-07-18)

```text
Date/time: 2026-07-18
HWMonitor version and executable path: 1.65, C:\Temp\tandem\hwmonitor_1.65_official_inspection\HWMonitor_x64.exe
Cooling: air
Water transition during run: no
Fan mode or fixed behavior: auto (stock EC behavior) - NOTE: later runs use fixed 70% duty; re-baseline required
Workload and exact settings: none (idle)
Baseline duration: 300 s (114 samples per channel)
Thermal capture file: captures\blackwell-1.65-air-idle-baseline-run1.jsonl
NVIDIA telemetry file: captures\nvidia-1.65-air-idle-baseline-run1.csv (263 rows)
Observed anomalies: none
Initial interpretation:
  Means/SD: 0xAD0A90 53.21/0.21, 0xAD0A94 51.17/0.17, 0xAD0A98 51.89/0.17,
            0xAD0AD4 51.17/0.17, 0xAD0AD8 51.89/0.17, 0xAD0ADC 54.89/0.17, 0xAD0AE0 54.89/0.17
  Pairwise: 0xAD0AE0-0xAD0ADC mean 0.0016 C, exact-equal 112/114 (max diff 0.156 C)
            0xAD0AE0-0xAD0A90 mean 1.679 C, exact-equal 0/114
            0xAD0AD4-0xAD0A94 mean -0.002 C, exact-equal 78/114, symmetric jitter +/-0.16
            0xAD0AD8-0xAD0A98 mean 0.001 C, exact-equal 76/114, symmetric jitter +/-0.19
Confidence and unresolved alternatives: 0xAD0AE0/0xAD0ADC bit-identical in 98% of samples strongly
  suggests alias or same underlying read; AD4/A94 and AD8/A98 look like the same sensors sampled at
  slightly different instants. Idle data cannot fully rule out closely coupled independent sensors;
  load transients (Phase 2) are the discriminator.
```

### Run: Phase 1b Fixed-Fan Idle Re-Baseline, run2 (2026-07-18)

```text
Date/time: 2026-07-18
HWMonitor version and executable path: 1.65, C:\Temp\tandem\hwmonitor_1.65_official_inspection\HWMonitor_x64.exe
Cooling: air
Fan mode or fixed behavior: fixed 70% duty (set by Jon in MechControl)
Power configuration: 165 W actual limit (reads as 124 W)
Workload and exact settings: none (idle)
Baseline duration: 180 s (72 samples per channel)
Thermal capture file: captures\blackwell-1.65-air-fan70-idle-baseline-run2.jsonl
NVIDIA telemetry file: captures\nvidia-1.65-air-fan70-idle-baseline-run2.csv
Observed anomalies: capture caught the active cooling transient after the fan change (all channels
  falling ~6 C during the run, SD ~1.8); acceptable since run3 has its own lead-in.
Initial interpretation: 0xAD0AE0 == 0xAD0ADC bit-identical 72/72 including mid-transient.
```

### Run: Phase 2 FurMark GL 60 s Pulse, run3 (2026-07-18) - AE0 IDENTITY RESOLVED

```text
Date/time: 2026-07-18
HWMonitor version and executable path: 1.65, C:\Temp\tandem\hwmonitor_1.65_official_inspection\HWMonitor_x64.exe
Cooling: air
Water transition during run: no
Fan mode or fixed behavior: fixed 70% duty
Power configuration: 165 W actual limit (reads 124 W; shunt factor ~1.337)
Workload and exact settings: furmark --demo furmark-gl --width 1920 --height 1080 --max-time 60
Reported NVIDIA power range: peak 124.0 W reported (~166 W actual); 60 samples at 100% util
Baseline duration: 60 s lead-in
Load duration: 60 s (t=60 to ~t=122)
Cooldown duration: ~205 s
Thermal capture file: captures\blackwell-1.65-air-fan70-furmarkgl60s-run3.jsonl
NVIDIA telemetry file: captures\nvidia-1.65-air-fan70-furmarkgl60s-run3.csv (311 rows)
Observed anomalies: none; FurMark reported max 62 C public, clocks 1628-2040 MHz
Results:
  Peaks: a90 65.31 (rise 30.1), a94 57.22, a98 55.88, ad4 57.22, ad8 55.84, adc 64.06, ae0 65.31
  0xAD0AE0 vs 0xAD0ADC: exact-equal only 108/134 overall; separated by up to +1.25 C under load.
    The idle equality was NOT aliasing.
  0xAD0AE0 vs 0xAD0A90: equal at load onset and at peak (65.31 both).
  0xAD0AE0 == max(other six channels) in 123/134 samples; worst |diff| 0.34 C (fast-slew timing).
  Argmax handoff: idle max channel = 0xAD0ADC (108 samples), load max channel = 0xAD0A90 (26);
    0xAD0AE0 followed the maximum through the regime change.
  0xAD0AD4/0xAD0A94 and 0xAD0AD8/0xAD0A98 stayed within 0.25 C through a 24 C swing:
    these pairs ARE duplicates/aliases of the same sensors.
Conclusion:
  0xAD0AE0 is a hardware MAXIMUM/hotspot aggregate register (max over the die thermal channels,
  possibly including internal channels beyond the six exposed), not an independent sensor and
  NOT a VRM sensor. HWMonitor 1.65's "[HotSpot #2]" label was approximately correct; 1.65.1's
  software max-of-six reproduces the same quantity from individual channels.
  No channel in the 0xAD0Axx block showed VRM-like behavior (no lagged rise, no power-vs-
  utilization divergence, all channels rose >10 C within one polling batch of load onset and
  decayed immediately on load stop). The VRM-temperature hypothesis is rejected for this
  register block. If a GPU VRM temperature exists on this platform it is elsewhere (likely an
  I2C voltage-controller readout, e.g. via HWiNFO), not in BAR0 0xAD0Axx.
Confidence: high for the max-aggregate identity (argmax handoff + dual-regime equality).
  Phase 4 water A/B no longer needed for AE0 identity; optional for cooling characterization.
```

### Run: Phase 5 HWMonitor 1.65.1 Water Pulse, run4 (2026-07-18) - AA0 IS ALSO A MAX AGGREGATE

```text
Date/time: 2026-07-18
HWMonitor version and executable path: 1.65.1, C:\Users\Jon\code\Mechcontrol\hwmonitor_1.65.1\HWMonitor_x64.exe
Cooling: water (loop active; idle GPU 27 C vs 42 C on air earlier same day)
Water transition during run: no
Fan mode or fixed behavior: NOT fixed (auto); Jon judged fan state irrelevant as water dominates
Power configuration: 234 W actual limit (reads as 175 W; shunt factor ~1.337)
Workload and exact settings: furmark --demo furmark-gl --width 1920 --height 1080 --max-time 60
Reported NVIDIA power range: peak 165.4 W reported (~221 W actual); 60 samples at 100% util
Baseline duration: 60 s lead-in
Load duration: 60 s (t=60 to ~t=120); clocks 2288-2700 MHz (vs 1628-2040 on air at 165 W)
Cooldown duration: ~205 s
Thermal capture file: captures\blackwell-1.65.1-water-234W-furmarkgl60s-run4.jsonl
NVIDIA telemetry file: captures\nvidia-1.65.1-water-234W-furmarkgl60s-run4.csv (302 rows)
Observed anomalies: one earlier aborted attempt (partial files deleted, orphan processes killed)
Results:
  Peaks: a90 54.41 (rise 25.9 from 28.5 base), a94 44.47, a98 42.00, a9c 53.16, aa0 54.41, aa4 50.56
  0xAD0A9C vs 0xAD0AA0: equal at idle (109/135 exact) but SEPARATED under load by up to 1.84 C.
    They are not duplicates.
  0xAD0AA0 == max(other five channels) in 135/135 samples, ZERO deviation.
  Argmax handoff: idle max = 0xAD0A9C (109 samples), load max = 0xAD0A90 (26 samples);
    0xAD0AA0 followed the maximum through the regime change - same fingerprint as 0xAD0AE0 in run3.
Conclusion:
  0xAD0AA0 is a hardware MAXIMUM/hotspot aggregate (like 0xAD0AE0 in the old 1.65 map, possibly
  the same underlying hardware max exposed at a different offset). 0xAD0A9C is an individual die
  channel that happens to be hottest at idle. HWMonitor 1.65.1's software max-of-six therefore
  includes an aggregate among its inputs (harmless: max(max(x),x) = max(x)).
  1.65.1 open question (channel identities of A9C/AA0) is RESOLVED.
  VRM re-check at ~221 W actual under water: all six HWMonitor channels rose within one polling
  batch and decayed immediately at load stop; the VRM rejection stands for this known die/hotspot
  block. HWiNFO (checked by Jon) exposes no labeled VRM sensor.
  SUPERSEDED IN PART BY RUNS 5-6: the later approved full-page BAR0 sweep found non-die-like
  candidates at 0xAD021C and 0xAD06EC outside HWMonitor's six-channel block. Their identities are
  open; do not generalize this run's rejection to the entire 0xAD0000 page.
  Air/water note: at ~221 W actual under water, hotspot peaked 54.4 C versus 65.3 C at ~166 W on
  air - the water loop absorbs ~55 W more at 11 C lower peak.
Confidence: high (zero-deviation max identity; argmax handoff observed in both register maps).
```

### Run: Phase 6 Constrained Thermal-Page Sweep, run5 (2026-07-18)

```text
Date/time: 2026-07-18
Cooling: water
Power configuration: 234 W actual configured limit
GPU state: awake in P0; 26-27 C before each sweep
Access: cpuz165 PCI BAR read IOCTL 0x9C4025D4 only; BAR0 offsets 0xAD0000-0xAD0FFC,
  aligned dwords, one read per offset; no generic physical-memory IOCTL and no writes
Tool: tools\BlackwellThermalPageSweep.cs / .exe (range is hardcoded; accepts no arguments)
Idle output: captures\blackwell-thermal-page-idle-sweep.csv
Idle log: captures\blackwell-thermal-page-idle-sweep.log
Loaded output: captures\blackwell-thermal-page-load-sweep.csv
Loaded log: captures\blackwell-thermal-page-load-sweep.log
Workload for loaded snapshot: FurMark GL 1920x1080, snapshot approximately 45 s into a 60 s pulse
Results: 1,024/1,024 successful reads in each sweep; zero IOCTL errors, no TDR or instability.
  Plausibility filter found 49 idle and 51 loaded words with bit 30 set and low 16 bits decoding
  to 0-150 C. This filter admits unrelated control/status words; dynamic behavior is required.
Findings:
  - Known six-channel block and old high-offset mirror were recovered.
  - 0xAD0AD0/AD4/AD8/ADC/AE0/AE4 mirror A90/A94/A98/A9C/AA0/AA4.
  - B50/B70 mirror the hardware max; B54/B74 mirror AA4; B58/B78 and B5C/B7C are additional
    paired thermal-looking values.
  - Additional die-like channels/mirrors occur at 0xAD0074, 0xAD0104, 0xAD013C/014C,
    0xAD0460/0464, 0xAD04B8-04C4, and 0xAD0844/0848.
  - 0xAD021C and 0xAD06EC were qualitatively different: about 37 C idle and only about
    39-40 C loaded while die channels reached about 53 C. They became the focused candidates.
```

### Run: Phase 7 Fixed-Candidate Water Pulse, run6 (2026-07-18)

```text
Date/time: 2026-07-18
Cooling: water
Fan behavior: auto / not fixed; cooling dominated by water
Power configuration: 234 W actual configured limit
Workload: furmark --demo furmark-gl --width 1920 --height 1080 --max-time 60
Timeline: ~32-92 s load inside a 180 s capture (30 s lead-in, ~88 s cooldown)
Reported power: 166.64 W peak after rejecting malformed >500 W telemetry; ~222.8 W actual
  using the 1.337 shunt factor; 60 telemetry rows at >=90% utilization
Thermal capture: captures\blackwell-thermal-candidates-water-234W-pulse.csv (346 rows)
NVIDIA telemetry: captures\nvidia-thermal-candidates-water-234W-pulse.csv (171 rows)
Tool log: captures\blackwell-thermal-candidates-water-234W-pulse.log (zero IOCTL errors)
Additional baseline: captures\blackwell-thermal-candidates-water-idle-180s.csv (347 rows,
  zero errors). This was an unintended no-load capture caused by PowerShell holding redirected
  child handles; it is valid and retained as a stable water-cooled idle baseline.
Results (baseline mean / terminal-load mean / rise / peak):
  0xAD0A90: 26.78 / 53.17 / +26.39 / 53.81 C (known direct die channel)
  0xAD021C: 37.06 / 39.44 / +2.38 / 40.23 C
  0xAD06EC: 37.02 / 39.92 / +2.90 / 40.30 C
Transient summary (5 s bins):
  baseline: A90 26.8, 21C 37.1, 6EC 37.0 C
  first loaded 35-40 s bin: A90 48.3, 21C 38.8, 6EC 39.4 C
  final loaded 85-90 s bin: A90 53.4, 21C 39.5, 6EC 39.9 C
  95-100 s cooldown bin: A90 33.9, 21C 37.9, 6EC 37.9 C
  175-180 s bin: A90 28.1, 21C 37.2, 6EC 37.1 C
Encoding/update observations:
  - Known die channels generally quantize in 1/32 C increments (raw fractional byte multiple of 8).
  - 21C and 6EC use the full 1/256 C fractional byte, evidence of a different source/processing path.
  - 21C alternates between two close bands under load (~1.3 C separation), suggesting mux/update
    behavior or a derived/control quantity rather than a simple continuously sampled thermistor.
  - 6EC passed validity/plausibility in 196/346 rows despite zero IOCTL failures; its validity or
    update semantics differ from the mostly-valid die channels (341/346 for A90 in this run).
Conclusion:
  21C and 6EC are credible non-die derived/control candidates, but a physical VRM temperature is
  unlikely. With the shared HSF near 24 C, die near 26-27 C, low idle VRM losses, and good VRM
  contact, their ~37 C idle values are not physically credible for the VRMs or GPU package case.
  A remote PCB/internal-air sensor not touching the HSF remains physically plausible. Their small
  response, alternating/update behavior, and intermittent validity also permit filtered estimates
  or control quantities that happen to use an 8.8-like representation. HWiNFO exposes no labeled
  VRM temperature.
Confidence: high that their behavior and encoding differ from the known die channels; low on
  physical identity. Do not expose or name them in production without another discriminator.
```

### Workload Tooling (2026-07-18)

- FurMark 2.10.2 win64 portable downloaded from official Geeks3D (`https://geeks3d.com/dl/get/830`).
- ZIP SHA-256: `27AB2E723E2E65DF720BCAFEA681D2104744EDA4A1E0A0374D7E61EAA820E63B` (35,267,572 bytes) at `C:\Temp\tandem\furmark2.zip`.
- Extracted: `C:\Temp\tandem\furmark2\FurMark_win64\furmark.exe` (reports 2.10.2.0; binary not Authenticode-signed, which is normal for FurMark).
- FurMark 2 is CLI-first. Core-heavy pulse: `furmark --demo furmark-gl --width 1920 --height 1080 --max-time 60`.
- Memory-heavy option for Phase 2: `--furmark-vram-test-gb N`. Extra stress: `--furmark-n2o`.
- Plan agreed with Jon: fixed 70% fan duty for load experiments, 165 W actual air-cooled power limit, first pulse = 60 s FurMark GL after a fixed-fan idle re-baseline.

## Device Access Behavior and Tool Limitations

The direct C# probe initially requested `GENERIC_READ | GENERIC_WRITE`, which returned Access Denied. Opening `\\.\cpuz165` with desired access `0` works because the relevant IOCTL is `FILE_ANY_ACCESS`.

One direct probe run returned `0xFFFFFFFF` for every register, consistent with a power-gated dGPU BAR. A later handle could not be opened while HWMonitor owned or used the driver. Live Frida capture inside HWMonitor is the reliable current correlation method.

`tools\CpuzGpuRegs.cs` is not production-ready:

- hardcoded BDF `0x80020000`
- hardcoded 1.65.1 offsets
- generic physical-memory dump and poll modes unrelated to this feature
- no power-state strategy to avoid waking the dGPU

Its all-ones display bug has been fixed, but do not use its arbitrary physical-memory modes for this investigation.

Purpose-built constrained alternatives added on 2026-07-18:

- `tools\BlackwellThermalPageSweep.cs`: hardcoded read-only PCI BAR0 range `0xAD0000`-`0xAD0FFC`, aligned dwords, no arguments, no physical-memory IOCTL.
- `tools\BlackwellThermalCandidatePoll.cs`: polls only a fixed compiled-in candidate list for 1-300 seconds; accepts no addresses and exposes no write or physical-memory operation.
- Both compiled with the .NET Framework C# 5 compiler and completed the documented captures with zero IOCTL errors. They are diagnostic tools only and still hardcode this test system's BDF.

## Evidence and Tools

### Captures

- `captures\hwmonitor-ioctl-1.jsonl`
  - Full 30-second 1.65.1 capture
  - Approximately 45,303 IOCTL calls
  - Includes ten cycles of the six Blackwell reads
- `captures\blackwell-thermal-idle.jsonl`
  - Focused 1.65.1 idle capture
  - Public state at the end: 52 C, approximately 20 W, 0% utilization
- `captures\blackwell-thermal-hwmonitor-1.65.jsonl`
  - Focused 15-second 1.65 capture
  - Includes direct `0xAD0A90`, target `0xAD0AE0`, and the five other old channels
- `captures\hwmonitor-createfile.jsonl`
  - Shows HWMonitor opening `\\.\cpuz165` and `\\.\NvAdminDevice`
- 2026-07-18 experiment captures (thermal jsonl + matching `nvidia-*.csv` telemetry):
  - `captures\blackwell-1.65-air-phase0-validation.jsonl` (10 s validation)
  - `captures\blackwell-1.65-air-idle-baseline-run1.jsonl` (300 s, auto fans)
  - `captures\blackwell-1.65-air-fan70-idle-baseline-run2.jsonl` (180 s, 70% fans)
  - `captures\blackwell-1.65-air-fan70-furmarkgl60s-run3.jsonl` (pulse; AE0 identity resolved)
  - `captures\blackwell-1.65.1-water-234W-furmarkgl60s-run4.jsonl` (pulse; AA0 identity resolved)
  - `captures\blackwell-thermal-page-idle-sweep.csv` and `.log` (1,024-dword constrained page sweep)
  - `captures\blackwell-thermal-page-load-sweep.csv` and `.log` (matched loaded page sweep)
  - `captures\blackwell-thermal-candidates-water-idle-180s.csv` and `.log` (stable candidate baseline)
  - `captures\blackwell-thermal-candidates-water-234W-pulse.csv` and `.log` (focused candidate pulse)
  - Matching sweep/candidate NVIDIA telemetry files are named `captures\nvidia-thermal-*.csv`; the loaded page-sweep telemetry file is empty due process-output buffering, while run6 telemetry is valid.

### Probe and Capture Tools

- `tools\NvApiProbe.cs`
- `tools\CpuzGpuRegs.cs`
- `tools\frida_blackwell_thermal.js`
- `tools\frida_ioctl.js`
- `tools\frida_createfile.js`
- `tools\frida_attach.py`
- `tools\frida_spawn.py`
- `tools\frida_nvapi_hook.js`
- `tools\frida_nvapi.py`
- `tools\frida_hello.js`
- `tools\analyze_pulse.py` (pulse-capture analysis: per-channel stats, alias checks, timeline, shunt-corrected power)
- `tools\BlackwellThermalPageSweep.cs` (single constrained read-only thermal-page sweep)
- `tools\BlackwellThermalCandidatePoll.cs` (fixed-candidate transient capture)

Generated `.exe` files for standalone C# probes may be present under `tools`. They are probe outputs, not MechControl app artifacts.

### Ghidra Helpers

- `tools\DumpExports.cs`
- `tools\DumpNvapiXrefs.cs`
- `tools\DumpAllDecomp.java`
- `tools\FindGpuThermalDecomp.java`
- `tools\DumpCallersDecomp.java`
- `tools\FindSensorLabelsDecomp.java`
- `tools\DumpFunctionDecomp.java`
- `tools\DumpFunctionListing.java`

`hwmonitor_1.65.1\nvapi-candidates.txt` contains HWMonitor's candidate NVAPI IDs.

### Temporary Reverse-Engineering Outputs

Under `C:\Temp\tandem`:

- Ghidra project `Cpuz165Ghidra`
- Ghidra project `HwMonitorGhidra`
- Ghidra project `HwMonitor165Ghidra`
- `cpuz165-decomp.c`
- `hwmonitor-gpu-thermal-decomp.c`
- `hwmonitor-gpu-thermal-callers.c`
- `hwmonitor-gpu-vtable-callers.c`
- `hwmonitor-hot-spot-string-callers.c`
- `hwmonitor-sensor-labels.c`
- `hwmonitor-sensor-update.c`
- `hwmonitor-gpu-sensors.asm`
- `hwmonitor-1.65-gpu-thermal.c`
- `hwmonitor-1.65-sensor-labels.c`
- `hwmonitor-1.65-gpu-sensors.asm`
- `hwmonitor_1.65.zip`
- `hwmonitor_1.65_official_inspection\`

## Production Integration Constraints

CPUID's driver is proprietary and is evidence/probe infrastructure only. Do not redistribute or make it a production dependency.

Open production-access options:

1. A properly signed minimal driver.
2. An evaluated redistributable or open-source hardware-access component.
3. An optional HWiNFO shared-memory provider requiring HWiNFO to run.

A portable, validly signed HWiNFO64 8.44-5935 binary exists at:

`C:\Users\Jon\OneDrive\PC&laptop SETUP\SOFTWARE\INSTALL\Direct to Program Files\Direct to Program_Files\HWiNFO64.exe`

Jon checked HWiNFO during this work; it exposes no labeled GPU VRM temperature on this system. Shared-memory integration remains untested and would not provide a VRM temperature unless a future HWiNFO version adds one.

Windows VBS/HVCI reports disabled on this test system. That is local state only and cannot be assumed on other supported machines.

A production provider would need to:

- enumerate the NVIDIA GPU and PCI BDF dynamically
- confirm Blackwell architecture at runtime
- read `0xAD0AA0` for hotspot: it is the hardware max aggregate and matches HWMonitor 1.65.1's
  software max-of-six in a single read (established 2026-07-18, run4, 135/135 exact); the five
  individual channels remain available as a cross-check/validation
- require the valid bit and reject all ones and implausible temperatures
- return unavailable while the dGPU is power-gated
- avoid waking the dGPU merely to poll
- hide unsupported sensors
- preserve EC `gpuTemp` as the general existing GPU temperature
- keep `0xAD021C` and `0xAD06EC` diagnostic-only unless their physical identities and validity/update semantics are established across machines

Do not begin production integration until the sensor identity question and access mechanism have been considered explicitly.

## Immediate Next-Session Checklist

Hotspot identification completed on 2026-07-18 (runs 0-4). The approved page sweep and focused transient (runs 5-6) then found unresolved non-die candidates at `0xAD021C` and `0xAD06EC`. HWMonitor 1.65.1 was restored as the resident version after every direct probe.

If picking this up again:

1. Read Current Conclusions and runs 5-6 before interpreting any thermal-page value.
2. Hotspot identity is closed. Product decisions remain: whether to expose hotspot, what to name it, and which production access mechanism to use (see Production Integration Constraints). Discuss with Jon before writing production code.
3. Physical identity is open for `0xAD021C` and `0xAD06EC`, but VRM is disfavored by the water-cooled idle thermal geometry. Best next test, if their semantics still matter: matched core-heavy versus memory-heavy workloads at similar actual power. External physical correlation is the strongest discriminator.
4. Also inspect unidentified EC temperature registers (`ec_trace_decoded.md`) through MechControl's existing safe read path.
5. Do not broaden BAR0 access beyond the approved `0xAD0000` page. Use the constrained tools, never `CpuzGpuRegs` generic physical-memory modes.
6. Analysis tooling for known-channel captures: `tools\analyze_pulse.py`. Candidate-run method and summary are documented in run6.
