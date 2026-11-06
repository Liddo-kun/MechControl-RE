$output = powercfg /qh 381b4222-f694-41f0-9685-ff5bb260df2e 2>&1
$lines = $output -split "`n"
$subgroup = ''
foreach ($l in $lines) {
    if ($l -match 'Subgroup GUID: (\S+)') { $subgroup = $Matches[1] }
    if ($l -match 'e69653ca') { Write-Output "Subgroup: $subgroup"; break }
}
