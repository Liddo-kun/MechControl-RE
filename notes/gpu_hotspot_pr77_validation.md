# GPU Hot Spot — PR #77 Validation Record

Working record for the plan in `notes/gpu_hotspot_pr77_plan.md`. Section
numbers below refer to that plan.

## Phase 1 — Pin (completed 2026-07-19)

| Item | Value |
|---|---|
| PR | https://github.com/namazso/PawnIO.Modules/pull/77 |
| Head SHA (pinned) | `e1b8f133319daa91edc9785688357f6d0f0894ab` |
| Local branch | `pr77-e1b8f13` in `C:\Users\Jon\code\PawnIO.Modules` |
| Source file | `Nvidia.p` (root), LGPL-2.1-or-later SPDX header confirmed |
| `Nvidia.p` SHA-256 | `BB6A72B6C9B22F5DD3BDE670CC8D3E6702FCFFDD70AE8F6498C268AC4D6071A8` |
| `Nvidia.p` git blob | `35baf7fe6ae47aa5e3efce601ff168d2c55cb6a8` (API fetch == git checkout) |
| Compiler | Pawn 4.1.7152 (`C:\Temp\tandem\pawn-toolkit\bin\pawncc.exe`) |
| CI flags (verified from `.github/workflows/ci.yml:23`) | `-iinclude -C64 -;+ -(+ -p` |
| Compile result | Success, **zero warnings** |
| `Nvidia.amx` SHA-256 | `09949D6F7BCEEDEA09C57E5CFDD73733B334E58053A15924AEEFDD913140C069` (9528 bytes) |
| Dev `Nvidia.bin` SHA-256 | `4302E2BD029D1FB14D4D5045647067FDB6012ED86B63341DDD5167C5BB1EA2FF` (4 zero bytes + AMX; **dev only, never ship**) |
| Dev artifacts location | `C:\Temp\tandem\pr77\` (outside both repos; `*.amx` is repo-gitignored) |

### PR review state at pin time

- Open, mergeable-clean; last activity 2026-07-18.
- namazso review: `CHANGES_REQUESTED` (2026-07-16), not yet re-reviewed.
- Author implemented raw-values change 2026-07-17 (six contiguous DWORDs
  `0xAD0A90..0xAD0AA4`; obsolete `0xAD0AE0` access removed).
- Third-party report 2026-07-17: works on desktop RTX 5060 Ti, matches
  HWMonitor. All upstream hardware reports remain desktop cards.
- **Open scoping question (2026-07-18, unanswered):** author asked namazso
  whether to keep one general `Nvidia.p` or split into per-domain modules
  (thermal/power/fan). If split is chosen, the filename and IOCTL name will
  change. Keep module/IOCTL names in one place in the provider; re-check this
  thread before Phase 6/7.

### Source-inspection findings (pinned head)

- ABI confirmed exactly as plan §3.2: `ioctl_read_thermal_registers`,
  in `[bus, device, function]` (sizes validated 0..255/0..31/0..7), out = six
  raw DWORDs. Read-only; maps `NV_PMC_BOOT_0` (4 bytes, unmapped after gate
  check) plus the 24-byte thermal span only. GB202/203/205/206/207 gate.
  Revalidates config-space identity + BAR0 on every request; remaps on change;
  unmaps on read failure and unload.
- **D3cold nuance for the consumer state model (plan §5):** `get_bar0`
  re-reads PCI config space per request. A deeply power-gated GPU returns FFs
  from config space, so the vendor check fails and the IOCTL returns
  `STATUS_NOT_SUPPORTED` — the "asleep" case can present as an IOCTL *error*,
  not as six `0xFFFFFFFF` data values. Provider must treat
  `NOT_SUPPORTED`-after-confirmed-support as likely-asleep, not `Faulted`.
  Validate empirically in §7.6; this is laptop-specific evidence upstream
  does not have.

## Phase 2 — PR #77 ABI adoption (completed 2026-07-19)

- `BlackwellGpuThermProvider` now targets `Nvidia.bin` +
  `ioctl_read_thermal_registers([bus, device, function]) -> 6 raw DWORDs`.
  Module/IOCTL names are single constants (upstream rename/split risk).
  One IOCTL per poll; requires exactly six outputs; decode/selection
  unchanged in C#.
- Removed: custom `ioctl_get_count`/`ioctl_get_info`/`ioctl_therm_read`
  assumptions, scratch-register gate, unknown-offset (`0x21C`/`0x6EC`)
  diagnostic reads, `NvBlackwellTherm.p`/`.blob` in `tools/pawnio/`, the
  competing `NvBlackwellTherm.p`/`.amx` in the PawnIO.Modules clone, and the
  `NvBlackwellTherm.blob` Content rules in both csproj files.
- Added `PciBdf` struct; `TryCreate` takes caller-supplied BDF candidates and
  fails before any PawnIO DLL/driver access when none exist. MainForm passes
  an empty candidate list until Phase 3's passive SetupAPI locator lands, so
  the in-app feature is intentionally inert this phase.
- Probe: `BlackwellThermProbe <Nvidia.bin> <bus:device.function> [seconds]`
  (explicit dev-module path per plan; BDF parse covered by self-test).
- `.gitignore`: `tools/pawnio/Nvidia.bin` (dev copy) + existing `*.amx`.
  Docs updated: `tools/pawnio/README.md`, `architecture.md` §1/§7,
  `EcMonitor` comment.
- Verification 2026-07-19: `build-release.ps1` — 0 errors, only the 2 known
  `MainForm.cs` CS8602 warnings, no linker warnings; probe `--self-test` all
  PASS (12 checks incl. new BDF cases); selfcontained release zip contains no
  module artifacts.

## Phase 3 — Passive PCI BDF discovery (completed 2026-07-19)

- Added `MechControl/Hardware/PawnIo/NvidiaPciDeviceLocator.cs`: SetupAPI
  enumeration of present display-class devices on the `PCI` enumerator,
  filtered to `VEN_10DE` via the first `SPDRP_HARDWAREID` entry. BDF from
  `SPDRP_BUSNUMBER` + `SPDRP_ADDRESS` (`(device << 16) | function`), with
  range validation (bus 0..255, device 0..31, function 0..7) and no candidate
  cap. Pure PnP property reads — no NVAPI/NVML/D3D, nothing that can wake a
  gated GPU. No Jon-specific VID/DID/BDF in production code.
- MainForm now feeds `FindCandidates()` into the provider; zero candidates
  still means zero PawnIO DLL/driver access.
- Probe gained `--discover` (passive candidate listing) and 8 new
  deterministic self-tests for hardware-ID parsing and address decoding.

### Address-encoding verification (plan §Phase 3 step 4, Jon's machine)

| Device | DEVPKEY BusNumber/Address | Decoded | Device Manager LocationInfo |
|---|---|---|---|
| NVIDIA RTX 5090 Laptop (`10DE:2C18`) | bus=2, `0x00000000` | `02:00.0` | "PCI bus 2, device 0, function 0" ✔ |
| Intel iGPU (`8086:7D67`) | bus=0, `0x00020000` | `00:02.0` | "PCI bus 0, device 2, function 0" ✔ |

The iGPU row proves the `(device << 16) | function` packing with a nonzero
device number; the locator correctly excluded it (vendor filter) while
discovering `10DE:2C18 02:00.0`, instance
`PCI\VEN_10DE&DEV_2C18&SUBSYS_60401D05&REV_A1\...`.

- Verification 2026-07-19: `build-release.ps1` — 0 errors, only the 2 known
  CS8602 warnings; probe `--self-test` 20/20 PASS; `--discover` exit 0 with
  the expected single NVIDIA candidate.

## Phase 4 — Retry-safe startup and runtime state (completed 2026-07-19)

Kept deliberately simple: one state enum, a tick-counter retry, and a minimal
`IPawnIoExecutor` seam so the state machine is testable with a scripted fake.

- Single `BlackwellGpuThermState` enum: `Pending` / `Available` / `Asleep` /
  `Faulted`. `Create` returns a provider whenever the module loads and NVIDIA
  candidates exist (Pending if the first probe fails); null only for terminal
  conditions (no candidates, no PawnIO, no module file).
- Pending retries once per 15 Poll ticks (1 Hz monitor -> ~15 s cadence, zero
  module traffic between attempts); all candidates tried per attempt; first
  passing BDF binds. Confirmed reads run every tick.
- D3cold handling per the Phase 1 source finding: `STATUS_NOT_SUPPORTED`
  (0xC00000BB / 0xD00000BB HRESULT_FROM_NT) after confirmed support maps to
  `Asleep`, not `Faulted`. `PawnIoClient` now preserves the HRESULT on thrown
  exceptions.
- UI: monitor row created hidden, shown on first non-Pending state (no restart
  needed); `Asleep` shows "-- (asleep)", `Faulted`/no-valid shows
  "-- (unavailable)". Startup status text distinguishes active / power-gated /
  pending / unavailable.
- `MainForm.Dispose(bool)` now also disposes the provider (idempotent; covers
  nonstandard disposal in addition to the existing FormClosing ordering).
  Toggle-off launch path unchanged (skips PawnIO entirely).
- Provider `Poll()` never throws, so GPU failures cannot trip the EC monitor's
  5-consecutive-failure auto-stop.
- Verification 2026-07-19: `build-release.ps1` — 0 errors, only the 2 known
  CS8602 warnings; probe `--self-test` 33/33 PASS, now including scripted
  state-machine scenarios: pending->confirm (with quiet-cadence call counting),
  first-sample-all-ones, available->asleep(D3cold)->available,
  available->faulted->available, malformed output count, and second-candidate
  binding.

## Phase 5 — Driver setup gate: BLOCKED on public unrestricted edition (2026-07-19)

System state: Secure Boot OFF, VBS/HVCI OFF, testsigning ON (armed for the
test-signed driver). PawnIO 2.2.0 (`PawnIO_setup.exe`,
SHA-256 `1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032`,
Authenticode valid, signer `CN=namazso.eu`).

Goal was to load the zero-signature dev `Nvidia.bin` on the unrestricted
driver. Result: **the publicly released 2.2.0 unrestricted edition still
enforces module signatures**, so it cannot load any unsigned module.

Evidence (independent P/Invoke load harness against `PawnIOLib.dll`):

| Module container | `pawnio_load` HRESULT | Meaning |
|---|---|---|
| Official `Echo.bin` (real sig) | `0x00000000` | loads |
| `Echo.amx` + dummy 512-byte sig | `0x80090006` | NTE_BAD_SIGNATURE |
| `Echo.amx` + zero-length sig | `0x80070057` | E_INVALIDARG (0-len sig -> BCrypt) |
| dev `Nvidia.bin` (zero-length sig) | `0x80070057` | E_INVALIDARG |

The dummy-sig case uses a **known-good** Echo AMX, so it isolates signature
enforcement from any AMX-content issue: if the unrestricted bypass were active
the driver would force `STATUS_SUCCESS` and Echo would load (`hr=0`). It does
not. The 2.2.0 source tag *does* contain the
`#ifdef PAWNIO_UNRESTRICTED { DbgPrint(...); status = STATUS_SUCCESS; }` bypass,
so the released unrestricted binary appears to have been built **without**
`PAWNIO_UNRESTRICTED` defined.

