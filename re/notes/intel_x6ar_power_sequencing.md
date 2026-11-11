# Intel X6AR EC power sequencing (EC 77.0)

Static analysis of the Intel X6AR EC image (`ec.bin`, SHA-256 `2C90BAE2...DC13`, EC version
77.0, the version `IDY_7Y.json` allows). Nothing here was tested on hardware unless stated.
Addresses are EC RAM addresses as MechControl uses them. Code addresses are `bank:address`.
The disassembly, listings and cross-reference index are local only, in
`C:/Temp/tandem/ec-poweron/` (`evidence-listings.txt`, `init-tables.txt`, `xref.json`).
Do not commit them: they are derived from OEM firmware.

## Bytes MechControl writes

None of the bytes in `IDY_7Y.json`'s allowlist, nor the `0x726`/`0x727` init writes, is read by
a path that powers the machine on from S5, except `0x726` bit 3, which MechControl preserves.

| Byte | Firmware behaviour | Rule |
|---|---|---|
| `0x726` bit 3 | Enables "power on when the adapter is detected" (B1 `BB11`). Restored from a saved copy at EC cold start (B0 `E987`-`E9A5`). | Write only bit 7, masked. A whole-byte write clears bit 3. |
| `0x706` | One of the four bytes the EC saves (`0x9400`, `0x9401`, `0x706`, `0x7E8`; table at common `5AFB`). `0xFF` after EC cold start without a valid save (B0 `E98F`). | The save routine (B0 `E71D`) runs only on the battery-only shut-off path (B2 `85E0`: off, no adapter, countdown `0x845` expired). On a machine kept on AC, `0x706`, `0x726` bit 3 and `0x7E8` are never saved. |
| `0x741` bit 4 | Read during power-on (B0 `CD7D`): if set, the EC clears it and drives GPIO D5 high instead of low. Also cleared at B0 `CDEC`. | Never set. `0x05` is fine. |
| `0x741` bit 7 | Set when bytes `0x14` and `0x32` have both appeared in the FIFO at `0xB00`-`0xBFC` (B0 `D715`-`D764`). Cleared at the start of every S5 to S0 sequence (B0 `D59E`). | EC-owned; ignore on read-back. Matches the AMD finding in `ec_0x741_bits.md`. |
| `0x7C6` bit 6 | A "press the PCH power button" request. The S0/S3 button handler (B1 `8175`) clears it and, only if `0x440` = 0 (no ACPI OS registered), pulls the PCH power-button line low for 200 delay units. Cleared at S0 entry (B0 `D1E1`). | Never set. Write `0x7C6` masked (`fanCurveLatch` mask `0x04` in `IDY_7Y.json`). |
| `0x7C6` bit 2 | Cleared at S5 entry (B0 `D2C9`). Its reader in this image was not traced. | Not persistent across shutdown; EC init rewrites it. |
| `0x7C5` bits 3, 7 | Cleared at S5 entry (B0 `D2B2`, `D2C2`). | The fan-curve enable does not survive shutdown; EC init rewrites it. |
| `0x7C7` bit 1 | Cleared at S0 entry (B0 `D090`). | Matches the cooler-flag note in `register_reference.md` 11.9.1. |
| `0x743` bits 0, 2 | Cleared at S0 entry (B0 `D04A`). | |
| `0x786`, `0x787` | Zeroed at S0 entry (B0 `D035`-`D03C`); `0x788` and `0x7D5` set to `0xFF` (B0 `D051`-`D05A`). | |
| `0x780` = `0xA2` | Outside S0 (`0x6E6` != 1) the EC clears `0x780`, sets two bits in SFR `0xD8` and loops forever (B2 `8AEF`). The datasheet describes starting the watchdog without clearing it as how firmware resets the EC domain, so this is a deliberate EC self-reset. | A bypass left set across sleep or shutdown resets the EC mid-transition. App exit and power notifications clear it; a kill, crash or forced power-off does not, and the next start clears a leftover value. |

## State the EC rebuilds at power-on

