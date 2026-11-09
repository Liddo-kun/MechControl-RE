# Liquid cooler connection and EC power policy

2026-11-06. AMD static analysis and Intel IDY_7Y measurements. Live comparison
uses a scratch-only CLI build with bit-1 read-modify-write through WriteGuard;
the installed application and firmware are not replaced.

## Connection signal

**0x7C7 bit 1 (mask 0x02)** is the OEM host-reported cooler connection flag.
The recovered Eluktronics 4.23.47.11 service sets it on BLE Connected and clears
it on its default/disconnected path, preserving the other bits. Intel ACPI names
it `LCSE`; bits 2-4 are `OCPL`, a separate OC/power level. Details and source
references: [`register_reference.md`, section 11.9.1](../register_reference.md#1191-liquid-cooler-connection-signal-0x7c7-bit-1).

Read-only sample on `YAOSHI Series-X6AR55xY`, BIOS `N.1.13MRO16`:
0x7A4=0x80 (support), 0x7C7=0x0C (LCSE clear, OCPL=3), 0x7E6=0xA8,
0x7E7=0x87. Native initialization writes 0x7C7=0x0C; the BLE implementation
does not set LCSE. The host notification itself is an EC write and does not
require Bluetooth. A manual notification feature would need explicit user intent.

### Why MechControl writes 0x0C

`src/controller.cpp:186` unconditionally writes the whole byte during EC
initialization. This was inherited from `v1.6.0:MechControl/Features/EcActivationControl.cs`
(`AddrFanCurveParam`, write at lines 153-154). The original local OEM capture
`re/captures/frida_startup_log.txt:336,342` reads 0x0C and writes 0x0C back.
That observation does **not** prove the OEM deliberately selects OCPL=3:
the recovered older service's connection setter clears only LCSE on disconnect,
which would also turn an existing 0x0C into 0x0C. The capture has no call stack
establishing that setter as its source. The constant startup write should not
be described as a deliberate maximum-power selection or required fan parameter.

## Exact Intel firmware: Jon's installed EC

Jon supplied `Intel-X6AR-EC.bin`, 262144 bytes, SHA-256
`2C90BAE23881906B838351DB6FE9EF0874A422847CBED356B73D38013E40DC13`.
Its version initializer contains 4D/00, matching the running EC. Fresh disassembly
and instruction execution establish these Intel-specific results:

- B2 A32E reduces adaptive PL1/PL2 caps by **10 per nominal second**. B2 AA98
  aggregates caps and AA37 publishes 46A/B. B0 F408/F422 submits those values to
  the CPU interface, encoded as watts times eight plus the enable bit. Rewriting
  783/784 does not reset the adaptive controller.
- Normal adapter classification comes from **49F bits 3-6**, not a direct D42
  read. Class 0B selects record 6EC0, **449 >210**, adaptive floors 55/55 and
  charger-command-3F word **0C40**. These differ from the AMD values below.
  Other/unrecognized classes select lower policies; the normal table has no
  700 W or arbitrary-watt entry. The threshold is a feedback setpoint.
- The overload condition also accepts its battery-discharge magnitude >=500.
  The **497-bit0-clear** measurement branch incorrectly tests bit7 of the low
  current byte 50A instead of the sign in 50B. Executing the original instructions
  with signed current +200 produces a discharge magnitude of 65336 and can trigger
  descent below the adapter-current threshold. Live idle497=45 selects the other
  branch; its selection must be logged before attributing a run to this bug.
- Seven explicit LCSE consumers affect fan policy, stock GPU tables, OCPL tables
  and an SMBus device at address byte40. The last path sends 0083 or00A0 to commands
  E5-E8. **The AMD LCSE battery-debounce/G1 policy is absent from this Intel image.**
  LCSE does not gate Intel's input-current controller or CPU record selection.
- **780=A2** skips normal CPU-cap aggregation and preserves CPU transactions,
  but also changes thermal-cap, fan and protection-output behavior. It publishes
  the custom requests (or781 when nonzero) once per24 nominal-one-second calls.
  Restoring780=0 restores normal aggregation on the next publication. Outside
  running state6E6=1 it clears780, sets two SFR bits and loops indefinitely.
  No A2 write had occurred when these static results were recorded.
- With 497 bit0 set (live 45), battery current is `(ADC2 - (512 + 399)) * 34` mA,
  ADC2 = 604:605 and live 399 = 50. Raw ADC2 below 547 (>=500 mA discharge) meets
  the overload condition. B2 C138-C17F stores `floor(ADC6*10/17) & 255` in 449 and
  DA6E compares that byte with 210, so ADC6 435 gives 255 and 436 gives 0: heavy
  adapter current can wrap and hide the adapter-current trigger. 448 = 200 is the
  nominal 20.0 V conversion factor, not a power limit.
- The host read path (ACPI ECRR, `FE410000+address`) only reaches 000-7FF and
  C00-FFF (B0 EE32-EE67). 800-BFF, including the charger shadows 830-83E, reads FF.
- 3B5 counts estimated discharge mAh (B1 CEC2, 200 ms integration, divide by
  18000). On AC, B1 C8DF-C8F0 blocks the normal 61 mAh SOC step, and 432/434
  normally stay 0 while 490&7 = 7. A separate B1 B590-B62B correction forces SOC
  100 -> 99 on AC after battery voltage stays below a cell-count threshold for ten
  nominal seconds: 16000 mV for four cells (live 491 = C0, 3A2 bit3 set, 492 = 1).

CPU-only validation held about192 W for40 s while46A/B remained210/210, even at
the configured90 C temperature target. Combined-load descent is therefore not a
CPU-only elapsed-time policy or failure to generate CPU load.

Exact offsets, all cap inputs, classification cases, LCSE callers, executable
verification and emulation are in `C:/Temp/tandem/intel-x6ar-ec-investigation/`:
`findings.md`, `limiter.md`, `adapter-and-lcse.md`, `bypass-and-tests.md` and
`emulation.json`. The dump is local evidence, not a firmware file to publish.

## AMD firmware evidence

Input: `XxFR5xxx_Pro_14.810` from `LP 3123079 BIOS (1).zip`, 262144 bytes,
SHA-256 `99A329D6CDE5DC17A0FB833C7B8DD640F9EC68DB2B5F47545FBE93D208C7E735`.
Archive entry and existing analysis image hashes match. This is **AMD X6FR**,
not the Intel firmware. B2 CODE addresses map to file offset +0x10000;
B1 CODE addresses map to file offset +0x8000. These are instruction locations,
not host-write addresses.

The recurring cooler gate checks 0x7A4 bit 7, **0x7C7 bit 1**, and 0x7C8 bit 5
(helpers B2 E702, E762, E7F8). B2 9176-926B initializes the last flag alongside
factory fan-table metadata. No host write to that additional flag is established.

| Path | Verified effect | CODE locations |
| --- | --- | --- |
| CPU presets | With additional mode/activation gates, selects the fifth pointer in the preset group. All eight groups at ROM 767A-76C9 point to record **73D7**, whose first three raw bytes are **162/162/195**, versus **145/145/174** in ordinary high record 726F. PL4 encoding must be handled separately. | B2 A210-A278, helper ED73; record consumption A460-A479 |
| Custom CPU limits | Nonzero 783/784/785 overrides replace preset limits when activation/CUME conditions pass. Higher preset numbers alone do not explain an effect with custom limits already applied. | B2 A54F-A59F |
| Additional CPU channels | Cooler record raises two separate raw maxima 125 ->140 and 176 ->200. Custom 783/784/785 does not replace these. They reach CPU-interface commands 35/36 with payload multiplied by 1000; physical names/units remain unverified. They are not direct 46A/B clamps. | Record offsets 03/04; B2 A0B0 decoder; B0 D20A-D24C, D434 |
| GPU policy | On the firmware-table branch, selects uploaded XDATA RAM table base 91C0 instead of 9190. The activation+CUME custom branch bypasses this selection. The matching BIOS uploader's 1A/1B payloads are decoded below. | B2 A839-A883, A88E-A8E0 |
| Internal fans | Selects alternate factory curve pointers. Custom RAM-curve handling follows separately. | B2 9707-97B8, 97CA onward |
| Battery/protection | Freezes battery-policy latches, selects released/input GPIO G1 on the ordinary AC path, and prevents selective removal of B2/J1 output-triggering reasons. These effects survive custom controls. Pin direction/data semantics are verified; motherboard net names are not. | B2 D0CC-D163, 9C49-9CD6, 9D2E-9D43, 9DCC-9DE1 |

The CPU cooler-selection branch is incompletely represented in the existing
decompiler output; use the instruction listing. The listing's instruction bytes
were checked against all three bank images (18674/20098/20282 matches).

Full 45-byte record decoding also confirms **identical primary adaptive floors
25/25/54**, separate-channel floors 60/90, and qualification counts 10/30 in
ordinary 726F and cooler 73D7. Nonzero custom PL1/PL2 replace both primary requests
and their adaptive ceilings, but not floors or the additional CPU channels above.
Neither record contains a 100/120/125 adaptive floor. Intermediate live plateaus
must not be presented as terminal floors. Decoder and CPU-command evidence:
`C:/Temp/tandem/amd-sustained-power-investigation/cpu-record-decoder.md`.

## OCPL is a thermal state in this AMD firmware

Instruction-level analysis of B2 **8926-8A63**, including execution of the actual
producer instructions against synthetic RAM, resolves the field as follows:

| Level | Masked value | Firmware meaning | Direct cTGP adjustment |
| --- | --- | --- | --- |
| 0 | 00 | Producer eligibility gate inactive/reset | None located |
| 1 | 04 | Not generated by the normal table; special hold case recognizes an externally supplied value | None located |
| 2 | 08 | Hot-GPU state, entered at **87 C or above** | **-5 W offset**, firmware-table branch only |
| 3 | 0C | Normal state, with or without a cooler | None |
| 4-7 | 10-1C | Not generated by the table | None located |

The producer requires RAM 06E6=1, 0440 nonzero, and 0456 bit 7. Gate failure
clears only OCPL and its internal state. **It does not test LCSE**, activation,
CUME, or cooler support. The ROM table at 6A91 is:
up thresholds `87,255,...`, down thresholds `0,82,255,...`, levels `3,2,2,...`.

Recovery to level 3 requires GPU temperature **below 82 C** (81 C or lower)
and an expired hold. Each reported level change loads RAM 06DC with 60;
60 subsequent invocations decrement it, and the next can recover. This is
not 60 consecutive cold samples: the counter runs while hot too. An upward
87 C crossing overrides the normal hold. The special level-1 case inhibits
an upward row transition while the hold is at least 30.

The scheduler invokes this routine once per **200 serviced Timer0 ticks**:
0533 ISR -> 0DD1 scheduler -> 0E61 -> 0EAD -> stub 158E -> B2 8FC4/8FDF.
Timer reload is F106. ITE's IT5570 datasheet specifies a fixed 9.2 MHz internal
timer clock; with the /12 timer divisor this supports a nominal 5 ms base tick,
1 s invocation and approximately 60 s hold. Exact chip/clock applicability is
not a physical measurement. This is not a high-wattage elapsed-time timer.

The exact numerical consumer at B2 **A99B-A9AD** is:
`if OCPL == 2 && RAM[09EA] >= 5: RAM[09EA] -= 5`.
It acts on a freshly loaded table value, so is not cumulative. Values 0-4
remain unchanged. B2 **8766-877B** publishes 09EA to EC **0788/CTWA** and
queues query 83; the ACPI/NVIDIA path below establishes a **5 W cTGP-offset
reduction**, not an absolute GPU power cap. No directly OCPL-dependent CPU
PL1/PL2/PL4 adjustment was located.

**Custom-control bypass:** B2 **A839-A883** selects a separate branch when
activation (0741 bit 0) and CUME (0727 bit 6) are both set. It takes GPU values
from 0744/0745/0746 according to 0743 enable bits and jumps past the OCPL
subtraction. The branch itself bypasses the subtraction even if individual
GPU enable bits are clear. MechControl normally sets activation and CUME;
on this AMD image that path, not its startup write of OCPL=3, bypasses this
particular reduction.

Changes can emit application event C2 when activated, otherwise ACPI query 8A,
unless 0466 bit 4 suppresses notification. The producer compares against shadow
06DD, not the previous live register value, and preserves LCSE. It can overwrite
a host-written level without another notification if the result matches that
shadow. Thus writing 0C is not a persistent maximum-power mode selection.

The GPU policy tables are uploaded to RAM by mailbox commands 81-86 and 90-91
(B2 DF85-E107), not embedded at CODE addresses 9190/91C0. Each command copies
10 bytes from 0FD0. ROM bytes at those numerical addresses are not table data.

Full static evidence, recovered switch decoding and synthetic instruction
execution: `C:/Temp/tandem/amd-ocpl-investigation/evidence.md`, `raw-b2.txt`,
`emulate_ocpl.py`, `emulation.json`. These results are AMD-specific and do not
establish Intel thresholds or bypass behavior.

## AMD ACPI handoff

The same package's main BIOS has now been unpacked offline. Its EFI updater
contains an AMI UAF `@ROM` module at file offset A7040. EFI decompression
(`TianoCompress --uefi`, not the default Tiano variant) yields a 32 MiB image,
SHA-256 `EB3FA10FCF44CA41C39850F5949D452518CFBA9E2939E86E3B03B9378BEEA562`.
UEFIExtract A76 exposes checksum-valid ACPI tables; iASL 20251212 disassembles
the relevant tables successfully. No OEM executable was run.

The extracted AMD DSDT independently maps LCSE/OCPL at 0x7C7. Its `_Q8A`
calls `INOU.PWUP(OCPL)`, but **PWUP ignores its argument** and only notifies
NPCF (0xC0) and the NVIDIA device (0xC5). It contains no level-to-wattage table.
Actual power parameters are handed off separately:

| EC event | ACPI operation | NVIDIA dynamic-parameter output |
| --- | --- | --- |
| 0x83 | ACBT = 0x788 (`CTWA`) * 8 | TGPA: configurable-TGP offset |
| 0x84 | When DBEN: DBAC=0, AMAT = 0x7D5 (`DBAP`) * 8; otherwise DBAC=1 | Controller enable/disable and MAGA maximum offset |
| 0x9C | When CUME is clear: ATPP = 0x7F7 (`ETPP`) * 8 | TPPA: TPP offset |

These protocol fields use **1/8 W units**. Thus a five-count change in CTWA
means a 5 W change in the cTGP offset, not 40 W. NVIDIA source evidence and
correction of older interpretations: [`gpu_power_exploration.md`](gpu_power_exploration.md).

Scratch root: `C:/Temp/tandem/amd-ocpl-bios/`; reproducible unpacking in
`extract_bios.py`, ACPI carving/checksums in `find_acpi.py`. Evidence in
`acpi/DSDT_AMI_8aae6ca772.dsl:8649-8652,9461-9495,9525-9529,11851-11855`
and `acpi/SSDT_OemNv2T_767e99248a.dsl:2098-2154` (the alternate Opt2 table
has the same power-field handoff). These are firmware-package tables, not an
observation of which GPU SSDT an AMD machine has loaded at runtime.

### Matching BIOS GPU-table uploader

`OemHooks` DXE callback **RVA 1CA4** builds the actual upload payloads.
It reads configuration D41 (OEM), D60 (GPU class), D42 (adapter type), and
D45 bit 4 (cooler support). Helper **2EE0** copies ten bytes to EC FD0-FD9,
sets FDE=FC and FDF=`command | 80`, and waits for acknowledgement. Calls at
20B9-2113 upload air rows with commands 81-83, cooler rows with 84-86 when
support is enabled, and source-specific rows with 90-91.

For GPU configuration **1B**, the first seven bytes of each row are:

| Uploaded row | Air | Cooler |
| --- | --- | --- |
| 0 | 50,5,50,5,45,45,55 | 50,5,50,5,60,60,205 |
| 1 | 55,25,55,25,65,65,100 | 55,25,55,25,110,110,200 |
| 2 | 55,25,50,5,50,50,85 | 55,25,55,25,90,90,200 |

Last three bytes are zero. Configuration **1A** uses GPU-offset values 65/70
where 1B uses 50/55; the other fields match. Fields +0/+1 supply the cTGP/DB
working values; +6 supplies ETPP. An additional activated 72B-bit1 branch uses
+2/+3 for the GPU values and publishes +4/+5 to 763/764. B2 B580-B5A1 and
B642-B663 prove these are additional first/second CPU power caps, applied by
minimum arbitration when APPE, 72B bit 1, AC, and nonzero cap conditions hold.
For example, row 1 raises those caps from 65/65 to 110/110. The custom GPU
branch's EDDA helper clears both, so they do not add a cooler advantage there.
These are inputs to the policy, not a promise of actual measured power, and the
APPE+CUME custom GPU branch bypasses this entire table selection.

Evidence: `C:/Temp/tandem/amd-bios-power/OemHooks.efi.asm`, `callbacks.c`,
`gpu-upload-tables.json`. `decode_gpu_tables.py` verifies all 2227 instruction
byte sequences against the extracted PE and reconstructs the 1A/1B payloads
from the immediate stack stores at 1DE8-1EAE.

The same package's `OemServiceSmm` adapter-type table at RVA DAB0 maps **0B
to 420 W**, 00 to 330 W, 08 to 280 W, 0A to 250 W, and 0C to 240 W.
`OemAdpTypeDxe` copies D42 into 49F bits 3-6. Jon's read-only Intel sample
has D42=0B and 49F=5A, consistent with that encoding, but this AMD table alone
is not independent proof of the Intel adapter-type enum or physical PSU rating.

### Adapter selector feasibility

The examined AMD BIOS has thirteen named adapter types, topping out at **420 W
(0B)**. There is no 700 W entry. Unknown values fall back to internal class 21
and a nominal 220 W boundary, rather than disabling the limiter. The normal-source
420 W class selects threshold 215 and charger-command-3F payload 0D00; the exact
charger IC/current scaling remains unidentified.

The OEM `AdpType /Set` implementation is a **persistent platform configuration**
operation: SMM validates the enum, writes D42, then writes DFF=79. An EC state
transition changes the marker to A5 and commits D00-DFF to EC flash at 01F800.
When DFF=A5, D42 takes precedence over the boot-time mirror in 49F bits 3-6.
Working adapter classes and thresholds are periodically recalculated. This is
not equivalent to changing a live current-limit byte. No OEM utility was executed.

A larger physical PSU need not change that classification. Above-threshold power
is possible because the adaptive loop reduces CPU limits to floors; it cannot
necessarily bring GPU-heavy total load below the boundary. Separately, the ADC
conversion stores only the low byte of current: on the 10/17 scaling branch,
ADC6 >=436 wraps 449 through zero (approximately 512 W-equivalent). Log raw ADC6
as well as 449/700:701 when investigating high power; a low derived reading can
be overflow rather than reduced load.

The Intel test machine reports D42=0B and DFF=A5. AMD-private working locations
08B8, 08E9 and 083A/B read FF through its current host interface and cannot verify
the AMD internal classification/charger values on Intel. A selector needs
model-specific validation; adding an invented 700 W code would not solve this.
Full mapping, setter instructions and persistence evidence:
`C:/Temp/tandem/amd-sustained-power-investigation/adapter-classification.md`.

## Battery/protection effects that survive custom controls

All eleven explicit LCSE-helper callers were audited. The following paths have
**no APPE/CUME bypass**:

- **Battery latches:** full cooler gate skips updates of 94E9/94EA, preserving
  their current values. Without that gate, the low-load qualification requires
  battery class 41/42, filtered power below 200, SOC >=31%, and voltage >15400 mV.
  It takes up to ten healthy samples to qualify; the first failing sample leaves
  that state. This is not a multi-minute high-power entry timer.
- **GPIO G1:** on the ordinary AC-source branch, the cooler gate releases G1 to
  input instead of allowing the low-load latch to drive it low. Source-state
  overrides and a separate fast override can still drive it low.
- **GPIO B2/J1 decisions:** with LCSE clear, a qualified low-load state masks
  reason fields (`0985 &=41`, `9123 &=46`). LCSE set prevents those masks.
  It can therefore retain additional output-triggering reasons, not simply
  disable protection. B2 is driven low when its remaining reasons are nonzero;
  J1 uses a two-high/two-low cycle when its reasons are nonzero.

ITE register definitions and the primary IT5570 datasheet establish 1641 as
GPCRG1 (40=output, 80=input), 1607 as GPDRG, 1602 as GPDRB, and 160A as GPDRJ.
The board firmware initializes these pins as outputs. **The motherboard net
names remain unknown**: these writes must not be labeled charger-current
settings, battery-boost enable, or PROCHOT without additional evidence.

## Sustained high-power behavior: threshold found, long delay unresolved

The 420 W adapter configuration (D42=0B) selects internal adapter class 42,
which sets current threshold 08E9=215 (B2 A189). The independent adaptive loop
tests **0449 >215**. It does not depend on LCSE and continues after custom CPU
requests are read.

On AC, B2 CEBD-D060 sets 0448=200, computes 0449 from ADC6, then computes
power as `0448*0449/100` and filters it into big-endian 700:701. Under its nominal
20 V/deciamp convention, the threshold is about **430 W**, with the first discrete
over-threshold value corresponding to **432 W**. Recovery uses lower boundaries
around 410/400. This is a concrete lead for the reported roughly 420 W region,
not proof of the same threshold in Intel or an externally calibrated wattage.

The loop can step CPU limits down/up by five after its over-budget/recovery
counts. Ordinary CPU record 726F and cooler record 73D7 both use **10/30**
slow-count thresholds, with a divide-by-ten counter. The recovered scheduler
invokes adaptation every two base ticks: nominally **~1 s over-budget / ~3 s
recovery steps**, not 2-3 minutes. LCSE neither raises the adapter-current
threshold nor disables this comparison. The charger/source configuration writer
also contains no LCSE read; its exact charger IC remains unidentified.

The screenshots' fixed 2-3-minute onset is therefore still consistent with an
Intel-specific longer policy, charger/adapter peak-to-sustained behavior, thermal
behavior, or delayed visible battery reporting. The AMD threshold match does
not resolve the timing mismatch. Correct cooler notification should be tested
as OEM parity, not presented as a proved 420 W bypass.

Read-only Intel baseline: 600 samples over five minutes, no AC loss, no battery
discharge, no sustained over-threshold interval (only five samples at 449>215).
The measured ratios corroborate ADC6*10/17 ->449 and approximately2*449 ->700:701.
**60C:60D is ADC6 input in this firmware, not direct watts**; the native monitor's
current `system_watts` interpretation needs a separate correction. The baseline
validates useful telemetry relationships but does not reproduce the fault.

### Live Intel load comparison

User confirmed a running liquid cooler and 420 W adapter. The GPU is shunt-modded:
175 W raw NVIDIA telemetry represents approximately 232 W actual. Afterburner's
existing GPU monitoring formula is `x*1.34`; its shared-memory GPU power is already
corrected and must not be multiplied again. CPU power was logged from Afterburner
shared memory without adding another hardware-monitoring driver.

Tools: hash-verified portable FurMark 2.11.0.4 (OpenGL, 1920x1080, vsync off) and
Prime95 30.19b20. The first 128K-FFT run showed 46A/B decreasing 195 ->115 in
ten-unit steps, starting 4.24 s after CPU load and reaching 115 at 20.24 s, while
requested 783/784 remained 195. After removing load, 46A/B recovered in ten-unit
steps roughly every three seconds. Sampled CPU temperature peaked at 88 C versus
a requested 90 C temperature limit. No GPU failsafe trip occurred.

Subsequent comparison used 16K in-place FFTs, CPU requests 210/210 W, unchanged
GPU request 151+25 W reported, and fan boost. A scratch-only CLI command required
the cooler support flag, called `WriteGuard::bitfield(7C7,02,...)`, and explicitly
checked readback. Its seven synthetic checks passed, including bit preservation
and rejected/ignored writes. The installed runtime was restored after each test.

The final runs started CPU and GPU together and stopped load after about 40 s:

| Run | 7C7 throughout load | Initial CPU peak | Last-10-second median CPU power | Median 46A/B in that interval |
| --- | --- | --- | --- | --- |
| Cooler on, first | 0E | 200.8 W | 125.1 W | 120 |
| Cooler off | 0C | 198.9 W | 100.2 W | 100 |
| Cooler on, repeat | 0E | 200.0 W | 111.6 W | 110 |

All three retained requests 210/210 and support/gate bytes 7A4=80, 7C8=60.
Corrected GPU power was about 234 W. Reductions began around 5-5.5 s; measured
CPU power followed the descending 46A/B values. No AC loss or thermal-failsafe
trip was recorded; battery current was not logged in these runs. A preceding longer bit-off run settled
around 111.6 W at 46A/B=110, so the first apparent 25 W on/off advantage is **not
a demonstrated fixed cooler allowance**. The positive result is that LCSE does
not prevent this substantial ramp-down on the tested Intel firmware/settings.

The data strongly supports an active adaptive limit rather than insufficient
CPU workload. It does not identify every Intel limiter variable or reproduce
the separate reported 2-3-minute battery-draw onset. No adapter classification,
charger register or GPIO was written. Loads are stopped; original CPU/GPU/fan
settings and LCSE-clear state were restored. Captures, exact commands and
`short-comparison.json`: `C:/Temp/tandem/cooler-load-tests/`.

### Staged GPU increase and additional control tests

Further tests used 40 s of combined load: start at GPU 150+25 W and CPU requests
210/210 W; after 20 s, request CPU 110/110 W before raising GPU to 170+25 W.
This holds CPU demand down during the GPU change. An independent process stops
the two workload PIDs after 42 s even if a CLI call stalls. CLI calls also have
timeouts. Settings are restored after load stops.

| Completed test | Outcome |
| --- | --- |
| Staged GPU increase, LCSE=0E | GPU about 259-260 W corrected; CPU cap fell from the 110 W request to 80, measured CPU about 80 W in the final five seconds. |
| Same increase, LCSE=0C | GPU about 259 W corrected; CPU cap fell to 90, measured CPU about 90 W. |
| Reapply CPU 210/210 at 20 s, GPU 150+25 | Reapplication did not reset the reduction; cap eventually reached 110, measured CPU about 112 W. |
| Disable Dynamic Boost, request GPU 175 W at 20 s | Driver enforced 150 W; corrected GPU fell to about 200 W. CPU cap remained 120. This was not a matched-actual-power comparison. |
| Disable Dynamic Boost, request GPU 176 W at 20 s | Existing above-stock budget path restored roughly 234 W corrected GPU power; CPU cap remained 110, measured CPU about 115 W. Disabling Boost did not remove the CPU reduction. |

No completed run recorded AC loss. At the higher GPU demand,
both cooler states still reduce CPU power below the explicit 110 W request. The
observed one-step difference does not establish a fixed advantage for either state.

The 175/176 W discontinuity was traced to `nv::Power::apply` using the original
VBIOS maximum instead of the live F7 budget. Read-only probing showed DB-off
reducing F7 to150000 while requested power remained175000 and platform budget
remained350000. A176000 request took the old raise path and setF7/platform to
176000/376000.

The source fix now qualifies and maintains the live F7 budget for unlocked power
requests. Mode changes release owned platform headroom and let EC/ACPI publish
the new mode before the delayed GPU check recalculates a needed override.
`gpu-budget-probe-02` verified175000/375000 and actual175 W enforcement with Boost
off, then exact restoration of the original175000/350000 after Boost was restored.
Repository and scratch Release builds passed all seven CTest fixtures; a new
controller case guards preflight-before-write for a stock-ceiling DB-off request.
The installed executable was restored after the temporary live check.

The first staged run (`gpu-step-150-to-170-01`) was interrupted by Jon's manual
reboot. It is not evidence of a power-brick trip. Its incomplete CSV has a trailing
zero-filled region. The next run completed, but Afterburner had not restarted, so
its CPU power channel is absent; the matched LCSE runs above include that channel.
One later attempt stopped before its second stage because CPU telemetry was absent;
subsequent scripts validate the monitor before starting load.

All completed traces, phase events and analysis are under
`C:/Temp/tandem/cooler-load-tests/`, especially `test-cooler-on-gpu-step-01`,
`test-cooler-off-gpu-step-01`, `cpu-reapply-210-01`,
`dynamic-boost-off-matched-175-01`, and `dynamic-boost-off-above-stock-176-02`.

For a future reproduction of battery draw, capture the actual offending workload
and independent input-power measurement. Stop once the relevant transition is
captured. AC loss indicates an input dropout; stable AC with battery discharge
requires examining power sharing. The fast CPU ramp-down and the reported
multi-minute battery event must not be conflated. Replace the whole-byte 0C
initialization when implementing connection state, but do not advertise LCSE as
an input-limit bypass.

Full instruction evidence, GPIO sources, timer derivation, all LCSE callers and
decision tables: `C:/Temp/tandem/amd-sustained-power-investigation/evidence.md`.
Read-only logger/source/trace: `C:/Temp/tandem/cooler-ec-investigation/`.

## Implementation status

Both features now ship, gated per model/EC version. The **LCSE connection flag**
is saved per profile slot (`liquidCoolerConnected`): EC init writes 0x7C7=0x0C,
slot loads and runtime changes are bit-1-only WriteGuard writes with readback, and
it is reapplied after resume and EC recovery. The **0x780=0xA2 bypass** is enabled for
`IDY_7Y.json` (Intel EC 77.0) and `X6FR5xxY.json` (AMD EC 112.0), whose exact EC
images were analysed and whose version-byte loaders (B1 851E Intel, B1 10611 AMD)
were confirmed. At Jon's request `X6FR5xxY.json` also allows AMD EC 113.0-117.0,
which have not been analysed; a 117.0 image is to be checked against 112.0. It is never saved and is cleared on every power transition, a
thermal trip, EC reinit, exit and leftover-at-startup. Other models stay disabled
until their EC image is checked the same way; a bare sanity read of 0x780 cannot
establish the semantics on an unanalysed firmware. The value remains advertised as
OEM-parity benchmark behaviour, not a proven input-current-limit bypass.

## Lifecycle and shared state

B1 F665-F8D5 generates 7E6/7E7 fan/pump requests without requiring LCSE.
Nonzero values there do not prove connection. B1 F8D6-F8F6 clears LCSE and these
requests in reset/power-transition paths. AMD B2 8926-8A63 also updates OCPL
while preserving other bits: the byte is not exclusively host-owned.

Scratch evidence: `C:/Temp/tandem/ecbios/`, instruction listings and verification
tool in `C:/Temp/tandem/lp3123079-review/`, read-only probe/source/sample in
`C:/Temp/tandem/cooler-ec-investigation/`. OEM decryption provenance is in
[`ec_0x741_bits.md`](ec_0x741_bits.md).