Install mechanics learned (all reproducible):
- Silent `PawnIO_setup.exe -install -silent -unrestricted` and the GUI
  "unrestricted" checkbox both initially yielded the **official**
  MS-attestation-signed driver, because two official `pawnio.inf` packages
  (`oem197`=2.1.0, `oem198`=2.2.0) were cached in the DriverStore and PnP binds
  the highest-ranked cached driver for the `SoftwareDevice`-class node.
- A running PawnIO kernel driver is **not** unloaded by uninstall alone; it
  persists until `sc stop`/`sc delete` or reboot. Reinstalling over a loaded
  driver is why earlier attempts kept getting the enforcing edition.
- Fix that finally staged the test-signed unrestricted driver: full uninstall,
  `sc delete PawnIO`, `pnputil /delete-driver oem197.inf /uninstall /force`
  (and `oem198.inf`), reboot, then GUI install with the unrestricted checkbox.
  Confirmed by driver signer flipping to `CN=namazso.eu` (test cert). Even so,
  module-signature enforcement remained — hence the block.

Conclusion: local dev-module validation is not possible with the current
public PawnIO release. The official (MS-signed) edition loads official signed
modules perfectly (Echo `hr=0`), so the stack is otherwise healthy.

### Upstream engagement (2026-07-19, approved by Jon, posted as Liddo-kun)

