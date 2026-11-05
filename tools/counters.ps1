param(
    [Parameter(Mandatory)][ValidateSet('Install', 'Remove', 'Rollback')][string]$Action,
    [Parameter(Mandatory)][string]$Manifest,
    [Parameter(Mandatory)][string]$BackupDirectory
)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this script from an elevated shell.' }
$Manifest = (Resolve-Path $Manifest).Path
$BackupDirectory = [IO.Path]::GetFullPath($BackupDirectory)
if (Get-Process -Name MechControl,ui-pages-check,foundation-check -ErrorAction SilentlyContinue) { throw 'Stop every MechControl publisher before changing counter registration.' }
$key = 'HKLM\SYSTEM\CurrentControlSet\Services\MechControl\Performance'
$provider = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Perflib\_V2Providers\{78D1FCD4-2A33-4DD9-9C89-EC307B75E1F4}'
function NativeCommand([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE) { throw "$Program failed with exit code $LASTEXITCODE." }
}
function Rollback {
    $snapshot = Join-Path $BackupDirectory 'counters.ini'
    if (-not (Test-Path $snapshot)) { throw 'The counter registration snapshot is missing.' }
    if ((Test-Path (Join-Path $BackupDirectory 'legacy.reg')) -and -not (Test-Path "$env:windir/System32/netfxperf.dll")) { throw 'The saved legacy counter resource netfxperf.dll is missing.' }
    if (Test-Path $provider) { NativeCommand unlodctr @("/m:$Manifest") }
    if (Test-Path (Join-Path $BackupDirectory 'legacy.reg')) { NativeCommand reg @('import', (Join-Path $BackupDirectory 'legacy.reg')) }
    NativeCommand lodctr @("/r:$snapshot")
    if (Test-Path (Join-Path $BackupDirectory 'legacy.reg')) {
        $legacy = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\MechControl\Performance'
        if ($legacy.Library -notlike '*netfxperf.dll') { throw 'Legacy rollback did not restore netfxperf.dll ownership.' }
    } elseif (Test-Path $provider) { throw 'Native registration remains after rollback.' }
}
if ($Action -eq 'Rollback') { Rollback; return }
if ($Action -eq 'Remove') {
    NativeCommand unlodctr @("/m:$Manifest")
    if (Test-Path $provider) { throw 'Native counter provider remains registered.' }
    return
}
if (Test-Path $provider) { throw 'The native provider is already registered. Remove it with its current manifest before installing another copy.' }
if (Test-Path $BackupDirectory) { throw 'Choose a new backup directory; existing rollback evidence will not be overwritten.' }
New-Item -ItemType Directory $BackupDirectory | Out-Null
NativeCommand lodctr @('/s:' + (Join-Path $BackupDirectory 'counters.ini'))
if (Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\MechControl\Performance') {
    $legacy = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\MechControl\Performance'
    if ($legacy.Library -notlike '*netfxperf.dll') { throw 'The existing category is not the known legacy netfxperf.dll owner.' }
    NativeCommand reg @('export', $key, (Join-Path $BackupDirectory 'legacy.reg'))
    NativeCommand unlodctr @('MechControl')
}
try {
    NativeCommand lodctr @("/m:$Manifest")
    if (-not (Test-Path $provider)) { throw 'Native provider registration is missing.' }
    $category = Get-Counter -ListSet MechControl
    if ($category.Paths -notcontains '\MechControl\CPU Fan RPM' -or $category.Paths -notcontains '\MechControl\GPU Fan RPM') { throw 'The exact MechControl fan counter paths are missing.' }
} catch {
    $failure = $_
    Rollback
    throw $failure
}
