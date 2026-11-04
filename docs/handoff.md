# Deployment and rollback

## Current decision

The CLI-enabled build is installed and in daily use. Routine updates use
`tools/deploy.ps1` in the source repository, not this procedure. The
greater-than-72-hour soak, write/no-wake tracing and remaining device-specific
qualification are still open.

The final handoff candidates are under
`C:/Temp/tandem/mechcontrol-native-phase9-final/`: `final-default/MechControl-native-win-x64-default.zip`
and `final-cli/MechControl-native-win-x64-cli.zip`, with adjacent `artifacts.json`
inventories and separate symbols ZIPs. They include the corrected counter rollback
preflight, `handoff.ps1` and these docs. The frozen phase-8 archives are audit
evidence only, superseded for handoff; their EXE-specific hardware observations
remain applicable only where executable/profile identity is verified unchanged.

The tools deliberately reuse Task Scheduler and `counters.ps1`, rather than add
an installer or migration service. `handoff.ps1` stages an identified archive and
transfers current state in either direction. It does not certify qualification,
stop processes, switch tasks, register counters or start hardware control.
The commands below run from the extracted package root. In the source repository,
use `./tools/handoff.ps1` and `./tools/counters.ps1` instead of their root paths.

## Stage without changing ownership

Use the exact runtime ZIP and its adjacent inventory for the candidate being checked.
Keep symbols separately. The inventory must come from the trusted release output;
a hash is identity checking, not a signature. Use an elevated interactive shell
for later steps. Here `$old` is the functioning installation, not a comparison
build, source tree or a test extraction. Use fresh `$new`, `$backup` and `$counter`.

```powershell
$old = 'C:/Program_Files/MechControl'
$new = 'C:/Program_Files/MechControl-native-<release>'
$backup = 'C:/Temp/tandem/cutover-<unique>'
$counter = 'C:/Temp/tandem/counters-<unique>'
./handoff.ps1 -Action Stage -Archive <runtime.zip> -Inventory <artifacts.json> -ToDirectory $new
Export-ScheduledTask -TaskName MechControl -TaskPath '\' | Set-Content <enabled-task-backup.xml> -Encoding Unicode
```

Record the active PID/path/hash, current slot/profile/model, fans, linked state,
thermal Stage/Held and hardware requests, owned NVIDIA overrides/originals,
lighting/cooler completion state, task XML and live counter samples. Check the
release's open qualification before proceeding. Keep the old release until daily
use and rollback are proven. Do not delete shared system DLLs or NVIDIA originals.

## Qualified cutover only

Disable the task, then use the current controller's tray Exit. Do not use
`Stop-ScheduledTask` or force termination as an orderly shutdown substitute.
Wait for the process and `Global\MechControl` to disappear, even if synchronous
device I/O delays exit. Do not start the replacement while the old owner remains.

```powershell
Disable-ScheduledTask -TaskName MechControl -TaskPath '\'
# Exit through the tray and wait for complete process exit.
./handoff.ps1 -Action Transfer -FromDirectory $old -ToDirectory $new -BackupDirectory $backup
./counters.ps1 -Action Install -Manifest "$new/MechControl-counters.man" -BackupDirectory $counter
```

Transfer snapshots both complete directories, disabled task XML, shared NVIDIA
recovery files and hashes before touching target state. It copies current settings,
five committed slots (including absence), lighting/cooler files and logs byte-for-
byte, not stale rollback settings. Slot `.tmp` files remain only in snapshots.
The live shared NVIDIA directory is never restored from a backup. Counter Install
backs up the owned legacy key and PerfLib snapshot, unloads `MechControl`, registers
the native manifest and checks exact counter names. Do not run unrelated counter
installers during this maintenance window: the existing rollback uses `lodctr /r`
with its machine-wide snapshot. Keep all rollback files and the registered EXE.
If a step fails, leave the task disabled and retain the snapshots. Do not start
either owner until state and registration are consistent; transfer is not a
transaction across files, registry and Task Scheduler.