At the start of every S5 to S0 sequence (B0 `D5E2`-`D619`) the EC rebuilds bits from the
saved byte `0x7E8`:

- Adapter present (`0x7CC` bit 0): `0x751` bits 4 and 7 come from `0x7E8` bits 4 and 7, and
  `0x727` bit 6 (`CUME`) is set if `0x7E8` bit 5 is set, otherwise cleared.
- No adapter: `0x751` bits 4 and 7 come from `0x7E8` bits 0 and 3, and `0x727` bit 6 is cleared.

`0x7E8` bit 5 is therefore the remembered `CUME` state, and `0x727` can read `0xC0` before
MechControl's init. `0x727` bit 7 is set at S0 entry and at EC cold start (B0 `D0B3`, `EAEC`).

## Bytes reset at each transition

From the init tables applied by B0 `D174` (S0 entry) and B1 `E766` (S5 entry):

- **S0 entry:** `0x440`=0 (ACPI writes it again), `0x457`, `0x45B`, `0x45C`, `0x45F`=0,
  `0x46A`/`0x46B`/`0x46E`/`0x46F`=0 (published PL1/PL2/PL3/PL4), `0x474`/`0x475`=0,
  `0x477`-`0x47A`=0, `0x495`/`0x496`/`0x498`/`0x49D`/`0x49E`=0, `0x43E`=`0x20`, `0x725`=0,
  `0x72C`=4, `0x752`=`0x6C`. Bit clears: `0x459` bit 3, `0x471` bit 7, `0x49F` bits 0-5,
  `0x456` bits 0-4, `0x7C6` bit 6 (B0 `D1B8`-`D1E7`).
- **S5 entry:** `0x457`=5, `0x459`=0, `0x440`=0, `0x480`=0, `0x769`-`0x76B`=0 (logo colour),
  plus the bit clears in the first table.

`0x47E`, `0x47F` and `0x6EC` are in neither table and survive into the next boot.

## Bytes that must never be written

None is in any profile allowlist. Each changes power behaviour:

| Byte | Effect |
|---|---|
| `0x459` | Bit 3 is a power-on request the S5 handler treats like a held power button (B1 `E55D`). Bit 1 disables the long-press forced power-off (B0 `D64C`). |
| `0x45D` | Wake timer: N > 0 powers the machine on N seconds after S4/S5 entry, or pulses the PCH power button in S3 (B1 `E900`-`E951`). Re-arms at every sleep entry; a real button press clears it. |
| `0x45E`, `0x45F` | `0x45F`=`0xAA` submits the code in `0x45E` to the EC event queue (B1 `E8D2`-`E8E7`). Codes `0x90`-`0xDD` are internal actions (B0 `E351`), including `0x91` reset the EC at the next S5 entry, `0x9E` shut down now, `0x9F` arm the restart signature. `0x45F`=`0xDB` is a test mode that keeps `0x4FE`/`0x4FF`. |
| `0x4FE`, `0x4FF` | Test overrides: `0x4FE` bit 0 forces "adapter present", bit 1 ignores the adapter pin (B1 `8E58`-`8E65`). Zeroed every second unless `0x45F`=`0xDB`. |
| `0x47E`, `0x47F` | Shutdown cause and reason. The power-down sequence branches on `0x47E` (B0 `CD1C`, `CD3D`); the PCH-wake follower does nothing when it is `CD` or `FC` (B1 `9454`). |
| `0x77E`, `0x77F` | `A5 78` makes the EC, at the next S5 entry, save settings, reset itself and power the machine back on (B0 `D30A`-`D390`, B1 `E873`-`E8A7`). Idle value `55 AA`. |
| `0x7FD`-`0x7FF` | The restart signature (`5A 55 AA`). |
| `0xDFF` | `0x79` has the same effect as `0x77E/F` = `A5 78`. |
| `0x6E6`-`0x6EC` | The power state machine's variables. |
| `0x7E8` | Saved mode bits (see above). |

The host command handler also has an arbitrary EC RAM write (B0 `E304`-`E350`: `A2`/`A3` set an
address, `A5` writes a byte). Do not use that command set.

