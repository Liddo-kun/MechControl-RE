# GPU Hotspot via PawnIO — Implementation Plan

**SUPERSEDED 2026-07-19:** Do not implement or submit the custom
`NvBlackwellTherm.p` described below. Active upstream PawnIO.Modules PR #77
already provides the GB20x raw thermal interface. Continue from the replacement
plan: `notes/gpu_hotspot_pr77_plan.md`.

**Historical status: IMPLEMENTED IN SOURCE; hardware validation and upstream signing pending.**
The module, shared C# provider, standalone probe, settings gate, monitor row, and
trimmed-app integration were implemented on 2026-07-18. The module compiles with
Pawn 4.1.7152 and the exact upstream CI flags with no warnings; the C# decode and
fallback self-test passes. PawnIO is not installed on the test machine yet, so
the idle/load/power-gating validation in section 8 has not run. No signed module
blob exists yet, so production builds remain dark and fail soft. This document is
self-contained. It supersedes the
RE investigation in `notes/rtx50_hotspot.md` for implementation purposes; consult that
file only if you need historical capture data or methodology. Everything required to
build the feature is restated here.

Decided with Jon on 2026-07-18:

- MechControl will read the RTX 50 (Blackwell) GPU hotspot **natively** — no HWiNFO,
  no CPUID/cpuz driver, no shelling out to third-party tools.
- Access mechanism: **PawnIO** (signed, HVCI-compatible, open-source kernel driver
  with sandboxed modules) with a **custom read-only module** we write and upstream.
- We will capture **all channels**: the hardware max aggregate plus every individual
  die channel (see register table). UI exposes one "GPU Hot Spot" value.
- The two unidentified registers (`0xAD021C`, `0xAD06EC`) stay **diagnostic-only**:
  they may be logged behind a flag but must never be named or shown as a temperature
  in the UI (identity unconfirmed; see Background).

## 1. Goal

Add a "GPU Hot Spot" sensor to MechControl for NVIDIA Blackwell (RTX 50 series)
laptop GPUs. NVIDIA removed this sensor from public and partner NVAPI at Blackwell
launch; the die sensors are still readable via PCI BAR0 MMIO. As of July 2026,
HWMonitor 1.65+, HWiNFO, and AIDA64 all read it this way (each via their own
proprietary kernel drivers). MechControl will do the same via PawnIO.

