# ============================================================================
#  Build-Cli.ps1 —— 只重建 NvpwrCtl.exe
#
#  为什么不用 build.ps1：它从驱动开始按顺序构建，而驱动链接在当前的 VS 版本上会挂
#  （LNK1117: 选项"SUBSYSTEM:NATIVE,"10.00""中的语法错误）—— 那是新版本 link.exe
#  对 SUBSYSTEM 参数格式更严了，和本仓库的源码无关。驱动本身已经构建好并部署过，
#  不需要重编；这里只走 CLI 的编译与链接，参数与 build.ps1 里那两步完全一致。
# ============================================================================

param([ValidateSet('Release','Debug')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'

# 源码根目录就是本脚本所在目录（和 build.ps1 同级）。
$srcRoot = $PSScriptRoot

# ---- 工具链定位（与 build.ps1 相同的回退顺序）----
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
if (-not (Test-Path $cl))   { throw "cl.exe not found under $vsRoot" }
if (-not (Test-Path $link)) { throw "link.exe not found under $vsRoot" }

$pkg = "$env:USERPROFILE\.nuget\packages"
$sdkCppVer = '10.0.28000.1721'
$sdkUmInc     = "$pkg\microsoft.windows.sdk.cpp\$sdkCppVer\c\Include\10.0.28000.0\um"
$sdkSharedInc = "$pkg\microsoft.windows.sdk.cpp\$sdkCppVer\c\Include\10.0.28000.0\shared"
$sdkUcrtInc   = "$pkg\microsoft.windows.sdk.cpp\$sdkCppVer\c\Include\10.0.28000.0\ucrt"
$sdkUmLib     = "$pkg\microsoft.windows.sdk.cpp.x64\$sdkCppVer\c\um\x64"
$sdkUcrtLib   = "$pkg\microsoft.windows.sdk.cpp.x64\$sdkCppVer\c\ucrt\x64"
foreach ($p in @($sdkUmInc,$sdkSharedInc,$sdkUcrtInc,$sdkUmLib,$sdkUcrtLib)) {
    if (-not (Test-Path $p)) { throw "Required build dependency not found: $p" }
}

# ---- 编译 ----
$cliOut = Join-Path $PSScriptRoot "cli\x64\$Configuration"
New-Item -ItemType Directory -Force -Path $cliOut | Out-Null

$cxxFlags = @('/c','/nologo','/W4','/O2','/std:c++17','/utf-8','/EHsc','/DUNICODE','/D_UNICODE')
$appInc = @('/I', $sdkUmInc, '/I', $sdkSharedInc, '/I', $sdkUcrtInc, '/I', $msvcInc,
            '/I', (Join-Path $srcRoot 'shared'), '/I', (Join-Path $srcRoot 'app'))

Write-Host "Compiling nvpwrctl.cpp..." -ForegroundColor Yellow
& $cl @cxxFlags @appInc "/Fo$cliOut/" (Join-Path $srcRoot 'cli\nvpwrctl.cpp')
if ($LASTEXITCODE -ne 0) { throw 'CLI compilation failed.' }

# ---- 链接 ----
Write-Host "Linking NvpwrCtl.exe..." -ForegroundColor Yellow
& $link /nologo /SUBSYSTEM:CONSOLE /MACHINE:X64 /RELEASE /OPT:REF /OPT:ICF `
  /MANIFEST /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" `
  /LIBPATH:"$msvcLib" /LIBPATH:"$sdkUmLib" /LIBPATH:"$sdkUcrtLib" `
  Advapi32.lib Kernel32.lib `
  /OUT:"$cliOut\NvpwrCtl.exe" "$cliOut\nvpwrctl.obj"
if ($LASTEXITCODE -ne 0) { throw 'CLI linking failed.' }

Write-Host ""
Write-Host "Built: $cliOut\NvpwrCtl.exe" -ForegroundColor Green
Get-Item "$cliOut\NvpwrCtl.exe" | ForEach-Object { Write-Host ("  {0} bytes  {1}" -f $_.Length, $_.LastWriteTime) }

# ---- 后台服务 ----
# 服务与 CLI 共用 app/ 下的状态与 IPC 模块，所以它也必须跟着重建 ——
# DesiredState 里新增字段时，只重建 CLI 会让服务和 GUI 对同一份 state.ini 有两种理解。
Write-Host ""
Write-Host "Building background service..." -ForegroundColor Yellow
$svcOut = Join-Path $srcRoot "service\x64\$Configuration"
New-Item -ItemType Directory -Force -Path $svcOut | Out-Null
$svcSources = @(
    (Join-Path $srcRoot 'service\service_main.cpp'),
    (Join-Path $srcRoot 'app\nvpwr_ipc.cpp'),
    (Join-Path $srcRoot 'app\nvpwr_ui_state.cpp'),
    (Join-Path $srcRoot 'app\nvpwr_mvolt_bridge.cpp')
)
& $cl @cxxFlags @appInc "/I" (Join-Path $srcRoot 'app') "/Fo$svcOut/" @svcSources
if ($LASTEXITCODE -ne 0) { throw 'Service compilation failed.' }

& $link /nologo /SUBSYSTEM:CONSOLE /MACHINE:X64 /RELEASE /OPT:REF /OPT:ICF `
  /MANIFEST /MANIFESTUAC:"level='asInvoker' uiAccess='false'" `
  /LIBPATH:"$msvcLib" /LIBPATH:"$sdkUmLib" /LIBPATH:"$sdkUcrtLib" `
  Advapi32.lib Kernel32.lib User32.lib `
  /OUT:"$svcOut\NvpwrSvc.exe" `
  "$svcOut\service_main.obj" "$svcOut\nvpwr_ipc.obj" `
  "$svcOut\nvpwr_ui_state.obj" "$svcOut\nvpwr_mvolt_bridge.obj"
if ($LASTEXITCODE -ne 0) { throw 'Service linking failed.' }

Write-Host ""
Write-Host "Built: $svcOut\NvpwrSvc.exe" -ForegroundColor Green
Get-Item "$svcOut\NvpwrSvc.exe" | ForEach-Object { Write-Host ("  {0} bytes  {1}" -f $_.Length, $_.LastWriteTime) }
