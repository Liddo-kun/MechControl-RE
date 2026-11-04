param(
    [string]$BuildDirectory = 'C:/Temp/tandem/MechControl-native-build',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$WithCli
)
$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path $OutputDirectory) { throw 'Choose a new output directory so existing packages and evidence are preserved.' }
& "$PSScriptRoot/build-native.ps1" -BuildDirectory $BuildDirectory -Configuration Release -WithCli:$WithCli -Check
if (-not $?) { throw 'Release validation failed.' }
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$variant = if ($WithCli) { 'cli' } else { 'default' }
$name = "MechControl-native-win-x64-$variant"
New-Item -ItemType Directory $OutputDirectory | Out-Null
$artifacts = foreach ($component in @('Runtime', 'DeveloperSymbols')) {
    $suffix = if ($component -eq 'Runtime') { '' } else { '-symbols' }
    $stage = Join-Path $OutputDirectory ($name + $suffix)
    & $cmake --install $BuildDirectory --config Release --component $component --prefix $stage | Out-Host
    if ($LASTEXITCODE) { throw "Installation of $component failed." }
    $zip = "$stage.zip"
    Compress-Archive -Path "$stage/*" -DestinationPath $zip -CompressionLevel Optimal
    $files = @(Get-ChildItem $stage -Recurse -File)
    [ordered]@{
        component = $component; archive = (Split-Path $zip -Leaf)
        archiveBytes = (Get-Item $zip).Length; sha256 = (Get-FileHash $zip -Algorithm SHA256).Hash
        payloadBytes = ($files | Measure-Object Length -Sum).Sum
        files = @($files | ForEach-Object { [ordered]@{
            path = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
            bytes = $_.Length; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
        } })
    }
}
[ordered]@{ variant = $variant; created = [DateTimeOffset]::Now.ToString('o'); artifacts = @($artifacts) } |
    ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutputDirectory 'artifacts.json') -Encoding utf8
$artifacts | Select-Object component, archive, archiveBytes, payloadBytes, sha256 | Format-Table -AutoSize
