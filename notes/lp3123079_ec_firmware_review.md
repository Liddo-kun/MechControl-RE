# LP 3123079 EC firmware review

Date: 2026-10-29. Static analysis only; no firmware flashing, hardware access,
application execution, deployment, or runtime source changes.

## Summary

This package supplies useful independent evidence for the X6FR AMD family. It
confirms several existing controls, exposes concrete decoding/profile issues,
and identifies a keyboard-history setter for the previously unexplained 0x741
bit 7 **in this image**. It is not the Intel X6AR firmware running on Jon's laptop.

Recommended application work, in priority order:

1. Mask CPU temperature-limit readback consistently in monitoring, validation,
   and logging, as the power controller already does.
2. Resolve known EC project-ID collisions using explicit baseboard identity;
   give X6FR its own profile rather than selecting Intel IDY_7Y by ID alone.
3. Use the X6FR GPU tachometer pair 0x46C/0x46D in that profile, with read-only
   hardware confirmation of the corresponding firmware version.
4. Make fan boost a masked bit operation, including its readback and capability
   checks; audit fan-table control for the same shared-byte problem.
5. Stop treating the 0x7A7 power preset as universally "Battery Saver". Its
   semantic name must be model-supported, not inferred from a plausible wattage.

These are recommendations, not implemented changes. No new EC write address or
model-independent power limit is justified by this package.

## Input and analysis provenance

Source directory: `C:\Users\Jon\Downloads\LP 3123079 BIOS (1)`.

| File | Size | Role |
| --- | ---: | --- |
| `XxFR5xxx_Pro_14.810` | 262,144 | Raw banked 8051 EC image, with additional PD firmware content |
| `X6FR55xxN139PCS10_CAP.efi` | 9,061,080 | x64 EFI update executable with appended payload |
| `ifux64.efi` | 94,784 | EC updater referenced by `ecflash.nsh` |
| `AMIDEEFIx64.EFI` | 366,384 | SMBIOS identity utility referenced by `flash.nsh` |

SHA-256:

- EC: `99A329D6CDE5DC17A0FB833C7B8DD640F9EC68DB2B5F47545FBE93D208C7E735`
- EFI update: `3BB45972B2C541888B7DB5C4E9E8BAD9F788641076D22737459F7AD7121DE307`

`flash.nsh` specifies PCSpecialist, product/baseboard `X6FR559Y`, family `FRR`,
and board manufacturer AiStone. This is package intent, not a reading of a
machine's current SMBIOS. `ecflash.nsh` selects the separate EC image.

Existing artifacts in `C:\Temp\tandem\ecbios` were reused after verifying that
`ec.bin` and `bios.cap` match the supplied files exactly, and that the three
bank images match slices of the supplied EC. Fresh Ghidra instruction listings
were exported using read-only project processing. Every listed instruction's
bytes were compared against its input bank: 18,674 / 20,098 / 20,282 matches.
Decompiler output was used to find code; key claims below were checked against
the 8051 instructions, including indirect-pointer helper effects.

Fresh scratch artifacts: `C:\Temp\tandem\lp3123079-review`:

- `inspect_ec.py`: hashes, bank-image verification, function references, byte
  dumps, instruction-range display, and instruction-byte comparisons.
- `EcListing.java`, `img_b0.bin.txt`, `img_b1.bin.txt`, `img_b2.bin.txt`.
- `listing-run.log`, `listing-extra-run.log`.

Address convention below:

- Common CODE 0x0000–0x7FFF maps directly to file offsets.
- B0 CODE 0x8000–0xFFFF maps directly to file offsets.
- B1 CODE 0x8000–0xFFFF maps to CODE + 0x8000.
- B2 CODE 0x8000–0xFFFF maps to CODE + 0x10000.

The common bank-call stubs at CODE 0x1100/0x1114/0x1128 select these banks. CODE
addresses are instruction locations, **not host-writable EC RAM addresses**.
The main BIOS payload was not fully unpacked/analyzed; no conclusion about its
UEFI variable layout or safe NVRAM writes follows from this review. The embedded
`ITE EC-V14.6` string is not sufficient to decode the package's `14.810` version.

## 1. Project-ID collision is proved in firmware

B0 CODE **0xE45A–0xE45F** explicitly initializes EC RAM **0x740 = 0x1A**.
The Intel X6AR system also reports 0x1A. Both shift to 13.

`Profile/ProfileLoader.cs:44–63` immediately returns an EC-ID match before
consulting baseboard patterns. Therefore merely adding an X6FR baseboard pattern
does not fix this collision; adding a second profile with ID 13 would also make
the first-match directory scan ambiguous.

