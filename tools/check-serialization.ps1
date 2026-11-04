param(
    [Parameter(Mandatory)][string]$NativeFixtureDirectory,
    [Parameter(Mandatory)][string]$NativeCheck,
    [string]$ReferenceDirectory = 'C:/Users/Jon/code/Mechcontrol',
    [string]$Dotnet = 'C:/Users/Jon/.dotnet/dotnet.exe',
    [string]$ScratchRoot = 'C:/Temp/tandem/mechcontrol-native-phase2/serialization'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$scratch = Join-Path $ScratchRoot ([guid]::NewGuid().ToString('N'))
$source = Join-Path $scratch 'reference-source'
New-Item -ItemType Directory -Force $source, "$source/Profile", "$source/Features" | Out-Null
# Copy only the schema/serializer owners. All builds and writes stay in scratch.
$files = @('AppJsonContext.cs', 'Profile/ProfileSchema.cs', 'Profile/UniversalDefaults.cs',
    'Profile/UserProfile.cs', 'Profile/UserProfileManager.cs', 'Profile/GpuFailsafeSettings.cs',
    'Profile/ThermalProtectionState.cs', 'Features/LightingState.cs', 'Features/LiquidCoolerState.cs', 'Features/LiquidCoolerControl.cs')
foreach ($file in $files) { Copy-Item (Join-Path "$ReferenceDirectory/MechControl" $file) (Join-Path $source $file) }
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
