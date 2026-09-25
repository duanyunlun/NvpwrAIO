@echo off
chcp 65001 >nul
title NvpwrControl OSD 桥接

rem ============================================================================
rem  把 HWiNFO 的传感器数据写成英文，推进 RTSS 的 OSD
rem
rem  依赖：
rem    · HWiNFO 运行中，且已打开【共享内存支持】（设置 → 主要设置）
rem    · RTSS 运行中
rem    · 需要管理员权限（要和 RTSS/HWiNFO 同级才能映射它们的共享内存）
rem
rem  关闭这个窗口即停止并释放 OSD 槽位。
rem ============================================================================

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo 需要管理员权限，正在提权...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo ============================================================
echo   NvpwrControl OSD 桥接
echo   HWiNFO 数据 -^> RTSS 叠加层（英文标签）
echo ============================================================
echo.
echo   关闭本窗口即停止。
echo.

pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0rtss-osd-bridge.ps1"

echo.
echo 已退出。
pause