**Apply:** explicit model identity should resolve collisions before accepting a
generic EC-ID candidate. Keep EC-ID fallback for machines without a known board
mapping. Treat `X6FR559Y` from this package and `X6FR5xxY` from the field log as
separate observed identity strings until their profile coverage is established.
Do not map this family to the Intel profile just because the EC ID is identical.

## 2. CPU temperature readback has an asynchronous high-bit flag

B1 CODE **0xF4B8–0xF4E4**:

- The user override is consumed only when 0x786 bit 7 and 0x741 bit 0 are set.
- The effective value is **0x786 & 0x7F**.
- On a change, the firmware publishes **effective value | 0x80** at 0x463.

B2 CODE **0xB49C–0xB4B4** subsequently clears 0x463 bit 7 and sets an internal
pending-work flag (0x9DD bit 5), conditional on 0x440. This explains why either
high-bit state can be observed; it is not part of the temperature itself.

`CpuPowerControl.cs:81,174` already masks this correctly. The following do not:

- `Monitoring/EcMonitor.cs:190–191`.
- `Features/StartupValidator.cs:148–153`.
- `DiagnosticLog.cs:276–279`.
- `Profile/RegisterEncoding.cs:38–39` subtracts the entire raw byte and still
  describes 0x463 as having no enable bit.

**Apply:** mask with 0x7F before either direct or inverted-offset decoding in
every read path. For example, AMD raw 0xDF represents 95 C, not 223 C. Intel
raw 0x8A with TjMax 105 represents 95 C, not -33 C. Preserve the existing
profile-derived encoding, commit bit, and delayed masked write verification.
On this AMD image, an incorrectly Intel-encoded write of 0x8A instead requests
**10 C**. That explains the historical 600 MHz symptom more directly than the
older interpretation that the firmware was trying to enforce 138 C: readback
138 is 10 plus the high-bit flag.
Regression coverage should exercise the actual monitor/validator/log decoder
paths with both high-bit states, rather than testing the already-correct power
controller alone.

## 3. GPU fan RPM uses a different pair from the current profiles

B0 CODE **0xE8EF–0xE92A** has parallel CPU/GPU tachometer publication routines:

| Fan | High byte | Low byte |
| --- | --- | --- |
| CPU | 0x464 | 0x465 |
| GPU | **0x46C** | **0x46D** |

Both call the same conversion routine and store R6 followed by R7 into adjacent
bytes. B2 CODE **0x9409–0x9435** independently tests those same pairs for zero
RPM when evaluating fan failure. In contrast, B2 CODE **0xB444–0xB468** updates
0x46B from a separate power-control value; it is not this image's tachometer low
byte.

Current IDY_7Y and placeholder profile mappings use GPU low 0x46B/high 0x46C.
`EcMonitor.cs:181–183` combines exactly those profile addresses. The high byte
can make this wrong pairing appear approximately plausible while its low byte
is unrelated.

**Apply:** X6FR's profile should use GPU low 0x46D/high 0x46C. Confirm via a
read-only comparison on an X6FR machine with matching firmware before declaring
the profile hardware-verified. Do not globally change the Intel mapping using
this AMD image; investigate that mapping independently. The current profile
schema already supports the correction without new hardcoded monitor addresses.

## 4. Fan-control registers are shared bitfields

The firmware tests **0x751 bit 6** for fan boost, while B1 CODE
**0xF32A–0xF364** also uses **bits 7 and 4** to choose temperature-policy rows.
Those other bits are not disposable padding.

Current `FanControl.cs` treats the byte as an enum in several places:

- `ValidateFailsafeRegisters` accepts only 0x00/0x40 (line 71).
- `SetFanBoost` overwrites it with 0x00/0x40 (line 86).
- `ReadFanBoost` compares the entire byte to 0x40 (line 116).

**Apply:** use the verified boost bit mask through WriteGuard, preserve the
other mode bits, and make capability/readback checks consistent with the mask.
For example, 0x50 has boost set as well as an independently meaningful mode bit.
Cover both preservation and thermal-hold maintenance when implementing this.

Similarly, the custom-table path tests **0x7C6 & 0x04**, not whole-byte equality
(B2 CODE **0x97CF–0x97D5**). Other routines use its bits 6 and 7.
`FanControl.ReadCustomFanCurveMode` currently requires `mode == 0x04`, and
`ApplyCurves` writes whole-byte 0x00/0x04. Audit those writes and state checks
against the model's shared-bit/command semantics. Retain the verified transition
around table writes; this is not evidence that the transition is unnecessary.

## 5. The "Battery Saver" preset is not universally battery-saving

B2 CODE **0xA359–0xA45F** fills the exposed mode-default registers from ROM
tables. For the pointer group at file offset **0x767A**:

