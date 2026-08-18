# GPU Hot Spot via PawnIO PR #77 - Integration, Validation, and Upstream Plan

**Status: SHELVED 2026-07-19 (Jon's decision — feature will not be
implemented).** Phases 1-4 were completed and are dormant-but-working in the
tree (fail-soft without `Nvidia.bin`); Phase 5 hardware validation was blocked
by `namazso/PawnIO.Setup#11` (public unrestricted PawnIO cannot load unsigned
modules) and the validation offer was withdrawn from PR #77. See
`notes/gpu_hotspot_pr77_validation.md` for the full record. If ever revived:
drop a signed `Nvidia.bin` beside the exe and resume at Phase 5/7.

Originally: **AGREED PLAN** written 2026-07-19 for a new session to execute.
This document supersedes `notes/gpu_hotspot_pawnio_plan.md` for all
implementation decisions.

The prior session built a working transitional MechControl integration and a
competing local Pawn module before discovering that upstream PawnIO.Modules PR
[#77](https://github.com/namazso/PawnIO.Modules/pull/77) already implements the
same NVIDIA GB20x thermal access. Do not continue or submit our competing
module. Selectively refactor the reusable app work around PR #77, validate the
exact PR code on Jon's RTX 5090 Laptop GPU, then post the results to PR #77. If
validation finds a module defect, propose a focused fix to #77 rather than
opening another competing module PR.

## 1. Executive Decision

Use upstream PR #77 as the module implementation and expected production ABI.

Do **not** wholesale-revert the current work. Retain the generic PawnIO client,
temperature decoding, aggregate fallback, settings, monitor integration, UI,
and standalone probe. Remove our `NvBlackwellTherm.p` and replace only the
module-facing assumptions and code.

The seven agreed phases are:

1. Pin an exact PR #77 commit for development and validation.
2. Use `Nvidia.p` / `Nvidia.bin` and `ioctl_read_thermal_registers`.
3. Discover NVIDIA PCI BDFs passively through Windows, without NVAPI/NVML.
4. Keep the provider loaded and retry if support cannot be proven at startup.
5. Validate the exact PR #77 module and MechControl consumer end to end.
6. Post a concise evidence report to PR #77 and propose focused fixes if needed.
7. After merge, pin and ship the official signed `Nvidia.bin` release artifact.

No production release may contain a zero-signature development module or
require users to enable Windows test-signing.

## 2. Upstream State to Recheck First

As of 2026-07-19:

- PR: `https://github.com/namazso/PawnIO.Modules/pull/77`
- Author: `DevTechProfile` / CapFrameX
- Current head: `e1b8f133319daa91edc9785688357f6d0f0894ab`
- Source file: root-level `Nvidia.p`
- State: open, mergeable, with an older maintainer `CHANGES_REQUESTED` review.
- The requested mapping-cache/removable-device work appears implemented in the
  current head, but the review has not yet been formally re-approved.
- PR #78 is an overlapping implementation. Its author and the #77 author agreed
  publicly that #77 should be the consolidation base.
- The maintainer explicitly recommended returning raw values and keeping
  decoding in userspace.

**Do not assume that head SHA or ABI is still current in a new session.** Fetch
the PR metadata and source again before editing. Record the exact source commit
and SHA-256 used for every hardware result. If the PR changes after testing,
diff the changes and rerun all affected validation before posting evidence.

## 3. Ground Truth That Does Not Change With the Pivot

### 3.1 Six GB20x channels

PR #77 returns six contiguous raw DWORDs:

| Output index | BAR0 offset | MechControl interpretation |
|---:|---:|---|
| 0 | `0xAD0A90` | Individual die channel; hottest under load on Jon's GPU |
| 1 | `0xAD0A94` | Individual die channel |
| 2 | `0xAD0A98` | Individual die channel |
| 3 | `0xAD0A9C` | Individual die channel; hottest at idle on Jon's GPU |
| 4 | `0xAD0AA0` | Empirical hardware maximum aggregate |
| 5 | `0xAD0AA4` | Individual die channel |

The module must return raw values only. MechControl applies:

```text
valid   = raw != 0xFFFFFFFF && (raw & 0x40000000) != 0
tempC   = (raw & 0xFFFF) / 256.0
require 0 < tempC < 130
```

The UI value remains:

1. `0xAD0AA0` (output index 4) when valid.
2. Otherwise, max of valid individual indices `0,1,2,3,5`.
3. If none is valid, unavailable for that cycle.

This selection is a MechControl policy, not part of the upstream module ABI.
PR #77 currently documents channel 0 as the value displayed by HWMonitor. Our
validation must report measured behavior precisely without demanding that the
raw-access module choose or decode a channel.

All six channels equal to `0xFFFFFFFF` is the expected power-gated/asleep case.
Do not initialize NVAPI, NVML, run `nvidia-smi`, or use another API merely to
check power state; those can wake the discrete GPU.

### 3.2 PR #77 ABI at the pinned head

```text
module source: Nvidia.p
release file: Nvidia.bin

ioctl_read_thermal_registers
  input:  [bus, device, function]
  output: six raw DWORDs for 0xAD0A90..0xAD0AA4
```

At the current head, the module:

- Requires x64.
- Validates NVIDIA vendor and display class.
- Requires PCI memory decoding to be enabled.
- Accepts only ordinary 32-bit or 64-bit memory BAR0 types.
- Gates GB202/203/205/206/207 through `NV_PMC_BOOT_0`.
- Caches one 24-byte thermal mapping.
- Revalidates BDF, device identity, and BAR0 on every request.
- Remaps when the target or BAR changes.
- Unmaps on device failure and module unload.
- Performs no writes.

`main()` succeeds on x64 without selecting a GPU. The caller supplies a BDF to
the IOCTL. Therefore module load success alone does not prove Blackwell support.

The official signed artifact format is `.bin`, not `.blob`. It is a 4-byte
little-endian signature length, an RSA PKCS#1 v1.5 SHA-256 signature, then the
AMX image. An unrestricted-development artifact uses four zero bytes followed
by AMX, but must remain ignored tooling output and never enter release packages.

## 4. Current Local State: Keep vs. Remove

The Mechcontrol worktree was already dirty before this feature. Do not revert,
delete, stage, or alter unrelated user files or capture archives.

### 4.1 Reuse and adapt

- `MechControl/Hardware/PawnIo/PawnIoClient.cs`
  - Dynamic `PawnIOLib.dll` loading and exact native ABI are useful.
  - Keep generic open/load/execute/close behavior.
- `MechControl/Hardware/PawnIo/BlackwellGpuThermProvider.cs`
  - Keep decode, plausibility, `AA0` priority, fallback, transition logging,
    and no-error-spam behavior.
  - Replace custom module discovery/read IOCTLs with PR #77 ABI and state model.
- `MechControl/Monitoring/EcMonitor.cs`
  - Keep optional-provider polling separate from EC failure accounting.
- `MechControl/UI/MonitorPanel.cs`
  - Keep the `GPU Hot Spot` row, but make visibility/state dynamic and do not
    label every null value as asleep.
- `MechControl/Profile/UserProfileManager.cs`
  - Keep `gpuHotspotEnabled` and default-off diagnostic setting if still useful.
- `MechControl/UI/SystemInfoTab.cs` and `MainForm.cs`
  - Keep user toggle/status integration; update provider creation and disposal.
- `tools/pawnio/BlackwellThermProbe/`
  - Keep the shared-source harness and decode/fallback tests; adapt to PR #77.
- `architecture.md`, `.gitignore`, and project content rules
  - Update names, ABI, provenance, and signed-artifact handling.

### 4.2 Remove or replace

- Delete `tools/pawnio/NvBlackwellTherm.p`.
- Delete `C:\Users\Jon\code\PawnIO.Modules\NvBlackwellTherm.p` and its ignored
  generated `.amx`; these were our competing upstream candidate.
- Delete the ignored zero-signature `tools/pawnio/NvBlackwellTherm.blob`.
- Remove custom `ioctl_get_count`, `ioctl_get_info`, and `ioctl_therm_read`
  assumptions from the provider and probe.
- Remove app packaging rules for `NvBlackwellTherm.blob`.
- Remove production diagnostic reads of unknown offsets `0x21C` and `0x6EC`;
  PR #77 intentionally exposes only the six production channels.
- Correct all ECDSA/`.blob` documentation left by the superseded plan.

Do not use `git reset`, `git checkout --`, or a blanket revert. Make selective
edits with `apply_patch`, preserving concurrent/user changes.

## 5. Target MechControl Architecture

```text
EcMonitor (1 second)
  -> BlackwellGpuThermProvider
       -> passive NVIDIA PCI locator (SetupAPI / Configuration Manager)
       -> PawnIoClient
            -> PawnIOLib.dll
                 -> official PawnIO driver
                      -> signed Nvidia.bin from PawnIO.Modules
                           -> ioctl_read_thermal_registers(B,D,F)
```

The feature remains GPU-detected. Nothing goes into model profile JSON. The EC
`gpuTemp` sensor remains unchanged on every model.

Recommended provider states:

| State | Meaning | UI behavior |
|---|---|---|
| `Disabled` | User toggle off | No PawnIO DLL/driver access; no sensor row |
| `Unavailable` | DLL/module/BDF absent | No sensor row; settings status explains why |
| `Pending` | Module loaded and NVIDIA candidate exists, support not yet proven | Keep provider alive; row hidden; retry quietly |
| `Available` | PR #77 IOCTL succeeded and at least one channel is valid | Show row and temperature |
| `Asleep` | Support was proven and successful IOCTL returned all ones | Show row as unavailable/asleep; no error |
| `Faulted` | Previously supported, but transport/read failed | Show `unavailable`, not `asleep`; log transitions only |

An IOCTL success proves that PR #77's module-side GB20x architecture gate
passed. A startup failure does not prove the GPU is unsupported because a
power-gated device may make `NV_PMC_BOOT_0` unreadable. Keep the loaded module
and retry; do not dispose it permanently after one failed probe.

## 6. Implementation Phases 1-7

### Phase 1 - Pin PR #77 exactly

1. Query PR #77 and record the latest head SHA, source URL, review state, and
   current IOCTL contract.
2. Fetch the PR head into `C:\Users\Jon\code\PawnIO.Modules` as a dedicated
   local branch or detached worktree. Do not overwrite upstream `main` or our
   MechControl worktree.
3. Confirm the source is LGPL-2.1-or-later and unchanged from the PR.
4. Record SHA-256 of `Nvidia.p`.
5. Compile the entire PawnIO.Modules tree with Pawn 4.1.7152 and exact CI flags:

   ```powershell
   pawncc Nvidia.p "-iinclude" "-C64" "-;+" "-(+" "-p"
   ```

6. Require zero compiler warnings.
7. For unrestricted development only, create `Nvidia.bin` as four zero bytes
   plus `Nvidia.amx`. Keep it outside production app content and ignored by git.
8. Put the PR URL, head SHA, source SHA-256, compiler version, and generated
   AMX/development-bin hashes in the validation record.

### Phase 2 - Adopt `Nvidia.bin` and its IOCTL

1. Change the provider module filename from `NvBlackwellTherm.blob` to
   `Nvidia.bin`.
2. Keep `PawnIoClient` generic; do not encode NVIDIA semantics in the interop
   wrapper.
3. Replace seven page-read executions with one call per poll:

   ```text
   Execute("ioctl_read_thermal_registers", [bus, device, function], 6)
   ```

4. Require exactly six outputs and retain their raw 32-bit values.
5. Decode and select the displayed value in C# using section 3.1.
6. Remove scratch and unknown-register reads, since PR #77 does not expose them.
7. Keep the harness able to accept an explicit development `Nvidia.bin` path.
8. Do not add a project `Content` item for an unsigned development artifact.
   Add the production content rule only in phase 7 for a pinned signed file.

### Phase 3 - Passive PCI BDF discovery

PR #77 deliberately requires the consumer to supply bus, device, and function.
Never hardcode Jon's `02:00.0` or `10DE:2C18`.

1. Add one small Windows PCI locator, suggested name:
   `MechControl/Hardware/PawnIo/NvidiaPciDeviceLocator.cs`.
2. Enumerate present display-class devices using SetupAPI / Configuration
   Manager properties.
3. Filter hardware IDs for `PCI\VEN_10DE`.
4. Read the PCI bus number and PCI address/device-function properties. Verify
   Windows' address encoding against Device Manager and the known test BDF
   before relying on it; do not guess or parse a machine-specific string.
5. Return all NVIDIA display BDF candidates. Do not impose an arbitrary four-GPU
   cap; PR #77 can revalidate/remap per request.
6. Validate ranges: bus `0..255`, device `0..31`, function `0..7`.
7. Log discovered instance ID, VID:DID, and BDF for diagnostics without serials
   or machine-specific allowlists.
8. Do not initialize NVAPI/NVML, call `nvidia-smi`, open a rendering context, or
   invoke any API that could wake a sleeping GPU.
9. Add deterministic tests for property/address decoding. On Jon's machine,
   require discovery of `10DE:2C18` at `02:00.0` without hardcoded expected data
   in production code.

### Phase 4 - Retry-safe startup and runtime state

1. Split transport creation from hardware proof:
   - DLL/module load + candidate BDFs -> provider can enter `Pending`.
   - Successful PR #77 IOCTL -> support confirmed.
2. Do not dispose the provider after one startup IOCTL failure.
3. While pending, retry quietly at a conservative cadence (for example every
   10-30 seconds, not every UI tick on a known pre-Blackwell device). Once a
   candidate succeeds, select it and poll at the normal 1-second cadence.
4. If multiple NVIDIA candidates exist, try each until one passes the module's
   architecture gate and channel validity checks.
5. On first successful IOCTL:
   - If at least one channel is valid, transition to `Available`.
   - If all six are all-ones, the module's architecture gate already passed;
     transition to `Asleep` and retain confirmed support.
6. After support is confirmed:
   - All ones -> `Asleep`.
   - Some valid channels -> `Available` with aggregate/fallback selection.
   - Successful IOCTL but no valid channels and not all ones -> `Faulted` or
     unavailable for that cycle.
   - Transport/HRESULT failure -> `Faulted`, retry, and do not affect EC monitor
     failure accounting.
7. Propagate provider state through `MonitorData`; do not collapse every null
   temperature into `asleep`.
8. Create the monitor label once but keep it hidden until support is confirmed,
   or add it on a support-confirmed event. It must become visible without an app
   restart when a startup-sleeping GPU wakes and validates.
9. Log state transitions only unless diagnostic logging is explicitly enabled.
10. Dispose the provider both during normal `FormClosing` ordering and from
    `MainForm.Dispose(bool)` so nonstandard disposal cannot leak the module/MMIO
    mapping until process exit.
11. User toggle off must skip PawnIO entirely on the next launch.

### Phase 5 - Validate exact PR #77 implementation

Run the full validation matrix in section 7. Validation must use the exact
pinned PR #77 source and development binary, not our deleted module. Do not wire
an unvalidated binary into the deployed app.

### Phase 6 - Contribute evidence and fixes to PR #77

After validation:

1. Prepare a concise, sanitized Markdown report and machine-readable CSV.
2. Post one evidence comment on PR #77, not a new issue or competing PR.
3. State the exact PR head SHA tested, all hashes, hardware/driver/PawnIO
   versions, methodology, pass/fail table, key measurements, and limitations.
4. Add only new evidence. PR #77 already contains reverse-engineering summaries,
   desktop RTX 5090/5060 Ti reports, and the corrected six-channel decode. Jon's
   value is exact-module validation on an RTX 5090 **Laptop**, sustained
   idle/load/cooldown data, `AA0` aggregate proof, power-gating behavior, and
   MechControl consumer integration.
5. Do not upload proprietary binaries or decompiled code. Link public references
   and provide hashes/method summaries only.
6. Obtain Jon's explicit approval before posting with `gh`.

If a module issue is found:

1. First reproduce it with the standalone probe against the exact PR head.
2. Separate module defects from MechControl BDF discovery/consumer defects.
3. Capture the smallest reproducible data set.
4. Comment on PR #77 with the failure, expected behavior, exact hardware/SHA,
   and a focused proposed correction.
5. If code is warranted, base a minimal patch on PR #77's head and offer the
   commit/diff to its author. Coordinate rather than opening PR #80 or replacing
   #77.
6. Open a separate PR only if the #77 author or maintainer explicitly requests
   that workflow.
7. Re-run the full affected matrix on the proposed fix before recommending it.

Likely review points to test, not assume:

- Channel-0 wording versus empirical `AA0` hardware aggregate behavior.
- Startup and runtime behavior when the laptop GPU is power-gated.
- PCI/BAR revalidation across sleep/resume.
- Multi-GPU candidate switching and cached remapping.
- Malformed/all-ones BAR upper DWORD or address overflow handling.
- Whether a successful cached mapping can later return all ones safely.

### Phase 7 - Consume the official signed release

Only after PR #77 merges and PawnIO.Modules publishes a signed release:

1. Download the exact release ZIP; do not automatically follow `latest`.
2. Verify that `Nvidia.bin` corresponds to the expected merged source/ABI.
3. Record release tag, source commit, download URL, filename, and individual
   SHA-256 in repository documentation/build metadata.
4. Preserve the signed bytes exactly.
5. Add the signed file to MechControl packaging and verify its hash during the
   build. Never substitute the zero-signature development file.
6. Include LGPL notice, PawnIO.Modules `COPYING`, and exact corresponding source
   or a compliant source URL in the distribution/third-party notices.
7. Validate with the official signed PawnIO edition, test-signing off, Secure
   Boot and Memory Integrity compatible where available.
8. Initially direct users to `pawnio.eu` for the driver. Do not bundle or
   auto-install PawnIO until redistribution/update behavior is separately
   designed.
9. If PawnIO or `Nvidia.bin` is absent, hide the sensor and leave all other
   MechControl behavior unchanged.
10. Treat Authenticode signing of MechControl itself as a separate SmartScreen
    distribution decision.

## 7. Validation Matrix

### 7.1 Offline and unit validation

- [ ] Re-fetch PR #77 and pin its current head.
- [ ] Inspect the diff and open review threads; confirm ABI before coding.
- [ ] Compile `Nvidia.p` with Pawn 4.1.7152 and exact CI flags, zero warnings.
- [ ] Confirm module has no writes and maps only the six-register span.
- [ ] Confirm both C# projects build with zero new warnings.
- [ ] Run decode tests:
  - valid bit-30 sample
  - all-ones rejection
  - missing-valid-bit rejection
  - zero and `>=130C` rejection
- [ ] Run selection tests:
  - valid `AA0` wins
  - invalid `AA0` falls back to max of five individuals
  - no valid channel -> unavailable
- [ ] Add provider-state tests with a fake module executor:
  - pending startup failure followed by success
  - first successful all-ones sample
  - available -> asleep -> available
  - available -> HRESULT failure -> recovery
  - malformed output count
  - multiple candidate BDFs
- [ ] Test BDF property/address decoding independently of Jon's hardcoded BDF.
- [ ] Run `git diff --check` in both relevant repositories.

### 7.2 Driver setup gate

Prefer an official maintainer-signed test `Nvidia.bin` if one becomes available.
If only an unsigned development module is available:

- [ ] Explain the need for unrestricted PawnIO and Windows test-signing to Jon.
- [ ] Obtain explicit approval before installing the unrestricted driver or
  changing boot configuration.
- [ ] Record pre-change Secure Boot, VBS/HVCI, and BCD state.
- [ ] Plan the exact rollback (`testsigning off` plus reboot) before enabling it.
- [ ] Never ask downstream users to make this change.

### 7.3 Probe and identity

- [ ] Record Windows build, PawnIO/PawnIOLib version, NVIDIA driver, GPU name,
  `10DE:2C18`, and BDF reported by passive discovery.
- [ ] Confirm the module loads and one PR #77 IOCTL returns six DWORDs.
- [ ] Confirm production code contains no Jon-specific DID/BDF.
- [ ] Confirm the selected BDF matches Device Manager/PCI ground truth.
- [ ] Save exact source/AMX/bin hashes.

### 7.4 Idle sanity

- [ ] Capture at least 60 seconds at 1-second cadence.
- [ ] Save timestamp, all six raw DWORDs, all six decoded values, selected hot
  spot, provider state, and comparison-tool value where available.
- [ ] Require all non-sleep samples to satisfy bit 30 and `0 < temp < 130C`.
- [ ] Confirm the known idle relationship: `AA0` tracks the hottest channel,
  usually `A9C` on Jon's system.
- [ ] Compare selected MechControl value with HWMonitor 1.65.1 within expected
  quantization/sampling skew; report actual max/mean difference.

### 7.5 Load and cooldown

Jon must approve/run the workload. The prior reference workload was FurMark GL
for 60 seconds.

- [ ] Capture baseline, load ramp, sustained load, and cooldown in one trace.
- [ ] Record workload settings and GPU power if measured without changing the
  sensor path.
- [ ] Verify `AA0 == max(A90,A94,A98,A9C,AA4)` for every valid sample; report
  sample count and every deviation, not just a summary claim.
- [ ] Confirm expected argmax handoff from idle (`A9C`) toward load (`A90`) if it
  occurs; report observations rather than making it a hard support gate.
- [ ] Confirm prompt rise and plausible cooldown.
- [ ] Compare against HWMonitor 1.65.1 throughout.

### 7.6 Power gating and no-wake behavior

- [ ] Let the discrete GPU idle/gate if achievable.
- [ ] Do not use NVAPI, NVML, `nvidia-smi`, GPU-Z, or another power-state check
  that may itself wake the GPU during this test.
- [ ] Confirm all-ones is reported as asleep/unavailable without error spam.
- [ ] Confirm polling does not visibly wake the GPU.
- [ ] Confirm a startup-sleeping/pending provider later validates and makes the
  row visible without restarting MechControl.
- [ ] Confirm temperature recovers after wake.

### 7.7 Sleep/resume and mapping revalidation

- [ ] Capture BDF and provider state before sleep.
- [ ] Sleep/resume the laptop.
- [ ] Confirm PR #77 revalidates identity/BAR and readings recover.
- [ ] Confirm no stale mapping crash, persistent failure, or GPU wake loop.
- [ ] Record whether BAR/BDF changed; do not claim hotplug coverage if it did not.

### 7.8 App integration and fail-soft behavior

- [ ] Run the standard `build-release.ps1` build.
- [ ] Expect zero errors and only the two known `MainForm.cs` nullable warnings
  during trimmed publish; investigate anything else.
- [ ] Confirm trimmed self-contained publish has no linker warnings.
- [ ] Confirm unsigned `Nvidia.bin` is absent from production archives.
- [ ] With signed/dev module present in an isolated validation build, verify:
  - row hidden while pending/unproven
  - row visible after support confirmation
  - `Available`, `Asleep`, and `Faulted` text are distinct
  - tray-hidden polling continues
  - toggle off skips PawnIO on next launch
  - provider unloads cleanly on every disposal path
- [ ] Test no PawnIO, no module, no NVIDIA GPU, and pre-Blackwell NVIDIA paths as
  far as available; all must fail soft without affecting EC monitoring.
- [ ] No profile JSON may be modified for this GPU-detected feature.

## 8. Evidence Artifact and PR Comment

Create a sanitized report, suggested path:

`notes/gpu_hotspot_pr77_validation.md`

Store machine-readable captures under `captures/` with clear PR head and phase
in the filename. Keep large raw files out of the PR comment; link them from a
stable public commit or Gist only after checking privacy/licensing.

The final PR #77 comment should follow this shape:

```markdown
Validated PR #77 commit `<head-sha>` on an NVIDIA RTX 5090 Laptop GPU
(`10DE:2C18`, passively discovered at `02:00.0`) using NVIDIA driver `<version>`,
PawnIO `<version>`, and PawnIOLib `<version>`.

### Results

| Test | Result |
|---|---|
| Exact module load and BDF validation | PASS/FAIL |
| Six-channel idle validity | PASS/FAIL + sample count |
| HWMonitor 1.65.1 comparison | max/mean delta |
| Idle -> load -> cooldown | PASS/FAIL + sample count |
| `AA0 == max(other five)` | N/N, deviations |
| Power-gated all-ones handling | PASS/FAIL/NOT TESTED |
| Sleep/resume mapping recovery | PASS/FAIL/NOT TESTED |
| MechControl consumer integration | PASS/FAIL |

The consumer used bit 30 for validity and decoded `(raw & 0xFFFF) / 256.0`.
MechControl selects `0xAD0AA0` as the primary value and falls back to max of the
five individual channels if that aggregate is invalid.

Representative samples: `<small table>`

Artifacts and methodology: `<public link>`
Source/module/capture hashes: `<hashes>`

Limitations: one RTX 5090 Laptop system; `<anything untested>`.
```

If validation fails, replace the celebratory summary with a minimal reproducer,
expected/actual result, and proposed fix. Never conceal deviations to improve
acceptance odds.

## 9. Completion Criteria

The pivot is complete only when:

- [ ] Our competing Pawn module and custom ABI are removed.
- [ ] MechControl and the probe consume exact PR #77 ABI.
- [ ] Passive, model-independent NVIDIA BDF discovery works.
- [ ] Startup power-gating does not permanently hide a supported GPU.
- [ ] Runtime states distinguish asleep from faults.
- [ ] Exact PR #77 code passes the applicable validation matrix.
- [ ] Jon approves the final sanitized evidence report.
- [ ] Evidence is posted to PR #77.
- [ ] Any discovered module issues are proposed as focused #77 fixes and
  retested.
- [ ] Production waits for and pins an official signed `Nvidia.bin`.
- [ ] Official-driver validation passes with test-signing disabled.
- [ ] Documentation and third-party notices match the final released ABI,
  license, source commit, and artifact hash.

## 10. References

- PR #77: `https://github.com/namazso/PawnIO.Modules/pull/77`
- PR #78 consolidation discussion: `https://github.com/namazso/PawnIO.Modules/pull/78`
- PawnIO.Modules contribution guidelines:
  `https://github.com/namazso/PawnIO.Modules/wiki/Contribution-guidelines`
- PawnIO module-use guide:
  `https://github.com/namazso/PawnIO.Modules/wiki/Using-PawnIO-Modules`
- PawnIO.Modules CI:
  `https://github.com/namazso/PawnIO.Modules/blob/main/.github/workflows/ci.yml`
- Maintainer security-boundary guidance:
  `https://github.com/namazso/PawnIO.Modules/issues/27`
- Igor's Lab GB20x thermal-register corroboration:
  `https://www.igorslab.de/en/blackwell-hotspot-ibhe-estimation-register-findings-download-nvidia-question/`
- Historical local RE: `notes/rtx50_hotspot.md`
- Superseded local-module plan: `notes/gpu_hotspot_pawnio_plan.md`
