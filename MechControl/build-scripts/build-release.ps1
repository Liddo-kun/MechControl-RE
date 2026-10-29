# MechControl Release Build Script
# Produces two release zips:
#   1. Framework-dependent (small, requires .NET 8 Desktop Runtime)
#   2. Self-contained (larger, runs on any Win10+ x64 machine)
#
# Usage: Run from any directory. Requires dotnet CLI.

param(
    [string]$DotnetPath,
    [switch]$SkipSelfContained
)

$ErrorActionPreference = 'Stop'

# Resolve dotnet.exe: explicit param > PATH > standard install locations.
# Matches the 7z autodetect pattern below so the script works on any dev box
# without requiring -DotnetPath. Runtime-only installs can't build, so skip
# any dotnet.exe that reports no SDKs.
if (-not $DotnetPath) {
    $DotnetPath = @(
        (Get-Command dotnet -ErrorAction SilentlyContinue).Source,
        'C:\Program Files\dotnet\dotnet.exe',
        'C:\Program Files (x86)\dotnet\dotnet.exe',
        "$env:USERPROFILE\.dotnet\dotnet.exe"
    ) | Where-Object { $_ -and (Test-Path $_) -and (& $_ --list-sdks 2>$null) } | Select-Object -First 1
}
if (-not $DotnetPath -or -not (Test-Path $DotnetPath)) {
    throw "No dotnet.exe with an SDK found. Install the .NET 8 SDK or pass -DotnetPath explicitly."
}