| Exposed group | ROM record | First three raw power bytes |
| --- | --- | --- |
| 0x730–0x732 | 0x72C9 | 75, 85, 85 |
| 0x734–0x736 | 0x72F6 | 45, 45, 45 |
| 0x7A7–0x7A9, ordinary branch | **0x726F** | **145, 145, 174** |
| 0x7A7–0x7A9, alternate branch | 0x729C | 100, 110, 110 |

The alternate branch requires activation and 0x728 bit 0. Other pointer groups
exist, so these are not universal defaults even within the image. With doubled
PL4, the ordinary group's third byte means 348 W. These exact ordinary/gaming/
office triples match the earlier AMD field log in `other_machines.md`.

This confirms that the high numbers are intentional preset data, rather than
an accidental read of uninitialized RAM. It does **not** establish an official
user-facing name for that preset.

`HardwareDetection.ExtractModeDefaults():320–323` currently exposes these bytes
as `batterySaver`; its plausibility check accepts PL1=145. Power-limit validation
alone cannot detect the semantic mismatch because 145 W is within the model's
reported maximum.

**Apply:** make the mode-register naming/availability model-specific; omit the
Battery Saver action where that identity is unverified. Do not replace 145 W
with an invented lower value or rename the group universally to Turbo.

## 6. 0x741 bit 7 is set by keyboard-buffer history in this image

B0 CODE **0xCA07–0xCA5D**, periodically called from B2 CODE 0x8D40:

- Unless 0x9EE bit 4 inhibits it, scan all **0xB00–0xBFC**.
- Seeing byte 0x14 sets bit 0 in internal latch 0x816.
- Seeing byte 0x32 sets bit 1 in that latch.
- Once the latch equals 3, set **0x741 bit 7**.

Common CODE **0x2CC6** enqueues keyboard bytes in that ring; **0x2C8F** consumes
them without erasing the slots. Keyboard callers emit the familiar set-2 0xF0
break and 0xE0 extended prefixes (e.g. CODE 0x21BD/0x2218). Set-2 0x14 and 0x32
are Ctrl and B. The scan does not require a simultaneous Ctrl+B chord, parse
make/break prefixes, or restrict itself to unread entries. Its accumulated
history can outlive a key press and immediately reassert a host-cleared bit.

The latch/flag are cleared together in reset/power-transition paths at B0
CODE **0xC84E** and B2 CODE **0xCE71** (within 0xC81E and 0xCDBA respectively).
The vendor-facing purpose of this keyboard-history flag remains unidentified.

Separately, B2 CODE **0x93EE–0x9444** sets **0x741 bit 5** when a timed fan check
reaches its final count and either tachometer pair is zero. That independently
supports the older OEM application's bit-5 fan-alert mapping.

**Apply:** do not interpret 0x81 as fan failure or failed activation. Keep
activation checks scoped to the owned bits. This supplies a concrete explanation
consistent with Jon's persistent 0x81 observation, but the same setter must be
confirmed in Intel X6AR firmware before claiming that machine's bit 7 is solved.
Any future fan-error diagnostic should use the model-supported bit-5 meaning
and account for its firmware latch lifecycle, rather than inventing an RPM-zero
rule that flags intentional fan-stop operation.

## Confirmations and additional leads

- B2 CODE **0x9339–0x93A9** implements right-point evaluation: move up when
  temperature is greater than the current up threshold; move down when below
  the current down threshold; fetch duty at offset +0x20. This supports the
  existing curve interpretation. It does not mandate MechControl's chosen 4 C
  hysteresis policy.
- B2 CODE **0x97CA–0x9809** checks activation, 0x7C6 bit 2, 0x727 bit 6, and
  both first duty entries being zero before taking the RAM-table path. Keep the
  zero-duty first entry and existing activation prerequisites.
- B2 CODE **0xA843–0xA879** distinguishes GPU control bits: bit 2 accepts the
  TGP value; bit 0 accepts the two DB parameters; bit 1 is passed as the DB
  enable state. Current 0x05/0x07 sequences cover these correctly; shorthand
  descriptions treating bit 0 purely as another TGP bit are incomplete.
- B2 CODE **0x9AB5–0x9AD0** consumes the enabled, nonzero low seven bits of
  0x787 for fan-ramp timing. No basis to change the current encoding.
- B2 CODE **0xDE96–0xDF2D** contains charge-limit processing around 0x7B9,
  including a masked threshold and high-bit state. This is a lead for a separate
  capability/enforcement investigation, not evidence to enable a universal
  charge-percentage slider. The full charger write path was not established.

The 0x7CC consumers examined are power-source/status logic. This review does not
establish that periodically reading it is a watchdog keepalive requirement.
Do not add such polling on the strength of its historical label.

Related firmware-oriented analysis: [`ec_firmware_X6FR5_findings.md`](ec_firmware_X6FR5_findings.md).
That separate pass also records candidate factory-preset and fixed-duty paths;
this review concentrates on comparisons with the current application code.
