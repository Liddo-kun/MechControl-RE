param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$ScratchRoot,
    [string]$DotnetPath = 'dotnet'
)
$ErrorActionPreference = 'Stop'
$Package = (Resolve-Path $Package).Path
$scratch = Join-Path $ScratchRoot ('packaged-alert-' + [Guid]::NewGuid().ToString('N'))
$harness = Join-Path $scratch 'harness'
$testPackage = Join-Path $scratch 'package'

& $DotnetPath build (Join-Path $PSScriptRoot 'PackagedAlertSmoke/PackagedAlertSmoke.csproj') -c Release `
    -o $harness "-p:BaseIntermediateOutputPath=$scratch/obj/"
if ($LASTEXITCODE -ne 0) { throw 'Smoke harness build failed.' }

# Preserve the real package's apphost, runtimeconfig, deps, runtime, and libs.
# Substitute only the entry assembly in a scratch copy; load the actual app as
# the test target without ever invoking its hardware-initializing entry point.
Copy-Item -LiteralPath $Package -Destination $testPackage -Recurse
Move-Item (Join-Path $testPackage 'MechControl.dll') (Join-Path $testPackage 'MechControl.target.dll')
Copy-Item (Join-Path $harness 'PackagedAlertSmoke.dll') (Join-Path $testPackage 'MechControl.dll')
$process = Start-Process (Join-Path $testPackage 'MechControl.exe') -WorkingDirectory $testPackage -PassThru
if (-not $process.WaitForExit(20000)) {
    $process.Kill()
    throw 'Packaged alert test timed out.'
}
$result = Join-Path $testPackage 'smoke-result.txt'
if (-not (Test-Path $result)) { throw "Test exited without a result (exit $($process.ExitCode)); package: $testPackage" }
$message = [IO.File]::ReadAllText($result)
Write-Output $message
Write-Output "Test package: $testPackage"
if ($process.ExitCode -ne 0 -or -not $message.StartsWith('PASS:')) { throw 'Packaged alert test failed.' }
