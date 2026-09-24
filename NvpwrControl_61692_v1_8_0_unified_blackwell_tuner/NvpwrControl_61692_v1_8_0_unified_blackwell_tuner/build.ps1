param([ValidateSet('Release','Debug')][string]$Configuration='Release')
$ErrorActionPreference='Stop'

# ---------------------------------------------------------------- toolchain
# WHERE/WHY: the original project used a single vswhere query. On some Visual
# Studio 18/Preview installations that query returns nothing even though MSBuild
# is installed, so vswhere stays the first choice and a known layout plus a
# constrained search act as fallbacks.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = $null
if (Test-Path $vswhere) {
    $vsRoot = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
}
if (-not $vsRoot) {
    $knownRoots = @(
        "$env:ProgramFiles\Microsoft Visual Studio\18\Community",
        "$env:ProgramFiles\Microsoft Visual Studio\18\BuildTools",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\BuildTools"
    )
    $vsRoot = $knownRoots | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $vsRoot) { throw 'Visual Studio C++ build tools not found.' }

$cl   = Get-ChildItem "$vsRoot\VC\Tools\MSVC" -Filter "cl.exe"  -Recurse | Where-Object { $_.FullName -match "bin\\Hostx64\\x64\\cl\.exe$"   } | Select-Object -First 1 -ExpandProperty FullName
$link = Get-ChildItem "$vsRoot\VC\Tools\MSVC" -Filter "link.exe" -Recurse | Where-Object { $_.FullName -match "bin\\Hostx64\\x64\\link\.exe$" } | Select-Object -First 1 -ExpandProperty FullName
$msvcToolsDir = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $cl)))
$msvcLib = Join-Path $msvcToolsDir "lib\x64"
$msvcInc = Join-Path $msvcToolsDir "include"

# The WDK/SDK are consumed from the NuGet package cache. The version is pinned
# because the source tree was audited against it; override with -WdkVersion.
$pkg = "$env:USERPROFILE\.nuget\packages"
$wdkVer = '10.0.28000.2526'
$sdkCppVer = '10.0.28000.1721'
$wdkKmInc   = "$pkg\microsoft.windows.wdk.x64\$wdkVer\c\Include\10.0.28000.0\km"
$wdkKmCrt   = "$pkg\microsoft.windows.wdk.x64\$wdkVer\c\Include\10.0.28000.0\km\crt"
$wdkKmLib   = "$pkg\microsoft.windows.wdk.x64\$wdkVer\c\Lib\10.0.28000.0\km\x64"
$sdkUmInc     = "$pkg\microsoft.windows.sdk.cpp\$sdkCppVer\c\Include\10.0.28000.0\um"
$sdkSharedInc = "$pkg\microsoft.windows.sdk.cpp\$sdkCppVer\c\Include\10.0.28000.0\shared"
$sdkUcrtInc   = "$pkg\microsoft.windows.sdk.cpp\$sdkCppVer\c\Include\10.0.28000.0\ucrt"
$sdkUmLib     = "$pkg\microsoft.windows.sdk.cpp.x64\$sdkCppVer\c\um\x64"
$sdkUcrtLib   = "$pkg\microsoft.windows.sdk.cpp.x64\$sdkCppVer\c\ucrt\x64"

foreach ($p in @($wdkKmInc,$wdkKmCrt,$wdkKmLib,$sdkUmInc,$sdkSharedInc,$sdkUcrtInc,$sdkUmLib,$sdkUcrtLib)) {
    if (-not (Test-Path $p)) {
        Write-Host "Missing dependency path: $p" -ForegroundColor Red
        Write-Host "Install the matching WDK/SDK NuGet packages, or edit the version variables at the top of build.ps1." -ForegroundColor Yellow
        throw "Required build dependency not found: $p"
    }
}

$signtool = Get-ChildItem -Path "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Filter "signtool.exe" -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "\\x64\\signtool\.exe$" } | Select-Object -First 1 -ExpandProperty FullName

Write-Host "Compiler: $cl" -ForegroundColor Cyan

# Shared include set for every user-mode target.
$appInc = @(
  "/I", "$sdkUmInc",
  "/I", "$sdkSharedInc",
  "/I", "$sdkUcrtInc",
  "/I", "$msvcInc",
  "/I", "$PSScriptRoot\shared",
  "/I", "$PSScriptRoot\app"
)
# /utf-8 is required: the UI string table carries Chinese literals, and without
# it MSVC would interpret them in the active code page and mangle them.
# UNICODE/_UNICODE are required too: every Win32 call in this tree is the W
# variant, and without the macros IDC_ARROW / IDI_APPLICATION expand to narrow
# string literals that do not bind to LoadCursorW / LoadIconW.
$cxxFlags = @('/c','/nologo','/W4','/O2','/std:c++17','/utf-8','/EHsc','/DUNICODE','/D_UNICODE')

