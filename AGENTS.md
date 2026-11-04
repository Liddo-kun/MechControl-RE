# Native MechControl implementation

This is a fresh, independent repository for the native rewrite. The existing C#
repository at `C:/Users/Jon/code/Mechcontrol` is a read-only behavioral reference.
Do not create or modify any file there, including build outputs, test outputs,
notes, or Git metadata. Put temporary work in `C:/Temp/tandem`.

## Goals

- Build a Windows x64 C++20 replacement without .NET or WinForms.
- Prioritize lighter packaging, lower memory use, low idle overhead, and simple,
  maintainable code. Measure results instead of assuming native means faster.
- Preserve existing supported features and configuration semantics. Do not add
  features without asking the user. Add logic only when necessary for existing
  behavior, correctness, or the implementation's ownership/lifetime requirements.
- Use native Windows controls, the Windows SDK, C++/WinRT for BLE, and a small
  pinned JSON dependency. Use consistent static C/C++ runtime linking.
- Do not add generic frameworks, speculative abstractions, redundant guards, or
  compatibility machinery without a concrete need.
- Tests should cover consequential regressions or hard-to-observe behavior.
  Prefer existing behavioral fixtures and real workflow smoke checks to routine
  low-level test proliferation.

## Execution

The coordinator assigns one phase to each fresh agent. Start with these shared
instructions and only that phase's plan under `plans/`. Then read the source
interfaces and implementations directly involved in the assigned work. Do not
load prior phase reports, the behavioral contract, or the parity checklist in
full. When a concrete question requires historical evidence, search for the
specific symbol or topic and read only the relevant section. Prior phase reports
are an audit archive, not required onboarding. Future phase plans must carry the
short integration facts their implementer actually needs, rather than directing
the implementer to read the entire history. Do not read other phases' plans
unless the coordinator subsequently asks you to revise named future phases.
Implement work directly rather than spawning additional agents.

Use the existing application's source, registers reference, architecture, and
evidence as needed. Historical notes can be stale; verify claims against current
source. Runtime profiles are in `MechControl/Profiles/MECHREVO` within the old
repository. Root-level `Profiles/MECHREVO` there is an OEM evidence archive.

Hardware checks, including EC reads, are allowed. Use the available environment
to validate work rather than imposing an offline-only policy. EC writes must use
verified ground truth and the central WriteGuard. Preserve read-modify-write for
bitfields and model-derived encodings/limits. Never run two hardware-controlling
instances concurrently. Do not infer new register semantics.

Model detection must preserve baseboard-pattern precedence, CPU-vendor filters,
direct then shifted project-ID fallback, filename matching, and manual selection.
Intel and AMD project IDs collide. Features and limits belong to model profiles
or runtime detection, not hardcoded assumptions about Jon's test machine.

Keep phase implementation results and check evidence in `docs/phase-N-result.md`.
Report what was actually built and checked and any concrete blockers. Do not
claim physical hardware coverage from simulated tests. Preserve unrelated work.
Use patch for file-oriented changes. Keep instructions to other people in full
sentences and plain language.

## Deployment

When the user asks to deploy, run the checks relevant to the change, then run
`./tools/deploy.ps1` from an elevated shell. It builds the CLI Release, closes the
running app, copies the runtime files over the installed copy and starts it again
through the `MechControl` logon task. Settings, profiles and logs stay in place.
Do not add backups, status snapshots or deployment notes. `docs/handoff.md` is only
for switching between the original C# app and the native app.

## Documentation size

Keep result documents short, normally about 40–80 lines. Include delivered
decisions, material deviations and their reasons/disposition, final check results,
remaining work, and evidence locations. Put raw logs, command transcripts, hashes
and intermediate measurements in scratch evidence files, not repeated prose.
When a defect is fixed, update its status and retain a short deviation record;
do not append another complete account beside an obsolete one. Future plans need
only actionable integration facts, not every prior phase's history. Do not make
later agents read entire reports. See `docs/documentation.md` for archive location.
