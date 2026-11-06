param(
    [Parameter(Mandatory)][ValidateSet('Stage', 'Transfer')][string]$Action,
    [string]$Archive,
    [string]$Inventory,
    [string]$FromDirectory,
    [Parameter(Mandatory)][string]$ToDirectory,
    [string]$BackupDirectory,
    [string]$TaskName = 'MechControl',
    [string]$TaskPath = '\'
)
$ErrorActionPreference = 'Stop'
function FullPath([string]$Path) {
    if (-not $Path) { throw 'A required path is missing.' }
    return [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}
function Disjoint([string]$Left, [string]$Right) {
    if ($Left -ieq $Right -or $Left.StartsWith($Right + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $Right.StartsWith($Left + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Use separate, non-nested directories.' }
}
$ToDirectory = FullPath $ToDirectory
if ($Action -eq 'Stage') {
    $Archive = FullPath $Archive
    $Inventory = FullPath $Inventory
    if (Test-Path -LiteralPath $ToDirectory) { throw 'Stage into a new directory, not an installation or an earlier extraction.' }
    $runtime = @((Get-Content -LiteralPath $Inventory -Raw | ConvertFrom-Json).artifacts | Where-Object component -eq 'Runtime')
    if ($runtime.Count -ne 1 -or $runtime[0].archive -cne [IO.Path]::GetFileName($Archive) -or
        (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $runtime[0].sha256) { throw 'Runtime archive identity does not match the inventory.' }
    Expand-Archive -LiteralPath $Archive -DestinationPath $ToDirectory
    $files = @(Get-ChildItem -LiteralPath $ToDirectory -Recurse -File)
    if ($files.Count -ne $runtime[0].files.Count) { throw 'The extracted file count differs from the inventory.' }
    foreach ($file in $runtime[0].files) {
        $path = FullPath (Join-Path $ToDirectory $file.path)
        if (-not $path.StartsWith($ToDirectory + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Inventory path escapes the extraction.' }
        if ((Get-Item -LiteralPath $path).Length -ne $file.bytes -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) { throw "Payload mismatch: $($file.path)" }
    }
    Write-Output "Verified fresh runtime: $ToDirectory. Staging is not qualification or deployment."
    return
}

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Transfer requires an elevated shell.' }
$FromDirectory = FullPath $FromDirectory
$BackupDirectory = FullPath $BackupDirectory
Disjoint $FromDirectory $ToDirectory
Disjoint $FromDirectory $BackupDirectory
Disjoint $ToDirectory $BackupDirectory
foreach ($directory in @($FromDirectory, $ToDirectory)) {
    if (-not (Test-Path -LiteralPath (Join-Path $directory 'MechControl.exe') -PathType Leaf)) { throw "No release executable in $directory" }
    if (Get-ChildItem -LiteralPath $directory -Recurse -Force -Attributes ReparsePoint) { throw 'Release directories must not contain links or junctions.' }
}
$task = Get-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath
if ($task.Settings.Enabled -or $task.State -eq 'Running') { throw 'Disable the logon task and exit its controller before transfer.' }
if (Get-Process -Name MechControl,ui-pages-check,foundation-check -ErrorAction SilentlyContinue) { throw 'Exit every controller and counter publisher before transfer.' }
$mutex = $null
try { $mutex = [Threading.Mutex]::OpenExisting('Global\MechControl') } catch [Threading.WaitHandleCannotBeOpenedException] { }
if ($mutex) { $mutex.Dispose(); throw 'Global\MechControl still exists. Wait for its owner to exit.' }
if (Test-Path -LiteralPath $BackupDirectory) { throw 'Choose a new backup directory; previous rollback evidence is retained.' }
New-Item -ItemType Directory -Path $BackupDirectory | Out-Null
Copy-Item -LiteralPath $FromDirectory -Destination (Join-Path $BackupDirectory 'from') -Recurse
Copy-Item -LiteralPath $ToDirectory -Destination (Join-Path $BackupDirectory 'to') -Recurse
Export-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath | Set-Content (Join-Path $BackupDirectory 'task.xml') -Encoding Unicode
$originals = Join-Path $env:ProgramData 'MechControl/NvidiaOverrides'
if (Test-Path -LiteralPath $originals) { Copy-Item -LiteralPath $originals -Destination (Join-Path $BackupDirectory 'NvidiaOverrides') -Recurse }
$snapshotFiles = @(Get-ChildItem -LiteralPath $BackupDirectory -Recurse -File)
$snapshotFiles | ForEach-Object {
    [pscustomobject]@{ path = $_.FullName.Substring($BackupDirectory.Length + 1); bytes = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
} | Export-Csv (Join-Path $BackupDirectory 'snapshot-hashes.csv') -NoTypeInformation -Encoding UTF8

# Do not reimplement recovery. The native owner must first durably save its recovered
# obligations in the existing Stage 2/Held representation, including pending holds.
if (Test-Path -LiteralPath (Join-Path $FromDirectory 'settings.json.tmp')) { throw 'Pending thermal settings were backed up. Materialize recovery with the native owner before transfer; do not delete the pending record.' }
$settings = Get-Content -LiteralPath (Join-Path $FromDirectory 'settings.json') -Raw | ConvertFrom-Json
$thermal = $settings.thermalProtection
if ($null -eq $thermal -or -not ($thermal.Stage -is [int] -or $thermal.Stage -is [long]) -or
    -not ($thermal.Held -is [int] -or $thermal.Held -is [long]) -or
    -not (($thermal.Stage -eq 0 -and $thermal.Held -eq 0) -or
        ($thermal.Stage -eq 2 -and $thermal.Held -ge 1 -and $thermal.Held -le 7))) {
    throw 'Thermal recovery is not materialized as valid Stage 0/Held 0 or Stage 2/Held 1..7. The snapshot is retained; no target state changed.'
}
$names = @('settings.json', 'lighting_state.json', 'liquid_cooler_state.json') + @(1..5 | ForEach-Object { "userprofile_$_.json" })
foreach ($name in $names) {
    $source = Join-Path $FromDirectory $name
    if (Test-Path -LiteralPath $source) { $null = Get-Content -LiteralPath $source -Raw | ConvertFrom-Json }
}
foreach ($name in $names) {
    $source = Join-Path $FromDirectory $name
    $target = Join-Path $ToDirectory $name
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $target -Force }
    elseif (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target }
    # Slot temporaries are not authoritative. Old target thermal pending state is
    # superseded only by this stopped current owner's validated committed state.
    if (Test-Path -LiteralPath ($target + '.tmp')) { Remove-Item -LiteralPath ($target + '.tmp') }
}
Get-ChildItem -LiteralPath $FromDirectory -File -Filter 'mechcontrol*.log*' | Copy-Item -Destination $ToDirectory -Force
foreach ($name in $names) {
    $source = Join-Path $FromDirectory $name
    if ((Test-Path -LiteralPath $source) -and (Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath (Join-Path $ToDirectory $name)).Hash) { throw "State copy verification failed: $name" }
}
Write-Output "Current state transferred without changing binaries, task, counters or NVIDIA originals. Snapshot: $BackupDirectory"