Non-goals: writing any GPU register (read-only feature, always), replacing the EC
`gpuTemp` sensor (it remains the general GPU temperature on all models), supporting
pre-Blackwell hotspot (those still work through other tools via NVAPI private calls
we don't use).

## 2. Background facts (verified on the test system, 2026-07-18)

Established by RE of HWMonitor 1.65/1.65.1 + a constrained BAR0 page sweep on Jon's
machine (RTX 5090 Laptop GPU, `10DE:2C18`, BDF `0000:02:00.0`, driver 595.97).
Independently corroborated by Igor's Lab (IBHE article, July 2026): THERM block at
`BAR0+0xAD0000`, six-DWORD BJT sensor array at `BAR0+0xAD0A90` on GB202 and GB206.

### 2.1 Register map (all offsets are BAR0-relative)

THERM page: `0xAD0000`–`0xAD0FFC`, aligned 32-bit reads only.

| Offset | Identity | Production role |
|---|---|---|
| `0xAD0A90` | Individual die channel (HWMonitor 1.65's visible "Hot Spot"; hottest under load) | capture |
| `0xAD0A94` | Individual die channel | capture |
| `0xAD0A98` | Individual die channel | capture |
| `0xAD0A9C` | Individual die channel (hottest at idle) | capture |
| `0xAD0AA0` | **Hardware hotspot-max aggregate** — equals max of the other five, verified 135/135 samples with zero deviation through an idle→load→cooldown regime change | **primary UI value** |
| `0xAD0AA4` | Individual die channel | capture |
| `0xAD0AD0`–`0xAD0AE4` | Mirrors of `0xAD0A90`–`0xAD0AA4` (same sensors, duplicate reads within 0.25 C through a 24 C swing); `0xAD0AE0` is the old-map max aggregate | optional cross-check only |
| `0xAD00BC` | Scratch/marker register, reads `0x000000FF` on examined Blackwell GPUs | plausibility gate |
| `0xAD021C`, `0xAD06EC` | **Unknown.** ~37 C water-cooled idle, only +2–3 C under ~223 W load, full 1/256 C resolution (die channels quantize at 1/32 C), intermittent validity. VRM identity considered unlikely. | diagnostic-only, never named/shown |

The whole page also contains additional die-like channels/mirrors (`0xAD0074`,
`0xAD0104`, `0xAD013C/014C`, `0xAD0460/0464`, `0xAD04B8–04C4`, `0xAD0844/0848`,
`0xAD0B50–0xAD0B7C`); none is needed for production.

### 2.2 Decode and validity (apply to every read)

```text
valid   = (raw & 0x40000000) != 0          // bit 30
reject  raw == 0xFFFFFFFF                  // power-gated dGPU returns all-ones,
                                           // which FALSELY passes the bit-30 test
tempC   = (raw & 0xFFFF) / 256.0           // 8.8 fixed point
require 0 < tempC < 130                    // plausibility clamp
```

A power-gated/sleeping dGPU returning all-ones is normal — report the sensor as
"unavailable" for that cycle, don't error, and don't wake the GPU to poll (do NOT
initialize NVML/NVAPI or run nvidia-smi as a "power state check"; those can wake
it — the harmless all-ones MMIO read IS the check).

### 2.3 Support gate (multi-model safety)

Never hardcode Jon's device ID (`10DE:2C18`) or bus number. Feature is enabled only
when ALL of these pass at runtime:

1. PawnIO driver installed and module loads.
2. Module found an NVIDIA (`VID 0x10DE`) display-class PCI device and mapped its
   BAR0 THERM page.
3. Scratch check: `0xAD00BC` reads `0x000000FF`.
4. At least one of the six channels reads valid + plausible (or all-ones, which
   means "supported but asleep" — treat as supported if the scratch check passed
   while awake at least once).

Otherwise the sensor is hidden entirely (non-NVIDIA models, pre-Blackwell NVIDIA,
no PawnIO). Nothing about this feature goes in the model profile JSONs — it is
GPU-detected, not EC/model-detected. EC `gpuTemp` behavior is unchanged everywhere.

## 3. Why PawnIO, and its rules

- Signed and HVCI-compatible (works on machines with Memory Integrity ON — unlike
  the test rig, other supported models may have it enabled). Used in production by
  LibreHardwareMonitor and FanControl.
- Driver: `https://pawnio.eu` (installer; source at `github.com/namazso/PawnIO`).
  Two editions: **official** (signed; only loads module blobs ECDSA-signed by the
  project) and **unrestricted** (unsigned driver, no module signature checking —
  for development only, requires driver signature enforcement disabled to load).
- Modules: `github.com/namazso/PawnIO.Modules` — Pawn scripts (`.p`) compiled to
  AMX, wrapped in a blob (`4-byte LE signature length || ECDSA signature || AMX
  image`; unsigned dev blob = 4 zero bytes + AMX). Signed builds ship in that
  repo's GitHub Releases.
- Maintainer's acceptance bar (Issue #27, paraphrased): module signing exists to
  avoid AV blocklisting, not as a security boundary (admin-only ACL is the
  boundary). Modules must not provide malware/cheat primitives — specifically no
  arbitrary physical-memory mapping; resolve addresses module-side and expose only
  constrained access. **Our module is read-only, resolves the GPU BAR itself, and
  exposes only aligned dword reads inside one 4 KB thermal page — comfortably
  inside the bar.**
- Licensing: modules loaded into PawnIO must be GPL-compatible; the project
  recommends **LGPL-2.1** for modules. Plan for our module to carry LGPL-2.1 (it
  will live in their repo; MechControl itself only talks to the driver via
  DeviceIoControl/PawnIOLib, which the PawnIO license exception explicitly permits).
- Client API (`PawnIOLib.dll`, installed with the driver under
  `C:\Program Files\PawnIO\`): `pawnio_version`, `pawnio_open(&handle)`,
  `pawnio_load(handle, blob, size)` (one blob per handle),
  `pawnio_execute(handle, "ioctl_name", ulong64[] in, inCount, ulong64[] out,
  outCount, &returnedCount)`, `pawnio_close(handle)`. Function names ≤ 31 chars.
  Trivially P/Invokable from C#; LibreHardwareMonitor's source has a working C#
  interop to crib from, and PR #4 in PawnIO.Modules has a complete Python ctypes
  example showing the exact signatures.

## 4. Architecture

```
MechControl (C#, admin)                         Kernel
┌─────────────────────────────┐
│ MonitorLoop                 │
│   └─ BlackwellGpuTherm      │    pawnio_execute("ioctl_therm_read", ...)
│        Provider             │ ──────────────────────────────────────────┐
│   └─ PawnIoClient (P/Invoke │                                           ▼
│        of PawnIOLib.dll)    │                                  ┌────────────────┐
└─────────────────────────────┘                                  │ PawnIO driver  │
                                                                 │  └─ NvBlackwell│
   ships NvBlackwellTherm.blob in app dir                        │     Therm.amx  │
   (signed blob from PawnIO.Modules release)                     │  (maps BAR0+   │
                                                                 │  0xAD0000 page,│
                                                                 │  read-only)    │
                                                                 └────────────────┘
```

## 5. The PawnIO module (`NvBlackwellTherm.p`)

New module to be written, tested locally, then PR'd to `namazso/PawnIO.Modules`.

Behavior:

- `main()`: enumerate PCI config space for display-class (`0x03`) devices with
  vendor `0x10DE`. For each (support up to a small fixed count, e.g. 4): read BAR0
  from config offset `0x10` (handle the 64-bit BAR case — memory BAR, type bits
  `0b10` mean 64-bit, upper half in `0x14`), then `io_space_map` **only**
  `[BAR0 + 0xAD0000, +0x1000)`. Store per-GPU: BDF, vendor/device ID, mapping.
  If no NVIDIA GPU: return not-supported status.
- IOCTL surface (all read-only, client never supplies an address):
  - `ioctl_get_count` → number of NVIDIA GPUs found.
  - `ioctl_get_info` in: `[gpuIndex]` → out: `[vendorDeviceId, encodedBDF]`.
  - `ioctl_therm_read` in: `[gpuIndex, pageOffset]` → out: `[rawDword]`.
    Validate: index in range, `pageOffset <= 0xFFC`, `(pageOffset & 3) == 0`.
    `pageOffset` is relative to `0xAD0000` so the module physically cannot read
    outside the THERM page.
- `unload()`: unmap everything.
- No writes, no arbitrary offsets, no physical-memory pass-through, x64-only gate
  via `get_arch()` unless ARM64 is trivially supportable.

Implementation references inside the PawnIO.Modules repo:

- `include/pawnio.inc` (pulls in `core.inc`, `util.inc`, `ntstatus.inc`,
  `native.inc`, `extra.inc`); MMIO natives (`io_space_map`, `virtual_read_dword`)
  are in `native.inc`.
- PCI config access: check `native.inc` for dedicated PCI config natives first
  (VERIFY); if absent, use port I/O natives with the legacy `0xCF8/0xCFC`
  mechanism (fine for bus ≤ 255; the I2cPIIX4 module PR shows config-space access
  precedent).
- Existing modules are the style guide; match their structure, status codes, and
  `DEFINE_IOCTL` conventions.

Keep the module architecture-generic on the GPU side: it maps the THERM page for
any NVIDIA display device and leaves Blackwell gating to the client (scratch
check + validity). This keeps the module dumb, small, and reviewable. If upstream
review prefers a device-side gate, add the `0xAD00BC == 0xFF` check in `main()`.

## 6. MechControl integration

New files (suggested):

- `MechControl/Hardware/PawnIo/PawnIoClient.cs` — thin P/Invoke wrapper
  (`pawnio_open/load/execute/close`, blob loading from app dir, availability
  probe: DLL present + open succeeds). No new NuGet deps; trim-safe (pure
  P/Invoke, no reflection; any JSON stays on `AppJsonContext`).
- `MechControl/Hardware/PawnIo/BlackwellGpuThermProvider.cs` — the feature:
  - Startup probe implementing the support gate (section 2.3). Result: one of
    `Unsupported` (hide everything), `Available`, `Asleep`.
  - Per-poll: read all six channels `0xAD0A90`–`0xAD0AA4` (page offsets
    `0xA90`–`0xAA4`) + scratch. Decode per section 2.2.
  - **Hot Spot value = `0xAD0AA0`** (page offset `0xAA0`). Fallback: if `AA0` is
    invalid this cycle but ≥1 individual channel is valid, use max of the valid
    individual channels (this is exactly what HWMonitor 1.65.1 computes).
  - Keep the individual channels: log them at debug level, and consider exposing
    them as perf counters alongside the existing `\MechControl\*` counters.
  - All-ones on every channel → report unavailable this cycle (dGPU power-gated);
    no error spam (log state transitions only, like the EC watchdog does).
  - Optional diagnostic flag (settings.json, default off) to also log page
    offsets `0x21C` and `0x6EC` raw — never decoded into a named UI temperature.
- UI: one new row "GPU Hot Spot" in the monitor panel, visible only when the
  provider probe passed. Keep the existing EC `gpuTemp` row untouched and
  labeled as-is.
- Settings: user toggle to disable the feature (skips PawnIO entirely when off).

Polling cadence: same as the existing monitor loop; one `pawnio_execute` per
register per cycle is microseconds of kernel time — no batching needed initially.

## 7. Dev environment — what to clone/install

| Thing | Needed? | Notes |
|---|---|---|
| `github.com/namazso/PawnIO.Modules` (clone) | **Yes** | Provides `include/*.inc` framework, existing modules as style reference, build tooling, and it's the PR target. Clone to `C:\Users\Jon\code\PawnIO.Modules` (outside the Mechcontrol repo). Keep our module's working copy in `Mechcontrol/tools/pawnio/NvBlackwellTherm.p` (version-controlled with us) and sync into the clone for builds/PR. |
| `github.com/namazso/PawnIO` (driver source, clone) | No | Only for reference if IOCTL/blob details need checking. Install the driver from the pawnio.eu release instead. |
| PawnIO driver install | **Yes** | Dev: "unrestricted edition" from the installer's developer option. Production: official signed edition. One at a time. |
| Pawn compiler (`pawncc`) | **Yes** | VERIFY exact source: check PawnIO.Modules' CI workflow / build scripts for the expected compiler version and flags; build our `.p` the same way CI does. |
| `PawnIOLib.dll` | Comes with the driver install (`C:\Program Files\PawnIO\`) | P/Invoke target. |

Dev-machine caveat (Jon's machine): loading the **unrestricted** (unsigned)
edition requires disabling driver signature enforcement — either
`bcdedit /set testsigning on` (needs Secure Boot off) or the per-boot advanced
startup option. VBS/HVCI is already off on this machine, but Secure Boot state is
unverified. **This is a boot-configuration change: ask Jon before touching it**,
and plan to revert once the signed upstream module exists. Dev blob format:
prepend 4 zero bytes to the compiled `.amx`.

## 8. Validation plan (on Jon's machine, before any PR or UI exposure)

Build a small standalone harness first (console tool in `tools/`, can be modern C#
run via `dotnet`, or C# 5 for `csc.exe` — either is fine since it P/Invokes
PawnIOLib, no framework dependency on the app):

1. **Probe**: module loads, finds `10DE:2C18`, scratch reads `0xFF`.
2. **Idle sanity**: six channels decode to plausible values; known idle
   relationship holds: `0xAD0AA0 == 0xAD0A9C` (aggregate tracks the
   hottest-at-idle channel).
3. **Cross-tool check**: run HWMonitor 1.65.1 side by side (different driver, no
   device conflict with PawnIO) — our `AA0` should match its "Hot Spot" within
   quantization (die channels step in 1/32 C).
4. **Load pulse** (Jon runs/approves the workload — FurMark GL 60 s as used
   before): verify `AA0 == max(five individual channels)` every sample, argmax
   handoff from `A9C` (idle) to `A90` (load), prompt rise and decay.
5. **Power-gating**: with the dGPU idle/gated (if achievable on this always-on
   machine), confirm all-ones handling reports "unavailable" without waking it.
6. Only after 1–5 pass: wire the provider into MechControl behind the probe, run
   the standard build (expect 0 errors / exactly 2 pre-existing CS8602 warnings),
   deploy per the AGENTS.md redeploy recipe, verify in `mechcontrol.log`.

## 9. Upstreaming and production rollout

1. Finish local validation with the unsigned module.
2. PR `NvBlackwellTherm.p` to `namazso/PawnIO.Modules` (read the repo wiki's
   "Contribution guidelines" page first; LGPL-2.1 header; module does read-only,
   page-constrained MMIO — cite the maintainer's Issue #27 guidance if asked).
3. When a signed release containing the module ships: switch Jon's machine to the
   official signed PawnIO edition, restore driver signature enforcement, ship the
   signed `.blob` in the MechControl app directory.
4. MechControl does not bundle the PawnIO installer initially: the provider
   detects absence and the UI can show a one-line "install PawnIO to enable GPU
   Hot Spot" hint. (Bundling/auto-install is a later decision — check
   redistribution terms then.)
5. Until the signed module exists, the feature can ship dark: code merged, hidden
   on every machine without the module, fully functional on the dev machine.

## 10. Risks and open decision points

- **NVIDIA may close the MMIO path in a future driver** (they've made no
  statement as of 2026-07-18; every tool doing this — HWMonitor, HWiNFO, AIDA64 —
  shares the risk). Feature must fail soft to "hidden," never break monitoring.
- **Upstream acceptance/timing is not guaranteed.** Fallback options if rejected:
  keep the feature dev-only on the unrestricted edition (ugly), or revisit an
  own-signed driver (EV cert + attestation; previously judged not worth it).
- **Blob distribution**: signed blobs come from PawnIO.Modules releases; pin the
  release version and hash in the repo when integrating.
- **Sensor naming**: "GPU Hot Spot" (matches industry convention). Final UI
  wording is Jon's call.
- **Desktop GPUs / other laptops**: register map verified on GB202/GB206 laptop +
  desktop parts per Igor's Lab, but our validation is one machine. The gate
  (section 2.3) must do the work on unknown hardware — when in doubt, hide.
- VERIFY items called out inline: pawncc toolchain/source, PCI config natives in
  `native.inc`, Secure Boot state on Jon's machine, exact contribution guidelines.

## 11. References

- Historical RE record (baggage, not needed for implementation):
  `notes/rtx50_hotspot.md` — full capture logs, HWMonitor decompilation details,
  rejected hypotheses. The register facts in section 2 above are the distilled,
  final conclusions from it.
- Existing constrained-read probe tools (useful as decode reference, NOT
  production path — they use CPUID's proprietary driver):
  `tools/BlackwellThermalPageSweep.cs`, `tools/BlackwellThermalCandidatePoll.cs`.
- PawnIO: `https://pawnio.eu`, `github.com/namazso/PawnIO`,
  `github.com/namazso/PawnIO.Modules` (+ wiki: Getting started, Using PawnIO
  Modules, Contribution guidelines), Issue #27 (maintainer's module-acceptance
  guidance), PR #4 (client interop example).
- LibreHardwareMonitor source — production-grade C# PawnIO interop reference.
- Igor's Lab IBHE article (independent confirmation of THERM page, sensor array,
  scratch register):
  `https://www.igorslab.de/en/blackwell-hotspot-ibhe-estimation-register-findings-download-nvidia-question/`
