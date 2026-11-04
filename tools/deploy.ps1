#Requires -RunAsAdministrator
# Builds the CLI Release, closes the running app, copies its runtime files over the installed
# copy and starts it again. Settings, profiles and logs in the install directory are not touched.
param(
    [string]$BuildDirectory = 'C:/Temp/tandem/native-cli',
    [string]$TaskName = 'MechControl'
)
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot/build-native.ps1" -Configuration Release -WithCli -BuildDirectory $BuildDirectory
$vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
# The logon task starts the installed app, so its action names the install directory.
$install = Split-Path (Get-ScheduledTask -TaskName $TaskName).Actions[0].Execute

$running = Get-Process -Name MechControl -ErrorAction SilentlyContinue
if ($running) {
    Add-Type -Namespace MechControlDeploy -Name Window -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string className, string title);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
'@
    $window = [MechControlDeploy.Window]::FindWindowW('MechControl.Native.Main', [NullString]::Value)
    if ($window -eq [IntPtr]::Zero) { throw 'MechControl is running but its window was not found. Exit it from the tray, then run this again.' }
    # WM_ENDSESSION runs the same orderly shutdown as the tray's Exit command.
    [void][MechControlDeploy.Window]::PostMessageW($window, 0x16, [IntPtr]1, [IntPtr]::Zero)
    $running | Wait-Process -Timeout 60
}

& $cmake --install $BuildDirectory --config Release --component Runtime --prefix $install
if ($LASTEXITCODE) { throw 'Copying the runtime files failed.' }
Start-ScheduledTask -TaskName $TaskName
$deadline = (Get-Date).AddSeconds(15)
while (-not ($started = Get-Process -Name MechControl -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
if (-not $started) { throw "The $TaskName task did not start MechControl. See $install/mechcontrol.log." }
"MechControl restarted from $install (PID $($started.Id))."