- The unrestricted-edition defect was already reported as
  `namazso/PawnIO.Setup#11` (nikicat, 2026-06-26; root cause: the
  `releaseunrestricted` build config never passes `-DPAWNIO_UNRESTRICTED=ON`).
  We posted corroborating evidence (HRESULT table, dummy-sig methodology,
  DriverStore edition-switch gotcha):
  https://github.com/namazso/PawnIO.Setup/issues/11#issuecomment-5016724625
- PR #77 comment posted: compile verification at `e1b8f13`, consumer
  integration summary, the D3cold `STATUS_NOT_SUPPORTED` doc suggestion, and
  the ask to namazso for a signed test `Nvidia.bin`:
  https://github.com/namazso/PawnIO.Modules/pull/77#issuecomment-5016724890

**Waiting on namazso.** Options if no response: build unrestricted PawnIO
from master locally (`-DPAWNIO_UNRESTRICTED=ON`, self-sign, testsigning is
already armed), or wait for merge + signed modules release.

Current machine state while waiting: official-edition behavior is fine for
production later, but the machine currently has the **test-signed unrestricted
2.2.0 driver** installed and **testsigning ON**. Rollback recipe when done:
uninstall PawnIO, `bcdedit /set testsigning off`, reboot. (Reinstall official
edition whenever the signed Nvidia.bin ships.)

## FINAL — Feature shelved (2026-07-19)

Jon decided not to implement the GPU hotspot feature. Wrap-up actions:

- Validation-offer withdrawal posted to PR #77 (so namazso doesn't cut a
  signed test build for nothing); the D3cold note was left standing for
  future laptop testers:
  https://github.com/namazso/PawnIO.Modules/pull/77#issuecomment-5324571733
- Issue #11 corroboration left as-is (stands on its own as a bug report).
- Machine rollback performed: PawnIO uninstalled, service deleted,
  `testsigning off`, reboot. Dev artifacts in `C:\Temp\tandem\pr77` deleted.
- Phases 1-4 code (provider, PciBdf, locator, state machine, UI wiring,
  probe) **retained in the tree** — fully dormant and fail-soft without
  `Nvidia.bin`; nothing was ever deployed. Revival = signed `Nvidia.bin`
  beside the exe + resume plan at Phase 5/7.
- `C:\Users\Jon\code\PawnIO.Modules` clone found already deleted at wrap-up
  time (not by this session). No loss: the pinned head SHA and all source/
  artifact hashes are recorded above; re-fetchable from the PR if ever needed.

