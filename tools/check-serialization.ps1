param(
    [Parameter(Mandatory)][string]$NativeFixtureDirectory,
    [Parameter(Mandatory)][string]$NativeCheck,
    [string]$ReferenceCommit = 'v1.6.0',
    [string]$Dotnet = 'C:/Users/Jon/.dotnet/dotnet.exe',
    [string]$ScratchRoot = 'C:/Temp/tandem/mechcontrol-check/serialization'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$scratch = Join-Path $ScratchRoot ([guid]::NewGuid().ToString('N'))
$source = Join-Path $scratch 'reference-source'
New-Item -ItemType Directory -Force $source | Out-Null
# Extract only the C# app's schema/serializer owners. All builds and writes stay in scratch.
$files = @('AppJsonContext.cs', 'Profile/ProfileSchema.cs', 'Profile/UniversalDefaults.cs',
    'Profile/UserProfile.cs', 'Profile/UserProfileManager.cs', 'Profile/GpuFailsafeSettings.cs',
    'Profile/ThermalProtectionState.cs', 'Features/LightingState.cs', 'Features/LiquidCoolerState.cs', 'Features/LiquidCoolerControl.cs')
git -C $root archive -o "$scratch/reference.tar" $ReferenceCommit ($files | ForEach-Object { "MechControl/$_" })
if ($LASTEXITCODE) { throw "Cannot extract the C# sources from $ReferenceCommit." }
tar -xf "$scratch/reference.tar" -C $source --strip-components=1
if ($LASTEXITCODE) { throw 'Cannot unpack the C# sources.' }
Copy-Item "$root/tests/serialization/*" $scratch
$oldTemp=$env:TEMP; $oldTmp=$env:TMP; $oldCli=$env:DOTNET_CLI_HOME
$oldCert=$env:DOTNET_GENERATE_ASPNET_CERTIFICATE; $oldTelemetry=$env:DOTNET_CLI_TELEMETRY_OPTOUT; $oldLogo=$env:DOTNET_NOLOGO
try {
    $env:TEMP=$scratch; $env:TMP=$scratch; $env:DOTNET_CLI_HOME=$scratch
    $env:DOTNET_GENERATE_ASPNET_CERTIFICATE='false'; $env:DOTNET_CLI_TELEMETRY_OPTOUT='1'; $env:DOTNET_NOLOGO='1'
    & $Dotnet build "$scratch/ReferenceReader.csproj" -c Release "-p:ReferenceSource=$source" --nologo
    if ($LASTEXITCODE) { throw 'Isolated reference reader build failed.' }
    & $Dotnet "$scratch/bin/Release/net8.0/ReferenceReader.dll" $NativeFixtureDirectory "$scratch/roundtrip"
    if ($LASTEXITCODE) { throw 'Original representation rejected native output.' }
    & $NativeCheck --read-reference "$scratch/roundtrip"
    if ($LASTEXITCODE) { throw 'Native reader rejected C# round-trip output.' }
    Write-Output "Serialization evidence: $scratch"
} finally {
    $env:TEMP=$oldTemp; $env:TMP=$oldTmp; $env:DOTNET_CLI_HOME=$oldCli
    $env:DOTNET_GENERATE_ASPNET_CERTIFICATE=$oldCert; $env:DOTNET_CLI_TELEMETRY_OPTOUT=$oldTelemetry; $env:DOTNET_NOLOGO=$oldLogo
}
