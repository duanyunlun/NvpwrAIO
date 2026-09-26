/*
    main.cpp — Nvpwr Control 1.9.0 GUI.

    WHERE: the interactive front end. It owns no GPU state of its own: every
           setting is written to the desired-state file and, when the background
           service is installed, driven through it; otherwise the GUI talks to
           \\.\Nvpwr directly exactly as 1.8.0 did.

    WHY THE 1.8.0 LAYOUT WAS REPLACED:
      1.8.0 put every control on one fixed-size dark canvas positioned by
      absolute coordinates, and translated strings by scanning them for known
      English substrings. Adding voltage rails, six tuning slots, a service
      panel and a third language to that structure would have meant an
      unmaintainable coordinate table and a translation pass that silently
      mangles mixed-language output. This build uses a page stack with a
      navigation rail, addresses strings by id, and sizes the window from the
      client area so text length never has to be predicted.

    WORKFLOW THIS IS BUILT AROUND:
      The dominant use is iterative tuning - apply, measure, adjust, apply again.
      Three mechanisms exist specifically for that:
        * slots        - save the current setup to one of six slots, load any of
                         them back, rename or clear them
        * one-click defaults - return everything to OEM/zero in a single action
        * confirmation watchdog - a voltage/clock change is provisional for a few
                         seconds and is reverted automatically unless confirmed,
                         so a setting that blanks the display costs seconds
                         rather than a reboot
*/

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

#include "..\\shared\\nvpwr_ioctl.h"
#include "nvpwr_ui_state.h"
#include "nvpwr_ipc.h"
#include "ui_theme.h"
#include "nvapi_power_policies.h"
#include "nvpwr_telemetry.h"
#include "nvpwr_mvolt_bridge.h"
#include "nvapi_tuner.h"
#include "xmg_probe.h"
#include "nvapi_probe.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")

using nvpwr::DesiredState;
using nvpwr::ConfigSlot;
using nvpwr::ProfileStore;

/*
    Mirror of the driver's POWER_CEILING_DEV. Used only to pre-fill the ceiling
    field and to size the profile range hint; the kernel remains the authority
    and clamps whatever is sent.
*/
static constexpr unsigned int kPowerCeilingMw = 350000u;
static constexpr int kPageCount = 7;

/* Page indices in navigation order, named so adding a page cannot silently
   renumber the ones after it. */
enum PageIndex {
    PAGE_POWER = 0,
    PAGE_STATUS,
    PAGE_VOLTAGE,
    PAGE_CLOCKS,
    PAGE_SLOTS,
    PAGE_LOG,
    PAGE_SETTINGS,
};

/* ---------------- control ids ---------------- */

enum : int {
    ID_LANGUAGE = 100,
    ID_NAV_POWER, ID_NAV_STATUS, ID_NAV_VOLTAGE, ID_NAV_ADVANCED,
    ID_NAV_SLOTS, ID_NAV_LOG, ID_NAV_SETTINGS,

    ID_POWER_TARGET = 200,
    ID_POWER_CEILING = 201,
    ID_POWER_QUICK = 202,
    ID_POWER_APPLY = 203,
    ID_POWER_RESTORE = 204,
    ID_POWER_REFRESH = 205,
    ID_POWER_APPLY_ALL = 206,

    ID_VOLT_NVDD_MIN = 300,
    ID_VOLT_NVDD_REL = 301,
    ID_VOLT_NVDD_ALT = 302,
    ID_VOLT_NVDD_OV = 303,
    ID_VOLT_MSVDD_MIN = 304,
    ID_VOLT_MSVDD_REL = 305,
    ID_VOLT_MSVDD_ALT = 306,
    ID_VOLT_MSVDD_OV = 307,
    ID_VOLT_DEM_CORE = 308,
    ID_VOLT_DEM_XBAR = 309,
    ID_VOLT_DEM_SYS = 310,
    ID_VOLT_DEM_VIDEO = 311,
    ID_VOLT_APPLY = 312,
    ID_VOLT_RESET = 313,
    ID_VOLT_DEFAULT = 314,
    ID_VOLT_IMPORT = 315,
    ID_VOLT_BACKEND_CHECK = 316,

    ID_ADV_CORE = 400,
    ID_ADV_MEM = 401,
    ID_ADV_XBAR = 402,
    ID_ADV_APPLY = 403,
    ID_ADV_RESET = 404,

    ID_LOG_TEXT = 500,
    ID_LOG_COPY = 501,
    ID_LOG_FOLDER = 502,
    ID_LOG_CLEAR = 503,

    ID_SET_LANG = 600,
    ID_SET_START_WIN = 601,
    ID_SET_START_MIN = 602,
    ID_SET_SVC_STATE = 603,
    ID_SET_SVC_INSTALL = 604,
    ID_SET_SVC_REMOVE = 605,
    ID_SET_STANDARD_MODE = 606,

    ID_SLOT_BASE = 700,          /* 700..705 = load slot 1..6 */
    ID_SLOT_SAVE_BASE = 720,     /* 720..725 = save into slot 1..6 */
    ID_SLOT_CLEAR_BASE = 740,    /* 740..745 = clear slot 1..6 */
    ID_SLOT_NAME_BASE = 760,     /* 760..765 = slot name edit */
    ID_SLOT_STATUS_BASE = 780,   /* 780..785 = slot status label */

    ID_UNDO_LAST = 800,
    ID_SAFETY_CONFIRM = 801,
    ID_SAFETY_REVERT = 802,
    ID_SAFETY_BANNER = 803,
};

enum : UINT_PTR {
    TIMER_SAFETY = 1,
    TIMER_STATUS = 2,
};

/* ---------------- global state ---------------- */

static HINSTANCE g_inst = nullptr;
static HWND g_main = nullptr;

static HWND g_nav[kPageCount]{};
static HWND g_page[kPageCount]{};
static HWND g_language = nullptr;
static int  g_currentPage = 0;

/* Live status page. Slots are filled by RefreshStatusPanel; a null slot means
   "this build does not show that reading", which keeps the layout table and the
   sampling code from having to agree by convention. */
enum StatusSlot {
    SS_ADAPTER = 0, SS_DRIVER, SS_VBIOS,
    SS_POWER_NOW, SS_POWER_LIMIT,
    SS_CORE_CLK, SS_MEM_CLK,
    SS_TEMP, SS_HOTSPOT, SS_HEADROOM, SS_TLIMIT, SS_UTIL,
    SS_VRAM, SS_FAN, SS_PSTATE,
    SS_AC, SS_BATTERY, SS_MUX, SS_ADAPTERS, SS_SOURCE,
    SS_COUNT
};
static HWND g_stHelp = nullptr;
static HWND g_stLabel[SS_COUNT]{};
static HWND g_stValue[SS_COUNT]{};
static HWND g_stThrottleLabel = nullptr, g_stThrottleValue = nullptr;
static HWND g_stLimiterLabel = nullptr, g_stLimiterValue = nullptr;
static HWND g_stVoltNote = nullptr;
static HWND g_stThrottleHint = nullptr;
static HWND g_stLimiterHint = nullptr;
static nvpwr::EnvStatus g_env{};

/* power page */
static HWND g_pwTarget = nullptr, g_pwCeiling = nullptr, g_pwQuick = nullptr;
static HWND g_pwApply = nullptr, g_pwRestore = nullptr, g_pwRefresh = nullptr;
static HWND g_pwApplyAll = nullptr, g_pwApplyAllHelp = nullptr;
static HWND g_pwCurLabel = nullptr, g_pwCurValue = nullptr;
static HWND g_pwBaseLabel = nullptr, g_pwBaseValue = nullptr;
static HWND g_pwReqLabel = nullptr, g_pwReqValue = nullptr;
static HWND g_pwCeilLabel = nullptr, g_pwCeilValue = nullptr;
static HWND g_pwMeasLabel = nullptr, g_pwMeasValue = nullptr;
static HWND g_pwHelp = nullptr, g_pwTargetLabel = nullptr, g_pwCeilingLabel = nullptr;
static HWND g_pwQuickLabel = nullptr, g_pwVerdict = nullptr;

/* voltage page */
static HWND g_voRailLabel[2]{};
static HWND g_voFieldLabel[2][4]{};      /* [rail][vmin/rel/alt/ov] */
static HWND g_voFieldEdit[2][4]{};
static HWND g_voRangeLabel[2]{};
static HWND g_voDemandLabel = nullptr;
static HWND g_voDemandName[4]{};
static HWND g_voDemandEdit[4]{};
static HWND g_voApply = nullptr, g_voReset = nullptr, g_voDefault = nullptr;
static HWND g_voNote = nullptr;
static HWND g_voImport = nullptr, g_voBackendLabel = nullptr, g_voBackendState = nullptr;
static HWND g_voImportHelp = nullptr;

/*
    Voltage backend state.

    This project cannot write voltage rails at all (no public NVAPI setter, and
    Pstates20 carries no voltage domain on the target GPU — both measured), so the
    companion tool does the write. These fields track whether it is usable, and
    the UI reflects that instead of offering a control that cannot work.
*/
static std::wstring g_mvoltPath;      /* resolved executable, empty if not found */
static bool         g_mvoltReady = false;

/* clocks page */
static HWND g_adLabel[3]{}, g_adEdit[3]{}, g_adRange[3]{};
static HWND g_adApply = nullptr, g_adReset = nullptr, g_adNote = nullptr;

/* log page */
static HWND g_logText = nullptr, g_logCopy = nullptr, g_logFolder = nullptr, g_logClear = nullptr;

/* settings page */
static HWND g_stLangLabel = nullptr, g_stLangCombo = nullptr;
static HWND g_stStartWin = nullptr, g_stStartMin = nullptr;
static HWND g_stSvcLabel = nullptr, g_stSvcState = nullptr;
static HWND g_stSvcInstall = nullptr, g_stSvcRemove = nullptr;
static HWND g_stStandardMode = nullptr, g_stModeHelp = nullptr;

/* slots */
static HWND g_slotName[nvpwr::kSlotCount]{};
static HWND g_slotLoad[nvpwr::kSlotCount]{};
static HWND g_slotSave[nvpwr::kSlotCount]{};
static HWND g_slotClear[nvpwr::kSlotCount]{};
static HWND g_slotStatus[nvpwr::kSlotCount]{};
static HWND g_slotHeading = nullptr;

/* safety */
static HWND g_safetyBanner = nullptr, g_safetyConfirm = nullptr, g_safetyRevert = nullptr;
static int  g_safetySecondsLeft = 0;
static bool g_safetyActive = false;

/* session / device */
static HANDLE g_device = INVALID_HANDLE_VALUE;
static DesiredState g_desired{};
static bool g_desiredLoaded = false;
static unsigned int g_profile = 0;
static std::wstring g_gpuName;
static unsigned int g_oemMw = 0;
static unsigned int g_activeMw = 0;
static unsigned int g_ceilingMw = 0;
static unsigned int g_state = NvpwrStateUnknown;
static std::wstring g_logPath;
static std::wstring g_lastError;

static nvpwr::VoltageState g_voltage{};
static NvapiTunerState    g_tuner{};

/* ---------------- logging ---------------- */

static void LogLine(const std::wstring& text) {
    if (g_logPath.empty()) {
        wchar_t base[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH)) {
            std::wstring dir = std::wstring(base) + L"\\NvpwrControl";
            CreateDirectoryW(dir.c_str(), nullptr);
            g_logPath = dir + L"\\nvpwr-control.log";
        }
    }
    if (g_logPath.empty()) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[64]{};
    swprintf_s(stamp, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    std::wstring line = std::wstring(stamp) + text + L"\r\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &utf8[0], n, nullptr, nullptr);

    DWORD wrote = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wrote, nullptr);
    CloseHandle(h);

    /* Mirror into the log page if the window exists. */
    if (g_logText) {
        int len = GetWindowTextLengthW(g_logText);
        SendMessageW(g_logText, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        std::wstring shown = line;
        SendMessageW(g_logText, EM_REPLACESEL, FALSE, (LPARAM)shown.c_str());
    }
}

static std::wstring Win32ErrorText(DWORD e) {
    LPWSTR msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                             reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring out;
    if (n && msg) {
        out.assign(msg, n);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n')) out.pop_back();
    } else {
        out = L"error " + std::to_wstring(e);
    }
    if (msg) LocalFree(msg);
    return out;
}

/* ---------------- device access (direct fallback path) ---------------- */

static std::wstring AppDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t p = s.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? L"." : s.substr(0, p);
}

static std::wstring ParentDirectory(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L"";
    if (p == 2 && path.size() >= 3 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, p);
}

static bool IsRegularFile(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring LocateDriverFile() {
    static const wchar_t kDriverFile[] = L"Nvpwr.sys";
    std::wstring dir = AppDirectory();
    for (int depth = 0; depth < 6 && !dir.empty(); ++depth) {
        const std::wstring candidates[] = {
            dir + L"\\" + kDriverFile,
            dir + L"\\dist\\" + kDriverFile,
            dir + L"\\driver\\x64\\Release\\" + kDriverFile,
            dir + L"\\driver\\x64\\Debug\\" + kDriverFile,
        };
        for (const std::wstring& c : candidates) if (IsRegularFile(c)) return c;
        std::wstring parent = ParentDirectory(dir);
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }
    return L"";
}

static bool WaitForServiceState(SC_HANDLE svc, DWORD want, DWORD timeoutMs, std::wstring& error) {
    DWORD start = GetTickCount();
    for (;;) {
        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed)) {
            error = L"QueryServiceStatusEx failed: " + Win32ErrorText(GetLastError());
            return false;
        }
        if (ssp.dwCurrentState == want) return true;
        if (ssp.dwCurrentState == SERVICE_STOPPED && want != SERVICE_STOPPED) {
            error = L"driver service stopped while starting (win32=" +
                    std::to_wstring(ssp.dwWin32ExitCode) + L", driver=" +
                    std::to_wstring(ssp.dwServiceSpecificExitCode) + L")";
            return false;
        }
        if (GetTickCount() - start >= timeoutMs) {
            error = L"timed out waiting for driver service state";
            return false;
        }
        Sleep(100);
    }
}

static void CloseDeviceHandle() {
    if (g_device != INVALID_HANDLE_VALUE) {
        CloseHandle(g_device);
        g_device = INVALID_HANDLE_VALUE;
    }
}

