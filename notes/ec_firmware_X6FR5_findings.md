# EC firmware static analysis: X6FR5 (AMD) — `XxFR5xxx_Pro_14.810`

2026-10-29. Source: PCSpecialist BIOS package "LP 3123079 BIOS" (`flash.nsh` sets
SMBIOS product/baseboard `X6FR559Y`, maker `PCSpecialist`, board maker `AiStone`).
Package contents: AMI BIOS capsule `X6FR55xxN139PCS10_CAP.efi` (high-entropy
throughout, not unpacked) and the 256 KB ITE EC image `XxFR5xxx_Pro_14.810`
(`ifux64 ... 0 1` flashes it). Header string `ITE EC-V14.6`.

This is the AMD sibling that collided with IDY_7Y (`other_machines.md` §15). It is
**not** Jon's IDY_7Y firmware. Treat everything below as firmware-specific until a
read-only check confirms it on another machine.

## Image layout

- `0x00000-0x07FFF` common code, `0x08000-0x1FFFF` three 32 KB code banks (Keil
  BL51 banking; trampolines `MOV DPTR,#target; LJMP 0x1100/0x1114/0x1128`).
- `0x20000+` is an ITE8850 USB-PD controller firmware (three copies), not relevant.
- EC addresses used by MechControl are XRAM addresses one-to-one (`MOV DPTR,#0x0741`).
  Fan table `0xF00-0xF5F` is reached through generic pointers (`R3:R2:R1 = 01:0F:00`).

## Findings

### 1. 0x740 is a hardcoded constant: `0x1A`
Board init (`bank0:e36b`) does `0x740 = 0x1A` unconditionally (bytes `90 07 40 74 1A F0`
at file `0xE45A`). That is the same value IDY_7Y reports (26 >> 1 = 13). The project
ID names the shared Uniwill platform, not the Intel/AMD SKU, so the IDY_7Y collision
is built into the firmware and cannot be fixed by better 0x740 matching.

Bytes that do differ by SKU in this firmware:
- `0x456` bit 7 = dGPU present (set when GPU module ID `0x7D2` != 0, after an SMBus probe).
- `0x7D2` GPU module ID; `0x16-0x18` and `0x19-0x1B` select different power/thermal tables.
- `0x771/0x772` ROM ID → derived board config at `0x73C`.
- `0x74C` (bit 6 valid, bits 0-5 ID), `0x773`, `0xD41`: OEM/customer sub-ID. The firmware
  branches on these in many places (IDs 0x04-0x20), e.g. to select default temp tables.
  `0x773`/`0xD41` are not written by EC code, so presumably the BIOS writes them.

### 2. CPU temp limit (0x786 → 0x463) on AMD: absolute °C, no EC clamp
`bank1:f1cd`, confirmed in disassembly at `f4b8-f4e3`:
```
target = per-SKU default from a code table (0x61 = 97 C seen)
if (0x786 & 0x80) && (0x741 & 0x01):  target = 0x786 & 0x7F
if target != last:  0x463 = target | 0x80
```
- The low 7 bits are used as-is as an absolute temperature. **No range check.**
- Explains the earlier 600 MHz lockup: Intel encoding `0x8A` (95 C as offset 10) became
  a 10 C limit; `0x463` then read `0x8A` = `10 | 0x80`, exactly the logged
  `verify 0x463 = 138`.
- `0x463` bit 7 = "new value", set by the EC on every change and cleared later
  (`bank2:b347`, when `0x440` != 0). Masking bit 7 on verify (current WriteGuard) is right.
- Bit 7 of 0x786 is ignored unless 0x741 bit 0 is set.

### 3. PL1/PL2/PL4 (0x783-0x785) acceptance
`bank2:a0b0`, `bank2:c15a`:
- Copied into the EC's working set only when `0x741 bit 0` **and** `0x727 bit 6` (CUME) are
  set, and only when non-zero (a 0 byte means "keep the previous value").
- PL4 doubled when `0x727 bit 7` (PL4D) is set. Matches `HardwareDetection.Pl4Doubled`.
- No upper clamp against ECSpec. Battery-SOC-based reductions are applied on top.
- When `0x741 bit 0` is clear the EC **zeroes 0x783, 0x784, 0x785 and 0x78B** (bytes at
  file `0x1C1A3`). That explains "PL1/PL2/PL4 all 0x00 at startup" on the X6FR5 log, and
  makes PL == 0 a reliable "activation lost" signal on this firmware.