## Read-only bytes

`0x6E6`, power state:

| Value | State |
|---|---|
| 0 | Just after EC reset; becomes 5 on the next tick |
| 1 | S0 |
| 3 | S3 |
| 5 | S5 / off |
| `0x81` | Powering on from S5 |
| `0x82` | Entering S3 |
| `0x83` | Resuming from S3 |
| `0x84` | Powering down |

Transitions last tens to hundreds of milliseconds, so "not 1" means off or in transition.

`0x47E`, cause of the last power-down (survives the next boot):

| Value | Meaning |
|---|---|
| `CC` | Normal: the PCH asserted SLP_S4#/SLP_S5# (B0 `D6E9`, B1 `E965`) |
| `CD` | Power button held about 10 s (2000 ticks, B0 `D626`) |
| `FC` | Forced by the EC; `0x47F` holds the reason |
| `EC` | The EC was cold-started since the last shutdown (B1 `855C`) |
| `E9` | Never written by the firmware; only a host could set it. Runs the "cut standby power" steps. |

`0x47F` reasons: `0x81` boot watchdog, `0x99` powered on with the lid closed, `0xB2`, `0xB3`,
`0xF2`, `0x25` (battery protection group, B1 `F051`-`F097`); `0x32`, `0x33`, `0xC3`, `0x15`,
`0x78` not traced.

Other bytes:

- `0x6EC`: failure code of the last power sequence (`0x80`-`0x88`, written when a wait step
  times out after 1000 ticks). 0 after a clean power-on.
- `0x756`: seconds left on the EC boot watchdog. Set to 180 at every power-on (B0 `CCB5`),
  zeroed once byte `0x1C` appears in the `0xB00` FIFO (B1 `8066`). At 0 the EC shuts the
  machine down hard and powers it on 5 s later (B1 `80C2`-`80E7`). What puts `0x1C` in the
  FIFO was not established.
- `0x490` bit 0: debounced adapter state. Set after 40 consecutive "present" ticks, cleared
  after one "absent" tick with the DC adapter (five with a Type-C source) (B1 `8E55`-`8F50`).
  `0x480` bit 0 is the undebounced state. A brief adapter glitch shows as an AC loss of
  about 200 ms.
- `0x476` bit 1: lid, 1 = closed, copied from GPIO E0 (B1 `8CCC`).

## Host window

The firmware sets up four host RAM windows at B0 `EE32` (bases `0x000`, `0x400`, `0x700`,
`0xC00`). `re/captures/ec_dump_0000_0FFF.bin` has data at `0x000`-`0x7FF` and `0xC00`-`0xFFF`
and `0xFF` in `0x800`-`0xBFF`; the public IT5570 datasheet's window sizes do not reproduce
that, so do not rely on them for this chip. The EC's own event log (47-byte ring at
`0x990`-`0x9BE`, write index `0x9BF`, writer B1 `F536`) is therefore not readable through the
driver.

## Timing

- The main tick reloads timer 0 with `0xF106` (common `0EB8`): 5 ms at ITE's usual
  9.2 MHz / 12, matching earlier measurements. The clock is not in the image.
- The power state machine runs every tick; slower work runs at 10 ms, 20 ms, 100 ms and 1 s
  (B1 `86CF`, common `0DD6`-`0E7B`).
- After a normal shutdown the EC ignores the power button for 4-5 s (`0x693` = 5, counted
  down each second; B0 `CD4F`, B1 `829D`, B1 `E541`).
- The main loop kicks an external watchdog every pass (B1 `958B`), so a hung EC resets.

## Method

Ghidra's listings have gaps. A small banked 8051 disassembler (`tools/d51.py` in the local
evidence directory) follows the bank-switch stubs and Keil switch tables; its instruction
lengths agree with Ghidra on all 57,167 lines Ghidra produced. Bank 1 holds two generations of
several routines (hand-written at `8000`-`97FF`, compiled C at `E400`-`F7FF`) and only one of
each pair is called, so check callers first. For example, the compiled PCH-wake follower at
`E714` has no caller; the live one is at `9441`.
