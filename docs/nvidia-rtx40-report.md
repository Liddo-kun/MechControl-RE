# NVIDIA on RTX 40 laptops

Status 2026-11-05: issues 1-3 implemented, offline checks only. No RTX 40 hardware has been
tested. Source: field log from a C# app user (i9-13900HX, baseboard `GMxPXxx`, detected as
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
3. **RTX 40 (Ada) BIOS table `30 32 53 14`** (`Bios::parse`, `Bios::ada`):
   - Entry `t+0x32+i*0x53`. Identity byte at +0, channel at +1 (<32), min/default/max at +2.
   - Power: index 2, identity 0x01 or 0x21. NVVDD: index 13,
     `byte((e[0x42]<<4)|(e[0]&15))==0x10` and `u32(e+0xE)==0xD00`. Board input: index 9,
     identity 0x15, optional. No MSVDD; index 14 is never read. Entries 7/8/11/12 are not checked.
   - Patches: power maximum, NVVDD default and maximum, board-input maximum only (as mVolt).
   - Same BIT/image mapping, inactive-profile check and `romOverrideNN` record format.
   - `30 34 5B 14` (also in mVolt) is still refused: no ROM to test it.
4. **Driver side:** `Api::rails()` accepts policy type 13 for NVVDD (rail ID 1) only. `Power::verify`
   still requires the driver's NVVDD range to equal the BIOS field, and MSVDD present in both or
   neither.
5. **MSVDD is optional end to end:** `Bios`, `Currents`, `Rails`, `Telemetry`, `Power`,
   `plan_profile` (a saved MSVDD value is skipped with a notice), profile capture, CLI
   (`--msvdd` refused as unavailable), and UI (slider disabled, live row and hint show `--`).
   Without MSVDD the current controller's telemetry match is unconfirmed, so its clock cap and
   predictions stay empty. Power-controller values are unaffected.

## Checks

`tests/nvidia_check.cpp` `rtx40()` covers: Ada parse using the 4060/4070 ROM values; patch set
and reparse; MSVDD override refused; optional board input; identity/channel/range mutations;
header bytes in the error; a type-13 NVVDD driver without MSVDD (currents, SET, telemetry);
type 13 ignored for MSVDD; ambiguous type 13 + 19 NVVDD; profile MSVDD skip. Existing
RTX 50 fixtures still pass. Release `-WithCli -Check` build: 0 errors, 7/7 CTest suites pass
(log `C:/Temp/tandem/native-rtx40-build2.log`). The read-only `nvidia-check --live` on the
RTX 5090 was not run because the installed app held `Global\MechControl`.

## Remaining (needs the field laptop)

- Whether the Ada driver accepts the overrides after a GPU restart is unverified, as are the
  board power policy shape, F8 request, F7/platform budget and board input runtime policy.
  `Power::verify` and the post-restart image check refuse on a mismatch; they do not prove
  enforcement.
- The field GPU is probably a 4080/4090, not the 4060/4070 ROMs decoded. Ask the user for the
  new log line from a first run (GPU identity, layout or mismatch detail) before suggesting
  the unlock.
- Whether Ada drivers expose DLPPC is unknown; if not, the existing "no DLPPC policy" error shows.

## Not done: GK7NXXR EC observations (issue 4)

- ECSpec reports TGP 150-150 W, but EC `0x744` reads `0x19` (175 W with offset 150). Apply may
  rewrite it to 150 W. Needs checking before relying on this profile.
- Detection matched by EC project ID only. Add `GMxPXxx` to `GK7NXXR.json` `baseboardPatterns`
  once that baseboard is confirmed to be this model.
