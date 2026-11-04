param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$BuildDirectory = 'C:/Temp/tandem/MechControl-native-build',
    [switch]$WithCli,
    [switch]$Check
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'Install Visual Studio 2022 Build Tools with Desktop development with C++ and Windows SDK.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'The MSVC x64 C++ tools are missing.' }
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
if (-not (Test-Path $cmake)) { throw 'Install the Visual Studio C++ CMake tools component.' }
New-Item -ItemType Directory -Force $BuildDirectory | Out-Null
$temp = 'C:/Temp/tandem/mechcontrol-native-tool-temp'
New-Item -ItemType Directory -Force $temp | Out-Null
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
try {
    $env:TEMP = $temp
    $env:TMP = $temp
    $cli = if ($WithCli) { 'ON' } else { 'OFF' }
    & $cmake -S $root -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 "-DMECHCONTROL_WITH_CLI=$cli"
    if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
    & $cmake --build $BuildDirectory --config $Configuration --parallel
    if ($LASTEXITCODE) { throw 'Native build failed.' }
    if ($Check) {
        & $ctest --test-dir $BuildDirectory -C $Configuration --output-on-failure --timeout 45
        if ($LASTEXITCODE) { throw 'Native checks failed.' }
    }
} finally {
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
}
