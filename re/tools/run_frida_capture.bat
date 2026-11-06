@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM  run_frida_capture.bat - One-click Frida capture wrapper
REM
REM  Usage: right-click, "Run as administrator"
REM
REM  Does:
REM    1. verifies admin
REM    2. verifies Python + frida installed (offers to install frida)
REM    3. lists candidate OEM service processes so user can verify
REM    4. confirms GCUService.exe is running
REM    5. launches frida_full_capture.py
REM ============================================================

title Frida EC Capture - MechControl fan curve investigation

echo.
echo ============================================================
echo   Frida EC Capture - MechControl fan curve investigation
echo ============================================================
echo.

REM --- 1. Admin check ---
net session >nul 2>&1
if errorlevel 1 (
    echo [ERROR] This script must run as Administrator.
    echo.
    echo   Right-click run_frida_capture.bat and choose
    echo   "Run as administrator".
    echo.
    pause
    exit /b 1
)
echo [OK] Running as Administrator

REM --- 2. Move to the .bat's own directory so relative paths work ---
cd /d "%~dp0"

REM --- 3. Check the .py and .js files are here ---
if not exist "frida_full_capture.py" (
    echo [ERROR] frida_full_capture.py not found in this folder:
    echo   %~dp0
    echo.
    echo Make sure frida_full_capture.py and frida_full_hook.js are
    echo in the SAME folder as this .bat file.
    echo.
    pause
    exit /b 1
)
if not exist "frida_full_hook.js" (
    echo [ERROR] frida_full_hook.js not found in this folder:
    echo   %~dp0
    echo.
    pause
    exit /b 1
)
echo [OK] frida_full_capture.py and frida_full_hook.js present

REM --- 4. Check Python ---
where python >nul 2>&1
if errorlevel 1 (
    echo [ERROR] python not found in PATH.
    echo.
    echo Install Python 3.x from https://www.python.org/downloads/
    echo During install check "Add Python to PATH".
    echo Then re-run this script.
    echo.
    pause
    exit /b 1
)
for /f "delims=" %%V in ('python --version 2^>^&1') do set PYVER=%%V
echo [OK] Python found: !PYVER!

REM --- 5. Check frida ---
python -c "import frida" >nul 2>&1
if errorlevel 1 (
    echo [INFO] frida not installed. Installing frida + frida-tools...
    echo.
    python -m pip install --upgrade pip
    python -m pip install frida frida-tools
    if errorlevel 1 (
        echo.
        echo [ERROR] pip install failed. Check network connection and try:
        echo   python -m pip install frida frida-tools
        echo.
        pause
        exit /b 1
    )
    python -c "import frida" >nul 2>&1
    if errorlevel 1 (
        echo [ERROR] frida still not importable after install. Aborting.
        pause
        exit /b 1
    )
)
for /f "delims=" %%V in ('python -c "import frida; print(frida.__version__)" 2^>^&1') do set FRIDAVER=%%V
echo [OK] frida installed: !FRIDAVER!

REM --- 6. List candidate OEM service processes so user can verify name ---
echo.
echo ============================================================
echo   Detecting OEM service processes
echo ============================================================
powershell -NoProfile -Command "Get-Process | Where-Object { $_.ProcessName -match 'GCU|Control|Mech|Uniwill|Fan|CC' } | Select-Object Id, ProcessName, @{Name='Path';Expression={$_.Path}} | Format-Table -AutoSize | Out-String"

REM --- 7. Confirm GCUService is running and capture its PID ---
REM Uses PowerShell -EncodedCommand (UTF-16LE base64) to bypass all cmd.exe
REM escaping issues with pipes, quotes, and parens. The decoded command is:
REM   $p = @(Get-Process | Where-Object { $_.ProcessName -eq 'GCUService' });
REM   if ($p.Count -gt 0) { $p[0].Id }
set "ENCCMD_GCU=JABwACAAPQAgAEAAKABHAGUAdAAtAFAAcgBvAGMAZQBzAHMAIAB8ACAAVwBoAGUAcgBlAC0ATwBiAGoAZQBjAHQAIAB7ACAAJABfAC4AUAByAG8AYwBlAHMAcwBOAGEAbQBlACAALQBlAHEAIAAnAEcAQwBVAFMAZQByAHYAaQBjAGUAJwAgAH0AKQA7ACAAaQBmACAAKAAkAHAALgBDAG8AdQBuAHQAIAAtAGcAdAAgADAAKQAgAHsAIAAkAHAAWwAwAF0ALgBJAGQAIAB9AA=="
set "GCUPID="
for /f "delims=" %%P in ('powershell -NoProfile -EncodedCommand !ENCCMD_GCU! 2^>nul') do set "GCUPID=%%P"

if not defined GCUPID goto :gcu_missing
echo [OK] GCUService.exe running PID=!GCUPID!
goto :gcu_ok

:gcu_missing
echo.
echo [WARN] GCUService.exe is NOT running.
echo.
echo   - Launch the OEM Control Center app now
echo   - Wait until its UI is fully loaded
echo   - Re-run this .bat
echo.
echo If your OEM service has a different name, check the process table
echo above and tell Jon so the script can be updated.
echo.
pause
exit /b 1

:gcu_ok

REM --- 8. Launch scripted capture ---
echo.
echo ============================================================
echo   READY - launching scripted capture
echo ============================================================
echo.
echo The script will walk you through each step. For every step
echo you will:
echo.
echo   1. Read the on-screen instructions
echo   2. Press Enter to START the step   (marker opens)
echo   3. Do the action described
echo   4. Press Enter to END the step     (marker closes)
echo.
echo Type "quit" at any prompt to abort early.
echo.
echo Log will be saved to:
echo   %~dp0frida_full_capture.log
echo.
echo Press any key to launch the capture...
pause >nul

python frida_full_capture.py --scripted

echo.
echo ============================================================
echo   Capture finished. Please send this file back to Jon:
echo     %~dp0frida_full_capture.log
echo ============================================================
echo.
pause
endlocal