$7zPath = @(
    'C:\Program Files\7-Zip\7z.exe',
    'C:\Program Files (x86)\7-Zip\7z.exe',
    'C:\Program_Files\7-Zip\7z.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
$has7z = [bool]$7zPath
$projectDir = Split-Path $PSScriptRoot -Parent
$binDir = Join-Path $projectDir 'bin'
$releaseDir = Join-Path $binDir 'Release\net8.0-windows10.0.19041.0'
$scDir = Join-Path $binDir 'selfcontained'
$trimHelper = Join-Path $binDir 'trimhelper'
$outDir = Join-Path $binDir 'release-zips'

# ── DLLs to strip from self-contained build ──────────────────────
#
# RULES — before adding a DLL here, verify:
#   1. Not referenced by app code (grep the .cs files)
#   2. Not a transitive dependency of something the app uses
#   3. Not required by WinRT/BLE (test cooler scan + connect)
#   4. Not required by WinForms accessibility (UIAutomation)
#
# Known required DLLs (do NOT strip):
#   System.Security.Cryptography.dll      — WinRT Bluetooth Radio API
#   System.Linq.Expressions.dll           — WinRT event delegate marshaling
#   System.Configuration.ConfigurationManager.dll — WMI/PerfCounter on some systems
#   System.Formats.Asn1.dll               — crypto dependency chain (BLE certs)
#   UIAutomation*.dll                     — Windows accessibility / screen readers
#   System.Security.Cryptography.{Cng,X509Certificates,Primitives,Encoding,Algorithms}.dll
#                                         — crypto chain for untested BLE pairing paths

# WPF — pure WPF framework, never loaded by WinForms
$WpfDlls = @(
    'PresentationFramework.dll', 'PresentationCore.dll', 'PresentationUI.dll',
    'PresentationNative_cor3.dll',
    'PresentationFramework.Luna.dll', 'PresentationFramework.Aero.dll',
    'PresentationFramework.Aero2.dll', 'PresentationFramework.AeroLite.dll',
    'PresentationFramework.Classic.dll', 'PresentationFramework.Royale.dll',
    'PresentationFramework-SystemCore.dll', 'PresentationFramework-SystemData.dll',
    'PresentationFramework-SystemDrawing.dll', 'PresentationFramework-SystemXml.dll',
    'PresentationFramework-SystemXmlLinq.dll',
    'ReachFramework.dll', 'System.Xaml.dll', 'System.Printing.dll',
    'System.Windows.Controls.Ribbon.dll', 'System.Windows.Input.Manipulations.dll',
    'System.Windows.Presentation.dll', 'WindowsFormsIntegration.dll',
    'WindowsBase.dll',
    'wpfgfx_cor3.dll', 'D3DCompiler_47_cor3.dll', 'PenImc_cor3.dll',
    'DirectWriteForwarder.dll', 'vcruntime140_cor3.dll'
)

# Debug / diagnostic tools — not needed in production
$DebugDlls = @(
    'mscordaccore.dll', 'mscordbi.dll', 'clretwrc.dll',
    'Microsoft.DiaSymReader.Native.amd64.dll', 'createdump.exe'
)

# Design-time only — never needed at runtime
$DesignDlls = @(
    'System.Windows.Forms.Design.dll', 'System.Windows.Forms.Design.Editors.dll',
    'System.CodeDom.dll', 'System.Design.dll', 'System.Drawing.Design.dll',
    'System.Resources.Writer.dll',
    'Microsoft.VisualBasic.Forms.dll', 'Microsoft.VisualBasic.Core.dll',
    'Microsoft.VisualBasic.dll'
)

# Crypto — strip only what BLE/WinRT definitely does not need.
# Keep: .dll (adapter), .Cng/.X509/.Primitives/.Encoding/.Algorithms (chain), Formats.Asn1
$CryptoDlls = @(
    'System.Security.Cryptography.Pkcs.dll',
    'System.Security.Cryptography.Xml.dll',
    'System.Security.Cryptography.OpenSsl.dll',
    'System.Security.Cryptography.Csp.dll',
    'System.Security.Cryptography.ProtectedData.dll',
    'System.Security.SecureString.dll'
)

# Networking — app has no network code
$NetworkDlls = @(
    'System.Net.dll', 'System.Net.Http.dll', 'System.Net.Http.Json.dll',
    'System.Net.HttpListener.dll', 'System.Net.Mail.dll', 'System.Net.NameResolution.dll',
    'System.Net.NetworkInformation.dll', 'System.Net.Ping.dll', 'System.Net.Primitives.dll',
    'System.Net.Quic.dll', 'System.Net.Requests.dll', 'System.Net.Security.dll',
    'System.Net.ServicePoint.dll', 'System.Net.Sockets.dll', 'System.Net.WebClient.dll',
    'System.Net.WebHeaderCollection.dll', 'System.Net.WebProxy.dll',
    'System.Net.WebSockets.Client.dll', 'System.Net.WebSockets.dll', 'msquic.dll'
)

# Data / XML — no database or XML usage
$DataXmlDlls = @(
    'System.Data.Common.dll', 'System.Data.DataSetExtensions.dll', 'System.Data.dll',
    'System.Private.DataContractSerialization.dll', 'System.Transactions.Local.dll',
    'System.Private.Xml.dll', 'System.Private.Xml.Linq.dll',
    'System.Xml.dll', 'System.Xml.Linq.dll', 'System.Xml.ReaderWriter.dll',
    'System.Xml.Serialization.dll', 'System.Xml.XDocument.dll', 'System.Xml.XmlDocument.dll',
    'System.Xml.XmlSerializer.dll', 'System.Xml.XPath.dll', 'System.Xml.XPath.XDocument.dll',
    'System.Runtime.Serialization.Xml.dll'
)

# Other unused — verified not referenced by app code or runtime dependencies
$OtherUnused = @(
    'System.DirectoryServices.dll',
    'System.Reflection.Metadata.dll',
    'System.Runtime.Numerics.dll',
    'System.IO.Compression.Native.dll',
    'System.Diagnostics.EventLog.dll',
    'System.Diagnostics.EventLog.Messages.dll',
    'System.Runtime.Serialization.Formatters.dll',
    'System.Runtime.Serialization.Primitives.dll',
    'System.Runtime.Serialization.dll',
    'System.Reflection.Emit.dll',
    'System.Linq.Parallel.dll',
    'System.Linq.Queryable.dll',
    'System.IO.IsolatedStorage.dll',
    'System.IO.Pipes.dll',
    'System.IO.Compression.dll', 'System.IO.Compression.Brotli.dll',
    'System.IO.Compression.FileSystem.dll', 'System.IO.Compression.ZipFile.dll',
    'System.IO.FileSystem.Watcher.dll',
    'System.Formats.Tar.dll',
    'Microsoft.CSharp.dll',
    'System.Threading.Tasks.Dataflow.dll',
    'System.Threading.Tasks.Parallel.dll',
    'System.IO.Packaging.dll',
    'System.Collections.Immutable.dll',
    'System.Reflection.DispatchProxy.dll',
    'System.Runtime.InteropServices.JavaScript.dll',
    'System.Web.HttpUtility.dll',
    'System.ComponentModel.Annotations.dll',
    'System.Resources.Extensions.dll',
    'System.Security.dll',
    'System.Security.Permissions.dll',
    'System.Text.Encoding.CodePages.dll',
    'System.Threading.AccessControl.dll',
    'System.Windows.Extensions.dll',
    'System.Diagnostics.DiagnosticSource.dll',
    'System.Threading.Channels.dll',
    'System.IO.FileSystem.DriveInfo.dll',
    'System.Diagnostics.FileVersionInfo.dll',
    'System.Diagnostics.TextWriterTraceListener.dll',
    'System.Text.RegularExpressions.dll'
)

$AllStrip = $WpfDlls + $DebugDlls + $DesignDlls + $CryptoDlls + $NetworkDlls + $DataXmlDlls + $OtherUnused

function Remove-Files($dir, $names) {
    foreach ($f in $names) {
        $p = Join-Path $dir $f
        if (Test-Path $p) { Remove-Item $p -Force }
    }
}

function Get-DirSizeMB($dir) {
    [math]::Round((Get-ChildItem $dir -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB, 1)
}

# ── Step 1: Clean ──────────────────────────────────────────────────

Write-Host "`n=== Cleaning bin directory ===" -ForegroundColor Cyan
if (Test-Path $binDir) { Remove-Item $binDir -Recurse -Force }

# ── Step 2: Regular build ──────────────────────────────────────────

Write-Host "`n=== Building (Release) ===" -ForegroundColor Cyan
& $DotnetPath build $projectDir -c Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# ── Step 3: Trimmed publish (only to get slim WinRT DLLs) ─────────

Write-Host "`n=== Trimmed publish (extracting slim WinRT DLLs) ===" -ForegroundColor Cyan
& $DotnetPath publish $projectDir -c Release -r win-x64 --self-contained true `
    -p:PublishTrimmed=true -p:TrimMode=partial -p:_SuppressWinFormsTrimError=true `
    -o $trimHelper --nologo -v q
if ($LASTEXITCODE -ne 0) { throw "Trimmed publish failed" }

# Swap trimmed WinRT DLLs into regular build
Copy-Item (Join-Path $trimHelper 'Microsoft.Windows.SDK.NET.dll') $releaseDir -Force
Copy-Item (Join-Path $trimHelper 'WinRT.Runtime.dll') $releaseDir -Force
Remove-Item $trimHelper -Recurse -Force

# Clean up win-x64 intermediate that publish leaves behind.
$winx64 = Join-Path $releaseDir 'win-x64'
if (Test-Path $winx64) { Remove-Item $winx64 -Recurse -Force }

# Drop the RID-neutral System.Management.dll stub at the root. deps.json has a
# runtimeTargets override for rid=win pointing to runtimes/win/lib/net8.0/, so
# CoreCLR loads the real 312 KB Windows impl and the 73 KB root stub is never
# used. Keep runtimes/win/ — deleting it breaks LocalDimmingControl with
# "Could not load file or assembly 'System.Management'".
$rootStub = Join-Path $releaseDir 'System.Management.dll'
if (Test-Path $rootStub) { Remove-Item $rootStub -Force }

$regSize = Get-DirSizeMB $releaseDir
Write-Host "Framework-dependent build: ${regSize} MB" -ForegroundColor Green

# ── Step 4: Self-contained build ──────────────────────────────────

if (-not $SkipSelfContained) {
    Write-Host "`n=== Self-contained publish (trimmed) ===" -ForegroundColor Cyan
    & $DotnetPath publish $projectDir -c Release -r win-x64 --self-contained true `
        -p:PublishTrimmed=true -p:TrimMode=partial -p:_SuppressWinFormsTrimError=true `
        -o $scDir --nologo -v q
    if ($LASTEXITCODE -ne 0) { throw "Self-contained publish failed" }

    # Clean up win-x64 intermediate again
    if (Test-Path $winx64) { Remove-Item $winx64 -Recurse -Force }

    $beforeSize = Get-DirSizeMB $scDir
    Write-Host "Before stripping: ${beforeSize} MB"

    # Swap in trimmed WinRT DLLs
    Copy-Item (Join-Path $releaseDir 'Microsoft.Windows.SDK.NET.dll') $scDir -Force
    Copy-Item (Join-Path $releaseDir 'WinRT.Runtime.dll') $scDir -Force

    # Remove locale folders
    Get-ChildItem $scDir -Directory |
        Where-Object { $_.Name -match '^[a-z]{2}(-[A-Za-z]+)?$' } |
        Remove-Item -Recurse -Force

    # Remove versioned mscordaccore
    Get-ChildItem $scDir -Filter 'mscordaccore_*' | Remove-Item -Force

    # Remove all unnecessary DLLs
    Remove-Files $scDir $AllStrip

    # Remove PDBs
    Get-ChildItem $scDir -Filter '*.pdb' | Remove-Item -Force

    $afterSize = Get-DirSizeMB $scDir
    Write-Host "After stripping: ${afterSize} MB (saved $([math]::Round($beforeSize - $afterSize, 1)) MB)" -ForegroundColor Green

    # ── Organize: move managed DLLs to libs/ subfolder ──────────────
    Write-Host "Organizing DLLs into libs/ subfolder..."
    $libsDir = Join-Path $scDir 'libs'
    New-Item $libsDir -ItemType Directory -Force | Out-Null

    # Files that must remain in root (native host + app entry point)
    $rootKeep = @(
        'MechControl.exe', 'MechControl.dll',
        'MechControl.deps.json', 'MechControl.runtimeconfig.json',
        # Native runtime — loaded by the host before deps.json is consulted
        'coreclr.dll', 'clrjit.dll', 'clrgc.dll',
        'hostfxr.dll', 'hostpolicy.dll', 'mscorrc.dll',
        # Loaded by CoreCLR before the managed assembly resolver can run
        'System.Private.CoreLib.dll', 'System.Runtime.dll', 'System.Runtime.Loader.dll'
    )

    Get-ChildItem $scDir -Filter '*.dll' -File |
        Where-Object { $rootKeep -notcontains $_.Name } |
        Move-Item -Destination $libsDir

    # Remove moved assemblies from deps.json so they are not in the TPA list;
    # the app's AssemblyLoadContext.Resolving handler loads them from libs/ instead.
    $depsPath = Join-Path $scDir 'MechControl.deps.json'
    $depsJson = Get-Content $depsPath -Raw | ConvertFrom-Json
    $targetName = $depsJson.runtimeTarget.name
    $depsTarget = $depsJson.targets.$targetName
    $movedDlls = Get-ChildItem $libsDir -Filter '*.dll' -Name

    foreach ($lib in $depsTarget.PSObject.Properties) {
        if (-not $lib.Value.runtime) { continue }
        $newRuntime = [ordered]@{}
        foreach ($entry in $lib.Value.runtime.PSObject.Properties) {
            if ($entry.Name -notin $movedDlls) {
                $newRuntime[$entry.Name] = $entry.Value
            }
        }
        $lib.Value.runtime = [PSCustomObject]$newRuntime
    }

    $depsJson | ConvertTo-Json -Depth 20 | Set-Content $depsPath -Encoding utf8

    $rootCount = (Get-ChildItem $scDir -File).Count
    $libsCount = (Get-ChildItem $libsDir -File).Count
    Write-Host "Root: $rootCount files + libs/ ($libsCount DLLs) + Profiles/" -ForegroundColor Green
}

# ── Step 5: Create zips ───────────────────────────────────────────

Write-Host "`n=== Creating release zips ===" -ForegroundColor Cyan
New-Item $outDir -ItemType Directory -Force | Out-Null

# Framework-dependent zip (top-level files + Profiles only)
$staging = Join-Path $binDir '_staging'
New-Item $staging -ItemType Directory -Force | Out-Null
Get-ChildItem $releaseDir -File | Copy-Item -Destination $staging
Copy-Item (Join-Path $releaseDir 'Profiles') (Join-Path $staging 'Profiles') -Recurse
$runtimes = Join-Path $releaseDir 'runtimes'
if (Test-Path $runtimes) {
    Copy-Item $runtimes (Join-Path $staging 'runtimes') -Recurse
}

$fwZip = Join-Path $outDir 'MechControl-release.zip'
Compress-Archive -Path "$staging\*" -DestinationPath $fwZip -Force
Remove-Item $staging -Recurse -Force
$fwSize = [math]::Round((Get-Item $fwZip).Length / 1MB, 2)
Write-Host "  $fwZip ($fwSize MB)" -ForegroundColor Green

if (-not $SkipSelfContained) {
    if ($has7z) {
        $sc7z = Join-Path $outDir 'MechControl-selfcontained.7z'
        & $7zPath a -t7z -mx=9 -mfb=273 -ms=on -md=64m $sc7z "$scDir\*" | Out-Null
        $scSize = [math]::Round((Get-Item $sc7z).Length / 1MB, 2)
        Write-Host "  $sc7z ($scSize MB)" -ForegroundColor Green
    }
    $scZip = Join-Path $outDir 'MechControl-selfcontained.zip'
    Compress-Archive -Path "$scDir\*" -DestinationPath $scZip -Force
    $scSize = [math]::Round((Get-Item $scZip).Length / 1MB, 2)
    Write-Host "  $scZip ($scSize MB)" -ForegroundColor Green
}

Write-Host "`n=== Done ===" -ForegroundColor Cyan
