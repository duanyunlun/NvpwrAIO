@echo off
chcp 65001 >nul
title NvpwrControl OSD Bridge
cd /d "%~dp0"

echo ============================================================
echo    NvpwrControl OSD Bridge
echo    HWiNFO sensors  -^>  RTSS overlay (English labels)
echo ============================================================
echo.
echo    Keep this window open. Close it to stop.
echo.

where pwsh >nul 2>&1
if %errorlevel% equ 0 (
    pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0rtss-osd-bridge.ps1"
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0rtss-osd-bridge.ps1"
)

echo.
echo ------------------------------------------------------------
echo   Stopped. Press any key to close this window.
echo ------------------------------------------------------------
pause >nul
