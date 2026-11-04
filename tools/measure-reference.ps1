param(
    [Parameter(Mandatory)][int]$ProcessId,
    [int]$Seconds = 60,
    [string]$Label = 'observed',
    [string]$OutputPath = 'C:/Temp/tandem/mechcontrol-native-phase0/baseline.json',
    [string]$OutputDirectory,
    [int]$IntervalSeconds = 1,
    [string]$Conditions = '',
    [switch]$NoGpu
)

$ErrorActionPreference = 'Stop'
if ($Seconds -lt 1 -or $IntervalSeconds -lt 1) { throw 'Durations must be positive.' }
if ($OutputDirectory) {
    if (Test-Path $OutputDirectory) { throw 'Choose a fresh evidence directory.' }
    New-Item -ItemType Directory $OutputDirectory | Out-Null
}
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class BaselineWindows {
    [StructLayout(LayoutKind.Sequential)] public struct Io {
        public ulong readOperations, writeOperations, otherOperations, readBytes, writeBytes, otherBytes;
    }
    [StructLayout(LayoutKind.Sequential)] public struct Power {
        public byte ac, flags, percent, saver; public uint life, fullLife;
    }
    [DllImport("kernel32", SetLastError=true)] public static extern bool GetProcessIoCounters(IntPtr process, out Io value);
    [DllImport("kernel32")] public static extern bool GetSystemPowerStatus(out Power value);
    public delegate bool Callback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int length);
}
'@

function Read-GpuPower {
    @(Get-PnpDevice -Class Display -PresentOnly | Where-Object InstanceId -Like 'PCI\VEN_10DE*' | ForEach-Object {
        $power = Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_PowerData
        [ordered]@{ name = $_.FriendlyName; instanceId = $_.InstanceId; mostRecentPowerState = [BitConverter]::ToUInt32([byte[]]$power.Data, 4) }
    })
}

