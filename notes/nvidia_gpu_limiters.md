# NVIDIA GPU limits: what limits the clock

Measured 2026-10-29/30 on Jon's RTX 5090 Laptop (driver 617.14, VBIOS 98.03.5e.00.1e) with
read-only probes and the deployed app. One GPU and one driver: treat every "how" here as
measured, not documented. The GPU is shunt-modded; all readings are the sensors' reported
values, which are also what the limits compare against.

Related: `nvidia_power_integration.md` (app behavior), `nvidia_board_input_current.md` (board
input RE), `nvidia_power_override_research.md` (BIOS overrides).

## Summary

- **Five things limit the clock, and they act together.** Power, NVVDD current, MSVDD current,
  board input current, and the inline power controller (IPC) clipping power peaks. Over any
  readable interval several hold at once and blend; there is no clean "the limiter".
- **Power** and the **two rail currents** work through driver controllers that each set a clock
  cap; the lowest cap applies. NVVDD and MSVDD are two limits enforced by **one** current
  controller with one cap: whichever rail runs out first sets it.
- **Board input** has no controller or cap. The IPC enforces it in hardware, pulling the clock
  below the caps. The IPC also clips power peaks, so it is active even with board input maxed.
- **NVVDD/MSVDD "currents" are rail power / a fixed voltage** (0.630 V / ~0.74 V), so the current
  limits are effectively rail power limits (210 A ≈ 132 W NVVDD).
- **Measured current below its limit is normal while that rail limits.** The controller limits a
  prediction at its cap clock; the GPU runs below the cap, so the measured value is lower. The
  binding rail's prediction at the cap equals its limit by construction, so it says nothing.
- **What shows each limit** (the app's colors, Jon's thresholds from watching each hold): reading
  / limit for GPU power (94-99%), NVVDD (89-99%), MSVDD (90-99%) and board input (68-81%; it is
  enforced on fast peaks, so its average stays well below the limit), and IPC activity (52-80%).

## 1. The limits

| Limit | Set by | Stock | Unlocked max | Enforced by |
|---|---|---|---|---|
| GPU power (TGP) | Lowest of our request (F8) and the laptop budget (F7: firmware/Dynamic Boost from EC TGP + DB) | 175 W (BIOS max) | 250 W | Power policy 2 → power controller |
| NVVDD (core) current | Policy 13 limit (slider) | 210 A | 294 A | Current controller |
| MSVDD (fabric) current | Policy 14 limit (slider) | 60 A | 84 A | Current controller |
| Board input current | Policy 9 limit (slider) | 11.053 A | 15.474 A | Hardware threshold → IPC |

- The power policy's limit arbitration lists its client requests (0xF7 laptop budget, 0xF8 ours,
  0xFC/0xFE unlimited); its output equals NVML's enforced limit, the lowest request.
- Raised maximums come from the registry BIOS override and apply at the next GPU restart.
- 11.053 A is the BIOS default = maximum, shared by several RTX 50 laptops; 210 W / 19 V fits
  (as does the 5060's 7.263 A = 138 W / 19 V), but the origin is unconfirmed.

## 2. How the driver turns limits into clocks

Power-policy STATUS publishes new values every **20 ms** (3,199 back-to-back reads). Each entry
has a measured value (`valueCurr`) and active limit (`limitCurr`).

**Controllers** are status type 0x15. Each predicts its inputs at a candidate clock and sets a
clock cap where the prediction reaches its limit; the arbiter applies the lowest cap.

- **Current controller (policy 10):** observes NVVDD and MSVDD (its total = their sum), predicts
  each at its cap and lowers the cap until neither prediction exceeds its rail's limit. The
  prediction is the reading scaled to the cap's clock and voltage (47.8 A at 2386 MHz / 970 mV →
  59.3 A at 2595 MHz / 1075 mV).
- **Power controller (policy 0):** observes two power channels (the core rails, e.g. 85.6 +
  47.5 W) and predicts them at its cap. Its limit = TGP policy limitCurr − rest-of-board power
  (policy 1, = TGP minus the two channels; exact in every sample).