static bool EnsureDriverService(std::wstring& error, bool forceRestart) {
    std::wstring sys = LocateDriverFile();
    if (sys.empty()) { error = UiText(T_ERR_DRIVER_PATH); return false; }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr,
                                   SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        error = L"OpenSCManager failed: " + Win32ErrorText(GetLastError());
        return false;
    }

    const DWORD access = SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS |
                         SERVICE_CHANGE_CONFIG | DELETE;
    SC_HANDLE svc = OpenServiceW(scm, L"Nvpwr", access);
    if (!svc) {
        DWORD e = GetLastError();
        if (e != ERROR_SERVICE_DOES_NOT_EXIST) {
            error = L"OpenService failed: " + Win32ErrorText(e);
            CloseServiceHandle(scm);
            return false;
        }
        svc = CreateServiceW(scm, L"Nvpwr", L"Nvpwr GPU Power Control", access,
                             SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                             SERVICE_ERROR_NORMAL, sys.c_str(),
                             nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!svc) {
            error = L"CreateService failed: " + Win32ErrorText(GetLastError());
            CloseServiceHandle(scm);
            return false;
        }
        LogLine(L"EnsureDriverService: created demand-start kernel service at " + sys);
    } else {
        if (!ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DEMAND_START,
                                  SERVICE_NO_CHANGE, sys.c_str(),
                                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
            error = L"ChangeServiceConfig failed: " + Win32ErrorText(GetLastError());
            CloseServiceHandle(svc); CloseServiceHandle(scm);
            return false;
        }
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed)) {
        error = L"QueryServiceStatusEx failed: " + Win32ErrorText(GetLastError());
        CloseServiceHandle(svc); CloseServiceHandle(scm);
        return false;
    }

    if (forceRestart && (ssp.dwCurrentState == SERVICE_RUNNING ||
                         ssp.dwCurrentState == SERVICE_START_PENDING)) {
        CloseDeviceHandle();
        SERVICE_STATUS tmp{};
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &tmp)) {
            DWORD e = GetLastError();
            if (e != ERROR_SERVICE_NOT_ACTIVE) {
                error = L"could not stop the stale driver: " + Win32ErrorText(e);
                CloseServiceHandle(svc); CloseServiceHandle(scm);
                return false;
            }
        }
        if (!WaitForServiceState(svc, SERVICE_STOPPED, 5000, error)) {
            CloseServiceHandle(svc); CloseServiceHandle(scm);
            return false;
        }
        ssp.dwCurrentState = SERVICE_STOPPED;
    }

    if (ssp.dwCurrentState != SERVICE_RUNNING) {
        if (!StartServiceW(svc, 0, nullptr)) {
            DWORD e = GetLastError();
            if (e != ERROR_SERVICE_ALREADY_RUNNING) {
                error = L"StartService failed: " + Win32ErrorText(e);
                CloseServiceHandle(svc); CloseServiceHandle(scm);
                return false;
            }
        }
        if (!WaitForServiceState(svc, SERVICE_RUNNING, 10000, error)) {
            CloseServiceHandle(svc); CloseServiceHandle(scm);
            return false;
        }
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

static bool OpenDevice(std::wstring& error) {
    if (g_device != INVALID_HANDLE_VALUE) return true;
    g_device = CreateFileW(NVPWR_DEVICE_WIN32, GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_device == INVALID_HANDLE_VALUE) {
        error = L"cannot open \\\\.\\Nvpwr: " + Win32ErrorText(GetLastError());
        return false;
    }
    return true;
}

static bool BootstrapDriver(std::wstring& error) {
    if (!EnsureDriverService(error, true)) return false;
    if (!OpenDevice(error)) {
        error += L"\r\n\r\n" + std::wstring(UiText(T_ERR_NOT_ADMIN));
        return false;
    }
    return true;
}

static bool QueryDriverStatus(NVPWR_STATUS& out, std::wstring& error) {
    DWORD got = 0;
    if (g_device == INVALID_HANDLE_VALUE) { error = UiText(T_ERR_NO_DRIVER); return false; }
    if (!DeviceIoControl(g_device, IOCTL_NVPWR_STATUS, nullptr, 0,
                         &out, sizeof(out), &got, nullptr)) {
        error = L"status query failed: " + Win32ErrorText(GetLastError());
        return false;
    }
    return true;
}

static bool SendPowerTarget(unsigned int milliwatts, unsigned int ceilingMw,
                            unsigned int profile, std::wstring& error)
{
    NVPWR_SET_POWER req{};
    req.Version = NVPWR_SET_VERSION;
    req.TargetMilliwatts = milliwatts;
    req.MaxMilliwatts = ceilingMw;
    req.Profile = profile;

    DWORD got = 0;
    if (g_device == INVALID_HANDLE_VALUE) { error = UiText(T_ERR_NO_DRIVER); return false; }
    if (!DeviceIoControl(g_device, IOCTL_NVPWR_SET_POWER, &req, sizeof(req),
                         nullptr, 0, &got, nullptr)) {
        error = std::wstring(UiText(T_ERR_IOCTL)) + L" (" + Win32ErrorText(GetLastError()) + L")";
        return false;
    }
    return true;
}

static bool SendRestore(std::wstring& error) {
    DWORD got = 0;
    if (g_device == INVALID_HANDLE_VALUE) { error = UiText(T_ERR_NO_DRIVER); return false; }
    if (!DeviceIoControl(g_device, IOCTL_NVPWR_RESTORE, nullptr, 0,
                         nullptr, 0, &got, nullptr)) {
        error = std::wstring(UiText(T_ERR_IOCTL)) + L" (" + Win32ErrorText(GetLastError()) + L")";
        return false;
    }
    return true;
}

/* ---------------- GPU detection ---------------- */

static std::wstring DetectGpuName() {
    std::wstring result;
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))) || !factory)
        return L"Unknown NVIDIA GPU";

    for (UINT i = 0;; ++i) {
        IDXGIAdapter* adapter = nullptr;
        if (factory->EnumAdapters(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            if (desc.VendorId == 0x10DE && wcsstr(desc.Description, L"Laptop")) {
                result = desc.Description;
                adapter->Release();
                break;
            }
            if (desc.VendorId == 0x10DE && result.empty()) result = desc.Description;
        }
        adapter->Release();
    }
    factory->Release();
    return result.empty() ? L"Unknown NVIDIA GPU" : result;
}

static unsigned int DetectProfile(const std::wstring& name) {
    if (name.find(L"RTX 5070 Ti Laptop") != std::wstring::npos) return NvpwrProfileRtx5070TiLaptop;
    if (name.find(L"RTX 5050 Laptop") != std::wstring::npos) return NvpwrProfileRtx5050Laptop;
    if (name.find(L"RTX 5060 Laptop") != std::wstring::npos) return NvpwrProfileRtx5060Laptop;
    if (name.find(L"RTX 5070 Laptop") != std::wstring::npos) return NvpwrProfileRtx5070Laptop;
    if (name.find(L"RTX 5080 Laptop") != std::wstring::npos) return NvpwrProfileRtx5080Laptop;
    if (name.find(L"RTX 5090 Laptop") != std::wstring::npos) return NvpwrProfileRtx5090Laptop;
    if (name.find(L"RTX 4090 Laptop") != std::wstring::npos || name.find(L"RTX 4090 Mobile") != std::wstring::npos) return NvpwrProfileRtx4090Laptop;
    if (name.find(L"RTX 4080 Laptop") != std::wstring::npos || name.find(L"RTX 4080 Mobile") != std::wstring::npos) return NvpwrProfileRtx4080Laptop;
    if (name.find(L"RTX 4070 Laptop") != std::wstring::npos || name.find(L"RTX 4070 Mobile") != std::wstring::npos) return NvpwrProfileRtx4070Laptop;
    if (name.find(L"RTX 4060 Laptop") != std::wstring::npos || name.find(L"RTX 4060 Mobile") != std::wstring::npos) return NvpwrProfileRtx4060Laptop;
    if (name.find(L"RTX 4050 Laptop") != std::wstring::npos || name.find(L"RTX 4050 Mobile") != std::wstring::npos) return NvpwrProfileRtx4050Laptop;
    return NvpwrProfileUnknown;
}

/* Range this profile accepts. Mirrors the driver's IsSupportedTarget so the UI
   can reject impossible input before it reaches the kernel, and so the user
   sees the bound before typing. The driver remains the authority. */
static void ProfileRange(unsigned int profile, unsigned int ceilingMw,
                         unsigned int& loMw, unsigned int& hiMw)
{
    unsigned int ceiling = (ceilingMw == 0u) ? kPowerCeilingMw : ceilingMw;
    switch (profile) {
    case NvpwrProfileRtx5050Laptop:
    case NvpwrProfileRtx5060Laptop:
    case NvpwrProfileRtx5070Laptop:  loMw = 120000u; hiMw = 140000u; break;
    case NvpwrProfileRtx5070TiLaptop:loMw = 145000u; hiMw = ceiling; break;
    case NvpwrProfileRtx5080Laptop:
    case NvpwrProfileRtx5090Laptop:  loMw = 175000u; hiMw = ceiling; break;
    case NvpwrProfileRtx4090Laptop:  loMw = 150000u; hiMw = ceiling; break;
    case NvpwrProfileRtx4080Laptop:  loMw = 150000u; hiMw = ceiling; break;
    case NvpwrProfileRtx4070Laptop:
    case NvpwrProfileRtx4060Laptop:  loMw = 120000u; hiMw = 150000u; break;
    case NvpwrProfileRtx4050Laptop:  loMw = 115000u; hiMw = 140000u; break;
    default:                         loMw = 0u;     hiMw = 0u;     break;
    }
}

/* ---------------- helpers ---------------- */

static bool ReadEditLong(HWND h, long& value) {
    wchar_t b[64]{};
    GetWindowTextW(h, b, 64);
    if (b[0] == 0) { value = 0; return true; }
    wchar_t* end = nullptr;
    long v = wcstol(b, &end, 10);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (end == b || (end && *end != 0)) return false;
    value = v;
    return true;
}

static bool ReadEditLongLong(HWND h, long long& value) {
    wchar_t b[64]{};
    GetWindowTextW(h, b, 64);
    if (b[0] == 0) { value = 0; return true; }
    wchar_t* end = nullptr;
    long long v = _wcstoi64(b, &end, 10);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (end == b || (end && *end != 0)) return false;
    value = v;
    return true;
}

static void SetEditLong(HWND h, long v) {
    if (!h) return;
    SetWindowTextW(h, std::to_wstring(v).c_str());
}

static void SetEditLongLong(HWND h, long long v) {
    if (!h) return;
    SetWindowTextW(h, std::to_wstring(v).c_str());
}

/* ---------------- UI construction helpers ---------------- */

static HWND MakeLabel(HWND parent, const wchar_t* text, HFONT font = nullptr) {
    HWND h = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                           0, 0, 0, 0, parent, nullptr, g_inst, nullptr);
    UiFont(h, font);
    return h;
}

static HWND MakeEdit(HWND parent, int id, int width) {
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                             0, 0, width, 26, parent, (HMENU)(INT_PTR)id, g_inst, nullptr);
    UiFont(h);
    return h;
}

/* Signed edits must not carry ES_NUMBER. */
static HWND MakeSignedEdit(HWND parent, int id, int width) {
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             0, 0, width, 26, parent, (HMENU)(INT_PTR)id, g_inst, nullptr);
    UiFont(h);
    return h;
}

static HWND MakeButton(HWND parent, int id, const wchar_t* text) {
    HWND h = CreateWindowW(L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                           0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, g_inst, nullptr);
    UiFont(h);
    return h;
}

static HWND MakeCheck(HWND parent, int id, const wchar_t* text) {
    HWND h = CreateWindowW(L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                           0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, g_inst, nullptr);
    UiFont(h);
    return h;
}

static void Place(HWND h, int x, int y, int w, int ht) {
    if (h) MoveWindow(h, UiScale(x), UiScale(y), UiScale(w), UiScale(ht), TRUE);
}

/* Re-applies the current UI font to every control. Called after a language
   switch, because a CJK face and a Latin face are not interchangeable. */
static void UiApplyFontsToChildren(HWND parent) {
    if (!parent) return;
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t cls[64]{};
        GetClassNameW(child, cls, 64);
        const bool mono = (wcscmp(cls, L"Edit") == 0 && child == g_logText);
        SendMessageW(child, WM_SETFONT, (WPARAM)(mono ? g_uiMonoFont : g_uiFont), TRUE);
        UiApplyFontsToChildren(child);
    }
}

/* ---------------- layout ---------------- */

static const int kNavWidth = 168;
static const int kMargin = 24;
static const int kRowH = 34;