Retarget only the existing task action, preserving its trigger, user and settings.
Do not recreate an approximate task. This host uses interactive/highest logon,
`ExecutionTimeLimit=PT0S` and both battery restrictions false. Verify those in XML.

```powershell
[xml]$task = Export-ScheduledTask -TaskName MechControl -TaskPath '\'
$task.Task.Actions.Exec.Command = "$new/MechControl.exe"
$task.Task.Actions.Exec.WorkingDirectory = $new
Register-ScheduledTask -TaskName MechControl -TaskPath '\' -Xml $task.OuterXml -Force
Enable-ScheduledTask -TaskName MechControl -TaskPath '\'
Start-ScheduledTask -TaskName MechControl -TaskPath '\'
```

Verify the running path and release hash, one controller/mutex, tray-hidden startup
and tray Show, model/profile/EC activation and validation, held actions and request
readback, fan tables/linking, slot and separate lighting/cooler files/outcomes.
If shipped, run elevated `mcctl status --json`; it addresses the active pipe owner,
not necessarily the executable beside it. Read both actual PDH consumers:

```powershell
Get-Counter '\MechControl\CPU Fan RPM','\MechControl\GPU Fan RPM'
Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Perflib\_V2Providers\{78D1FCD4-2A33-4DD9-9C89-EC307B75E1F4}'
```

Confirm the provider's resource paths point to the retained new EXE. Logon-task
demand start is not logoff/logon qualification. Retain smoke evidence and logs.

## Roll back current state, not yesterday's settings

Disable the task and exit the native owner. Before switching to C#, ensure current
holds are durably represented as valid Stage 2/Held in committed settings. Transfer
refuses pending or damaged thermal state after taking a snapshot; it never guesses
recovery or deletes a pending obligation. With native still the sole owner, use
an existing settings-save operation (for example Rename the selected slot to its
unchanged name) to persist recovered state, then exit and inspect it. Do not Reset
or release holds to make rollback pass. Retain the raw damaged/pending evidence.

Verify the older reader on copied current data before starting it. The installed
`MechControl.dll` can be loaded in a separate PowerShell process; instantiate its
`MechControl.Profile.UserProfileManager` with the copied directory and appropriate
GPU-recovery support, and inspect `ThermalProtection.Stage/Held`. Compare with the
native recovered obligations. Do not invoke setters against production data.

```powershell
# Run in a separate PowerShell process. $copy contains copied current settings.
$assembly = [Reflection.Assembly]::LoadFrom("$old/MechControl.dll")
$type = $assembly.GetType('MechControl.Profile.UserProfileManager', $true)
$reader = [Activator]::CreateInstance($type, [object[]]@($copy, $true))
$reader.ThermalProtection | Select-Object Stage,Held
```

The example's `$true` is this model's supported GPU recovery, not a universal
hardware assumption. Verify it for the target model. Reading copied data does not
verify physical enforcement; repeat request readback after the owner starts.

```powershell
./handoff.ps1 -Action Transfer -FromDirectory $new -ToDirectory $old -BackupDirectory C:/Temp/tandem/rollback-<unique>
./counters.ps1 -Action Rollback -Manifest "$new/MechControl-counters.man" -BackupDirectory $counter
```

Retarget the disabled task with the same XML commands above using `$old` instead
of `$new`, then enable/start it. Verify old EXE hash, sole ownership, current slot,
all held actions and fresh readback, preserved NVIDIA ownership and real legacy
PDH values (`netfxperf.dll`). Old NVIDIA F7 refusal does not prove stock hardware.
Do not overwrite newer originals, restored settings or a newly acquired hold with
the pre-cutover snapshot. If legacy cannot enforce an obligation, stop it and
return to the native owner with current state, not a second concurrent controller.
Keep backups and the candidate until the recovery is proven.
