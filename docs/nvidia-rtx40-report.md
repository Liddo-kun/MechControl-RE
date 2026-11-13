# NVIDIA on RTX 40 laptops

Status 2026-11-14: issues 1-3 implemented, offline checks only. No RTX 40 hardware has been
tested. The BIOS parser accepts both RTX 40 table headers and was run on nine RTX 40 Laptop ROMs
(4060 to 4090). Source: field log from a C# app user (i9-13900HX, baseboard `GMxPXxx`, detected as
`GK7NXXR`, unverified), where both NVIDIA checkboxes failed with
`Unsupported NVIDIA BIOS: power policy layout`. RE evidence (mVolt+ 0.50.9 and mVolt 0.50.8
static analysis, ROM decodes): `C:/Temp/tandem/mvoltplus-rtx40/findings.md`.

## Delivered

1. **DLPPC no longer depends on the BIOS parse.** If `nv::Power` refuses (unsupported BIOS
   limits), DLPPC uses a plain `nv::Api` (`Controller::open_dlppc/read_dlppc/write_dlppc`,
   `controller_gpu.cpp`). It has its own availability flag (`dlppcAvailable`). Only one NVAPI
   owner exists at a time: opening `Power` drops the plain Api. Startup applies the saved
   DLPPC setting even when `Power` fails. The periodic DLPPC re-check in `check_gpu` still
   runs only with `Power`, as before.
2. **Diagnosable refusals.** A `Power` open failure logs `NVIDIA GPU: name (PCI 10DE:dev/subsys),
   driver, BIOS`. A layout refusal names the four table header bytes. A driver/BIOS mismatch
   lists both sets of min/default/max values.
3. **RTX 40 (Ada) BIOS table `30 32 53 14` or `30 34 5B 14`** (`Bios::parse`, `Bios::ada`):
   - The second header appears in later BIOS branches. It has a longer header (0x34) and entry
     stride (0x5B) with the same entry fields; mVolt handles both in one branch.
   - Entry `t+header+i*stride`. Identity byte at +0, channel at +1 (<32), min/default/max at +2.
   - Power: index 2, identity 0x01 or 0x21. NVVDD: index 13,
     `byte((e[0x42]<<4)|(e[0]&15))==0x10` and `u32(e+0xE)==0xD00`. Board input: index 9,
     identity 0x15, optional. No MSVDD; index 14 is never read. Entries 7/8/11/12 are not checked.
   - Patches: power maximum, NVVDD default and maximum, board-input maximum only (as mVolt).
   - Same BIT/image mapping, inactive-profile check and `romOverrideNN` record format.
   - Any other header, including a mixed header and stride pair, is refused.
4. **Driver side:** `Api::rails()` accepts policy type 13 for NVVDD (rail ID 1) only. `Power::verify`
   still requires the driver's NVVDD range to equal the BIOS field, and MSVDD present in both or
   neither.
5. **MSVDD is optional end to end:** `Bios`, `Currents`, `Rails`, `Telemetry`, `Power`,
   `plan_profile` (a saved MSVDD value is skipped with a notice), profile capture, CLI
   (`--msvdd` refused as unavailable), and UI (slider disabled, live row and hint show `--`).
   Without MSVDD the current controller's telemetry match is unconfirmed, so its clock cap and
   predictions stay empty. Power-controller values are unaffected.

## Checks

`tests/nvidia_check.cpp` `rtx40()` covers, for both headers: Ada parse using the 4060/4070 ROM
values; patch set and reparse; MSVDD override refused; optional board input;
identity/channel/range mutations. It also covers: an unknown header refused with its bytes in the
error; a type-13 NVVDD driver without MSVDD (currents, SET, telemetry); type 13 ignored for
MSVDD; ambiguous type 13 + 19 NVVDD; profile MSVDD skip. Existing RTX 50 fixtures still pass.
Release `-WithCli -Check` build: 7/7 CTest suites pass. The read-only `nvidia-check --live` on the
RTX 5090 was not run because the installed app held `Global\MechControl`.

`Bios::parse` and `Bios::build` were also run on TechPowerUp ROM files (tool and output in
`C:/Temp/tandem/rtx40-rom-check`). All parse, and the unlock patch set changes only the intended
fields. Board power is default/maximum.

| ROM | BIOS | Header | Board power | NVVDD | Board input |
|---|---|---|---|---|---|
| Tongfang/Mechrevo 4060 | 95.07.16.80.7B | `30 32 53 14` | 115/140 W | 135 A | 12.434 A |
| Tongfang/Mechrevo 4070 | 95.06.15.40.63 | `30 32 53 14` | 115/140 W | 135 A | 12.434 A |
| Mechrevo 4080 | 95.04.3C.40.01 | `30 32 53 14` | 150/175 W | 189 A | 12.434 A |
| Mechrevo 4080 | 95.04.58.00.10 | `30 32 53 14` | 150/175 W | 189 A | 12.434 A |
| Tongfang 4090 | 95.03.2A.00.40 | `30 32 53 14` | 150/175 W | 189 A | 12.434 A |
| Tongfang 4090 | 95.03.2A.00.41 | `30 32 53 14` | 150/175 W | 189 A | 12.434 A |
| Eluktronics/Tongfang 4090 | 95.03.40.00.12 | `30 34 5B 14` | 150/175 W | 189 A | 12.434 A |
| Asus 4080 | 95.04.65.00.1A | `30 34 5B 14` | 80/125 W | 189 A | 9.237 A |
| Razer 4080 | 95.04.61.00.10 | `30 34 5B 14` | 80/175 W | 189 A | 12.434 A |

## Remaining (needs the field laptop)

- Whether the Ada driver accepts the overrides after a GPU restart is unverified, as are the
  board power policy shape, F8 request, F7/platform budget and board input runtime policy.
  `Power::verify` and the post-restart image check refuse on a mismatch; they do not prove
  enforcement.
- The field laptop's own ROM has not been seen. Ask the user for the new log line from a first
  run (GPU identity, layout or mismatch detail) before suggesting the unlock.
- Whether Ada drivers expose DLPPC is unknown; if not, the existing "no DLPPC policy" error shows.

## Not done: GK7NXXR EC observations (issue 4)

- ECSpec reports TGP 150-150 W, but EC `0x744` reads `0x19` (175 W with offset 150). Apply may
  rewrite it to 150 W. Needs checking before relying on this profile.
- Detection matched by EC project ID only. Add `GMxPXxx` to `GK7NXXR.json` `baseboardPatterns`
  once that baseboard is confirmed to be this model.