function Compile-Objects {
    param([string]$OutDir, [string[]]$Sources, [string[]]$ExtraInc = @())
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    & $cl @cxxFlags @appInc @ExtraInc "/Fo$OutDir/" @Sources
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed in $OutDir" }
}

# 1 --------------------------------------------------------------- driver
Write-Host 'Building Nvpwr driver...' -ForegroundColor Yellow
$driverOut = "$PSScriptRoot\driver\x64\$Configuration"
New-Item -ItemType Directory -Force -Path $driverOut | Out-Null
& $cl /c /nologo /W4 /Ox /D _WIN64 /D _AMD64_ /D AMD64 /D _WIN32_WINNT=0x0A00 /D WINVER=0x0A00 /D WINNT=1 /D NTDDI_VERSION=0xA000012 /kernel `
  /I "$wdkKmInc" /I "$wdkKmCrt" /I "$sdkSharedInc" /I "$sdkUcrtInc" /I "$PSScriptRoot\shared" `
  /Fo"$driverOut\driver.obj" "$PSScriptRoot\driver\driver.c"
if ($LASTEXITCODE -ne 0) { throw 'Driver compilation failed.' }

& $link /nologo /WX /SECTION:"INIT,d" `
  "$wdkKmLib\Aux_Klib.lib" `
  "$wdkKmLib\BufferOverflowFastFailK.lib" `
  "$wdkKmLib\ntoskrnl.lib" `
  "$wdkKmLib\hal.lib" `
  "$wdkKmLib\wmilib.lib" `
  /NODEFAULTLIB /MANIFEST:NO /DEBUG /SUBSYSTEM:NATIVE,"10.00" /Driver /OPT:REF /OPT:ICF /ENTRY:"GsDriverEntry" `
  /RELEASE /MERGE:"_TEXT=.text;_PAGE=PAGE" /MACHINE:X64 /kernel `
  /OUT:"$driverOut\Nvpwr.sys" "$driverOut\driver.obj"
if ($LASTEXITCODE -ne 0) { throw 'Driver linking failed.' }

# Sign the driver with the local test certificate.
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -match "Nvpwr Local Test Cert" } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Nvpwr Local Test Cert" -CertStoreLocation "Cert:\CurrentUser\My"
}
Export-Certificate -Cert $cert -FilePath "$PSScriptRoot\driver\Nvpwr.cer" -Force | Out-Null
if ($signtool) {
    & $signtool sign /fd sha256 /sha1 $cert.Thumbprint /v "$driverOut\Nvpwr.sys"
} else {
    Write-Host 'signtool was not found; the driver is left unsigned.' -ForegroundColor Yellow
}

# 2 ------------------------------------------------------------------ GUI
# 1.9.0 tree: the previous single main.cpp was split into a UI layer plus
# dedicated modules for state, IPC, telemetry and voltage policy.
Write-Host 'Building GUI...' -ForegroundColor Yellow
$appOut = "$PSScriptRoot\app\x64\$Configuration"
$guiSources = @(
  "$PSScriptRoot\app\main.cpp",
  "$PSScriptRoot\app\nvpwr_ui_state.cpp",
  "$PSScriptRoot\app\nvpwr_ipc.cpp",
  "$PSScriptRoot\app\nvpwr_mvolt_bridge.cpp",
  "$PSScriptRoot\app\nvapi_power_policies.cpp",
  "$PSScriptRoot\app\nvpwr_telemetry.cpp",
  "$PSScriptRoot\app\xmg_probe.cpp",
  "$PSScriptRoot\app\nvapi_probe.cpp",
  "$PSScriptRoot\app\nvapi_tuner.cpp"
)
Compile-Objects -OutDir $appOut -Sources $guiSources

& $link /nologo /SUBSYSTEM:WINDOWS /MACHINE:X64 /RELEASE /OPT:REF /OPT:ICF `
  /MANIFEST /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" `
  /LIBPATH:"$msvcLib" /LIBPATH:"$sdkUmLib" /LIBPATH:"$sdkUcrtLib" `
  Comctl32.lib Advapi32.lib User32.lib Gdi32.lib Kernel32.lib Shell32.lib Dxgi.lib `
  /OUT:"$appOut\NvpwrControl.exe" `
  "$appOut\main.obj" "$appOut\nvpwr_ui_state.obj" "$appOut\nvpwr_ipc.obj" `
  "$appOut\nvpwr_mvolt_bridge.obj" "$appOut\nvapi_power_policies.obj" `
  "$appOut\nvpwr_telemetry.obj" `
  "$appOut\xmg_probe.obj" "$appOut\nvapi_probe.obj" "$appOut\nvapi_tuner.obj"