- **TGP policy limitCurr** is an allowance with a ceiling of 1.2 × enforced (234 W at 195 W), pulled
  down continuously while power limits (195-222 W at a 194 W draw). Its integral fields are not
  decoded. The app no longer reads it.
- Policies 15 and 19 are controllers with no limit, capped at 3045 MHz (purpose unknown). The
  clock ceiling was 3090 MHz.

**Calculated currents.** Channel 27/28 current = channel power / a fixed voltage: 0.630 V NVVDD and
0.740-0.745 V MSVDD from idle to 195 W, while real voltage was 0.69-0.89 V and 0.76-1.0 V. Under
gaming load the readings are ~40% / 35% high. Locking clocks (`nvidia-smi --lock-gpu-clocks=2580,2880`)
switches the divisor to the VF voltage (0.845 / 0.935 V), so the same power reads ~23% fewer amps
and the current cap rose from ~2865 to ~3037 MHz with no change in power.

**Board input** is a HW_THRESHOLD policy (type 4) on input channel 8, outside the current
controller, with no prediction. Nothing the driver reports names it while it limits: the arbiter
says PWR_POLICY_GPC, NVML says SW power cap, its violation counter (+0x968) and NVML's board-limit
timers stay 0. Its only visible sign is the IPC counters.

**IPC activity** comes from thermal monitors of class 5 (ADC_IPC). ThermMonitorsGetInfo
0xB2C9D666 (size 0x94C, v1; utility clock kHz at +8, class at 76 + m × 72); GetStatus 0x4B4BD039
(size 0xA48, v1; class at 72 + m × 80, u64 count at 80 + m × 80). Here: 9 monitors, classes
7, 8, 5, 4, 4, 5, 5, 5, 3 (3 EDPP_VMIN, 4 EDPP_FONLY, 5 ADC_IPC; 7 and 8 unnamed). Only monitors 2
and 5 count under load, by identical amounts. Count rate / 108 MHz utility clock is read as the
share of time the IPC acts (an interpretation). The effective clock matched NVML's within 10 MHz,
so the IPC lowers the real clock (probably via voltage; not confirmed).

**Policy map on this GPU** (indices differ per GPU; the app discovers them by type):

| Index | Type | Unit / channel | Role |
|---|---|---|---|
| 0 | 0x15 | mW, ch5 | Power controller |
| 1 | 0x03 | mW, ch6 | Rest-of-board power (inactive limit) |
| 2 | 0x00 | mW, ch0 | GPU power (TGP); INFO 5 / 95 / 250 W |
| 3, 5 / 4, 6 | 0x12, 0x13 | mW, ch4 / ch1 | Core power channels A / B (inactive limits) |
| 9 | 0x04 | mA, ch8 | Board input current limit |
| 10 | 0x15 | mA, ch26 | Current controller |
| 11, 12 | 0x12 | mA, ch27 / ch28 | NVVDD / MSVDD current (inactive limits) |
| 13, 14 | 0x13 | mA, ch27 / ch28 | NVVDD / MSVDD current limits (the sliders) |
| 15, 19 | 0x15 | mW, ch5 | Unknown controllers (no limit) |

"Inactive" = 1001 W or 5001 W / 5001 A. Types 0x12, 0x13 and 0x15 are not in the public header;
their layouts were mapped from live dumps.

## 3. Key measurements

**Run 3** (2026-10-30, in a game standing still, 195 W enforced, NVVDD 210 A, fan boost on;
45 s per step via `mcctl`). Gap = lowest cap − NVML clock.

| Step | MSVDD | Board input | Power | Clock | Lowest cap | Gap | IPC | Board input avg |
|---|---|---|---|---|---|---|---|---|
| 0 | 60 A | 11.053 A | 165.6 W | 2580 | 2677 (current) | 97 | 47% | 73% |
| 1 | 66 A | 11.053 A | 182.6 W | 2677 | 2805 (current) | 128 | 66% | 81% |
| 2 | 66 A | 15.474 A | 193.9 W | 2737 | 2775 (power) | 38 | 36% | 62% |
| 3 | 66 A | 13.0 A | 190.8 W | 2722 | 2797 (current) | 75 | 47% | 72% |
| 5 | 60 A | 15.474 A | 175.2 W | 2640 | 2677 (current) | 37 | 25% | 55% |
| 6 | 72 A | 15.474 A | 193.8 W | 2722 | 2775 (power) | 53 | 36% | 62% |

