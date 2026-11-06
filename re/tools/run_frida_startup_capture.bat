@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM  run_frida_startup_capture.bat - Targeted GCUService startup capture
REM
REM  Usage: right-click, "Run as administrator"
REM
REM  Captures what GCUService does at startup to re-establish
REM  fan control after MechControl's broken apply.
REM
REM  IMPORTANT: GCUService should be STOPPED before running this.
REM  The script will auto-detect and attach when you start it.
REM ============================================================

title Frida Startup Capture - MechControl fan curve investigation

echo.
echo ============================================================
echo   Frida STARTUP Capture
echo   MechControl fan curve investigation
echo ============================================================
echo.

REM --- 1. Admin check ---
net session >nul 2>&1
if errorlevel 1 (
    echo [ERROR] This script must run as Administrator.
    echo.
    echo   Right-click run_frida_startup_capture.bat and choose
    echo   "Run as administrator".
    echo.
    pause
    exit /b 1
)
echo [OK] Running as Administrator

REM --- 2. Move to script directory ---
cd /d "%~dp0"

REM --- 3. Check required files ---
if not exist "frida_startup_capture.py" (
    echo [ERROR] frida_startup_capture.py not found in:
    echo   %~dp0
    echo.
    echo Make sure frida_startup_capture.py and frida_full_hook.js
    echo are in the SAME folder as this .bat file.
    echo.
    pause
    exit /b 1
)
if not exist "frida_full_hook.js" (
    echo [ERROR] frida_full_hook.js not found in:
    echo   %~dp0
    echo.
    pause
    exit /b 1
)
echo [OK] Script files present

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

echo.
echo The script will walk you through 4 steps:
echo   1. Set up OEM app (custom mode, 80%% flat fans)
echo   2. Stop GCUService
echo   3. Apply broken curve with MechControl
echo   4. Start GCUService and capture what it does
echo.
echo Log will be saved to:
echo   %~dp0frida_startup_capture.log
echo.
echo Press any key to launch...
pause >nul

python frida_startup_capture.py

echo.
echo ============================================================
echo   Capture finished. Please send this file back to Jon:
echo     %~dp0frida_startup_capture.log
echo ============================================================
echo.
pause
endlocal