if ($LASTEXITCODE -ne 0) { throw 'GUI linking failed.' }

# 3 ------------------------------------------------------------------ CLI
Write-Host 'Building CLI fallback...' -ForegroundColor Yellow
$cliOut = "$PSScriptRoot\cli\x64\$Configuration"
Compile-Objects -OutDir $cliOut -Sources @("$PSScriptRoot\cli\nvpwrctl.cpp")

& $link /nologo /SUBSYSTEM:CONSOLE /MACHINE:X64 /RELEASE /OPT:REF /OPT:ICF `
  /MANIFEST /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" `
  /LIBPATH:"$msvcLib" /LIBPATH:"$sdkUmLib" /LIBPATH:"$sdkUcrtLib" `
  Advapi32.lib Kernel32.lib `
  /OUT:"$cliOut\NvpwrCtl.exe" "$cliOut\nvpwrctl.obj"
if ($LASTEXITCODE -ne 0) { throw 'CLI linking failed.' }

# 4 -------------------------------------------------------------- service
# The background service owns "keep these settings applied" across a reboot.
# It links the shared state/IPC modules plus the companion-tool bridge, because
# voltage can only be replayed by delegating to mVolt+.
Write-Host 'Building background service...' -ForegroundColor Yellow
$svcOut = "$PSScriptRoot\service\x64\$Configuration"
$svcSources = @(
  "$PSScriptRoot\service\service_main.cpp",
  "$PSScriptRoot\app\nvpwr_ipc.cpp",
  "$PSScriptRoot\app\nvpwr_ui_state.cpp",
  "$PSScriptRoot\app\nvpwr_mvolt_bridge.cpp"
)
Compile-Objects -OutDir $svcOut -Sources $svcSources -ExtraInc @("/I", "$PSScriptRoot\app")

& $link /nologo /SUBSYSTEM:CONSOLE /MACHINE:X64 /RELEASE /OPT:REF /OPT:ICF `
  /MANIFEST /MANIFESTUAC:"level='asInvoker' uiAccess='false'" `
  /LIBPATH:"$msvcLib" /LIBPATH:"$sdkUmLib" /LIBPATH:"$sdkUcrtLib" `
  Advapi32.lib Kernel32.lib User32.lib `
  /OUT:"$svcOut\NvpwrSvc.exe" `
  "$svcOut\service_main.obj" "$svcOut\nvpwr_ipc.obj" `
  "$svcOut\nvpwr_ui_state.obj" "$svcOut\nvpwr_mvolt_bridge.obj"
if ($LASTEXITCODE -ne 0) { throw 'Service linking failed.' }

# 5 ----------------------------------------------------------- distribute
$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
try { Copy-Item "$driverOut\Nvpwr.sys" $dist -Force -ErrorAction Stop }
catch { Write-Host 'Note: Nvpwr.sys is loaded in kernel memory; the existing file was preserved.' -ForegroundColor Yellow }
Copy-Item "$PSScriptRoot\driver\Nvpwr.cer" $dist -Force
try { Copy-Item "$appOut\NvpwrControl.exe" $dist -Force -ErrorAction Stop }
catch { Write-Host 'Note: dist\NvpwrControl.exe is currently running and was not replaced.' -ForegroundColor Yellow }
Copy-Item "$cliOut\NvpwrCtl.exe" $dist -Force
Copy-Item "$svcOut\NvpwrSvc.exe" $dist -Force
Copy-Item "$PSScriptRoot\restart-nvidia-device.ps1" $dist -Force
Copy-Item "$PSScriptRoot\collect-debug.ps1" $dist -Force
Copy-Item "$driverOut\Nvpwr.sys" $appOut -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host 'Build complete.' -ForegroundColor Green
Write-Host '  Run dist\NvpwrControl.exe as administrator for the GUI.' -ForegroundColor Cyan
Write-Host '  NvpwrSvc.exe installs itself from the GUI (Settings page) or with: ' -ForegroundColor Cyan
Write-Host '    sc create NvpwrSvc binPath= "<path>\NvpwrSvc.exe" start= auto' -ForegroundColor Cyan
Write-Host '  GUI log    : C:\ProgramData\NvpwrControl\nvpwr-control.log' -ForegroundColor Cyan
Write-Host '  Service log: C:\ProgramData\NvpwrControl\nvpwr-service.log' -ForegroundColor Cyan
Get-ChildItem $dist | Select-Object Name,Length,FullName