(Steps 4 and 7 repeated 1 and 0 within a few MHz.)

- MSVDD set the cap (60 → 66 A raised it 128 MHz) while board input pulled the clock below it
  (raising board input alone shrank the gap 128 → 75 → 38 MHz). Both mattered at once.
- IPC activity tracks board input draw as a share of its limit, whatever MSVDD is: 55/25, 62/36,
  72-73/47, 81/66 (% of limit / IPC %).
- With board input 15.474 A and MSVDD ≥ 66 A the GPU reached 195 W (power lowest); 72 A added nothing.
- A repeat with synthetic mouse input (run 5) reproduced the MSVDD 60 ↔ 66 A effect (±90 MHz,
  ±16 W) twice, ruling out input-idle behavior.

**Other points**
- Power-limited at 120 W (suite1): IPC ~3%, gap 15-37 MHz. Board input lowered to limit at the same
  power: IPC 93.5%, gap 473 MHz.
- Board input 7.249 A at 175 W enforced: average 85% of the limit, IPC 86%, clock ~450 MHz below
  both caps.
- Board input maxed (15.474 A): IPC 6.5% at 120 W rising to 37-48% at 187-195 W — power-peak
  clipping without board input.
- MSVDD 30 A: unstable (cap swung 495-1900 MHz, MSVDD overshot to 47 A). The cap also moved up to
  ~140 MHz between samples at 60 A with predicted MSVDD pinned at ~60 A.
- Under load, (TGP − ~7 W) / board input ≈ 19.6 V, so 11.053 A averaged ≈ 224 W; it still limits
  from ~165 W because it acts on peaks (GPU ADC provider 0 peaks reached 12.0 A at the 11.053 A limit).

## 4. What can and can't be changed

- **Can:** the limits themselves (power via F8 and EC TGP/DB → F7; the three currents).
- **Can, not implemented:** the VF curve (undervolting lowers predicted current at a clock, so the
  cap lands higher), as Afterburner/mVolt do.
- **Can only lower clocks:** clock locks and maximum clocks.
- **Can't** through these interfaces: write the caps (recomputed every 20 ms) or switch a
  controller off.

## 5. Open questions

1. What the IPC compares against the board input limit (sensor, sample rate, filtering), and
   whether monitors 2 and 5 count anything besides board input and power peaks.
2. What core channels A and B are exactly (B ≈ MSVDD × voltage; A doesn't match NVVDD × voltage).
3. The TGP allowance's integral fields and time constant.
4. Why board input's violation counter (+0x968) stays 0 while it limits.
5. The rails' voltage-regulator overcurrent limits; the 140% unlock is a UI policy, not a rating.
6. Names of policy types 0x12/0x13/0x15 and the purpose of controllers 15 and 19.
7. Other limiters: XBAR's maximum alternates with RELIABILITY_MSVDD_0 (1025 mV) at MSVDD ≥ 66 A;
   the perf-policy Thrm reason is active 4-9% at 61-63 °C (cause unknown); thermal monitor
   classes 7 and 8.
8. Other GPUs and drivers: offsets are from 617.14. mVolt reports raised board input limits not
   applying on RTX 50 with driver 621.01+. Desktop boards use different board-input policies
   (0x17-0x19); RTX 40 laptops use a BIOS table the app doesn't parse.

## Evidence

- `C:\Temp\tandem\limit-probe\` (run 1, 20 ms cadence checks), `telemetry-live\` (production read
  code, run 2), `perfcap-probe\`, `bic-probe\` (idle board input RE).
- `C:\Temp\tandem\limiter-log\`: logger (`LimiterLog.exe log <maxSeconds> <intervalMs> <outDir>`),
  step script `steps.ps1`; `run2\` = run 3, `run3\` = MSVDD 30/45 A, `run4\` = run 5, `suite1\`.
- Field names: `C:\Temp\tandem\nvapi-research.h` (`NV_GPU_POWER_POLICY_STATUS_PARAMS_V10`).
