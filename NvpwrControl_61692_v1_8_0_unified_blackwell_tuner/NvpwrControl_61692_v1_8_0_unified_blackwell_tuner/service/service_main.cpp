/*
    service_main.cpp — Nvpwr Control background service (NvpwrSvc.exe).

    WHERE: a user-mode Windows service, the only component that is allowed to
           keep the tuning alive without a window on screen.
    WHAT:  owns the desired state, replays it at boot and after resume, and
           serves the tray GUI over a named pipe.
    WHY:   GPU power/voltage policy lives in memory owned by the NVIDIA driver
           and does not survive a driver reload or a reboot. "Keep my settings
           after a restart" can therefore only mean "re-apply them at startup".
           That is what this process does, and it is also why the replay has to
           report failures instead of assuming success: at boot the NVIDIA stack
           may not be ready yet, and the helper driver may have failed to load.

    SECURITY NOTE (read before shipping this more widely):
      The pipe grants FILE_ALL_ACCESS with a NULL security descriptor, which
      yields a default ACL reachable by the interactive user. Every command is
      re-validated here (profile range, voltage envelope, readback), so a local
      user cannot push an out-of-range value through the pipe - but they CAN
      change the power limit within the allowed range. For a wider deployment the
      descriptor should be replaced with one naming only BUILTIN\Administrators
      and the GUI should be required to be elevated.
*/

#include <windows.h>
#include <tlhelp32.h>
#include <sstream>
#include <string>
#include <vector>

#include "..\\app\\nvpwr_ipc.h"
#include "..\\app\\nvpwr_ui_state.h"
#include "..\\app\\nvpwr_mvolt_bridge.h"
#include "..\\shared\\nvpwr_ioctl.h"

using nvpwr::DesiredState;

static SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
static SERVICE_STATUS g_status{};
static HANDLE g_stopEvent = nullptr;

/*
    Signalled when a user logs on.

    The replay used to run the instant the service started, which on this machine is about
    twelve seconds after the kernel hands control over — while nvlddmkm is still building the
    display stack. Raising the power ceiling re-enters NVIDIA's own power-policy generator, and
    doing that at that moment hung the machine outright: no bugcheck, no dump, mouse frozen and
    no shell, and the only way out was the power button. The Kernel-Power 41 record for that
    event has BugcheckCode 0 and LongPowerButtonPressDetected true, which is the signature of a
    hard hang rather than a crash.

    So the replay waits for the desktop. There are two ways that happens and both are needed:

      - the service starts before the user logs on  -> the session-change notification arrives
      - the service starts after  the user logs on  -> the notification already fired, so the
        desktop is probed once at startup instead

    Neither alone covers the other. A service restarted by the SCM, or started by hand from an
    already-running session, would wait forever on an event that has been and gone.
*/
#define NVPWR_WTS_SESSION_LOGON  5
#define NVPWR_WTS_SESSION_UNLOCK 8

static HANDLE g_logonEvent = nullptr;
static const DWORD kSettleMs = 10000;          /* after the desktop appears */
static const DWORD kDesktopTimeoutMs = 600000; /* give up after ten minutes */

/* ------------------------------------------------------------------ */
/* logging                                                            */
/* ------------------------------------------------------------------ */

static void SvcLog(const std::wstring& text) {
    std::wstring dir = nvpwr::StateDirectory();
    if (dir.empty()) return;
    std::wstring path = dir + L"\\nvpwr-service.log";

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[64]{};
    swprintf_s(stamp, L"[%04u-%02u-%02u %02u:%02u:%02u] ",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    std::wstring line = std::wstring(stamp) + text + L"\r\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &utf8[0], n, nullptr, nullptr);
    DWORD wrote = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wrote, nullptr);
    CloseHandle(h);
}

static void SetServiceState(DWORD state, DWORD exitCode = NO_ERROR, DWORD hint = 0) {
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = hint;
    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
        g_status.dwControlsAccepted = 0;
    else
        g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN |
                                      SERVICE_ACCEPT_SESSIONCHANGE;
    SetServiceStatus(g_statusHandle, &g_status);
}

/* ------------------------------------------------------------------ */
/* device access                                                      */
/* ------------------------------------------------------------------ */

static std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t p = s.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? L"." : s.substr(0, p);
}

