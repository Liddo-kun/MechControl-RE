# MechControl Standalone Startup Requirements

Without GCUService running, MechControl must replicate the OEM's essential EC initialization handshake. On a cold reboot, the EC is in an uninitialized state — failing to handshake causes failsafe (CPU locked at 800MHz).

The **real** cause of failsafe (identified after more testing — see `notes/ec_activation_issue.md`) turned out to be the `0x726`/`0x727` power-management activation, NOT the fan-table write that was previously believed to be load-bearing. The fan table and `0x7C6` writes were removed from init in the fan refactor; see "Fan state is managed at orchestration level" below.

## Full Init Sequence (all steps required, in order)

```
Step 1: Write(0x709, 0x05)                  EC handshake step 1
        Write(0x708, 0x01)                  EC handshake step 2 ("OEM software present")

Step 2: Write(0x741, 0x05)                  EC activation (gates PL/fan/GPU writes)

Step 3: Write(0x748, 0x81)                  Feature enable register

Step 4: 0x726 |= 0x80                       Power management byte 9
        0x727 |= 0x40                       CUME bit (PECI power-management loop)

Step 5: 0x7C5 |= 0x60                       OEM init bits 5 & 6 (preserve bit 7)

Step 6: Write(0x7C7, 0x0C)                  Fan curve parameter (unknown purpose, OEM init byte)

Step 7: Write(0x706, 0x41)                  BIOS_CTRL init complete signal
```

150ms delay between steps (was 100ms; increased to leave margin for the ~250ms async 0x741 transform so the post-init verify read reliably sees 0x01).

Implemented in: `EcActivationControl.InitializeEc()`

## Why the full sequence is needed

On cold reboot, registers 0x708/0x709/0x748 are at power-on defaults. Our earlier testing missed this because we tested by stopping and restarting the OEM service — those registers persisted from the previous service run.

`0x726`/`0x727` are what actually prevent CPU failsafe; without the CUME bit set, the EC's PECI power-management loop is inactive and the CPU is locked at 800MHz regardless of what we write to PL registers.

## Cold-boot fan table state (empirically observed 2026-04)

On a cold boot with no OEM service ever having run (wiped EC + UEFI), the fan table at `0xF00-0xF5F` is **all 0x00**, not uninitialized garbage or 0xFF. This contradicts the earlier assumption in this document that the table contained "uninitialized data".

The CPU and GPU fan duty registers (`0x75B`/`0x75C`) read `0x32` (25%), which means the EC's **internal default** fan logic is running — it does NOT need a valid table in `0xF00-0xF5F` to keep the fans alive. The EC only reads that range when `0x7C6 == 0x04` AND `0x7C5` bit 7 is set. While those are clear, the table can be anything, including zeros.

This means the "must write a default fan table during init to prevent failsafe" claim was wrong. The fan table write was removed from init in the refactor and cold boots still work fine.

## Fan state is managed at orchestration level

`0x7C5` bit 7, `0x7C6`, and the fan table (`0xF00-0xF5F`) are **not** touched by `InitializeEc` anymore. They are managed by `FanSnapshot` + `FanControl.SetCustomFanCurveMode` at the `MainForm` orchestration level:

- Startup takes one `FanSnapshot.Read` (one pass, ~114 byte reads).
- If the snapshot is a valid curve, `HasGpuFan` and `ActiveEntries` detection applies overrides to the profile.
- If not (cold boot all-0x00, wiped EC), profile defaults are kept and custom mode is parked off.
- If a user profile slot exists, its saved curve is written and `SetCustomFanCurveMode(profile.Fan.CurveEnabled)` flips both registers atomically.

This replaces the old "always force custom mode on at init" behavior so the UI checkbox reflects real EC state.

## Register details

**0x709/0x708 (Handshake):** Written first, before anything else. Tells EC that OEM software is present. 0x709=0x05 then 0x708=0x01 (verify 0x708 reads back 0x01).