### 4. GPU (0x743-0x746)
`0x743 bit 2` → apply TGP offset `0x744`; `0x743 bit 0` → apply `0x745/0x746`, with DB enable
= bit 1. Matches register_reference §3. Same `0x741`/CUME gating as the PLs.

### 5. Custom fan table preconditions (confirmed, `bank2:97c3-97f5`)
The EC uses `0xF00-0xF5F` only when **all** of these hold, otherwise it silently uses its
built-in curve:
1. `0x741 bit 0` (activation)
2. `0x7C6 bit 2`
3. `0x727 bit 6` (CUME)
4. `0xF20 == 0` **and** `0xF50 == 0` (duty[0] of the CPU and GPU tables)

Rule 4 is the reason for the empirical "duty[0] must be 0x00" rule in register_reference §4:
it is a validity marker, not a curve value. If `0x456 bit 7` (dGPU) is clear, the second
fan follows the CPU table instead of `0xF30`.

### 6. RamFan1p5 "status" bytes are a host command to load a factory curve
`bank2:967b-9700`: if `0xF5D == 0xFD`, `0xF5E == 0xC9` and `0xF5F` in 1..7, the EC copies a
built-in preset (index `0xF5F`) into `0xF00-0xF2F` and `0xF30-0xF5F`. So the "status"
registers are really a signature + preset index written by the host. Normal padding
duties (≤ 200) can never form this signature, but nothing should write `0xFD/0xC9` there.
This could let MechControl restore the firmware's own default curve on unknown models
instead of hardcoding one (preset meaning unknown; needs testing).

### 7. Fan boost (0x751 bit 6) is cleared by the EC when the system is cool
`bank2:95c5-95fe`: every 60 ticks of counter `0x85F` (about 1 s each), if boost is on, and
the 16-bit power reading at `0x702:0x703` < 80 **and** `0x43E` (CPU) < 70 C **and** `0x44F`
(GPU) < 70 C, the EC clears `0x751 bit 6` and raises event `0xAC`. `0x768 bit 2` follows
boost state. MechControl's boost toggle and the failsafe's fan action can therefore
turn themselves off after the machine cools.

### 8. Fan stall alarm = 0x741 bit 5
`bank2:93aa`: at the end of a check window (`0x84C` countdown) the EC sets `0x741 bit 5` if
fan 1 RPM (`0x464/0x465`) or fan 2 RPM (`0x46C/0x46D`) reads 0. Cleared while the system
is off. Agrees with the OEM `GetECFanAlert()` (bit 5) in `ec_0x741_bits.md`.

The window counter `0x84C` is only decremented in decompiled code; nothing visible sets
it, so the check probably runs once from an init table (a boot-time spin check) rather
than continuously. Treat bit 5 as a latched diagnostic, not a live stall detector.

`0x741 bit 7` is set by `bank0:ca07` when the 253-byte buffer at `0xB00` contains both
`0x14` and `0x32`; cleared on reset paths. The second review (see below) identified the
buffer as the keyboard scan-code ring (writer `common:2cc6`, write index `0xBFE`;
spot-checked) and the bytes as set-2 Ctrl and B, so bit 7 = "Ctrl and B seen in keyboard
history". EC-owned, consistent with the IDY_7Y measurements.

### 9. Fixed-duty fan override: 0x759 / 0x75A
`bank2:93aa`: if `0x759 bit 7` is set, fan 1 duty = `(0x759 & 0x7F) * 2` (percent → raw,
100 % if ≥ 100) and the curve is bypassed; same for `0x75A` and fan 2. Not in any OEM
register list and not in the write allowlist. Verified on IDY_7Y (Intel) hardware; see
[`fan_duty_override.md`](fan_duty_override.md).

### 10. Other
- High-temperature event: CPU `0x43E` ≥ 95 C or GPU `0x44F` ≥ 85 C sets `0x7A5 bit 3` and
  raises event `0xBB` (TimAP_HighTemp).
- SSD temp `0x7D1` ≥ 65 C forces a minimum fan duty of 50 % (55 % on one OEM sub-ID).
- Charge limit `0x7B9`: bits 0-6 = limit %, bit 7 = "limit reached" (set by the EC on AC
  when SOC ≥ limit). Only active on battery/OEM variants selected by `0x7C3`/`0x770`
  (IDs 4/5), which also set `0x742 bit 2`. Do **not** treat `0x742 bit 2` as a universal
  capability bit: the OEM DefaultTool reads the same bit as an LED-vendor flag
  (`m_bitLiteon_glossy`). Consistent with charge limiting not working on IDY_7Y.