static void LayoutPage(int index, RECT& clientPx) {
    /* Convert to 96-dpi logical units so every constant below is DPI-free. */
    const int w = MulDiv(clientPx.right, 96, g_uiDpi);
    const int h = MulDiv(clientPx.bottom, 96, g_uiDpi);
    const int left = kNavWidth + kMargin;
    const int cw = w - left - kMargin;

    switch (index) {
    case PAGE_POWER: {
        int y = 96;
        Place(g_pwHelp, left, y, cw, 34); y += 42;
        Place(g_pwTargetLabel, left, y, 190, 18);
        Place(g_pwTarget, left, y + 20, 140, 28);
        Place(g_pwCeilingLabel, left + 170, y, 210, 18);
        Place(g_pwCeiling, left + 170, y + 20, 140, 28);
        Place(g_pwQuickLabel, left + 340, y, 120, 18);
        Place(g_pwQuick, left + 340, y + 20, 180, 220);
        y += 62;

        Place(g_pwApplyAll, left, y, 210, 44);
        Place(g_pwApply, left + 220, y, 150, 44);
        Place(g_pwRestore, left + 380, y, 140, 44);
        Place(g_pwRefresh, left + 530, y, 110, 44);
        y += 52;
        Place(g_pwApplyAllHelp, left, y, cw, 30);
        y += 40;

        const int col = cw / 3;
        HWND labels[5] = { g_pwCurLabel, g_pwBaseLabel, g_pwReqLabel, g_pwCeilLabel, g_pwMeasLabel };
        HWND values[5] = { g_pwCurValue, g_pwBaseValue, g_pwReqValue, g_pwCeilValue, g_pwMeasValue };
        for (int i = 0; i < 5; ++i) {
            int cx = left + (i % 3) * col;
            int cy = y + (i / 3) * 62;
            Place(labels[i], cx, cy, col - 12, 18);
            Place(values[i], cx, cy + 20, col - 12, 30);
        }
        y += 134;

        Place(g_pwVerdict, left, y, cw, 22);
        break;
    }

    case PAGE_STATUS: {
        int y = 96;
        Place(g_stHelp, left, y, cw, 40); y += 48;

        /* Two columns of label/value rows, then the two interpretation lines. */
        const int colW = cw / 2;
        for (int i = 0; i < SS_COUNT; ++i) {
            const int cx = left + (i % 2) * colW;
            const int cy = y + (i / 2) * 40;
            Place(g_stLabel[i], cx, cy, colW - 16, 16);
            Place(g_stValue[i], cx, cy + 16, colW - 16, 22);
        }
        const int rows = (SS_COUNT + 1) / 2;
        y += rows * 40 + 8;

        Place(g_stThrottleLabel, left, y, cw, 18);
        Place(g_stThrottleValue, left, y + 18, cw, 22);
        y += 48;
        Place(g_stThrottleHint, left, y, cw, 34);
        y += 42;
        Place(g_stLimiterLabel, left, y, cw, 18);
        Place(g_stLimiterValue, left, y + 18, cw, 22);
        y += 48;
        Place(g_stLimiterHint, left, y, cw, 44);
        y += 52;
        Place(g_stVoltNote, left, y, cw, 40);
        break;
    }

    case PAGE_VOLTAGE: {
        int y = 96;
        Place(g_voNote, left, y, cw, 40); y += 48;

        const int railH = 176;
        for (int r = 0; r < 2; ++r) {
            int ry = y + r * railH;
            Place(g_voRailLabel[r], left, ry, cw, 20);
            for (int f = 0; f < 4; ++f) {
                int fx = left + f * 150;
                Place(g_voFieldLabel[r][f], fx, ry + 24, 140, 18);
                Place(g_voFieldEdit[r][f], fx, ry + 44, 110, 26);
            }
            Place(g_voRangeLabel[r], left, ry + 78, cw, 18);
        }
        y += railH * 2 + 8;

        Place(g_voDemandLabel, left, y, cw, 20); y += 24;
        for (int i = 0; i < 4; ++i) {
            int fx = left + i * 140;
            Place(g_voDemandName[i], fx, y, 130, 18);
            Place(g_voDemandEdit[i], fx, y + 20, 110, 26);
        }
        y += 56;

        Place(g_voApply, left, y, 160, 40);
        Place(g_voReset, left + 170, y, 140, 40);
        Place(g_voImport, left + 320, y, 210, 40);
        Place(g_voDefault, left + 540, y, 190, 40);
        y += 54;
        Place(g_voBackendLabel, left, y, 150, 18);
        Place(g_voBackendState, left + 158, y, cw - 158, 18);
        y += 26;
        Place(g_voImportHelp, left, y, cw, 34);
        break;
    }

    case PAGE_CLOCKS: {
        int y = 96;
        Place(g_adNote, left, y, cw, 44); y += 56;
        for (int i = 0; i < 3; ++i) {
            int ry = y + i * 62;
            Place(g_adLabel[i], left, ry, 200, 18);
            Place(g_adEdit[i], left, ry + 20, 120, 28);
            Place(g_adRange[i], left + 132, ry + 24, cw - 132, 20);
        }
        y += 3 * 62 + 10;
        Place(g_adApply, left, y, 160, 40);
        Place(g_adReset, left + 170, y, 140, 40);
        break;
    }

    case PAGE_SLOTS: {
        int y = 96;
        Place(g_slotHeading, left, y, cw, 24); y += 34;
        for (int i = 0; i < nvpwr::kSlotCount; ++i) {
            Place(g_slotName[i], left, y, 170, 28);
            Place(g_slotStatus[i], left + 180, y + 6, cw - 180 - 260, 18);
            Place(g_slotSave[i], left + cw - 254, y, 80, 28);
            Place(g_slotLoad[i], left + cw - 168, y, 80, 28);
            Place(g_slotClear[i], left + cw - 82, y, 80, 28);
            y += 36;
        }
        break;
    }

    case PAGE_LOG: {
        int y = 96;
        Place(g_logCopy, left, y, 140, 36);
        Place(g_logFolder, left + 150, y, 140, 36);
        Place(g_logClear, left + 300, y, 140, 36);
        y += 46;
        Place(g_logText, left, y, cw, h - y - 16);
        break;
    }

    case PAGE_SETTINGS: {
        int y = 96;
        Place(g_stLangLabel, left, y, 220, 18);
        Place(g_stLangCombo, left + 230, y - 4, 220, 200);
        y += 40;
        Place(g_stStartWin, left, y, 320, 22); y += 30;
        Place(g_stStartMin, left, y, 320, 22); y += 44;

        Place(g_stSvcLabel, left, y, 220, 20);
        Place(g_stSvcState, left + 230, y, 300, 20);
        y += 30;
        Place(g_stSvcInstall, left, y, 200, 36);
        Place(g_stSvcRemove, left + 210, y, 200, 36);
        y += 52;

        Place(g_stStandardMode, left, y, 260, 36);
        Place(g_stModeHelp, left, y + 44, cw, 60);
        break;
    }

    default: break;
    }
}

static void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    /* Work in 96-dpi logical units so every constant in LayoutPage is DPI-free. */
    const int w = MulDiv(rc.right, 96, g_uiDpi);
    const int h = MulDiv(rc.bottom, 96, g_uiDpi);

    /*
        Page containers must cover the whole client area.

        Each page's controls are children of a container window, and Windows clips
        a child to its parent's client rectangle. The containers are created 0x0
        and their children are positioned in LOGICAL client coordinates, so the
        container has to be the full client area for those coordinates to land in
        the right place. Without this the entire page content is clipped away
        while still reporting itself as visible.
    */
    for (int i = 0; i < kPageCount; ++i) {
        if (g_page[i]) MoveWindow(g_page[i], 0, 0, rc.right, rc.bottom, TRUE);
    }

    for (int i = 0; i < kPageCount; ++i) Place(g_nav[i], 12, 78 + i * 40, kNavWidth - 24, 34);
    Place(g_language, 12, 30, kNavWidth - 24, 200);

    for (int i = 0; i < kPageCount; ++i) LayoutPage(i, rc);
    (void)w; (void)h;
}

static void ShowPage(int index) {
    g_currentPage = index;
    for (int i = 0; i < kPageCount; ++i) {
        ShowWindow(g_page[i], (i == index) ? SW_SHOW : SW_HIDE);
        if (g_nav[i]) {
            /* The selected page gets the accent colour; the rest stay muted.
               This is re-applied on every switch so no stale highlight remains. */
            InvalidateRect(g_nav[i], nullptr, TRUE);
        }
    }
}

/* ---------------- language ---------------- */

static void ApplyLanguage(nvpwr::Lang lang) {
    g_uiLang = lang;
    UiCreateFonts();
    UiApplyFontsToChildren(g_main);
    /* Every caption is re-set from the table, so switching language never leaves
       a mixed-language screen. */
    /* Navigation rail. One table drives both captions and order. */
    static const UiTextId navIds[kPageCount] = {
        T_NAV_POWER, T_NAV_STATUS, T_NAV_VOLTAGE, T_NAV_ADVANCED,
        T_NAV_SLOTS, T_NAV_LOG, T_NAV_SETTINGS
    };
    for (int i = 0; i < kPageCount; ++i)
        if (g_nav[i]) SetWindowTextW(g_nav[i], UiText(navIds[i]));

    SetWindowTextW(g_pwHelp, UiText(T_POWER_HELP));
    SetWindowTextW(g_pwTargetLabel, UiText(T_POWER_TARGET));
    SetWindowTextW(g_pwCeilingLabel, UiText(T_POWER_CEILING));
    SetWindowTextW(g_pwQuickLabel, UiText(T_POWER_QUICK));
    SetWindowTextW(g_pwApply, UiText(T_POWER_APPLY));
    SetWindowTextW(g_pwApplyAll, UiText(T_APPLY_ALL));
    SetWindowTextW(g_pwApplyAllHelp, UiText(T_APPLY_ALL_HELP));
    SetWindowTextW(g_pwRestore, UiText(T_RESTORE_OEM));
    SetWindowTextW(g_pwRefresh, UiText(T_REFRESH));
    SetWindowTextW(g_pwCurLabel, UiText(T_POWER_CURRENT));
    SetWindowTextW(g_pwBaseLabel, UiText(T_POWER_BASELINE));
    SetWindowTextW(g_pwReqLabel, UiText(T_POWER_LOADED));
    SetWindowTextW(g_pwCeilLabel, UiText(T_POWER_CEILING_EFFECTIVE));
    SetWindowTextW(g_pwMeasLabel, UiText(T_POWER_MEASURED));

    /* Status page */
    static const UiTextId statusIds[SS_COUNT] = {
        T_STATUS_ADAPTER, T_STATUS_DRIVER, T_STATUS_VBIOS,
        T_STATUS_POWER_NOW, T_STATUS_POWER_LIMIT,
        T_STATUS_CORE_CLK, T_STATUS_MEM_CLK,
        T_STATUS_TEMP, T_STATUS_HOTSPOT, T_STATUS_THERMAL_HEADROOM, T_STATUS_TLIMIT,
        T_STATUS_UTIL, T_STATUS_VRAM, T_STATUS_FAN, T_STATUS_PSTATE,
        T_STATUS_AC, T_STATUS_BATTERY, T_STATUS_MUX, T_STATUS_ADAPTERS, T_STATUS_SOURCE
    };
    SetWindowTextW(g_stHelp, UiText(T_STATUS_HELP));
    for (int i = 0; i < SS_COUNT; ++i)
        SetWindowTextW(g_stLabel[i], UiText(statusIds[i]));
    SetWindowTextW(g_stThrottleLabel, UiText(T_STATUS_THROTTLE));
    SetWindowTextW(g_stLimiterLabel, UiText(T_STATUS_LIMITER));
    SetWindowTextW(g_stVoltNote, UiText(T_STATUS_NO_VOLTAGE_NOTE));
    SetWindowTextW(g_stThrottleHint, UiText(T_STATUS_MUX_POWER_NOTE));

    SetWindowTextW(g_voNote, UiText(T_VOLT_HELP));
    SetWindowTextW(g_voRailLabel[0], UiText(T_VOLT_NVVDD));
    SetWindowTextW(g_voRailLabel[1], UiText(T_VOLT_MSVDD));
    const UiTextId fieldIds[4] = { T_VOLT_VMIN, T_VOLT_REL, T_VOLT_ALT, T_VOLT_OV };
    for (int r = 0; r < 2; ++r)
        for (int f = 0; f < 4; ++f)
            SetWindowTextW(g_voFieldLabel[r][f], UiText(fieldIds[f]));
    SetWindowTextW(g_voDemandLabel, UiText(T_VOLT_DEMAND));
    SetWindowTextW(g_voDemandName[0], UiText(T_VOLT_DEMAND_CORE));
    SetWindowTextW(g_voDemandName[1], UiText(T_VOLT_DEMAND_XBAR));
    SetWindowTextW(g_voDemandName[2], UiText(T_VOLT_DEMAND_SYS));
    SetWindowTextW(g_voDemandName[3], UiText(T_VOLT_DEMAND_VIDEO));
    SetWindowTextW(g_voApply, UiText(T_VOLT_APPLY));
    SetWindowTextW(g_voReset, UiText(T_VOLT_RESET));
    SetWindowTextW(g_voDefault, UiText(T_RESTORE_OEM));
    SetWindowTextW(g_voImport, UiText(T_VOLT_IMPORT));
    SetWindowTextW(g_voBackendLabel, UiText(T_VOLT_BACKEND));
    SetWindowTextW(g_voImportHelp, UiText(T_VOLT_IMPORT_HELP));

    SetWindowTextW(g_adNote, UiText(T_ADV_HELP));
    SetWindowTextW(g_adLabel[0], UiText(T_ADV_CORE));
    SetWindowTextW(g_adLabel[1], UiText(T_ADV_MEMORY));
    SetWindowTextW(g_adLabel[2], UiText(T_ADV_XBAR));
    SetWindowTextW(g_adApply, UiText(T_ADV_APPLY));
    SetWindowTextW(g_adReset, UiText(T_ADV_RESET));

    SetWindowTextW(g_logCopy, UiText(T_LOG_COPY));
    SetWindowTextW(g_logFolder, UiText(T_LOG_OPEN_FOLDER));
    SetWindowTextW(g_logClear, UiText(T_LOG_CLEAR));

    SetWindowTextW(g_stLangLabel, UiText(T_SET_LANGUAGE));
    SetWindowTextW(g_stStartWin, UiText(T_SET_START_WINDOWS));
    SetWindowTextW(g_stStartMin, UiText(T_SET_START_MIN));
    SetWindowTextW(g_stSvcLabel, UiText(T_SET_SERVICE_STATE));
    SetWindowTextW(g_stSvcInstall, UiText(T_SET_SERVICE_INSTALL));
    SetWindowTextW(g_stSvcRemove, UiText(T_SET_SERVICE_REMOVE));
    SetWindowTextW(g_stStandardMode, UiText(T_SET_MODE_STANDARD));
    SetWindowTextW(g_stModeHelp, UiText(T_SET_MODE_HELP));

    for (int i = 0; i < nvpwr::kSlotCount; ++i) {
        SetWindowTextW(g_slotSave[i], UiText(T_SLOT_SAVE));
        SetWindowTextW(g_slotLoad[i], UiText(T_SLOT_LOAD));
        SetWindowTextW(g_slotClear[i], UiText(T_SLOT_CLEAR));
    }
    SetWindowTextW(g_slotHeading, UiText(T_SLOT_HEADING));

    SetWindowTextW(g_safetyConfirm, UiText(T_SAFETY_KEEP));
    SetWindowTextW(g_safetyRevert, UiText(T_SAFETY_REVERT));

    InvalidateRect(g_main, nullptr, TRUE);
}

static nvpwr::Lang LanguageFromCombo() {
    int sel = (int)SendMessageW(g_language, CB_GETCURSEL, 0, 0);
    switch (sel) {
    case 1: return nvpwr::Lang::Chinese;
    case 2: return nvpwr::Lang::Russian;
    default: return nvpwr::Lang::English;
    }
}

static int ComboFromLanguage(nvpwr::Lang lang) {
    switch (lang) {
    case nvpwr::Lang::Chinese: return 1;
    case nvpwr::Lang::Russian: return 2;
    default: return 0;
    }
}

/* ---------------- status refresh ---------------- */

static void UpdatePowerReadouts() {
    SetWindowTextW(g_pwCurValue, UiFormatWatts(g_activeMw).c_str());
    SetWindowTextW(g_pwBaseValue, UiFormatWatts(g_oemMw).c_str());
    SetWindowTextW(g_pwReqValue, UiFormatWatts(g_desired.power.milliwatts).c_str());
    SetWindowTextW(g_pwCeilValue,
        UiFormatWatts(g_desired.power.ceilingMw ? g_desired.power.ceilingMw
                                                       : kPowerCeilingMw).c_str());
}

static void RefreshStatus(bool showErrors) {
    NVPWR_STATUS st{};
    std::wstring err;
    if (!QueryDriverStatus(st, err)) {
        SetWindowTextW(g_pwVerdict, err.c_str());
        g_state = NvpwrStateUnknown;
        UpdatePowerReadouts();
        if (showErrors) MessageBoxW(g_main, err.c_str(), UiText(T_APP_TITLE), MB_ICONERROR);
        return;
    }
    g_oemMw = st.OemBaseline ? st.OemBaseline : st.UpperBoundary;
    g_activeMw = st.CurrentEffective;
    g_ceilingMw = st.SessionMax ? st.SessionMax : st.CeilingMax;
    g_state = st.State;
    g_profile = st.ActiveProfile ? st.ActiveProfile : g_profile;

    const wchar_t* verdict = UiText(T_STATE_UNKNOWN);
    switch (st.State) {
    case NvpwrStateStockBaseline: verdict = UiText(T_STATE_OEM); break;
    case NvpwrStateApplied:       verdict = UiText(T_STATE_APPLIED); break;
    case NvpwrStateArmed:         verdict = UiText(T_STATE_ARMED); break;
    case NvpwrStateMixed:         verdict = UiText(T_STATE_MIXED); break;
    case NvpwrStateWrongBuild:    verdict = UiText(T_STATE_WRONG_BUILD); break;
    case NvpwrStateModuleNotFound:verdict = UiText(T_STATE_MODULE_MISSING); break;
    case NvpwrStateGpuNotFound:   verdict = UiText(T_STATE_GPU_MISSING); break;
    case NvpwrStateContextInvalid:verdict = UiText(T_STATE_CONTEXT_INVALID); break;
    case NvpwrStatePreconditionNotReady: verdict = UiText(T_STATE_PRECONDITION); break;
    default: break;
    }
    SetWindowTextW(g_pwVerdict, verdict);
    UpdatePowerReadouts();

    LogLine(L"Status: state=" + std::to_wstring(st.State) +
            L" oem=" + std::to_wstring(g_oemMw) +
            L" active=" + std::to_wstring(g_activeMw) +
            L" ceiling=" + std::to_wstring(g_ceilingMw) +
            L" upper=" + std::to_wstring(st.UpperBoundary) +
            L" max=" + std::to_wstring(st.MaxEffective) +
            L" f7=" + std::to_wstring(st.CurrentF7Value) +
            L" predicted=" + std::to_wstring(st.PredictedF7));
}