/*
    "Which binary am I?" — written to the log at every start.

    This service rewrites driver memory, so knowing which build produced a given boot's log is
    the difference between a diagnosis and a guess. It matters more than usual here because the
    same program is run from whatever folder it was extracted to, while the installed service
    points at one specific path — and a service that is already running keeps running its old
    image when sc config changes binPath. Install now stops it first, and this line is how to
    confirm that worked.
*/
static std::wstring SelfIdentity() {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return L"(unknown path)";

    std::wstring out = path;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        FILETIME local{};
        SYSTEMTIME st{};
        if (FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local) &&
            FileTimeToSystemTime(&local, &st)) {
            wchar_t stamp[64]{};
            swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            out += L"  (built ";
            out += stamp;
            out += L")";
        }
    }
    return out;
}

static bool IsRegularFile(const std::wstring& path) {    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The service may live next to the GUI or in its own folder; search a few
   plausible locations the same way the GUI does. */
static std::wstring LocateDriverFile() {
    static const wchar_t kDriverFile[] = L"Nvpwr.sys";
    std::vector<std::wstring> roots;
    roots.push_back(ExeDirectory());
    {
        wchar_t base[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
            roots.push_back(std::wstring(base) + L"\\NvpwrControl");
    }

    for (const std::wstring& root : roots) {
        std::wstring dir = root;
        for (int depth = 0; depth < 4 && !dir.empty(); ++depth) {
            const std::wstring candidates[] = {
                dir + L"\\" + kDriverFile,
                dir + L"\\dist\\" + kDriverFile,
                dir + L"\\driver\\x64\\Release\\" + kDriverFile,
                dir + L"\\driver\\x64\\Debug\\" + kDriverFile,
                dir + L"\\app\\x64\\Release\\" + kDriverFile,
            };
            for (const std::wstring& c : candidates)
                if (IsRegularFile(c)) return c;

            size_t p = dir.find_last_of(L"\\/");
            if (p == std::wstring::npos || p < 3) break;
            dir = dir.substr(0, p);
        }
    }
    return L"";
}

/*
    Driver Signature Enforcement, through EfiGuard's EfiDSEFix.

    Nvpwr.sys is self-signed, so Windows refuses to load it while DSE is enforced. EfiGuard
    patches the kernel during boot, which makes it possible to turn DSE off and back on at run
    time — no test-signing mode, so no desktop watermark and no BCD flag for anti-cheat to read.

    The window is deliberately tiny: off immediately before the load, on immediately after. It
    is restored even when the load fails, and a failure to restore is logged loudly because it
    is the one outcome here that is worse than not applying the power limit at all.
*/
static bool RunDseFix(const wchar_t* argument, std::wstring& error) {
    std::wstring dir = LocateDriverFile();
    if (!dir.empty()) {
        size_t p = dir.find_last_of(L"\\/");
        if (p != std::wstring::npos) dir = dir.substr(0, p);
    }
    if (dir.empty()) { error = L"cannot locate the program directory"; return false; }
    std::wstring exe = dir + L"\\EfiDSEFix.exe";

    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"EfiDSEFix.exe not found beside the service";
        return false;
    }

    std::wstring cmd = L"\"" + exe + L"\" " + argument;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        error = L"cannot start EfiDSEFix " + std::wstring(argument) +
                L" (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    /* EfiDSEFix writes with WriteConsoleW, which bypasses redirection, so the exit code is the
       only signal available. */
    DWORD wait = WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        error = L"EfiDSEFix " + std::wstring(argument) + L" timed out";
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
        if (code != 0) {
            wchar_t hex[32];
            swprintf_s(hex, L"0x%08X", code);
            error = L"EfiDSEFix " + std::wstring(argument) + L" exited " + hex;
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait != WAIT_TIMEOUT && code == 0;
}

/* True when this boot went through EfiGuard and the hook answers. */
static bool EfiGuardActive() {
    std::wstring err;
    return RunDseFix(L"-c", err);
}

/*
    Opens \\.\Nvpwr, registering and starting the kernel service if needed.

    At boot the NVIDIA driver may not be initialised yet, so a failure here is
    expected and must be retried rather than treated as fatal. The kernel side
    also refuses work until nvlddmkm.sys is present and its build signature
    matches, which is why the caller retries with a backoff.

    DSE is closed around the attempt and reopened immediately afterwards. When EfiGuard is not
    active the load cannot succeed, so the attempt is skipped and reported: that is a boot that
    did not go through the EfiGuard entry, and the power ceiling simply stays at the factory
    value.
*/
static bool OpenDevice(std::wstring& error) {
    /* Already open? Then this is a later replay in the same session and none of the ceremony
       below is needed. */
    {
        HANDLE probe = CreateFileW(NVPWR_DEVICE_WIN32, GENERIC_READ | GENERIC_WRITE,
                                   0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (probe != INVALID_HANDLE_VALUE) { CloseHandle(probe); return true; }
    }

    if (!EfiGuardActive()) {
        error = L"EfiGuard is not active for this boot — power settings were not applied";
        return false;
    }

    std::wstring dseErr;
    bool dseOff = RunDseFix(L"-d", dseErr);
    if (!dseOff) {
        error = L"could not disable DSE: " + dseErr;
        return false;
    }

    bool opened = false;

    /* Register the helper if it is not there yet. Manual start is fine: this
       service is the only thing that needs it, and it starts on demand. */
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr,
                                   SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, L"Nvpwr",
                                     SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
        if (!svc) {
            std::wstring sys = LocateDriverFile();
            if (!sys.empty()) {
                svc = CreateServiceW(scm, L"Nvpwr", L"Nvpwr GPU Power Control",
                                     SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG,
                                     SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                                     SERVICE_ERROR_NORMAL, sys.c_str(),
                                     nullptr, nullptr, nullptr, nullptr, nullptr);
                if (svc) SvcLog(L"registered kernel helper at " + sys);
            } else {
                error = L"Nvpwr.sys was not found in any expected location";
            }
        }
        if (svc) {
            SERVICE_STATUS_PROCESS ssp{};
            DWORD needed = 0;
            if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                     reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed)) {
                if (ssp.dwCurrentState != SERVICE_RUNNING) {
                    if (!StartServiceW(svc, 0, nullptr)) {
                        DWORD e = GetLastError();
                        if (e != ERROR_SERVICE_ALREADY_RUNNING)
                            error = L"StartService(Nvpwr) failed: " + std::to_wstring(e);
                    }
                }
            }
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    } else {
        error = L"OpenSCManager failed";
    }

    HANDLE dev = CreateFileW(NVPWR_DEVICE_WIN32, GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (dev == INVALID_HANDLE_VALUE) {
        if (error.empty())
            error = L"cannot open \\\\.\\Nvpwr (error " + std::to_wstring(GetLastError()) + L")";
    } else {
        CloseHandle(dev);
        opened = true;
    }

    /* Back on no matter how the attempt went. Left off, the machine would run without driver
       signature enforcement until the next reboot — worse than not applying the limit. */
    std::wstring restoreErr;
    if (!RunDseFix(L"-e", restoreErr)) {
        SvcLog(L"WARNING: driver signature enforcement could not be restored: " + restoreErr +
               L" — reboot to clear it");
    }

    return opened;
}

static bool SendPower(unsigned int milliwatts, unsigned int ceilingMw, unsigned int profile,
                      std::wstring& error)
{
    HANDLE dev = CreateFileW(NVPWR_DEVICE_WIN32, GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (dev == INVALID_HANDLE_VALUE) {
        error = L"device not open";
        return false;
    }
    NVPWR_SET_POWER req{};
    req.Version = NVPWR_SET_VERSION;
    req.TargetMilliwatts = milliwatts;
    req.MaxMilliwatts = ceilingMw;
    req.Profile = profile;

    DWORD got = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_NVPWR_SET_POWER, &req, sizeof(req),
                              nullptr, 0, &got, nullptr);
    if (!ok) error = L"SET_POWER failed (error " + std::to_wstring(GetLastError()) + L")";
    CloseHandle(dev);
    return ok != FALSE;
}

static bool SendRestore(std::wstring& error) {
    HANDLE dev = CreateFileW(NVPWR_DEVICE_WIN32, GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (dev == INVALID_HANDLE_VALUE) {
        error = L"device not open";
        return false;
    }
    DWORD got = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_NVPWR_RESTORE, nullptr, 0,
                              nullptr, 0, &got, nullptr);
    if (!ok) error = L"RESTORE failed (error " + std::to_wstring(GetLastError()) + L")";
    CloseHandle(dev);
    return ok != FALSE;
}

/* Reads OEM baseline, the active profile and the profile's minimum out of the driver. */
static bool QueryDriverBaseline(unsigned int& oemBaselineMw, unsigned int& profile,
                                unsigned int& supportedMinMw, std::wstring& error) {
    HANDLE dev = CreateFileW(NVPWR_DEVICE_WIN32, GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (dev == INVALID_HANDLE_VALUE) {
        error = L"device not open";
        return false;
    }
    NVPWR_STATUS st{};
    DWORD got = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_NVPWR_STATUS, nullptr, 0, &st, sizeof(st), &got, nullptr);
    CloseHandle(dev);
    if (!ok || got < sizeof(st)) {
        error = L"STATUS failed (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    oemBaselineMw = st.OemBaseline;
    profile = st.ActiveProfile;
    supportedMinMw = st.SupportedMin;
    return true;
}

/*
    Records the factory power wall, once.

    This runs before the first replay, which is the only moment the answer is still
    certain: nothing this program does has touched the wall yet, so the driver's
    OemBaseline is genuinely the value the card shipped with. After a replay it is
    not — the driver captures whatever the ceiling was when it first mutated it, so
    a service that starts on an already-raised wall records the raised value.

    It is written once and then left alone. The single exception is a different GPU
    profile, which means different hardware or a different VBIOS and therefore a
    different factory wall; nothing else may rewrite it.

    THE STOCK CHECK. At a boot the wall has just been reset by the display driver and
    equals the profile's minimum. If it does not, the wall has been raised since the
    last reload — the case that arises when this runs mid-session rather than at boot,
    or when a deploy unloaded the helper while the ceiling was up. Writing that number
    down would poison the record permanently, and the record is precisely the thing
    that cannot be corrected later. So a wall above the minimum is treated as "not
    now" and the field is left empty for the next real boot to fill in.

    Failing here is not fatal. The replay still runs, and the record is simply still
    absent, which the GUI also handles.
*/
/*
    The factory wall lives in the registry, under HKLM\SOFTWARE\NvpwrControl, keyed by GPU
    profile.

    It is a property of the card rather than of a tuning session, so it does not belong in the
    state file — that file belongs to whatever the user was last doing and is rewritten by
    every slot save, undo and restore-defaults. A fact that must never change should not live
    in the file that changes most. Both this service and the GUI read the same key, so neither
    has to locate the other's working directory, and a hit ends the search immediately.
*/
static const wchar_t* kFactoryWallKey = L"SOFTWARE\\NvpwrControl";

static bool ReadFactoryWall(unsigned int profile, unsigned int& watts) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kFactoryWallKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;

    bool ok = false;
    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    DWORD wall = 0;
    if (RegQueryValueExW(k, L"FactoryWallW", nullptr, &type, (LPBYTE)&wall, &size) == ERROR_SUCCESS &&
        type == REG_DWORD && wall > 0) {
        DWORD stored = 0;
        size = sizeof(DWORD);
        if (RegQueryValueExW(k, L"FactoryWallProfile", nullptr, &type, (LPBYTE)&stored, &size) == ERROR_SUCCESS &&
            type == REG_DWORD && stored == profile) {
            watts = wall;
            ok = true;
        }
    }
    RegCloseKey(k);
    return ok;
}

static bool WriteFactoryWall(unsigned int profile, unsigned int watts) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kFactoryWallKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = RegSetValueExW(k, L"FactoryWallW", 0, REG_DWORD,
                             (const BYTE*)&watts, sizeof(watts)) == ERROR_SUCCESS &&
              RegSetValueExW(k, L"FactoryWallProfile", 0, REG_DWORD,
                             (const BYTE*)&profile, sizeof(profile)) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

static void CapturePowerFloorIfAbsent(const DesiredState& current) {
    DesiredState st = current;

    unsigned int baselineMw = 0, driverProfile = 0, supportedMinMw = 0;
    std::wstring err;
    if (!QueryDriverBaseline(baselineMw, driverProfile, supportedMinMw, err)) {
        SvcLog(L"power floor: " + err);
        return;
    }

    /*
        Which profile to key the record by.

        The STORED profile, not the driver's ActiveProfile. The driver reports that as 0 until its
        power policy has been armed, and at this point in the replay it has not been — so on a
        cold boot the driver says 0 while the card is a 5090, and the record written under 0 is
        unreadable on the next start because the comparison is then 0 against 3.

        The GUI keys the record by the same stored number. That is the whole point: both entry
        points must agree on the identity they file the value under, or each writes a record the
        other cannot find. Observed before this change: the service logged profile 0 and the GUI
        logged profile 3 for the same card on the same boot.

        The driver's answer is only used when the state has none — a first run with no state file
        at all, where anything is better than nothing.
    */
    const unsigned int profile = (st.power.profile != 0) ? st.power.profile : driverProfile;
    if (profile == 0) {
        SvcLog(L"power floor: no profile known yet (state and driver both report 0), not recording");
        return;
    }

    /* Already on record for this GPU? Then there is nothing to look up. */
    unsigned int recorded = 0;
    if (ReadFactoryWall(profile, recorded)) {
        if (st.powerFloorW != recorded || st.powerFloorProfile != profile) {
            st.powerFloorW = recorded;
            st.powerFloorProfile = profile;
            nvpwr::SaveDesiredState(st, err);
        }
        return;
    }

    if (baselineMw == 0) {
        /*
            The driver reports no baseline. That is NOT a fallback case — there is nothing else
            to read it from. The enforced limit is the wall right now, not the one the card
            shipped with, so writing it down records a raised ceiling as factory.
        */
        SvcLog(L"power floor: driver reports no baseline yet, leaving the record empty");
        return;
    }

    const unsigned int floorW = baselineMw / 1000u;
    if (supportedMinMw != 0 && baselineMw > supportedMinMw) {
        SvcLog(L"power floor: not captured, wall is above stock (" + std::to_wstring(floorW) +
               L" W > " + std::to_wstring(supportedMinMw / 1000u) +
               L" W) — a reboot will reset it and this will record then");
        return;
    }

    st.powerFloorW = floorW;
    st.powerFloorProfile = profile;
    nvpwr::SaveDesiredState(st, err);
    if (WriteFactoryWall(profile, floorW)) {
        SvcLog(L"power floor recorded: " + std::to_wstring(floorW) + L" W (profile " +
               std::to_wstring(profile) + L") — written to HKLM\\SOFTWARE\\NvpwrControl");
    } else {
        SvcLog(L"power floor recorded: " + std::to_wstring(floorW) +
               L" W (profile " + std::to_wstring(profile) + L") but the registry write failed");
    }
}

/* ------------------------------------------------------------------ */
/* state replay                                                       */
/* ------------------------------------------------------------------ */

static bool g_replayed = false;

/*
    Applies the stored voltage by invoking the companion tool (mVolt+).

    WHY THIS IS A SEPARATE PROCESS AND NOT A CALL INTO THE DRIVER
      This service cannot write voltage rails itself: no public interface exists
      for it, and Pstates20 carries no voltage domain on the target GPU (measured
      on the reference machine). mVolt+ does have that access. So the service
      orchestrates: it runs mVolt+'s CLI with the offsets the user saved, then
      reads the result back.

    WHY THE RESULT IS VERIFIED RATHER THAN TRUSTED
      A companion tool can be missing, moved, or updated to a version whose CLI
      changed. Each of those has to surface as a specific log line rather than as
      "voltage silently did not apply".
*/
static bool ReplayVoltageViaCompanion(const DesiredState& state) {
    if (!state.voltage.enabled || state.voltage.IsZero()) return true;

    if (state.voltage.applier != nvpwr::VoltageApplier::CompanionTool) {
        SvcLog(L"voltage: no applier configured for this state; nothing to do");
        return true;
    }

    const std::wstring path = state.mvoltPath;
    if (!nvpwr::MVoltAvailable(path)) {
        std::wstring msg = L"voltage: companion tool (mVolt+) was not found at the "
                           L"configured location; voltage was NOT applied";
        if (!path.empty()) msg += L" (" + path + L")";
        SvcLog(msg);
        return false;
    }

    /* Prove it can be launched from this context before asking it to do work: a
       LocalSystem service and an interactive shell are different environments,
       and finding out here gives a far clearer log line than a failed apply. */
    {
        std::wstring version, terr;
        if (!nvpwr::TestMVoltLaunch(version, terr, path)) {
            SvcLog(L"voltage: companion tool could not be launched from the service "
                   L"context: " + terr);
            return false;
        }
        SvcLog(L"voltage: companion tool reachable (" + version + L")");
    }

    bool rounded = false;
    std::wstring verr;
    if (!nvpwr::ApplyVoltageViaMVolt(state.voltage, verr, &rounded, path)) {
        SvcLog(L"voltage: apply failed: " + verr);
        return false;
    }
    if (rounded) {
        /* Reported rather than hidden: a stored value finer than 1 mV cannot be
           sent through this CLI, so the applied value differs slightly from the
           saved one and the user should know. */
        SvcLog(L"voltage: applied with rounding to whole millivolts");
    }

    /* Read back and compare, so "applied" means "verified applied". */
    nvpwr::MVoltSnapshot snap{};
    if (nvpwr::QueryMVoltStatus(snap, path)) {
        const bool nvvddOk = (snap.nvvdd.vminUv == state.voltage.nvvdd.vminUv &&
                              snap.nvvdd.relUv  == state.voltage.nvvdd.relUv &&
                              snap.nvvdd.altUv  == state.voltage.nvvdd.altUv &&
                              snap.nvvdd.ovUv   == state.voltage.nvvdd.ovUv);
        const bool msvddOk = (snap.msvdd.vminUv == state.voltage.msvdd.vminUv &&
                              snap.msvdd.relUv  == state.voltage.msvdd.relUv &&
                              snap.msvdd.altUv  == state.voltage.msvdd.altUv &&
                              snap.msvdd.ovUv   == state.voltage.msvdd.ovUv);
        if (!nvvddOk || !msvddOk) {
            SvcLog(L"voltage: readback does not match the stored offsets");
            return false;
        }
        SvcLog(L"voltage: applied and verified");
    } else {
        SvcLog(L"voltage: applied, but readback could not be confirmed: " + snap.error);
    }
    return true;
}

/*
    Replays the stored clock offsets.

    Until this existed the service replayed power and voltage and stopped. The four clock
    offsets were written to the state file on every apply and then ignored at boot, so a reboot
    returned the power ceiling and silently dropped the overclock — the state file said one
    thing and the driver another, with nothing in the log either way.

    Runs last, after the ceiling is up and the rails are set, because a clock offset is only
    meaningful once there is voltage and power headroom for it to use.
*/
static bool ReplayClocksViaCompanion(const DesiredState& state) {
    if (!state.clock.enabled || state.clock.IsZero()) return true;
    if (!nvpwr::MVoltAvailable(state.mvoltPath)) {
        SvcLog(L"clocks: companion tool (mVolt+) was not found; clock offsets were NOT applied");
        return false;
    }

    std::wstring err;
    if (!nvpwr::ApplyClocksViaMVolt(state.clock, err, state.mvoltPath)) {
        SvcLog(L"clocks: apply failed: " + err);
        return false;
    }

    SvcLog(L"clocks: applied core=" + std::to_wstring(state.clock.coreOffsetMhz) +
           L" mem=" + std::to_wstring(state.clock.memoryOffsetMhz) +
           L" xbar=" + std::to_wstring(state.clock.xbarOffsetMhz) +
           L" sys=" + std::to_wstring(state.clock.sysOffsetMhz));
    return true;
}

/*
    Re-applies the persisted state. Retries because at boot this routinely runs
    before nvlddmkm.sys is ready; a single failure at startup must not be treated
    as "the user's settings do not work".
*/
static bool ReplayDesiredState(const wchar_t* reason, int attempts = 1, int delayMs = 0) {
    DesiredState state{};
    std::wstring err;
    if (!nvpwr::LoadDesiredState(state, err)) {
        SvcLog(std::wstring(L"replay(") + reason + L"): state unreadable: " + err);
        return false;
    }
    if (state.IsEmpty()) {
        SvcLog(std::wstring(L"replay(") + reason + L"): nothing configured");
        return true;
    }

    for (int i = 0; i < attempts; ++i) {
        std::wstring derr;
        if (OpenDevice(derr)) {
            bool ok = true;
            std::wstring stageErr;

            /*
                Record the factory wall HERE, not before the loop.

                It has to run once the device is open — the driver may not be loaded yet when the
                service starts, which is exactly what happened on this machine: every startup
                logged "device not open" and the record stayed empty, after which the GUI fell
                back to a live reading and wrote a raised ceiling down as factory.

                And it has to run before the first SendPower, because that is what raises the
                ceiling. Between those two points the driver's OemBaseline is the truth.
            */
            CapturePowerFloorIfAbsent(state);

            /*
                The stages retry as a group, and the retry is the point.

                Measured on this machine: a start where the driver had just been loaded returned
                ERROR_GEN_FAILURE from Phase A — the driver's own failure to converge — and the
                identical request succeeded a minute later with no other change. The device being
                OPEN is not the same as the driver being ready to rewrite its power policy, and
                OpenDevice only knows the former.

                Only the failure path costs anything: a successful replay returns after one pass.
                A persistent failure is reported once, after the last attempt, so the log does
                not fill with the same line four times.
            */
            const int kStageAttempts = 4;
            for (int stageTry = 0; stageTry < kStageAttempts; ++stageTry) {
                ok = true;
                stageErr.clear();

                /* Stage 1: power limit. This is the part only this project can do. */
                if (state.powerEnabled && state.power.milliwatts) {
                    std::wstring perr;
                    if (!SendPower(state.power.milliwatts, state.power.ceilingMw,
                                   state.power.profile, perr)) {
                        ok = false; stageErr += L"power: " + perr + L"; ";
                    }
                }

                /* Stage 2: voltage through the companion tool. Runs after power so
                   the ceiling is already raised when voltage widens what the GPU
                   will actually draw. A voltage failure does NOT undo the power
                   limit, and the log states that explicitly rather than implying the
                   whole replay failed. */
                if (!ReplayVoltageViaCompanion(state)) {
                    ok = false;
                    stageErr += L"voltage: see the line above; the power limit remains applied; ";
                }

                /* Stage 3: clock offsets. Last, so the ceiling and the rails are already in place
                   for them to use. A clock failure does not undo either. */
                if (!ReplayClocksViaCompanion(state)) {
                    ok = false;
                    stageErr += L"clocks: see the line above; power and voltage remain applied; ";
                }

                if (ok || stageTry + 1 >= kStageAttempts) break;

                SvcLog(std::wstring(L"replay(") + reason + L"): attempt " +
                       std::to_wstring(stageTry + 1) + L" of " + std::to_wstring(kStageAttempts) +
                       L" failed (" + stageErr + L"), retrying");
                if (WaitForSingleObject(g_stopEvent, 5000) == WAIT_OBJECT_0) break;
            }

            if (!stageErr.empty()) {
                SvcLog(std::wstring(L"replay(") + reason + L"): " + stageErr);
            } else {
                SvcLog(std::wstring(L"replay(") + reason + L"): applied");
            }
            g_replayed = ok;
            return ok;
        }

        SvcLog(std::wstring(L"replay(") + reason + L"): device not ready: " + derr);
        if (delayMs > 0) Sleep((DWORD)delayMs);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* pipe protocol                                                      */

/* ------------------------------------------------------------------ */
/* service body                                                       */
/* ------------------------------------------------------------------ */
/* waiting for a usable desktop                                       */
/* ------------------------------------------------------------------ */

/*
    Is explorer.exe running in that session?

    Matched by name because the shell's process id is not knowable in advance, and the session
    check matters: a service running as SYSTEM can see processes from every session, and an
    explorer.exe belonging to a disconnected RDP session says nothing about whether the console
    desktop is up.
*/
static bool ExplorerRunningInSession(DWORD session) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") != 0) continue;
            DWORD owner = 0;
            if (ProcessIdToSessionId(pe.th32ProcessID, &owner) && owner == session) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

/*
    The desktop is up when there is an active console session and the shell is running in it.

    Session 0 is the services session and is never the console, so it is rejected explicitly —
    WTSGetActiveConsoleSessionId returns 0 on a machine with no interactive session, and that
    must not be mistaken for a logged-in user.

    The shell is the signal rather than the session state alone because a session is already
    "active" while the logon UI is on screen, which is exactly the state the machine hung in.
*/
static bool IsDesktopReady() {
    DWORD session = WTSGetActiveConsoleSessionId();
    if (session == 0xFFFFFFFF || session == 0) return false;
    return ExplorerRunningInSession(session);
}

/*
    Waits for the desktop, then for it to settle, then replays.

    The settle delay is not padding. A fast logon finishes the shell in a second or two, and the
    display stack is still completing behind it — the same window in which raising the ceiling
    hung this machine. Ten seconds costs nothing at boot and removes the race.
*/
static void WaitForDesktopThenReplay() {
    if (IsDesktopReady()) {
        /* The notification for this logon has already been delivered, so there is nothing to
           wait for — a service started by hand, or restarted by the SCM after a failure. */
        SvcLog(L"replay(startup): desktop already up");
    } else {
        SvcLog(L"replay(startup): waiting for the interactive desktop");
        HANDLE events[2] = { g_stopEvent, g_logonEvent };
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, kDesktopTimeoutMs);

        if (wait == WAIT_OBJECT_0) return;                       /* stopping */
        if (wait == WAIT_TIMEOUT) {
            SvcLog(L"replay(startup): desktop did not appear within ten minutes; not replaying. "
                   L"Apply from the window when you are ready.");
            return;
        }
        SvcLog(L"replay(startup): logon seen");
    }

    SvcLog(L"replay(startup): letting the display stack settle for 10 s");
    if (WaitForSingleObject(g_stopEvent, kSettleMs) == WAIT_OBJECT_0) return;

    /* Retry within the replay itself: the display stack is usually up by now, but the driver
       may still refuse until its own readiness checks pass. */
    ReplayDesiredState(L"startup", /*attempts*/ 12, /*delayMs*/ 5000);
}

/* ------------------------------------------------------------------ */

static DWORD WINAPI ServiceThread(LPVOID) {
    SetServiceState(SERVICE_RUNNING);

    /*
        The factory wall is recorded inside ReplayDesiredState, once the device is actually open.
        Running it before the loop meant it ran before the driver had loaded, which on this
        machine is every boot.

        The replay itself no longer runs here directly — see WaitForDesktopThenReplay.
    */
    WaitForDesktopThenReplay();

    /*
        Nothing left to do but wait to be stopped.

        A named-pipe server used to live here, serving STATUS / APPLY / RESTORE / PING /
        SHUTDOWN. The GUI never spoke to it — it drives the device directly — and neither did
        anything else, so it was a protocol with no clients: code that could only ever break,
        never work, sitting in the one process that rewrites driver memory. Removed rather than
        left as an unused surface.
    */
    WaitForSingleObject(g_stopEvent, INFINITE);
    SetServiceState(SERVICE_STOPPED);
    return 0;
}

static DWORD WINAPI ServiceControl(DWORD control, DWORD eventType, LPVOID eventData, LPVOID) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        SetServiceState(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        if (g_stopEvent) SetEvent(g_stopEvent);
        return NO_ERROR;

    /*
        A user logged on or unlocked. Both count: the settings live in driver memory and are lost
        on a driver reload, which can happen without a reboot, and an unlock is as good a moment
        as any to notice. Nothing is replayed here — the handler must return promptly, so it only
        wakes the thread that is already waiting.
    */
    case SERVICE_CONTROL_SESSIONCHANGE:
        if ((eventType == NVPWR_WTS_SESSION_LOGON || eventType == NVPWR_WTS_SESSION_UNLOCK) &&
            g_logonEvent) {
            SetEvent(g_logonEvent);
        }
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(g_statusHandle, &g_status);
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(nvpwr::kServiceName, ServiceControl, nullptr);
    if (!g_statusHandle) return;

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwServiceSpecificExitCode = 0;
    SetServiceState(SERVICE_START_PENDING, NO_ERROR, 8000);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        SetServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    /* Auto-reset: the waiter consumes one logon, and a later unlock signals it again for the
       next wait. Manual-reset would leave it signalled forever and make every subsequent wait
       return instantly. */
    g_logonEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    SvcLog(L"service starting");
    /* 哪个二进制在跑 —— 见 SelfIdentity 的注释。这条日志是把"应该是新版"变成可核对的事实。 */
    SvcLog(L"running image: " + SelfIdentity());
    ServiceThread(nullptr);
    SvcLog(L"service stopped");
}

int wmain(int argc, wchar_t** argv) {
    /* Console mode exists for diagnosis: it runs the same body without the SCM
       so a failure can be watched directly instead of only in the event log. */
    bool console = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--console") == 0) console = true;
    }

    if (console) {
        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_logonEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        SvcLog(L"service starting in console mode");
        wprintf(L"NvpwrSvc running in console mode. Press Ctrl+C to stop.\n");
        /* No separate replay here: the body below waits for a desktop and then replays, and the
           person running this from a console is already looking at one, so that path takes the
           short branch. Replaying twice would only duplicate the log lines. */
        ServiceThread(nullptr);
        return 0;
    }

    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(nvpwr::kServiceName), ServiceMain },
        { nullptr, nullptr }
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        /* Not launched by the SCM. Report the hint rather than failing silently. */
        DWORD e = GetLastError();
        if (e == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            wprintf(L"NvpwrSvc must be started by the Service Control Manager.\n");
            wprintf(L"Run \"NvpwrSvc.exe --console\" to run it interactively.\n");
        }
        return (int)e;
    }
    return 0;
}