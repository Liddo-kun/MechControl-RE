@echo off
echo ============================================
echo  BLE Traffic Capture - START
echo  Run as Administrator!
echo ============================================
echo.
echo Starting Bluetooth ETW trace (multiple providers)...
logman create trace "BLE_Capture" -ow -o C:\BLE_trace.etl -nb 16 16 -bs 1024 -mode Circular -f bincirc -max 4096 -ets
logman update trace "BLE_Capture" -p "{8a1f9517-3a8c-4a9e-a018-4f17a200f277}" 0xffffffffffffffff 0xff -ets
logman update trace "BLE_Capture" -p "{36da592d-e43a-4e28-af6f-4bc57c5a11e8}" 0xffffffffffffffff 0xff -ets
logman update trace "BLE_Capture" -p "{2ff3e230-4e2b-4eeb-8471-2e071e32a552}" 0xffffffffffffffff 0xff -ets
logman update trace "BLE_Capture" -p "{ebb10b07-ccf0-42d8-b93e-6b44cb90bf4a}" 0xffffffffffffffff 0xff -ets
if errorlevel 1 (
    echo.
    echo ERROR: Failed to start trace. Make sure you are running as Administrator.
    pause
    exit /b 1
)
echo.
echo Trace is running. Now:
echo   1. Open the OEM Control Center
echo   2. Connect to the liquid cooler
echo   3. Change fan/pump/RGB settings
echo   4. Navigate to any monitoring/telemetry screens
echo   5. Wait 30 seconds
echo   6. Come back here and press any key to stop
echo.
pause
echo.
echo Stopping trace...
logman stop "BLE_Capture" -ets
echo.
echo Done! Capture saved to: C:\BLE_trace.etl
echo Please send this file for analysis.
echo.
pause
