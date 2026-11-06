$oemDir = (Get-ChildItem 'C:\Program Files\OEM' | Where-Object { $_.PSIsContainer } | Select-Object -First 1).FullName
$dllPath = "$oemDir\AiStoneService\MyControlCenter\ACPIDriverDll.dll"
Write-Output "Reading: $dllPath"

# Read as both ASCII and Unicode
$bytes = [System.IO.File]::ReadAllBytes($dllPath)
$ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
$unicode = [System.Text.Encoding]::Unicode.GetString($bytes)

Write-Output "`n=== ASCII device paths ==="
$m1 = [regex]::Matches($ascii, '\\\\\.\\[A-Za-z0-9_]+')
foreach ($m in $m1) { Write-Output $m.Value }

$m2 = [regex]::Matches($ascii, '\\\\Device\\[A-Za-z0-9_]+')
foreach ($m in $m2) { Write-Output $m.Value }

Write-Output "`n=== Unicode device paths ==="
$m3 = [regex]::Matches($unicode, '\\\\\.\\[A-Za-z0-9_]+')
foreach ($m in $m3) { Write-Output $m.Value }

$m4 = [regex]::Matches($unicode, '\\\\Device\\[A-Za-z0-9_]+')
foreach ($m in $m4) { Write-Output $m.Value }

Write-Output "`n=== UWACPI strings ==="
$m5 = [regex]::Matches($ascii, 'UWACPI[A-Za-z0-9_]*')
foreach ($m in $m5) { Write-Output $m.Value }
$m6 = [regex]::Matches($unicode, 'UWACPI[A-Za-z0-9_]*')
foreach ($m in $m6) { Write-Output $m.Value }

Write-Output "`n=== ACPI/INOU strings ==="
$m7 = [regex]::Matches($unicode, 'ACPI[#\\]INOU[A-Za-z0-9_]*')
foreach ($m in $m7) { Write-Output $m.Value }

Write-Output "`n=== All strings containing 'Driver' (Unicode, unique) ==="
$m8 = [regex]::Matches($unicode, '[A-Za-z0-9_]*Driver[A-Za-z0-9_]*')
$unique = @{}
foreach ($m in $m8) { if (-not $unique.ContainsKey($m.Value)) { $unique[$m.Value] = $true; Write-Output $m.Value } }
