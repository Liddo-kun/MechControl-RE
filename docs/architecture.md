# Architecture and ownership

`src/ui.cpp` is the production Win32 application. The UI owns controls, tray state and
presentation. `fan_chart.*` draws the fan curves and Link Fans status with GDI+, which is
delay-loaded on the first draw. `ControlWorker` (`worker.*`) owns a serialized command queue,
controller and monitor on one `std::jthread`. `Delivery` posts owned results to
the UI; it is detached before destruction. Shutdown stops producers and joins
the worker. Synchronous driver, WMI, NVIDIA and HID calls must return before
shutdown finishes; there is no guaranteed cancellation timeout.

Blocking NVIDIA, HID and WMI calls run on a helper thread while the worker waits; EC access
never leaves the worker. During the wait the worker runs only `Controller::wait_tick` and the
lifecycle notifications. `wait_tick` watches the EC bypass and activation, samples the thermal
failsafe (a new trip, or an EC re-activation while tripped, writes the EC holds at once; the
driver half and its verification follow when the driver is free), runs the lighting idle timer
and reads telemetry. Everything else waits for the next ordinary tick: the resume settle, hold
re-verification, slot re-apply after EC recovery, the delayed GPU check and linked fans. Other
commands stay queued.

Controlling startup acquires `Global\MechControl` before log or hardware access.
The optional named-pipe server submits to the same worker. It is compiled out by
default. `--diagnose <file>` is a separate write-free, mutex-exempt EC path, not
a controller.

`configuration` contains the pinned JSON parser, profile detection and stores.
Detection preserves baseboard-pattern precedence, CPU-vendor filtering, direct
then shifted project IDs, filename fallback and manual selection. Runtime models
live in the one `Profiles/MECHREVO` tree. Device capabilities, encodings and limits
come from that model and runtime validation, not assumptions about this host.
Limits the EC reports (ECSpec) replace the profile's, and startup validation disables
a feature whose registers read outside their sane range.
A `fanCurveLatch` bitfield selects read-modify-write of the `0x7C6` latch, and
`modePresets` limits which EC presets are exposed. X6FR5xxY uses both: firmware
`XxFR5xxx_Pro_14.810` shares other bits in `0x7C6`, and its `0x7A7` preset is 145 W,
not Battery Saver. IDY_7Y uses `fanCurveLatch` because Intel EC 77.0 treats `0x7C6`
bit 6 as a power-button request. Other profiles write the whole byte.

`ec.*` and `ec_session.*` own the OEM transport, activation and central WriteGuard.
The guard admits one fixed set of addresses on every model, plus the EC bypass
register of a profile that declares it.
`controller*` coordinates power, fans, presets and partial outcomes. Each command in
`ControlCommand` has one `run()` handler; `execute` adds the shared thermal check, failure
text and partial-application accounting. Its `traits()` entry beside the command structs
tells the worker whether it is a lifecycle notification (allowed during the wait for a
blocking device operation), whether its result reaches the UI, which tabs it makes stale
and whether it changes the slot catalogue. `controller_status.cpp` builds the UI
presentation and status JSON. `cli_commands.cpp` turns `mcctl` arguments into the same
typed commands and formats the replies; it is always built because the worker runs
`CliCommand`, while the pipe server and `mcctl` stay optional. Thermal
enforcement runs before linked fans and telemetry. Held actions constrain later
writes, and acquisitions during waits win. `thermal.*` persists existing Stage
2/Held obligations; startup combines committed and pending settings. An enabled
Reset requires a fresh valid below-trigger reading/mean. Hardware request readback
is not proof of physical watts or cooling. Recovery deliberately retains known
actions in damaged records that the C# app's reader can lose.

The EC power-limit bypass (0x780=0xA2, benchmark mode) is offered only where the
profile declares it and the live EC version is one the image was checked against.
Controls withheld because a model or its firmware is unverified (this bypass, the
NVIDIA unlock, features disabled by startup validation) stay visible but greyed, with
the reason and a link that opens a GitHub issue prefilled with it and the model.
The bypass is never saved, and the controller clears it on turn-off, sleep, display off,
AC loss, a thermal trip, EC reinitialization, exit and a leftover value at startup;
while a clear is pending, other EC writes are refused. The liquid cooler connection
flag (0x7C7 bit 1) is saved in profile slots like the other live settings, written
bit-only and reapplied after resume and EC recovery. `mcctl` changes neither
persistently.

The battery charge limit is an application setting in `settings.json`, not part of a
slot. The EC's own percentage limit works on few models, so `Controller::charge_limit`
uses its timed charge hold (0x7F4-0x7F6, target always 100): every 30 s it arms a
week-long hold once the charge reaches the limit on the adapter, and ends it five points
below. Losing the adapter ends it at once, so no hold is left armed on battery. The hold
stays in the EC while the app is not running. No profile enables this. Reads of the EC's
charge-voltage variables recognize the firmware family, and each run's first hold is a
two-minute one that the EC must count down before it is trusted. Where those variables are recognized, the Battery page
shows them and says what the battery modes do to the charge voltage. System Info shows the
EC's record of the last power-off.

`nvidia_*` owns NVAPI, ROM parsing and verified override records. Originals are
shared at `%ProgramData%/MechControl/NvidiaOverrides/originals.txt`; they are not
per-release settings and must not be replaced by an old backup during rollback.
The unlock sets each current policy's default and maximum to its ceiling, because
621+ drivers load the default as both the maximum and the starting current. After
a GPU start, rails still at the ceiling go back to the slot's or stock values, and
the delayed GPU check reapplies currents a driver reload reset. Overrides staged
by older releases (maximum only) are replaced for the next GPU start. NVML is opened
and closed around each reading, because its handles crash when used after a GPU reset
(a driver reset on waking from Modern Standby, a GPU restart or a driver install).
`peripherals.*`, `cooler.*` and `ble.*` own HID lighting and BLE operations. Queue
acceptance is not completed cooler Apply or saved state. Unlocked GPU power writes
qualify and maintain the live F7 laptop budget, which can fall below the VBIOS
maximum when Dynamic Boost is off. Independent usable GPU features survive
unsupported F7 data; unlocked power writes do not, except a thermal hold's minimum,
which is written without reading the budget.

Settings, five slots, peripheral state and logs live beside the executable.
Settings/slots use flushed temporary-file replacement; slot temporaries are not
authoritative. `settings.json.tmp` may contain thermal obligations. Lighting and
cooler saves retain direct, log-only failure behavior. UI outcomes distinguish
intent, applied, saved and partial results. There is no background migration.

`counters.*` publishes already observed RPM through PerfLib, without polling
hardware for consumers. Registration is explicit through `tools/counters.ps1`
in the source tree, or `counters.ps1` in the release package.
Static libraries are build boundaries, not independently deployed services.
See [build](build.md).