static void RefreshVoltagePanel() {
    /* Probe the native path too: on some GPU/driver combinations Pstates20 does
       expose a voltage domain, and when it does, using it is preferable to
       shelling out to a second program. */
    nvpwr::ProbeNvapiVoltage(g_voltage);

    /* Resolve the companion tool. This is what actually makes voltage adjustable
       on the reference GPU, so its availability drives the whole panel. */
    g_mvoltPath = nvpwr::FindMVoltExecutable(g_desired.mvoltPath);
    g_mvoltReady = !g_mvoltPath.empty() && nvpwr::MVoltAvailable(g_desired.mvoltPath);

    const bool nativeWritable = g_voltage.AnyWritable();
    const bool anyControl = nativeWritable || g_mvoltReady;

    struct RailUi { HWND (*edit)[4]; HWND range; };
    HWND (*edits[2])[4] = { &g_voFieldEdit[0], &g_voFieldEdit[1] };
    const nvpwr::RailSupport* rails[2] = { &g_voltage.nvvdd, &g_voltage.msvdd };

    /* Range text comes from the driver whenever it is readable. When only the
       companion is available the driver-side numbers are still shown if NVAPI
       exposes them, because the same envelope applies — it is the same policy
       object being edited. */
    for (int r = 0; r < 2; ++r) {
        const nvpwr::RailSupport& rs = *rails[r];
        const long long vals[4] = { rs.vminUv, rs.relUv, rs.altUv, rs.ovUv };
        for (int f = 0; f < 4; ++f) {
            HWND h = (*edits[r])[f];
            /* Editable whenever a backend exists: the values are stored here and
               handed to whichever applier can act on them. */
            EnableWindow(h, anyControl ? TRUE : FALSE);
            if (!GetFocus() || GetFocus() != h) SetEditLongLong(h, vals[f] / 1000);
        }
        std::wstring range;
        if (rs.available) {
            range = std::wstring(UiText(T_VOLT_DEVICE_RANGE)) + L": " +
                    std::to_wstring(rs.deviceMinUv / 1000) + L" .. " +
                    std::to_wstring(rs.deviceMaxUv / 1000) + L" mV";
        } else if (g_mvoltReady) {
            range = UiText(T_VOLT_IMPORT_HELP);
        } else {
            range = UiText(T_VOLT_UNAVAILABLE);
        }
        SetWindowTextW(g_voRangeLabel[r], range.c_str());
    }

    const long long dem[4] = { g_voltage.demand.coreMv, g_voltage.demand.xbarMv,
                               g_voltage.demand.sysMv,  g_voltage.demand.videoMv };
    for (int i = 0; i < 4; ++i) {
        EnableWindow(g_voDemandEdit[i], nativeWritable ? TRUE : FALSE);
        if (!GetFocus() || GetFocus() != g_voDemandEdit[i]) SetEditLongLong(g_voDemandEdit[i], dem[i]);
    }

    EnableWindow(g_voImport, g_mvoltReady ? TRUE : FALSE);
    EnableWindow(g_voApply, anyControl ? TRUE : FALSE);
    EnableWindow(g_voReset, anyControl ? TRUE : FALSE);

    if (g_voBackendState) {
        if (g_mvoltReady) {
            std::wstring s = UiText(T_VOLT_BACKEND_OK);
            s += L"  (";
            s += g_mvoltPath;
            s += L")";
            SetWindowTextW(g_voBackendState, s.c_str());
        } else {
            SetWindowTextW(g_voBackendState, UiText(T_VOLT_BACKEND_MISSING));
        }
    }

    if (g_voNote) {
        std::wstring note = UiText(T_VOLT_HELP);
        note += L"\r\n";
        note += UiText(T_VOLT_NATIVE_UNAVAILABLE);
        if (!g_mvoltReady && !nativeWritable) {
            note += L"\r\n";
            note += g_voltage.MissingReason();
        }
        SetWindowTextW(g_voNote, note.c_str());
    }
}

/* Reads the current rail values out of the companion tool and puts them into the
   controls. Importing is what makes the workflow workable: without it the user
   would retype four numbers per rail after every experiment. */
static bool ImportVoltageFromCompanion(bool quiet) {
    nvpwr::VoltageTuning imported{};
    std::wstring err;
    if (!nvpwr::ImportVoltageFromMVolt(imported, err, g_desired.mvoltPath)) {
        LogLine(L"Voltage import failed: " + err);
        if (!quiet) MessageBoxW(g_main, (std::wstring(UiText(T_VOLT_IMPORT_FAIL)) + L"\r\n\r\n" + err).c_str(),
                                UiText(T_VOLT_IMPORT), MB_ICONERROR);
        return false;
    }

    const long long nv[4] = { imported.nvvdd.vminUv, imported.nvvdd.relUv,
                              imported.nvvdd.altUv, imported.nvvdd.ovUv };
    const long long ms[4] = { imported.msvdd.vminUv, imported.msvdd.relUv,
                              imported.msvdd.altUv, imported.msvdd.ovUv };
    for (int f = 0; f < 4; ++f) {
        SetEditLongLong(g_voFieldEdit[0][f], nv[f] / 1000);
        SetEditLongLong(g_voFieldEdit[1][f], ms[f] / 1000);
        SetEditLongLong(g_voDemandEdit[f], f == 0 ? imported.demand.coreMv
                                        : f == 1 ? imported.demand.xbarMv
                                        : f == 2 ? imported.demand.sysMv
                                                 : imported.demand.videoMv);
    }

    g_desired.voltage = imported;
    g_desired.voltage.applier = nvpwr::VoltageApplier::CompanionTool;
    {
        std::wstring serr;
        nvpwr::SaveDesiredState(g_desired, serr);
    }
    LogLine(L"Voltage imported from mVolt+ (rel=" + std::to_wstring(imported.nvvdd.relUv / 1000) + L" mV)");
    if (!quiet) {
        MessageBoxW(g_main, UiText(T_VOLT_IMPORT_OK), UiText(T_VOLT_IMPORT), MB_ICONINFORMATION);
    }
    return true;
}

static void RefreshClocksPanel() {
    ProbeNvapiTuner(g_tuner, /*allowMsvdd*/ false);

    HWND edits[3] = { g_adEdit[0], g_adEdit[1], g_adEdit[2] };
    const bool supported[3] = { g_tuner.CoreMHz.Supported, g_tuner.MemoryMHz.Supported,
                                g_tuner.XbarWritable };
    const long cur[3] = { g_tuner.CoreMHz.Current, g_tuner.MemoryMHz.Current, g_tuner.XbarMHz };
    for (int i = 0; i < 3; ++i) {
        EnableWindow(edits[i], supported[i] ? TRUE : FALSE);
        SetEditLong(edits[i], cur[i]);
        std::wstring r = UiText(T_NA);
        if (i == 0 && supported[i]) r = std::to_wstring(g_tuner.CoreMHz.Min) + L" .. " + std::to_wstring(g_tuner.CoreMHz.Max) + L" MHz";
        if (i == 1 && supported[i]) r = std::to_wstring(g_tuner.MemoryMHz.Min) + L" .. " + std::to_wstring(g_tuner.MemoryMHz.Max) + L" MHz";
        if (i == 2 && supported[i]) r = L"-1000 .. +1000 MHz";
        SetWindowTextW(g_adRange[i], r.c_str());
    }
}

static void RefreshServicePanel() {
    nvpwr::ServiceStatus s = nvpwr::QueryServiceStatus();
    const wchar_t* text = UiText(T_SET_SERVICE_ABSENT);
    if (s == nvpwr::ServiceStatus::Running) text = UiText(T_SET_SERVICE_RUNNING);
    else if (s == nvpwr::ServiceStatus::Stopped) text = UiText(T_SET_SERVICE_STOPPED);
    SetWindowTextW(g_stSvcState, text);

    SendMessageW(g_stStartWin, BM_SETCHECK,
                 (WPARAM)(nvpwr::GetGuiAutoStart() ? BST_CHECKED : BST_UNCHECKED), 0);
    SendMessageW(g_stStartMin, BM_SETCHECK,
                 (WPARAM)(g_desired.startMinimized ? BST_CHECKED : BST_UNCHECKED), 0);
}

/*
    Live status page.

    Reads through NVML (falling back to nvidia-smi) and DXGI. Everything here is
    read-only, so it keeps working with the helper driver unloaded - which is
    exactly when a user most wants to see what the GPU is doing.
*/
static void RefreshStatusPanel() {
    nvpwr::SampleEnvironment(g_env);

    auto setText = [](HWND h, const std::wstring& s) {
        if (h) SetWindowTextW(h, s.empty() ? UiText(T_NA) : s.c_str());
    };
    auto fmtW  = [](const wchar_t* f, double v) { wchar_t b[80]{}; swprintf_s(b, f, v); return std::wstring(b); };

    setText(g_stValue[SS_ADAPTER], g_env.gpuName);
    setText(g_stValue[SS_DRIVER], g_env.driverVersion);
    setText(g_stValue[SS_VBIOS], g_env.vbiosVersion);

    setText(g_stValue[SS_POWER_NOW],
            g_env.hasPowerDraw ? fmtW(L"%.1f W", g_env.powerDrawW) : std::wstring());
    setText(g_stValue[SS_POWER_LIMIT],
            g_env.hasPowerLimit ? fmtW(L"%.1f W", g_env.enforcedLimitW) : std::wstring());
    setText(g_stValue[SS_CORE_CLK],
            g_env.hasCoreClock ? fmtW(L"%.0f MHz", g_env.coreClockMhz) : std::wstring());
    setText(g_stValue[SS_MEM_CLK],
            g_env.hasMemoryClock ? fmtW(L"%.0f MHz", g_env.memoryClockMhz) : std::wstring());
    setText(g_stValue[SS_TEMP],
            g_env.hasTemp ? fmtW(L"%.0f \u00B0C", g_env.tempC) : std::wstring());
    /* Hotspot is genuinely absent on GPUs whose NVML exposes only the edge
       sensor; it is shown as N/A with the reason in the note line rather than
       left blank, so the gap reads as a hardware limitation and not a bug. */
    setText(g_stValue[SS_HOTSPOT],
            g_env.hasHotspot ? fmtW(L"%.0f \u00B0C", g_env.hotspotC) : std::wstring());
    setText(g_stValue[SS_HEADROOM],
            g_env.HasThermalHeadroom() ? fmtW(L"%.0f \u00B0C", g_env.ThermalHeadroomC()) : std::wstring());
    setText(g_stValue[SS_TLIMIT],
            g_env.hasSpeedThreshold ? fmtW(L"%.0f \u00B0C", g_env.speedThresholdC) : std::wstring());
    setText(g_stValue[SS_UTIL],
            g_env.hasUtilization ? fmtW(L"%.0f %%", g_env.utilizationPct) : std::wstring());
    setText(g_stValue[SS_VRAM],
            (g_env.hasMemoryUsed && g_env.hasMemoryTotal)
                ? (fmtW(L"%.0f", g_env.memoryUsedMb) + L" / " + fmtW(L"%.0f MB", g_env.memoryTotalMb))
                : std::wstring());
    setText(g_stValue[SS_FAN],
            g_env.hasFanPct ? fmtW(L"%.0f %%", g_env.fanPct) : std::wstring());
    setText(g_stValue[SS_PSTATE],
            g_env.hasPstate ? (L"P" + std::to_wstring(g_env.pstate)) : std::wstring());

    if (g_env.acKnown) setText(g_stValue[SS_AC], g_env.onAcPower ? L"AC" : L"Battery");
    else setText(g_stValue[SS_AC], std::wstring());
    setText(g_stValue[SS_BATTERY],
            g_env.batteryPctKnown ? fmtW(L"%.0f %%", g_env.batteryPct) : std::wstring());

    if (g_env.discreteDirectKnown)
        setText(g_stValue[SS_MUX], g_env.discreteDirect ? UiText(T_STATUS_MUX_DIRECT)
                                                        : UiText(T_STATUS_MUX_HYBRID));
    else
        setText(g_stValue[SS_MUX], std::wstring());
    setText(g_stValue[SS_ADAPTERS], std::to_wstring(g_env.adapterCount));
    setText(g_stValue[SS_SOURCE], g_env.source);

    /* Throttle reasons and the interpretation of them. The interpretation is the
       part that answers "will a higher limit actually help?" - which is the
       question this whole panel exists for. */
    if (g_env.hasThrottle) {
        setText(g_stThrottleValue, nvpwr::DescribeThrottle(g_env.throttleBits));
        const std::wstring lim = nvpwr::DescribeLimiter(g_env);
        setText(g_stLimiterValue, lim);
        if (g_stLimiterHint) SetWindowTextW(g_stLimiterHint, L"");
    } else {
        setText(g_stThrottleValue, std::wstring());
        setText(g_stLimiterValue, std::wstring());
    }

    /* The note line carries every reason a field above reads N/A, so a gap is
       never mistaken for a defect in this tool. */
    {
        std::wstring note;
        if (!g_env.error.empty()) { note += g_env.error; note += L"  "; }
        if (!g_env.unavailableNote.empty()) { note += g_env.unavailableNote; note += L"  "; }
        note += UiText(T_STATUS_NO_VOLTAGE_NOTE);
        if (g_stVoltNote) SetWindowTextW(g_stVoltNote, note.c_str());
    }
}

static void RefreshSlots() {    ProfileStore store{};
    std::wstring err;
    if (!nvpwr::LoadProfileStore(store, err)) {
        LogLine(L"RefreshSlots: " + err);
        return;
    }
    for (int i = 0; i < nvpwr::kSlotCount; ++i) {
        const ConfigSlot& s = store.slots[i];
        if (s.used) {
            SetWindowTextW(g_slotName[i], s.name.c_str());
            std::wstring status = s.savedAt;
            if (!s.lastApplyNote.empty()) {
                status += L"  ";
                status += s.lastApplyOk ? L"[OK] " : L"[FAIL] ";
                status += s.lastApplyNote;
            }
            SetWindowTextW(g_slotStatus[i], status.c_str());
        } else {
            SetWindowTextW(g_slotName[i], (L"— " + std::to_wstring(i + 1) + L" —").c_str());
            SetWindowTextW(g_slotStatus[i], L"");
        }
        EnableWindow(g_slotLoad[i], s.used ? TRUE : FALSE);
        EnableWindow(g_slotClear[i], s.used ? TRUE : FALSE);
    }
}