$process = Get-Process -Id $ProcessId
$exe = $process.Path
$directory = Split-Path $exe
$files = @(Get-ChildItem $directory -Recurse -File)
$payload = @($files | Where-Object { $_.Name -notmatch '^(settings|userprofile_\d+|lighting_state|liquid_cooler_state)\.json$|\.log$|\.tmp$' })
$identity = @('MechControl.exe', 'MechControl.dll', 'mcctl.exe') | ForEach-Object {
    $path = Join-Path $directory $_
    if (Test-Path $path) { Get-FileHash $path -Algorithm SHA256 | Select-Object Path, Hash }
}
$visible = [System.Collections.Generic.List[object]]::new()
$callback = [BaselineWindows+Callback]{ param($window, $parameter)
    [uint32]$owner = 0
    [void][BaselineWindows]::GetWindowThreadProcessId($window, [ref]$owner)
    if ($owner -eq $ProcessId -and [BaselineWindows]::IsWindowVisible($window)) {
        $title = [Text.StringBuilder]::new(1024)
        [void][BaselineWindows]::GetWindowText($window, $title, $title.Capacity)
        $visible.Add([ordered]@{ handle = $window.ToInt64(); title = $title.ToString(); minimized = [BaselineWindows]::IsIconic($window) })
    }
    return $true
}
[void][BaselineWindows]::EnumWindows($callback, [IntPtr]::Zero)
$gpuBefore = $null
if (-not $NoGpu) { $gpuBefore = Read-GpuPower }
$start = [DateTimeOffset]::Now
$power = New-Object BaselineWindows+Power
[void][BaselineWindows]::GetSystemPowerStatus([ref]$power)
$metadata = [ordered]@{
    label = $Label; started = $start.ToString('o'); processId = $ProcessId; processStarted = $process.StartTime.ToString('o'); executable = $exe
    logicalProcessors = [Environment]::ProcessorCount; os = [Environment]::OSVersion.VersionString
    visibleTopLevelWindowsAtStart = @($visible.ToArray()); identity = @($identity)
    installedTotalBytes = ($files | Measure-Object Length -Sum).Sum
    installedPayloadBytesExcludingMutableState = ($payload | Measure-Object Length -Sum).Sum
    installedPayloadFiles = $payload.Count; requestedSeconds = $Seconds; intervalSeconds = $IntervalSeconds
    conditions = $Conditions; powerAtStart = $power; gpuQueriesDisabled = [bool]$NoGpu; gpuBefore = $gpuBefore
}
if ($OutputDirectory) { $metadata | ConvertTo-Json -Depth 8 | Set-Content "$OutputDirectory/identity.json" -Encoding utf8 }
$process.Refresh()
$cpuStart = $process.TotalProcessorTime.TotalSeconds
$cpuEnd = $cpuStart
$clock = [Diagnostics.Stopwatch]::StartNew()
$samples = [System.Collections.Generic.List[object]]::new()
$count = 0; $missed = 0; $exited = $false; $due = 0.0
$ws = 0.0; $private = 0.0; $firstHandles = $null; $lastHandles = 0; $peakHandles = 0
while ($true) {
    $delay = $due - $clock.Elapsed.TotalSeconds
    if ($delay -gt 0) { Start-Sleep -Milliseconds ([int]($delay * 1000)) }
    $process.Refresh()
    if ($process.HasExited) { $exited = $true; break }
    $io = New-Object BaselineWindows+Io
    if (-not [BaselineWindows]::GetProcessIoCounters($process.Handle, [ref]$io)) { throw 'Cannot read process I/O counters.' }
    $cpuEnd = $process.TotalProcessorTime.TotalSeconds
    $sample = [pscustomobject]@{ elapsedSeconds = $clock.Elapsed.TotalSeconds; cpuSeconds = $cpuEnd
        workingSetBytes = $process.WorkingSet64; privateBytes = $process.PrivateMemorySize64; handles = $process.HandleCount; threads = $process.Threads.Count
        readOperations = $io.readOperations; writeOperations = $io.writeOperations; otherOperations = $io.otherOperations }
    if ($OutputDirectory) { $sample | Export-Csv "$OutputDirectory/samples.csv" -Append -NoTypeInformation }
    else { $samples.Add($sample) }
    ++$count; $ws += $sample.workingSetBytes; $private += $sample.privateBytes
    if ($null -eq $firstHandles) { $firstHandles = $sample.handles }
    $lastHandles = $sample.handles; $peakHandles = [Math]::Max($peakHandles, $lastHandles)
    if ($due -ge $Seconds) { break }
    $due = [Math]::Min($Seconds, $due + $IntervalSeconds)
    while ($due -lt $Seconds -and $due -lt $clock.Elapsed.TotalSeconds) { ++$missed; $due = [Math]::Min($Seconds, $due + $IntervalSeconds) }
}
$elapsed = $clock.Elapsed.TotalSeconds
$cpu = $cpuEnd - $cpuStart
$gpuAfter = $null
if (-not $NoGpu) { $gpuAfter = Read-GpuPower }
$result = [ordered]@{
    samples = $count; missedObserverSamples = $missed; processExited = $exited
    elapsedSeconds = $elapsed; cpuSeconds = $cpu; cpuPercentOneCore = 100 * $cpu / $elapsed
    cpuPercentMachine = 100 * $cpu / $elapsed / [Environment]::ProcessorCount
    workingSetMeanBytes = $(if ($count) { $ws / $count } else { 0 })
    privateMeanBytes = $(if ($count) { $private / $count } else { 0 })
    handlesFirst = $firstHandles; handlesLast = $lastHandles; handlesPeak = $peakHandles
    gpuAfter = $gpuAfter
    note = 'Observer misses are not controller misses. Process I/O totals do not identify EC writes or GPU wakes.'
}
if ($OutputDirectory) { $result | ConvertTo-Json -Depth 8 | Set-Content "$OutputDirectory/summary.json" -Encoding utf8 }
else {
    foreach ($key in $metadata.Keys) { $result[$key] = $metadata[$key] }
    $result.samples = @($samples.ToArray())
    $result | ConvertTo-Json -Depth 8 | Set-Content $OutputPath -Encoding utf8
}
[pscustomobject]$result | Select-Object -Property * -ExcludeProperty samples | ConvertTo-Json -Depth 8
