# Thermal Failsafe UI Evidence

The native-150 images render the actual Thermal Failsafe tab at this Windows
system's native 144 DPI (150%). Hardware/actions/persistence are fakes; the outer
window/profile buttons are harness chrome, not the running hardware application.

The 100/200 simulation images scale fonts and geometry at 96 DPI. They exercise
wrapping/scrolling but are not proof of native per-monitor DPI transitions; native
checkbox glyphs and the shared Section title font do not scale identically in
that simulation. Native 100/200 display switching remains a manual check.

Compared with Jon's reference, the implementation retains the two sections,
slider/checkbox order, muted purpose/preview text and fit-content status. It adds
the plan's truthful immediate-trigger/manual-restoration copy, applied/saved hint,
compact details link, real Test button and explicit re-arm explanation. These
intentional additions make the layout taller than the reference.

Long trip/error history is scrollable. `*-status` captures show the lower viewport,
not a separate UI. The narrow native capture checks the foreground checkbox's
multiline wrapping; this caught and fixed native WinForms AutoSize clipping.

Regenerate using `tests/ThermalUi.Tests`; see
`notes/thermal_failsafe_validation.md` for commands and verification limitations.

The native/simulated hardware-free images were regenerated after the final resize
and Test-button sizing correction. They use the same GDI text-rendering default
as the production entry point.

`live-*-20260908.png` images are real on-screen captures of the hardware app in an
isolated release copy, not the UI fixture. `live-completion-*` records the successful
final live run (including individual restoration, defaults and Save), before the
subsequent UI-only sizing fix. No active test holds remained at completion.