static void RefreshAll(bool showErrors) {
    RefreshStatus(showErrors);
    RefreshStatusPanel();
    RefreshVoltagePanel();
    RefreshClocksPanel();
    RefreshServicePanel();
    RefreshSlots();
}

/* ---------------- safety watchdog ----------------
   The provisional-change guard.

   When a voltage or clock change is applied the new state is PROVISIONAL: a
   setting the driver accepts can still blank the display or reset the driver.
   The guard makes that recoverable in one click.

   It deliberately does NOT auto-revert on a timer. The dominant workflow is
   iterative tuning with a stress test running, and an automatic revert after N
   seconds would silently undo exactly the change the user is trying to measure.
   Instead the banner waits for an explicit choice, and the documented escape
   hatch for a dead display is a reboot, which always returns the machine to OEM
   limits because GPU policy lives in driver memory only.
*/

static void SafetyBegin(const std::wstring& label, unsigned int /*unusedSeconds*/) {
    nvpwr::SetRestorePoint(label, g_desired);
    g_safetyActive = true;
    g_safetySecondsLeft = 0;
    ShowWindow(g_safetyBanner, SW_SHOW);
    ShowWindow(g_safetyConfirm, SW_SHOW);
    ShowWindow(g_safetyRevert, SW_SHOW);
    SetTimer(g_main, TIMER_SAFETY, 1000, nullptr);
    LogLine(L"Safety: armed '" + label + L"' pending user confirmation");
}

static void SafetyEnd() {
    if (!g_safetyActive) return;
    KillTimer(g_main, TIMER_SAFETY);
    g_safetyActive = false;
    ShowWindow(g_safetyBanner, SW_HIDE);
    ShowWindow(g_safetyConfirm, SW_HIDE);
    ShowWindow(g_safetyRevert, SW_HIDE);
}

/* Re-applies the restore point captured before the provisional change. This is
   the "it went black" escape hatch, so it deliberately does the simplest thing
   that is known to work rather than anything clever. */
static void SafetyRevert() {
    nvpwr::RestorePoint rp{};
    if (!nvpwr::GetRestorePoint(rp)) { SafetyEnd(); return; }

    LogLine(L"Safety: reverting to restore point '" + rp.label + L"'");
    std::wstring err;
    bool ok = true;

    DesiredState target = rp.state;
    if (target.powerEnabled && target.power.milliwatts) {
        ok = SendPowerTarget(target.power.milliwatts, target.power.ceilingMw,
                             target.power.profile ? target.power.profile : g_profile, err) && ok;
    } else {
        ok = SendRestore(err) && ok;
    }

    if (target.voltage.enabled) {
        nvpwr::VoltageRequest vr{};
        vr.nvvdd.set = true;
        vr.nvvdd.vminUv = target.voltage.nvvdd.vminUv;
        vr.nvvdd.relUv  = target.voltage.nvvdd.relUv;
        vr.nvvdd.altUv  = target.voltage.nvvdd.altUv;
        vr.nvvdd.ovUv   = target.voltage.nvvdd.ovUv;
        vr.msvdd.set = true;
        vr.msvdd.vminUv = target.voltage.msvdd.vminUv;
        vr.msvdd.relUv  = target.voltage.msvdd.relUv;
        vr.msvdd.altUv  = target.voltage.msvdd.altUv;
        vr.msvdd.ovUv   = target.voltage.msvdd.ovUv;
        std::wstring verr;
        nvpwr::VoltageState post{};
        ok = nvpwr::ApplyNvapiVoltage(vr, post, verr) && ok;
    } else if (nvpwr::HasVoltageBaseline()) {
        std::wstring verr;
        nvpwr::VoltageState post{};
        nvpwr::ResetNvapiVoltage(post, verr);
    }

    g_desired = target;
    nvpwr::SaveDesiredState(g_desired, err);
    SafetyEnd();
    RefreshAll(false);
    LogLine(ok ? L"Safety: revert completed" : L"Safety: revert reported a failure");
}

/* ---------------- applying state ---------------- */

/* Single place that turns a DesiredState into reality. Every caller - page
   Apply buttons, slot load, one-click defaults, service replay - goes through
   here so the ordering and the fail-closed handling cannot diverge. */
static bool ApplyDesiredState(const DesiredState& wanted, bool confirmNeeded,
                              std::wstring& error)
{
    error.clear();
    DesiredState previous = g_desired;

    /* Record the restore point BEFORE the first mutation so the watchdog and the
       undo button both have the true prior state. */
    nvpwr::SetRestorePoint(L"before apply", previous);

    bool powerOk = true;
    if (wanted.powerEnabled && wanted.power.milliwatts) {
        unsigned int prof = wanted.power.profile ? wanted.power.profile : g_profile;
        powerOk = SendPowerTarget(wanted.power.milliwatts, wanted.power.ceilingMw, prof, error);
        if (!powerOk) {
            /* The driver rejected or rolled back; nothing was left half-applied
               because the kernel restores its own baseline on failure. */
            LogLine(L"ApplyDesiredState: power failed: " + error);
            g_desired = previous;
            return false;
        }
    } else {
        powerOk = SendRestore(error);
        if (!powerOk) { g_desired = previous; return false; }
    }

    bool voltageOk = true;
    if (wanted.voltage.enabled && !wanted.voltage.IsZero()) {
        /*
            Voltage is applied by the companion tool, never by this process: the
            public NVAPI surface has no voltage setter and Pstates20 carries no
            voltage domain on the target GPU. The native module is still tried
            first when it reports a writable rail, because using the driver
            directly is preferable when it is actually possible.
        */
        if (g_voltage.AnyWritable()) {
            nvpwr::VoltageRequest vr{};
            vr.nvvdd.set = true;
            vr.nvvdd.vminUv = wanted.voltage.nvvdd.vminUv;
            vr.nvvdd.relUv  = wanted.voltage.nvvdd.relUv;
            vr.nvvdd.altUv  = wanted.voltage.nvvdd.altUv;
            vr.nvvdd.ovUv   = wanted.voltage.nvvdd.ovUv;
            vr.msvdd.set = true;
            vr.msvdd.vminUv = wanted.voltage.msvdd.vminUv;
            vr.msvdd.relUv  = wanted.voltage.msvdd.relUv;
            vr.msvdd.altUv  = wanted.voltage.msvdd.altUv;
            vr.msvdd.ovUv   = wanted.voltage.msvdd.ovUv;

            std::wstring verr;
            nvpwr::VoltageState post{};
            voltageOk = nvpwr::ApplyNvapiVoltage(vr, post, verr);
            g_voltage = post;
            if (!voltageOk) {
                error = L"voltage stage failed natively: " + verr +
                        L" (power limit remains applied)";
                LogLine(L"ApplyDesiredState: " + error);
            }
        } else {
            bool rounded = false;
            std::wstring verr;
            voltageOk = nvpwr::ApplyVoltageViaMVolt(wanted.voltage, verr, &rounded,
                                                    g_desired.mvoltPath);
            if (!voltageOk) {
                /* Power already moved. Say precisely which half failed instead of
                   implying the whole apply was atomic: the user needs to know the
                   power limit IS live while voltage is not. */
                error = std::wstring(UiText(T_VOLT_APPLY_FAIL)) + L": " + verr +
                        L" (power limit remains applied)";
                LogLine(L"ApplyDesiredState: " + error);
            } else if (rounded) {
                LogLine(L"ApplyDesiredState: voltage applied with mV rounding");
            }
            /* Reflect what the driver now reports, so the panel shows reality. */
            nvpwr::VoltageState post{};
            nvpwr::ProbeNvapiVoltage(post);
            g_voltage = post;
        }
    } else if (nvpwr::HasVoltageBaseline()) {
        std::wstring verr;
        nvpwr::VoltageState post{};
        nvpwr::ResetNvapiVoltage(post, verr);
        g_voltage = post;
    } else if (g_mvoltReady && !g_desired.voltage.IsZero()) {
        /* Nothing requested now, but the machine may still be carrying an offset
           applied earlier in this session through the companion tool. */
        std::wstring verr;
        nvpwr::ResetVoltageViaMVolt(verr, g_desired.mvoltPath);
    }

    g_desired = wanted;
    if (!nvpwr::SaveDesiredState(g_desired, error))
        LogLine(L"ApplyDesiredState: state save failed: " + error);

    if (confirmNeeded && voltageOk) SafetyBegin(L"apply", nvpwr::kConfirmTimeoutSecVoltage);

    RefreshAll(false);
    return powerOk && voltageOk;
}

/* Reads the controls for the power page into a DesiredState copy. */
static bool CollectPowerState(DesiredState& out, std::wstring& error) {
    out = g_desired;

    long targetW = 0, ceilingW = 0;
    if (!ReadEditLong(g_pwTarget, targetW)) { error = UiText(T_POWER_INVALID_WATTS); return false; }
    if (!ReadEditLong(g_pwCeiling, ceilingW)) { error = UiText(T_POWER_INVALID_WATTS); return false; }

    if (ceilingW <= 0) ceilingW = (long)(kPowerCeilingMw / 1000u);
    if (targetW < 0) { error = UiText(T_POWER_INVALID_WATTS); return false; }

    out.powerEnabled = (targetW != 0);
    out.power.milliwatts = (unsigned int)targetW * 1000u;
    out.power.ceilingMw = (unsigned int)ceilingW * 1000u;
    if (!out.power.profile) out.power.profile = g_profile;

    if (out.powerEnabled) {
        if ((out.power.milliwatts % 5000u) != 0u) {
            error = UiText(T_POWER_INVALID_WATTS);
            return false;
        }
        if (out.power.milliwatts > out.power.ceilingMw) {
            error = UiText(T_POWER_ABOVE_CEILING);
            return false;
        }
        unsigned int lo = 0, hi = 0;
        ProfileRange(out.power.profile, out.power.ceilingMw, lo, hi);
        if (hi != 0u && (out.power.milliwatts < lo || out.power.milliwatts > hi)) {
            error = UiText(T_POWER_OUT_OF_RANGE);
            return false;
        }
    }
    return true;
}

static void CollectVoltageState(DesiredState& out) {
    long long v = 0;
    if (ReadEditLongLong(g_voFieldEdit[0][0], v)) out.voltage.nvvdd.vminUv = v * 1000;
    if (ReadEditLongLong(g_voFieldEdit[0][1], v)) out.voltage.nvvdd.relUv  = v * 1000;
    if (ReadEditLongLong(g_voFieldEdit[0][2], v)) out.voltage.nvvdd.altUv  = v * 1000;
    if (ReadEditLongLong(g_voFieldEdit[0][3], v)) out.voltage.nvvdd.ovUv   = v * 1000;
    if (ReadEditLongLong(g_voFieldEdit[1][0], v)) out.voltage.msvdd.vminUv = v * 1000;
    if (ReadEditLongLong(g_voFieldEdit[1][1], v)) out.voltage.msvdd.relUv  = v * 1000;
    if (ReadEditLongLong(g_voFieldEdit[1][2], v)) out.voltage.msvdd.altUv  = v * 1000;
    if (ReadEditLongLong(g_voFieldEdit[1][3], v)) out.voltage.msvdd.ovUv   = v * 1000;

    if (ReadEditLongLong(g_voDemandEdit[0], v)) out.voltage.demand.coreMv = v;
    if (ReadEditLongLong(g_voDemandEdit[1], v)) out.voltage.demand.xbarMv = v;
    if (ReadEditLongLong(g_voDemandEdit[2], v)) out.voltage.demand.sysMv  = v;
    if (ReadEditLongLong(g_voDemandEdit[3], v)) out.voltage.demand.videoMv= v;

    out.voltage.enabled = !out.voltage.IsZero();
    /*
        Always record who is expected to apply it, so a saved slot is
        self-describing and a later replay cannot silently pick a different path.
        The companion tool is the only route this build has: the native
        ApplyNvapiVoltage path is chosen automatically at apply time when the
        driver reports a writable rail, and does not need this flag.
    */
    out.voltage.applier = nvpwr::VoltageApplier::CompanionTool;
    if (!g_desired.mvoltPath.empty()) out.mvoltPath = g_desired.mvoltPath;
    else if (!g_mvoltPath.empty())    out.mvoltPath = g_mvoltPath;
}

static void CollectClockState(DesiredState& out) {
    long v = 0;
    if (ReadEditLong(g_adEdit[0], v)) out.clock.coreOffsetMhz = v;
    if (ReadEditLong(g_adEdit[1], v)) out.clock.memoryOffsetMhz = v;
    if (ReadEditLong(g_adEdit[2], v)) out.clock.xbarOffsetMhz = v;
    out.clock.enabled = (out.clock.coreOffsetMhz || out.clock.memoryOffsetMhz ||
                         out.clock.xbarOffsetMhz);
}

/* ---------------- owner-draw buttons ---------------- */

