# Architecture and ownership

`src/ui.cpp` is the production Win32 application. The UI owns controls, tray state and
presentation. `fan_chart.*` draws the fan curves and Link Fans status with GDI+, which is
delay-loaded on the first draw. `ControlWorker` (`worker.*`) owns a serialized command queue,
controller and monitor on one `std::jthread`. `Delivery` posts owned results to
the UI; it is detached before destruction. Shutdown stops producers and joins
the worker. Synchronous driver, WMI, NVIDIA and HID calls must return before
shutdown finishes; there is no guaranteed cancellation timeout.

Controlling startup acquires `Global\MechControl` before log or hardware access.
The optional named-pipe server submits to the same worker. It is compiled out by
default. `--diagnose <file>` is a separate write-free, mutex-exempt EC path, not
a controller or a substitute for release qualification.

`configuration` contains the pinned JSON parser, profile detection and stores.
Detection preserves baseboard-pattern precedence, CPU-vendor filtering, direct
then shifted project IDs, filename fallback and manual selection. Runtime models
live in the one `Profiles/MECHREVO` tree. Device capabilities, encodings and limits
come from that model and runtime validation, not assumptions about this host.
A `fanCurveLatch` bitfield selects read-modify-write of the `0x7C6` latch, and
`modePresets` limits which EC presets are exposed. X6FR5xxY uses both: firmware
`XxFR5xxx_Pro_14.810` shares other bits in `0x7C6`, and its `0x7A7` preset is 145 W,
not Battery Saver.

`ec.*` and `ec_session.*` own the OEM transport, activation and central WriteGuard.
`controller*` coordinates power, fans, presets and partial outcomes. Thermal
enforcement runs before linked fans and telemetry. Held actions constrain later
writes, and acquisitions during waits win. `thermal.*` persists existing Stage
2/Held obligations; startup combines committed and pending settings. An enabled
Reset requires a fresh valid below-trigger reading/mean. Hardware request readback
is not proof of physical watts or cooling. Native recovery deliberately retains
known actions in damaged records that the older reader can lose.

`nvidia_*` owns NVAPI, ROM parsing and verified override records. Originals are
shared at `%ProgramData%/MechControl/NvidiaOverrides/originals.txt`; they are not
per-release settings and must not be replaced by an old backup during rollback.
`peripherals.*`, `cooler.*` and `ble.*` own HID lighting and BLE operations. Queue
acceptance is not completed cooler Apply or saved state. Unlocked GPU power writes
qualify and maintain the live F7 laptop budget, which can fall below the VBIOS
maximum when Dynamic Boost is off. Independent usable GPU features survive
unsupported F7 data; unlocked power writes do not.

Settings, five slots, peripheral state and logs live beside the executable.
Settings/slots use flushed temporary-file replacement; slot temporaries are not
authoritative. `settings.json.tmp` may contain thermal obligations. Lighting and
cooler saves retain direct, log-only failure behavior. UI outcomes distinguish
intent, applied, saved and partial results. There is no background migration.

`counters.*` publishes already observed RPM through PerfLib, without polling
hardware for consumers. Registration is explicit through `tools/counters.ps1`
in the source tree, or root-level `counters.ps1` in the runtime package.
Static libraries are build boundaries, not independently deployed services.
See [build](build.md) and [handoff/qualification](handoff.md).
