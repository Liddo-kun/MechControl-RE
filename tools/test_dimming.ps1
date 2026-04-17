# Test local dimming toggle - mirrors exact OEM code pattern
# Run from elevated PowerShell: powershell -ExecutionPolicy Bypass -File C:\Users\Jon\clean\test_dimming.ps1

$obj = New-Object System.Management.ManagementObject("root\WMI", "AcpiTest_MULong.InstanceName='ACPI\PNP0C14\1_1'", $null)

Write-Host ""
Write-Host "=== Local Dimming Test ==="
Write-Host "Make sure a dark/black image is visible on screen."
Write-Host ""

# Turn OFF
$p = $obj.GetMethodParameters("GetSetULong")
$p["Data"] = [uint64]0x0000050000000001
$r = $obj.InvokeMethod("GetSetULong", $p, $null)
$ret = $r["Return"]
Write-Host "OFF command sent. Return: $ret (expect 170/0xAA)"
Write-Host ""
Read-Host "Did the display change? Press Enter to send ON command"

# Turn ON
$p2 = $obj.GetMethodParameters("GetSetULong")
$p2["Data"] = [uint64]0x0000050000000002
$r2 = $obj.InvokeMethod("GetSetULong", $p2, $null)
$ret2 = $r2["Return"]
Write-Host "ON command sent. Return: $ret2 (expect 187/0xBB)"
Write-Host ""
Read-Host "Press Enter to exit"