- EC reset path (`bank2:cdba`) restores a register table and clears `0x7C5 bit 7`,
  `0x7C6 bit 2`, `0x786`, `0x787`, `0x743 bits 0/2`, `0x741 bits 2/7`, `0x74C bits 6/7`.

## Cross-check with `lp3123079_ec_firmware_review.md`

A second, independent review of the same image. Agreement on: 0x740 = 0x1A, the 0x786 →
0x463 path and its bit 7 flag, the four custom-table conditions, GPU control bits,
0x741 bit 5 fan alert, 0x7CC not being a keepalive.

Its findings this pass missed, each spot-checked here:
- **0x463 bit 7 is not masked in read paths.** `EcMonitor.cs:190-191`,
  `StartupValidator.cs:148-153`, `DiagnosticLog.cs:276-279` and
  `RegisterEncoding.DecodeCpuTempLimit` decode the raw byte. With bit 7 set, Intel reads
  as 105 - 0x8A = -33 C and AMD as 0xDF = 223 C. Affects IDY_7Y too, and the validator
  can disable CPU power control on a startup read that catches the flag.
- **GPU tach pair is 0x46C (high) / 0x46D (low)** in this image (`bank0:e90d` mirrors the CPU
  routine `e8ef` for 0x464/0x465). 0x46B is written from a power-control value
  (`bank2:b347`). Confirmed on IDY_7Y 2026-10-29: 0x46B reads a constant 0xC3 while
  0x46C:0x46D tracks the fan; `IDY_7Y.json` now uses 0x46D.
- **0x751 is a shared bitfield.** Bits 4 and 7 are written by the EC from `0x7E8`
  (`bank0:caac/cab5`, `bank2:c4d4/eb5c`) and choose temp-limit table rows; the Fn hotkey
  toggles bit 6 with XOR. `FanControl` writes whole bytes 0x00/0x40, reads `== 0x40`,
  and `ValidateFailsafeRegisters` disables the failsafe fan action unless the byte is
  exactly 0x00/0x40.
- **0x7C6 is also shared**: the EC tests bit 2 only and clears bits 6/7 elsewhere, while
  `ReadCustomFanCurveMode` needs `== 0x04` and `ApplyCurves` writes whole bytes.
- **0x7A7-0x7A9 "Battery Saver" defaults** come from ROM tables (145/145/174 raw in the
  ordinary branch), so the field-log values are intentional, but the preset is not a
  battery-saving mode. `ExtractModeDefaults` should not label it universally.

Findings here the other review did not cover: EC zeroing PLs and needing CUME (§3),
fan boost auto-clear (§7), factory-preset load command (§6), fixed-duty candidate (§9),
and the AMD temperature-encoding fallback when ECSpec is unreadable.

## Applied to MechControl (2026-10-29, built and unit-tested, not deployed)

1. 0x463 readback masked in the single decoder `RegisterEncoding.DecodeCpuTempLimit(reg,
   direct, tjMax)`, used by monitor, validator, log, CPU control.
2. Detection: baseboard patterns before EC ID; profiles may declare `cpuVendor`, which all
   automatic steps check against CPUID; new unverified `X6FR5xxY.json`; IDY_7Y = `intel`.
3. `StartupValidator.ValidateCpuTempEncoding`: CPU power disabled when the encoding is not
   confirmed by ECSpec and contradicts the CPU vendor; Restore Defaults skips CPU registers
   when CPU power is disabled.
4. Fan boost is a `fanBoost` bitfield (0x751 bit 6, universal default); `FanControl` and
   Restore Defaults use read-modify-write. `UniversalDefaults.ApplyToProfile` now adds any
   missing universal bitfield instead of only filling an empty set.

Still open: 0x7C6 bit handling, "Battery Saver" mode naming, extra identity logging.

## Reproducing

Analysis files live in `C:\Temp\tandem\ecbios` (temporary): `prep.py` splits the image
into three 64 KB views (common + bank N) and lists trampoline targets; `EcPre.java`
seeds functions; `EcDump.java` decompiles; `EcList.java` dumps disassembly ranges.
Ghidra 12.0.4 headless, language `8051:BE:16:default`, BinaryLoader.
The decompiler garbles 8051 carry-flag compares; every claim marked "confirmed" above
was checked against the disassembly.