static void DrawButton(const DRAWITEMSTRUCT* dis) {
    const bool isPrimary = (dis->CtlID == ID_POWER_APPLY || dis->CtlID == ID_VOLT_APPLY ||
                            dis->CtlID == ID_ADV_APPLY);
    const bool isDanger  = (dis->CtlID == ID_POWER_RESTORE || dis->CtlID == ID_VOLT_RESET ||
                            dis->CtlID == ID_ADV_RESET || dis->CtlID == ID_VOLT_DEFAULT ||
                            dis->CtlID == ID_SET_STANDARD_MODE);
    const bool isPressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool isDisabled = (dis->itemState & ODS_DISABLED) != 0;

    COLORREF bg, border, text;
    if (isDisabled)      { bg = RGB(20,26,36);   border = RGB(34,44,60);   text = RGB(80,95,115); }
    else if (isPrimary)  { bg = isPressed ? RGB(95,150,0) : RGB(118,185,0);
                           border = isPressed ? RGB(118,185,0) : RGB(145,215,20);
                           text = RGB(12,20,8); }
    else if (isDanger)   { bg = isPressed ? RGB(75,25,32) : RGB(42,24,30);
                           border = isPressed ? RGB(180,50,60) : RGB(110,42,52);
                           text = RGB(252,165,165); }
    else                 { bg = isPressed ? RGB(36,46,64) : RGB(25,33,47);
                           border = isPressed ? RGB(65,84,115) : RGB(45,58,80);
                           text = RGB(241,245,249); }

    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    HBRUSH brush = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, UiScale(8), UiScale(8));
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    wchar_t buf[256]{};
    GetWindowTextW(dis->hwndItem, buf, 256);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    SelectObject(dc, g_uiButtonFont);
    DrawTextW(dc, buf, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* ---------------- commands ---------------- */

/*
    ONE-SHOT APPLY - "apply everything the panel currently shows".

    Order is fixed and deliberate:
      1. power limit   - the coarse knob, and the one that cannot hang the display
      2. voltage rails - lets the unlocked ceiling actually be drawn
      3. clock offsets - fine shaping on top
    A stage that fails stops the chain and reports exactly which stage failed and
    what is already live. Earlier stages are NOT silently rolled back: during
    tuning the user wants to know that e.g. the power limit took effect while the
    voltage write was refused, rather than being returned to a state they did not
    ask for.
*/
static void OnApplyAll() {
    DesiredState wanted = g_desired;

    std::wstring perr;
    DesiredState pw{};
    if (CollectPowerState(pw, perr)) {
        wanted.powerEnabled = pw.powerEnabled;
        wanted.power = pw.power;
    } else {
        /* A malformed power field must not block a voltage-only experiment. */
        wanted.powerEnabled = false;
        wanted.power = nvpwr::PowerTarget{};
        LogLine(L"ApplyAll: power fields not usable, skipping power stage: " + perr);
    }
    CollectVoltageState(wanted);
    CollectClockState(wanted);

    if (!wanted.powerEnabled && wanted.voltage.IsZero() && !wanted.clock.enabled) {
        MessageBoxW(g_main, UiText(T_NA), UiText(T_APPLY_ALL), MB_ICONINFORMATION);
        return;
    }

    std::wstring body = UiText(T_APPLY_ALL_CONFIRM);
    body += L"\r\n\r\n";
    body += UiText(T_POWER_CONFIRM_BODY);
    if (wanted.powerEnabled && wanted.power.milliwatts > 225000u) {
        body += L"\r\n\r\n";
        body += UiText(T_POWER_CONFIRM_EXPERIMENTAL);
    }
    if (MessageBoxW(g_main, body.c_str(), UiText(T_APPLY_ALL),
                    MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;

    LogLine(L"ApplyAll: begin (power=" + std::to_wstring(wanted.power.milliwatts) +
            L" mW, voltage=" + std::to_wstring(wanted.voltage.enabled ? 1 : 0) +
            L", clocks=" + std::to_wstring(wanted.clock.enabled ? 1 : 0) + L")");

    nvpwr::SetRestorePoint(L"before apply-all", g_desired);

    std::vector<std::wstring> failures;

    /* ---- stage 1: power ---- */
    if (wanted.powerEnabled && wanted.power.milliwatts) {
        std::wstring err;
        unsigned int prof = wanted.power.profile ? wanted.power.profile : g_profile;
        if (!SendPowerTarget(wanted.power.milliwatts, wanted.power.ceilingMw, prof, err)) {
            failures.push_back(L"power: " + err);
            LogLine(L"ApplyAll: power stage FAILED: " + err);
        } else {
            LogLine(L"ApplyAll: power stage OK");
        }
    } else {
        std::wstring err;
        if (!SendRestore(err)) {
            failures.push_back(L"power restore: " + err);
        }
    }

    /* ---- stage 2: voltage rails ---- */
    if (wanted.voltage.enabled && !wanted.voltage.IsZero()) {
        nvpwr::VoltageRequest vr{};
        vr.nvvdd.set = true;
        vr.nvvdd.vminUv = wanted.voltage.nvvdd.vminUv;
        vr.nvvdd.relUv  = wanted.voltage.nvvdd.relUv;
        vr.nvvdd.altUv  = wanted.voltage.nvvdd.altUv;
        vr.nvvdd.ovUv   = wanted.voltage.nvvdd.ovUv;
        vr.msvdd.set = true;
        vr.msvdd.vminUv = wanted.voltage.msvdd.vminUv;
        vr.msvdd.relUv  = wanted.voltage.msvdd.relUv;
        vr.msvdd.altUv  = wanted.voltage.msvdd.altUv;
        vr.msvdd.ovUv   = wanted.voltage.msvdd.ovUv;
        if (!wanted.voltage.demand.IsZero()) {
            vr.setDemand = true;
            vr.coreMv  = wanted.voltage.demand.coreMv;
            vr.xbarMv  = wanted.voltage.demand.xbarMv;
            vr.sysMv   = wanted.voltage.demand.sysMv;
            vr.videoMv = wanted.voltage.demand.videoMv;
        }
        std::wstring verr;
        nvpwr::VoltageState post{};
        if (!nvpwr::ApplyNvapiVoltage(vr, post, verr)) {
            failures.push_back(L"voltage: " + verr);
            LogLine(L"ApplyAll: voltage stage FAILED: " + verr);
        } else {
            LogLine(L"ApplyAll: voltage stage OK");
        }
        g_voltage = post;
    }

    /* ---- stage 3: clock offsets ---- */
    if (wanted.clock.enabled) {
        NvapiTuneRequest req{};
        req.SetCore = (g_tuner.CoreMHz.Supported != 0);
        req.CoreMHz = wanted.clock.coreOffsetMhz;
        req.SetMemory = (g_tuner.MemoryMHz.Supported != 0);
        req.MemoryMHz = wanted.clock.memoryOffsetMhz;
        req.SetXbar = (g_tuner.XbarWritable != 0);
        req.XbarMHz = wanted.clock.xbarOffsetMhz;
        std::wstring cerr;
        NvapiTunerState post{};
        if (!ApplyNvapiTuning(req, false, post, cerr)) {
            failures.push_back(L"clocks: " + cerr);
            LogLine(L"ApplyAll: clock stage FAILED: " + cerr);
        } else {
            LogLine(L"ApplyAll: clock stage OK");
        }
        g_tuner = post;
    }

    g_desired = wanted;
    {
        std::wstring serr;
        if (!nvpwr::SaveDesiredState(g_desired, serr))
            LogLine(L"ApplyAll: state save failed: " + serr);
    }

    RefreshAll(false);

    if (failures.empty()) {
        LogLine(L"ApplyAll: all stages OK");
        /* Provisional until confirmed: only voltage/clock changes can take the
           display down, so the banner is raised only when those were involved. */
        if (!wanted.voltage.IsZero() || wanted.clock.enabled)
            SafetyBegin(L"apply-all", nvpwr::kConfirmTimeoutSecVoltage);
        return;
    }

    std::wstring msg = UiText(T_APPLY_PARTIAL);
    msg += L"\r\n\r\n";
    for (const std::wstring& f : failures) { msg += L"• "; msg += f; msg += L"\r\n"; }
    MessageBoxW(g_main, msg.c_str(), UiText(T_APPLY_ALL), MB_ICONERROR);

    if (!wanted.voltage.IsZero() || wanted.clock.enabled)
        SafetyBegin(L"apply-all partial", nvpwr::kConfirmTimeoutSecVoltage);
}

static void OnApplyPower() {    DesiredState wanted{};
    std::wstring err;
    if (!CollectPowerState(wanted, err)) {
        MessageBoxW(g_main, err.c_str(), UiText(T_POWER_CONFIRM_TITLE), MB_ICONWARNING);
        return;
    }
    if (wanted.powerEnabled) {
        std::wstring body = UiText(T_POWER_CONFIRM_BODY);
        unsigned int lo = 0, hi = 0;
        ProfileRange(wanted.power.profile, wanted.power.ceilingMw, lo, hi);
        /* Anything above the highest physically validated value on this class of
           machine is flagged, rather than silently treated as equal. */
        if (wanted.power.milliwatts > 225000u) {
            body += L"\r\n\r\n";
            body += UiText(T_POWER_CONFIRM_EXPERIMENTAL);
        }
        if (MessageBoxW(g_main, body.c_str(), UiText(T_POWER_CONFIRM_TITLE),
                        MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
            LogLine(L"ApplyPower: cancelled by user");
            return;
        }
    }
    LogLine(L"ApplyPower: request " + std::to_wstring(wanted.power.milliwatts) + L" mW ceiling " +
            std::to_wstring(wanted.power.ceilingMw) + L" mW");
    if (!ApplyDesiredState(wanted, false, err)) {
        MessageBoxW(g_main, err.c_str(), UiText(T_APP_TITLE), MB_ICONERROR);
    }
}

static void OnApplyVoltage() {
    DesiredState wanted = g_desired;
    CollectVoltageState(wanted);

    const bool nativeWritable = g_voltage.AnyWritable();
    if (wanted.voltage.IsZero() && !nvpwr::HasVoltageBaseline()) {
        MessageBoxW(g_main, UiText(T_VOLT_UNAVAILABLE), UiText(T_VOLT_APPLY), MB_ICONINFORMATION);
        return;
    }
    if (!nativeWritable && !g_mvoltReady) {
        /* No backend at all. Say so before showing a confirmation dialog the user
           cannot act on. */
        std::wstring msg = UiText(T_VOLT_BACKEND_MISSING);
        msg += L"\r\n\r\n";
        msg += UiText(T_VOLT_NATIVE_UNAVAILABLE);
        MessageBoxW(g_main, msg.c_str(), UiText(T_VOLT_BACKEND), MB_ICONERROR);
        return;
    }
    if (MessageBoxW(g_main, UiText(T_VOLT_CONFIRM_BODY), UiText(T_VOLT_CONFIRM_TITLE),
                    MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;

    LogLine(nativeWritable ? L"ApplyVoltage: using the native NVAPI rail path"
                           : L"ApplyVoltage: delegating to mVolt+");

    std::wstring err;
    /* Voltage changes can blank the display, so this is the one path that arms
       the confirmation watchdog. */
    if (!ApplyDesiredState(wanted, true, err))
        MessageBoxW(g_main, err.c_str(), UiText(T_APP_TITLE), MB_ICONERROR);
}

static void OnApplyClocks() {
    DesiredState wanted = g_desired;
    CollectClockState(wanted);

    NvapiTuneRequest req{};
    req.SetCore = g_tuner.CoreMHz.Supported;
    req.CoreMHz = wanted.clock.coreOffsetMhz;
    req.SetMemory = g_tuner.MemoryMHz.Supported;
    req.MemoryMHz = wanted.clock.memoryOffsetMhz;
    req.SetXbar = g_tuner.XbarWritable;
    req.XbarMHz = wanted.clock.xbarOffsetMhz;

    if (!req.SetCore && !req.SetMemory && !req.SetXbar) {
        MessageBoxW(g_main, UiText(T_VOLT_UNAVAILABLE), UiText(T_ADV_APPLY), MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxW(g_main, UiText(T_VOLT_CONFIRM_BODY), UiText(T_VOLT_CONFIRM_TITLE),
                    MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;

    nvpwr::SetRestorePoint(L"before clocks", g_desired);

    std::wstring err;
    NvapiTunerState post{};
    if (!ApplyNvapiTuning(req, false, post, err)) {
        g_tuner = post;
        RefreshClocksPanel();
        MessageBoxW(g_main, err.c_str(), UiText(T_APP_TITLE), MB_ICONERROR);
        return;
    }
    g_tuner = post;
    g_desired = wanted;
    nvpwr::SaveDesiredState(g_desired, err);
    SafetyBegin(L"clocks", nvpwr::kConfirmTimeoutSecVoltage);
    RefreshClocksPanel();
}

static void OnRestoreDefaults(const wchar_t* why) {
    LogLine(std::wstring(L"RestoreDefaults: ") + why);
    DesiredState def = nvpwr::DefaultState();
    def.power.profile = g_profile;

    std::wstring err;
    if (nvpwr::HasVoltageBaseline()) {
        std::wstring verr;
        nvpwr::VoltageState post{};
        nvpwr::ResetNvapiVoltage(post, verr);
        g_voltage = post;
    }
    ApplyDesiredState(def, false, err);
    SafetyEnd();

    SetEditLong(g_pwTarget, 0);
    SetEditLong(g_pwCeiling, (long)(kPowerCeilingMw / 1000u));
    RefreshClocksPanel();
    RefreshVoltagePanel();
    LogLine(L"RestoreDefaults: done");
}

static void OnLoadSlot(int index) {
    DesiredState st{};
    std::wstring err;
    if (!nvpwr::LoadSlot(index, st, err)) {
        MessageBoxW(g_main, err.c_str(), UiText(T_APP_TITLE), MB_ICONERROR);
        return;
    }
    if (MessageBoxW(g_main, UiText(T_VOLT_CONFIRM_BODY), UiText(T_APPLY),
                    MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;

    if (st.powerEnabled) SetEditLong(g_pwTarget, (long)(st.power.milliwatts / 1000u));
    else                 SetEditLong(g_pwTarget, 0);
    SetEditLong(g_pwCeiling, (long)((st.power.ceilingMw ? st.power.ceilingMw
                                                        : kPowerCeilingMw) / 1000u));

    bool ok = ApplyDesiredState(st, true, err);
    /* Remember the outcome on the slot itself: the whole point of the slots is
       iterative tuning, so "which one misbehaved" must survive a reboot. */
    nvpwr::MarkSlotApplyResult(index, ok, ok ? L"" : err, err);
    RefreshSlots();
    if (!ok) MessageBoxW(g_main, err.c_str(), UiText(T_APP_TITLE), MB_ICONERROR);
}

static void OnSaveSlot(int index) {
    DesiredState st = g_desired;
    CollectVoltageState(st);
    CollectClockState(st);
    std::wstring err;
    std::wstring name;
    if (g_slotName[index]) {
        wchar_t buf[nvpwr::kSlotNameMax + 8]{};
        GetWindowTextW(g_slotName[index], buf, nvpwr::kSlotNameMax + 4);
        name = buf;
    }
    if (!nvpwr::SaveSlot(index, name, st, err))
        MessageBoxW(g_main, err.c_str(), UiText(T_APP_TITLE), MB_ICONERROR);
    else
        LogLine(L"SaveSlot " + std::to_wstring(index + 1) + L" saved as '" + name + L"'");
    RefreshSlots();
}

static void OnClearSlot(int index) {
    std::wstring err;
    if (!nvpwr::ClearSlot(index, err))
        MessageBoxW(g_main, err.c_str(), UiText(T_APP_TITLE), MB_ICONERROR);
    RefreshSlots();
}

static void OnUndoLast() {
    nvpwr::RestorePoint rp{};
    if (!nvpwr::GetRestorePoint(rp)) {
        MessageBoxW(g_main, UiText(T_NA), UiText(T_APP_TITLE), MB_ICONINFORMATION);
        return;
    }
    SafetyRevert();
}

/* ---------------- window procedure ---------------- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_main = hwnd;

        g_language = CreateWindowW(WC_COMBOBOXW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            0, 0, 0, 0, hwnd, (HMENU)ID_LANGUAGE, g_inst, nullptr);
        SendMessageW(g_language, CB_ADDSTRING, 0, (LPARAM)L"English");
        SendMessageW(g_language, CB_ADDSTRING, 0, (LPARAM)L"简体中文");
        SendMessageW(g_language, CB_ADDSTRING, 0, (LPARAM)L"Русский");
        UiFont(g_language);

        /*
            Navigation captions, in page order.

            This table must stay index-aligned with the PageIndex enum: entry i
            labels the button that shows page i. An earlier revision listed only
            five entries while seven pages existed, which silently mislabelled
            every button after the third one.
        */
        static const UiTextId navIds[kPageCount] = {
            T_NAV_POWER,    /* PAGE_POWER    */
            T_NAV_STATUS,   /* PAGE_STATUS   */
            T_NAV_VOLTAGE,  /* PAGE_VOLTAGE  */
            T_NAV_ADVANCED, /* PAGE_CLOCKS   */
            T_NAV_SLOTS,    /* PAGE_SLOTS    */
            T_NAV_LOG,      /* PAGE_LOG      */
            T_NAV_SETTINGS, /* PAGE_SETTINGS */
        };
        for (int i = 0; i < kPageCount; ++i) {
            g_nav[i] = MakeButton(hwnd, ID_NAV_POWER + i, UiText(navIds[i]));
            /* Page container: a plain static that hosts the page's controls.
               It carries no background of its own so the parent's paint shows
               through; WS_CLIPCHILDREN keeps the parent from erasing them. */
            g_page[i] = CreateWindowExW(0, L"STATIC", L"",
                                        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                        0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
        }

        /* ---- power page ---- */
        HWND pw = g_page[PAGE_POWER];
        g_pwHelp = MakeLabel(pw, UiText(T_POWER_HELP));
        g_pwTargetLabel = MakeLabel(pw, UiText(T_POWER_TARGET));
        g_pwTarget = MakeEdit(pw, ID_POWER_TARGET, 140);
        g_pwCeilingLabel = MakeLabel(pw, UiText(T_POWER_CEILING));
        g_pwCeiling = MakeEdit(pw, ID_POWER_CEILING, 140);
        g_pwQuickLabel = MakeLabel(pw, UiText(T_POWER_QUICK));
        g_pwQuick = CreateWindowW(WC_COMBOBOXW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL,
            0, 0, 0, 0, pw, (HMENU)ID_POWER_QUICK, g_inst, nullptr);
        UiFont(g_pwQuick);

        g_pwApply = MakeButton(pw, ID_POWER_APPLY, UiText(T_POWER_APPLY));
        g_pwApplyAll = MakeButton(pw, ID_POWER_APPLY_ALL, UiText(T_APPLY_ALL));
        g_pwApplyAllHelp = MakeLabel(pw, UiText(T_APPLY_ALL_HELP));
        g_pwRestore = MakeButton(pw, ID_POWER_RESTORE, UiText(T_RESTORE_OEM));
        g_pwRefresh = MakeButton(pw, ID_POWER_REFRESH, UiText(T_REFRESH));

        g_pwCurLabel = MakeLabel(pw, UiText(T_POWER_CURRENT));
        g_pwCurValue = MakeLabel(pw, L"—", g_uiMetricFont);
        g_pwBaseLabel = MakeLabel(pw, UiText(T_POWER_BASELINE));
        g_pwBaseValue = MakeLabel(pw, L"—", g_uiMetricFont);
        g_pwReqLabel = MakeLabel(pw, UiText(T_POWER_LOADED));
        g_pwReqValue = MakeLabel(pw, L"—", g_uiMetricFont);
        g_pwCeilLabel = MakeLabel(pw, UiText(T_POWER_CEILING_EFFECTIVE));
        g_pwCeilValue = MakeLabel(pw, L"—", g_uiMetricFont);
        g_pwMeasLabel = MakeLabel(pw, UiText(T_POWER_MEASURED));
        g_pwMeasValue = MakeLabel(pw, L"—", g_uiMetricFont);
        g_pwVerdict = MakeLabel(pw, UiText(T_STATE_UNKNOWN), g_uiStateFont);

        /* ---- live status page ---- */
        {
            HWND st = g_page[PAGE_STATUS];
            g_stHelp = MakeLabel(st, UiText(T_STATUS_HELP));
            static const UiTextId statusIds[SS_COUNT] = {
                T_STATUS_ADAPTER, T_STATUS_DRIVER, T_STATUS_VBIOS,
                T_STATUS_POWER_NOW, T_STATUS_POWER_LIMIT,
                T_STATUS_CORE_CLK, T_STATUS_MEM_CLK,
                T_STATUS_TEMP, T_STATUS_HOTSPOT, T_STATUS_THERMAL_HEADROOM, T_STATUS_TLIMIT,
                T_STATUS_UTIL, T_STATUS_VRAM, T_STATUS_FAN, T_STATUS_PSTATE,
                T_STATUS_AC, T_STATUS_BATTERY, T_STATUS_MUX, T_STATUS_ADAPTERS, T_STATUS_SOURCE
            };
            for (int i = 0; i < SS_COUNT; ++i) {
                g_stLabel[i] = MakeLabel(st, UiText(statusIds[i]));
                g_stValue[i] = MakeLabel(st, L"—", g_uiStateFont);
            }
            g_stThrottleLabel = MakeLabel(st, UiText(T_STATUS_THROTTLE));
            g_stThrottleValue = MakeLabel(st, L"—", g_uiStateFont);
            g_stThrottleHint = MakeLabel(st, UiText(T_STATUS_MUX_POWER_NOTE));
            g_stLimiterLabel = MakeLabel(st, UiText(T_STATUS_LIMITER));
            g_stLimiterValue = MakeLabel(st, L"—", g_uiStateFont);
            g_stLimiterHint = MakeLabel(st, L"");
            g_stVoltNote = MakeLabel(st, UiText(T_STATUS_NO_VOLTAGE_NOTE));
        }

        /* ---- tuning slots page ---- */
        {
            HWND sl = g_page[PAGE_SLOTS];
            g_slotHeading = MakeLabel(sl, UiText(T_SLOT_HEADING), g_uiHeadingFont);
            for (int i = 0; i < nvpwr::kSlotCount; ++i) {
                g_slotName[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    0, 0, 0, 0, sl, (HMENU)(INT_PTR)(ID_SLOT_NAME_BASE + i), g_inst, nullptr);
                UiFont(g_slotName[i]);
                g_slotStatus[i] = MakeLabel(sl, L"");
                g_slotSave[i] = MakeButton(sl, ID_SLOT_SAVE_BASE + i, UiText(T_SLOT_SAVE));
                g_slotLoad[i] = MakeButton(sl, ID_SLOT_BASE + i, UiText(T_SLOT_LOAD));
                g_slotClear[i] = MakeButton(sl, ID_SLOT_CLEAR_BASE + i, UiText(T_SLOT_CLEAR));
            }
        }

        /* ---- voltage page ---- */
        HWND vo = g_page[PAGE_VOLTAGE];
        g_voNote = MakeLabel(vo, UiText(T_VOLT_HELP));
        g_voRailLabel[0] = MakeLabel(vo, UiText(T_VOLT_NVVDD), g_uiStateFont);
        g_voRailLabel[1] = MakeLabel(vo, UiText(T_VOLT_MSVDD), g_uiStateFont);
        const UiTextId fieldIds[4] = { T_VOLT_VMIN, T_VOLT_REL, T_VOLT_ALT, T_VOLT_OV };
        const int fieldCtrl[2][4] = {
            { ID_VOLT_NVDD_MIN, ID_VOLT_NVDD_REL, ID_VOLT_NVDD_ALT, ID_VOLT_NVDD_OV },
            { ID_VOLT_MSVDD_MIN, ID_VOLT_MSVDD_REL, ID_VOLT_MSVDD_ALT, ID_VOLT_MSVDD_OV },
        };
        for (int r = 0; r < 2; ++r) {
            for (int f = 0; f < 4; ++f) {
                g_voFieldLabel[r][f] = MakeLabel(vo, UiText(fieldIds[f]));
                g_voFieldEdit[r][f] = MakeSignedEdit(vo, fieldCtrl[r][f], 110);
            }
            g_voRangeLabel[r] = MakeLabel(vo, L"");
        }
        g_voDemandLabel = MakeLabel(vo, UiText(T_VOLT_DEMAND), g_uiStateFont);
        const UiTextId demIds[4] = { T_VOLT_DEMAND_CORE, T_VOLT_DEMAND_XBAR,
                                     T_VOLT_DEMAND_SYS, T_VOLT_DEMAND_VIDEO };
        const int demCtrl[4] = { ID_VOLT_DEM_CORE, ID_VOLT_DEM_XBAR,
                                 ID_VOLT_DEM_SYS, ID_VOLT_DEM_VIDEO };
        for (int i = 0; i < 4; ++i) {
            g_voDemandName[i] = MakeLabel(vo, UiText(demIds[i]));
            g_voDemandEdit[i] = MakeSignedEdit(vo, demCtrl[i], 110);
        }
        g_voApply = MakeButton(vo, ID_VOLT_APPLY, UiText(T_VOLT_APPLY));
        g_voReset = MakeButton(vo, ID_VOLT_RESET, UiText(T_VOLT_RESET));
        g_voDefault = MakeButton(vo, ID_VOLT_DEFAULT, UiText(T_RESTORE_OEM));
        g_voImport = MakeButton(vo, ID_VOLT_IMPORT, UiText(T_VOLT_IMPORT));
        g_voBackendLabel = MakeLabel(vo, UiText(T_VOLT_BACKEND));
        g_voBackendState = MakeLabel(vo, L"");
        g_voImportHelp = MakeLabel(vo, UiText(T_VOLT_IMPORT_HELP));

        /* ---- clocks page ---- */
        HWND ad = g_page[PAGE_CLOCKS];
        g_adNote = MakeLabel(ad, UiText(T_ADV_HELP));
        const UiTextId advIds[3] = { T_ADV_CORE, T_ADV_MEMORY, T_ADV_XBAR };
        const int advCtrl[3] = { ID_ADV_CORE, ID_ADV_MEM, ID_ADV_XBAR };
        for (int i = 0; i < 3; ++i) {
            g_adLabel[i] = MakeLabel(ad, UiText(advIds[i]));
            g_adEdit[i] = MakeSignedEdit(ad, advCtrl[i], 120);
            g_adRange[i] = MakeLabel(ad, UiText(T_NA));
        }
        g_adApply = MakeButton(ad, ID_ADV_APPLY, UiText(T_ADV_APPLY));
        g_adReset = MakeButton(ad, ID_ADV_RESET, UiText(T_ADV_RESET));

        /* ---- log page ---- */
        HWND lg = g_page[PAGE_LOG];
        g_logCopy = MakeButton(lg, ID_LOG_COPY, UiText(T_LOG_COPY));
        g_logFolder = MakeButton(lg, ID_LOG_FOLDER, UiText(T_LOG_OPEN_FOLDER));
        g_logClear = MakeButton(lg, ID_LOG_CLEAR, UiText(T_LOG_CLEAR));
        g_logText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL | WS_VSCROLL,
            0, 0, 0, 0, lg, (HMENU)ID_LOG_TEXT, g_inst, nullptr);
        UiFont(g_logText, g_uiMonoFont);

        /* ---- settings page ---- */
        HWND se = g_page[PAGE_SETTINGS];
        g_stLangLabel = MakeLabel(se, UiText(T_SET_LANGUAGE));
        g_stLangCombo = CreateWindowW(WC_COMBOBOXW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            0, 0, 0, 0, se, (HMENU)ID_SET_LANG, g_inst, nullptr);
        SendMessageW(g_stLangCombo, CB_ADDSTRING, 0, (LPARAM)L"English");
        SendMessageW(g_stLangCombo, CB_ADDSTRING, 0, (LPARAM)L"简体中文");
        SendMessageW(g_stLangCombo, CB_ADDSTRING, 0, (LPARAM)L"Русский");
        UiFont(g_stLangCombo);

        g_stStartWin = MakeCheck(se, ID_SET_START_WIN, UiText(T_SET_START_WINDOWS));
        g_stStartMin = MakeCheck(se, ID_SET_START_MIN, UiText(T_SET_START_MIN));
        g_stSvcLabel = MakeLabel(se, UiText(T_SET_SERVICE_STATE));
        g_stSvcState = MakeLabel(se, UiText(T_SET_SERVICE_ABSENT));
        g_stSvcInstall = MakeButton(se, ID_SET_SVC_INSTALL, UiText(T_SET_SERVICE_INSTALL));
        g_stSvcRemove = MakeButton(se, ID_SET_SVC_REMOVE, UiText(T_SET_SERVICE_REMOVE));
        g_stStandardMode = MakeButton(se, ID_SET_STANDARD_MODE, UiText(T_SET_MODE_STANDARD));
        g_stModeHelp = MakeLabel(se, UiText(T_SET_MODE_HELP));

        /* ---- safety banner (floats over the pages) ---- */
        g_safetyBanner = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_CENTER,
                                       0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
        UiFont(g_safetyBanner, g_uiStateFont);
        g_safetyConfirm = MakeButton(hwnd, ID_SAFETY_CONFIRM, UiText(T_APPLY));
        g_safetyRevert = MakeButton(hwnd, ID_SAFETY_REVERT, UiText(T_RESTORE_OEM));
        ShowWindow(g_safetyBanner, SW_HIDE);
        ShowWindow(g_safetyConfirm, SW_HIDE);
        ShowWindow(g_safetyRevert, SW_HIDE);

        ShowPage(PAGE_POWER);

        /* Load persisted intent and reflect it in the controls. */
        std::wstring err;
        if (!nvpwr::LoadDesiredState(g_desired, err)) {
            LogLine(L"LoadDesiredState: " + err);
            MessageBoxW(hwnd, err.c_str(), UiText(T_APP_TITLE), MB_ICONWARNING);
            g_desired = nvpwr::DefaultState();
        }
        g_desiredLoaded = true;

        g_uiLang = nvpwr::LangFromIniValue(L"");
        {
            wchar_t buf[32]{};
            std::wstring ini = UiSettingsPath();
            if (!ini.empty()) {
                GetPrivateProfileStringW(L"Interface", L"Language", L"", buf, 32, ini.c_str());
                if (buf[0]) {
                    g_uiLang = nvpwr::LangFromIniValue(buf);
                } else {
                    /* Migrate the 1.8.0 two-valued [Interface] Russian key. */
                    int old = GetPrivateProfileIntW(L"Interface", L"Russian", -1, ini.c_str());
                    if (old == 1) g_uiLang = nvpwr::Lang::Russian;
                    else if (old == 0) g_uiLang = nvpwr::Lang::English;
                }
            }
        }
        SendMessageW(g_language, CB_SETCURSEL, ComboFromLanguage(g_uiLang), 0);
        SendMessageW(g_stLangCombo, CB_SETCURSEL, ComboFromLanguage(g_uiLang), 0);
        ApplyLanguage(g_uiLang);

        SetEditLong(g_pwCeiling, (long)((g_desired.power.ceilingMw ? g_desired.power.ceilingMw
                                                                  : kPowerCeilingMw) / 1000u));
        SetEditLong(g_pwTarget, (long)(g_desired.power.milliwatts / 1000u));

        /* Defaults for the quick-pick list; the driver re-validates on Apply. */
        {
            const wchar_t* quick[] = { L"OEM", L"180", L"200", L"225", L"250", L"300", L"350" };
            for (const wchar_t* q : quick) SendMessageW(g_pwQuick, CB_ADDSTRING, 0, (LPARAM)q);
            SendMessageW(g_pwQuick, CB_SETCURSEL, 0, 0);
        }

        g_gpuName = DetectGpuName();
        g_profile = DetectProfile(g_gpuName);
        LogLine(L"GPU: " + g_gpuName + L" profile=" + std::to_wstring(g_profile));

        if (!BootstrapDriver(err)) {
            LogLine(L"BootstrapDriver failed: " + err);
            SetWindowTextW(g_pwVerdict, err.c_str());
        } else {
            LogLine(L"Driver bootstrap OK");
            RefreshStatus(false);
        }

        RefreshVoltagePanel();
        RefreshClocksPanel();
        RefreshServicePanel();
        RefreshSlots();
        UpdatePowerReadouts();

        SetTimer(hwnd, TIMER_STATUS, 3000, nullptr);
        LogLine(L"NvpwrControl 1.9.0 started");
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_SAFETY) {
            /* Elapsed-time counter only. No automatic revert - see SafetyBegin. */
            if (g_safetyActive) {
                ++g_safetySecondsLeft;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (wp == TIMER_STATUS) {
            /* Cheap periodic refresh so the readouts track reality while the user
               experiments. The kernel status is cheap; the NVML sample is heavier
               and only runs while the status page is visible. */
            if (g_device != INVALID_HANDLE_VALUE) {
                NVPWR_STATUS st{};
                std::wstring e;
                if (QueryDriverStatus(st, e)) {
                    g_activeMw = st.CurrentEffective;
                    g_state = st.State;
                    UpdatePowerReadouts();
                }
            }
            if (g_currentPage == PAGE_STATUS) RefreshStatusPanel();
            return 0;
        }
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);

        if (id == ID_LANGUAGE && code == CBN_SELCHANGE) {
            nvpwr::Lang lang = LanguageFromCombo();
            SendMessageW(g_stLangCombo, CB_SETCURSEL, ComboFromLanguage(lang), 0);
            ApplyLanguage(lang);
            std::wstring ini = UiSettingsPath();
            if (!ini.empty()) {
                WritePrivateProfileStringW(L"Interface", L"Language",
                                           nvpwr::LangToIniValue(lang), ini.c_str());
                WritePrivateProfileStringW(L"Interface", L"LanguageSet", L"1", ini.c_str());
            }
            return 0;
        }
        if (id == ID_SET_LANG && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_stLangCombo, CB_GETCURSEL, 0, 0);
            nvpwr::Lang lang = nvpwr::Lang::English;
            if (sel == 1) lang = nvpwr::Lang::Chinese;
            else if (sel == 2) lang = nvpwr::Lang::Russian;
            SendMessageW(g_language, CB_SETCURSEL, ComboFromLanguage(lang), 0);
            ApplyLanguage(lang);
            std::wstring ini = UiSettingsPath();
            if (!ini.empty()) {
                WritePrivateProfileStringW(L"Interface", L"Language",
                                           nvpwr::LangToIniValue(lang), ini.c_str());
                WritePrivateProfileStringW(L"Interface", L"LanguageSet", L"1", ini.c_str());
            }
            return 0;
        }
        if (id >= ID_NAV_POWER && id <= ID_NAV_SETTINGS) {
            ShowPage(id - ID_NAV_POWER);
            return 0;
        }
        if (id == ID_POWER_QUICK && code == CBN_SELCHANGE) {
            wchar_t buf[64]{};
            GetWindowTextW(g_pwQuick, buf, 64);
            long v = wcstol(buf, nullptr, 10);
            if (v > 0) SetEditLong(g_pwTarget, v);
            return 0;
        }
        if (id == ID_POWER_APPLY_ALL) { OnApplyAll(); return 0; }
        if (id == ID_POWER_APPLY)   { OnApplyPower(); return 0; }
        if (id == ID_POWER_RESTORE) {
            DesiredState def = nvpwr::DefaultState();
            std::wstring err;
            nvpwr::SetRestorePoint(L"before restore", g_desired);
            if (!SendRestore(err)) MessageBoxW(hwnd, err.c_str(), UiText(T_APP_TITLE), MB_ICONERROR);
            def.power.profile = g_profile;
            g_desired = def;
            nvpwr::SaveDesiredState(g_desired, err);
            SetEditLong(g_pwTarget, 0);
            RefreshAll(false);
            LogLine(L"Restore OEM requested");
            return 0;
        }
        if (id == ID_POWER_REFRESH) { RefreshAll(true); return 0; }

        if (id == ID_VOLT_APPLY) { OnApplyVoltage(); return 0; }
        if (id == ID_VOLT_IMPORT) {
            if (ImportVoltageFromCompanion(false)) RefreshVoltagePanel();
            return 0;
        }
        if (id == ID_VOLT_BACKEND_CHECK) {
            std::wstring version, terr;
            if (nvpwr::TestMVoltLaunch(version, terr, g_desired.mvoltPath)) {
                std::wstring msg = UiText(T_VOLT_BACKEND_OK);
                msg += L"\r\n\r\n";
                msg += version;
                MessageBoxW(hwnd, msg.c_str(), UiText(T_VOLT_BACKEND), MB_ICONINFORMATION);
            } else {
                MessageBoxW(hwnd, terr.c_str(), UiText(T_VOLT_BACKEND), MB_ICONERROR);
            }
            RefreshVoltagePanel();
            return 0;
        }
        if (id == ID_VOLT_RESET) {
            /*
                Reset must clear whichever backend actually holds the offsets: a
                native baseline may exist from an earlier native apply, and the
                companion tool may also be carrying values. Both are cleared
                rather than assuming one.
            */
            bool ok = true;
            std::wstring all;

            if (nvpwr::HasVoltageBaseline()) {
                std::wstring verr;
                nvpwr::VoltageState post{};
                if (!nvpwr::ResetNvapiVoltage(post, verr)) { ok = false; all += L"native: " + verr + L"; "; }
                g_voltage = post;
            }
            if (g_mvoltReady) {
                std::wstring verr;
                if (!nvpwr::ResetVoltageViaMVolt(verr, g_desired.mvoltPath)) {
                    ok = false; all += L"mVolt+: " + verr + L"; ";
                }
            }
            if (!ok) MessageBoxW(hwnd, all.c_str(), UiText(T_VOLT_RESET), MB_ICONERROR);

            for (int r = 0; r < 2; ++r)
                for (int f = 0; f < 4; ++f) SetEditLongLong(g_voFieldEdit[r][f], 0);
            for (int i = 0; i < 4; ++i) SetEditLongLong(g_voDemandEdit[i], 0);

            g_desired.voltage = nvpwr::VoltageTuning{};
            g_desired.voltage.applier = nvpwr::VoltageApplier::CompanionTool;
            std::wstring serr;
            nvpwr::SaveDesiredState(g_desired, serr);
            LogLine(L"Voltage reset requested on all available backends");
            RefreshVoltagePanel();
            return 0;
        }
        if (id == ID_VOLT_DEFAULT) { OnRestoreDefaults(L"voltage page"); return 0; }

        if (id == ID_ADV_APPLY) { OnApplyClocks(); return 0; }
        if (id == ID_ADV_RESET) {
            std::wstring err;
            NvapiTunerState post{};
            ResetNvapiTuning(false, post, err);
            g_tuner = post;
            g_desired.clock = nvpwr::ClockTuning{};
            nvpwr::SaveDesiredState(g_desired, err);
            RefreshClocksPanel();
            return 0;
        }

        if (id == ID_LOG_COPY) {
            int len = GetWindowTextLengthW(g_logText);
            if (len > 0) {
                std::wstring text((size_t)len + 1, L'\0');
                GetWindowTextW(g_logText, &text[0], len + 1);
                text.resize((size_t)len);
                if (OpenClipboard(hwnd)) {
                    EmptyClipboard();
                    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
                    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (mem) {
                        void* dst = GlobalLock(mem);
                        if (dst) { memcpy(dst, text.c_str(), bytes); GlobalUnlock(mem); }
                        SetClipboardData(CF_UNICODETEXT, mem);
                    }
                    CloseClipboard();
                }
            }
            return 0;
        }
        if (id == ID_LOG_FOLDER) {
            std::wstring dir = nvpwr::StateDirectory();
            if (!dir.empty()) ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (id == ID_LOG_CLEAR) { SetWindowTextW(g_logText, L""); return 0; }

        if (id == ID_SET_START_WIN) {
            bool want = (int)SendMessageW(g_stStartWin, BM_GETCHECK, 0, 0) == BST_CHECKED;
            std::wstring err;
            if (!nvpwr::SetGuiAutoStart(want, err)) {
                MessageBoxW(hwnd, err.c_str(), UiText(T_APP_TITLE), MB_ICONERROR);
                SendMessageW(g_stStartWin, BM_SETCHECK, (WPARAM)(want ? BST_UNCHECKED : BST_CHECKED), 0);
            } else {
                g_desired.startWithWindows = want;
                nvpwr::SaveDesiredState(g_desired, err);
            }
            return 0;
        }
        if (id == ID_SET_START_MIN) {
            g_desired.startMinimized = ((int)SendMessageW(g_stStartMin, BM_GETCHECK, 0, 0) == BST_CHECKED);
            std::wstring err;
            nvpwr::SaveDesiredState(g_desired, err);
            return 0;
        }
        if (id == ID_SET_SVC_INSTALL) {
            std::wstring err;
            if (!nvpwr::InstallService(err))
                MessageBoxW(hwnd, err.c_str(), UiText(T_APP_TITLE), MB_ICONERROR);
            else
                LogLine(L"Background service installed and started");
            RefreshServicePanel();
            return 0;
        }
        if (id == ID_SET_SVC_REMOVE) {
            std::wstring err;
            if (!nvpwr::RemoveService(err))
                MessageBoxW(hwnd, err.c_str(), UiText(T_APP_TITLE), MB_ICONERROR);
            else
                LogLine(L"Background service stopped and removed");
            RefreshServicePanel();
            return 0;
        }
        if (id == ID_SET_STANDARD_MODE) {
            /* Standard mode exists because kernel anti-cheat needs Secure Boot on
               and the helper removed. Settings are kept so returning to unlock
               mode restores the user's setup. */
            if (MessageBoxW(hwnd, UiText(T_SET_MODE_CONFIRM), UiText(T_SET_MODE),
                            MB_OKCANCEL | MB_ICONWARNING) != IDOK) return 0;
            std::wstring err;
            nvpwr::RestorePoint rp{};
            if (!SendRestore(err)) LogLine(L"StandardMode: restore failed: " + err);
            if (nvpwr::HasVoltageBaseline()) {
                std::wstring verr;
                nvpwr::VoltageState post{};
                nvpwr::ResetNvapiVoltage(post, verr);
            }
            if (!nvpwr::RemoveService(err)) LogLine(L"StandardMode: service removal failed: " + err);
            CloseDeviceHandle();
            LogLine(L"StandardMode: entered; settings retained for unlock mode");
            RefreshAll(false);
            return 0;
        }

        if (id >= ID_SLOT_SAVE_BASE && id < ID_SLOT_SAVE_BASE + nvpwr::kSlotCount) {
            OnSaveSlot(id - ID_SLOT_SAVE_BASE); return 0;
        }
        if (id >= ID_SLOT_BASE && id < ID_SLOT_BASE + nvpwr::kSlotCount) {
            OnLoadSlot(id - ID_SLOT_BASE); return 0;
        }
        if (id >= ID_SLOT_CLEAR_BASE && id < ID_SLOT_CLEAR_BASE + nvpwr::kSlotCount) {
            OnClearSlot(id - ID_SLOT_CLEAR_BASE); return 0;
        }

        if (id == ID_SAFETY_CONFIRM) {
            LogLine(L"Safety: user confirmed the provisional change");
            nvpwr::ClearRestorePoint();
            SafetyEnd();
            RefreshAll(false);
            return 0;
        }
        if (id == ID_SAFETY_REVERT) { SafetyRevert(); return 0; }
        break;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis && dis->CtlType == ODT_BUTTON) { DrawButton(dis); return TRUE; }
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
        /* Dark theme: labels must not paint on the default white background. */
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kUiTextWhite);
        return reinterpret_cast<LRESULT>(g_uiBackground);
    }

    case WM_DPICHANGED: {
        UINT dpi = HIWORD(wp) ? HIWORD(wp) : LOWORD(wp);
        UiSetDpi(dpi);
        RECT* suggested = reinterpret_cast<RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ApplyLanguage(g_uiLang);
        Layout(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_SIZE:
        Layout(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = UiScale(980);
        mmi->ptMinTrackSize.y = UiScale(760);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, g_uiBackground);

        RECT accent{ 0, 0, rc.right, UiScale(3) };
        HBRUSH green = CreateSolidBrush(kUiNvidiaGreen);
        FillRect(dc, &accent, green);
        DeleteObject(green);

        RECT rail{ 0, UiScale(3), UiScale(kNavWidth), rc.bottom };
        HBRUSH railBrush = CreateSolidBrush(kUiCard);
        FillRect(dc, &rail, railBrush);
        DeleteObject(railBrush);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kUiTextWhite);
        SelectObject(dc, g_uiHeadingFont);
        RECT title{ UiScale(kNavWidth + kMargin), UiScale(24), rc.right - UiScale(kMargin), UiScale(62) };
        DrawTextW(dc, UiText(T_APP_TITLE), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(dc, g_uiSmallFont);
        SetTextColor(dc, kUiTextMuted);
        RECT sub{ UiScale(kNavWidth + kMargin), UiScale(60), rc.right - UiScale(kMargin), UiScale(82) };
        std::wstring subtitle = g_gpuName;
        if (g_oemMw) subtitle += L"   ·   OEM " + std::to_wstring(g_oemMw / 1000) + L" W";
        DrawTextW(dc, subtitle.c_str(), -1, &sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        /* Safety banner sits above the page content, in the accent colour so a
           pending confirmation is impossible to miss. */
        if (g_safetyActive) {
            RECT bar{ UiScale(kNavWidth + kMargin), UiScale(88), rc.right - UiScale(kMargin), UiScale(126) };
            HBRUSH warn = CreateSolidBrush(RGB(60, 44, 12));
            FillRect(dc, &bar, warn);
            DeleteObject(warn);
            SetTextColor(dc, kUiAmber);
            SelectObject(dc, g_uiStateFont);
            wchar_t b[200]{};
            swprintf_s(b, L"%s   (%d s)", UiText(T_SAFETY_PENDING), g_safetySecondsLeft);
            RECT text = bar;
            text.top += UiScale(2);
            DrawTextW(dc, b, -1, &text, DT_CENTER | DT_TOP | DT_SINGLELINE);

            SetTextColor(dc, kUiTextMuted);
            SelectObject(dc, g_uiSmallFont);
            RECT hint = bar;
            hint.top = bar.top + UiScale(22);
            DrawTextW(dc, UiText(T_SAFETY_NOTE), -1, &hint,
                      DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CLOSE:
        /* Kept running in the tray is a first-class option, not an accident. */
        if (g_desired.startMinimized) {
            ShowWindow(hwnd, SW_HIDE);
            LogLine(L"Window hidden; still running");
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_STATUS);
        SafetyEnd();
        CloseDeviceHandle();
        UiDestroy();
        PostQuitMessage(0);
        return 0;

    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------- entry point ---------------- */

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int show) {
    g_inst = inst;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    HDC screen = GetDC(nullptr);
    if (screen) { g_uiDpi = GetDeviceCaps(screen, LOGPIXELSY); ReleaseDC(nullptr, screen); }
    UiCreateFonts();
    UiInitBrushes();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"NvpwrControlWnd";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Nvpwr Control 1.9.0",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                UiScale(1060), UiScale(820),
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show == SW_SHOWMINIMIZED ? SW_SHOWMINIMIZED : SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