**0x741 (Activation byte):** EC clears to 0x00 when OEM service stops. While zero, EC silently ignores PL/fan/GPU writes. Write 0x05 (EC transforms to 0x01 asynchronously, ~250ms). Skip verification.

**0x748 (Feature enable):** Written 4 times by OEM during init, always 0x81. We write once.

**0x726 / 0x727 (Power management activation):** Bit 7 of 0x726 and bit 6 (CUME) of 0x727 activate the EC's PECI power-management loop so PL values are actually applied to the CPU. Without these, the CPU stays at 800MHz regardless of PL register writes — this is the real cause of "failsafe" symptoms previously misattributed to the fan table.

**0x7C5 (Multi-bitfield):** bit 7 = custom fan curve enable (user-facing toggle, managed by the orchestrator, NOT init), bit 6 = unknown "service managed" (set at init), bit 5 = logo flag (set at init). Init writes `|= 0x60` to preserve bit 7. Read-modify-write.

**0xF00-0xF5F (Fan table):** 96 registers: CPU tempUp[0-15], tempDown[0-15], duty[0-15], GPU tempUp[0-15], tempDown[0-15], duty[0-15]. Read by the EC only when `0x7C5` bit 7 AND `0x7C6 == 0x04` are both set. On cold boot this range is all 0x00 — the EC doesn't care until custom mode is enabled.

**0x7C7 (Fan curve param):** Written 0x0C by OEM during init. Purpose unknown. Included for completeness; harmless.

**0x7C6 (Fan curve mode flag):** 0x04 = EC honors the custom fan table, 0x00 = EC runs its internal default (~25% on our machine). Managed by the orchestrator via `FanControl.SetCustomFanCurveMode`, NOT init.

**0x706 (BIOS_CTRL):** Written 0x41 at end of init. May signal "init complete" to EC.

## After init: user profile auto-load

`MainForm` takes a `FanSnapshot.Read` after init completes, then:
- If a user profile slot exists, loads it (writes user curves + `SetCustomFanCurveMode`).
- Else if snapshot is valid, leaves EC alone.
- Else (cold boot or wiped EC), calls `SetCustomFanCurveMode(false)` to park custom mode off; EC runs its internal default fan logic.

Startup flow: EC init (handshake only) → fan snapshot → auto-load user profile OR park-off fallback → refresh UI.

## Testing evidence

| Test | Result |
|------|--------|
| 0x741 + 0x7C6 only (service stop/restart) | Works (other regs persisted from previous run) |
| 0x741 + 0x7C6 only (cold reboot) | FAILSAFE — CPU locked 800MHz |
| Full handshake without 0x726/0x727 | FAILSAFE — CPU locked 800MHz |
| Full handshake WITH 0x726/0x727, no fan table write (cold reboot, wiped EC) | Works — CPU unthrottles, fans run at EC internal default (~25%), fan table stays 0x00 until user applies a curve |

## What we got wrong along the way

1. Tested with service stop/restart instead of cold reboot. Registers 0x708/0x709/0x748 persisted from the previous service run, hiding the fact that they're required.
2. `Oem6Apply()` was coded as pulse-and-clear (write 0x04 then 0x00) — immediately undid 0x7C6.
3. `ec_handshake_test.exe` 0x741 mode had a bug (wrote back 0x00 instead of 0x05).
4. Assumed the fan table write was load-bearing for failsafe. It wasn't — `0x726`/`0x727` were the missing piece. The fan table assumption came from correlation (adding the table write fixed the symptom) rather than isolation.
5. Assumed cold-boot fan table was "uninitialized garbage". It's actually all-0x00, which broke two detection functions that were looking for 0xFF padding (`DetectActiveEntriesFromList` returned 16 on all-zero, `DetectGpuPresent` returned true on all-zero). Fixed with explicit zero handling + `IsValid()` gating in `MainForm` detection.
