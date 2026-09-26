#include <Windows.h>
#include <cstdio>
#include <cwchar>
#include "..\\shared\\nvpwr_ioctl.h"

static const char* StateName(ULONG s)
{
    switch (s) {
    case NvpwrStateArmed: return "ARMED";
    case NvpwrStateApplied: return "APPLIED";
    case NvpwrStateMixed: return "MIXED";
    case NvpwrStateWrongBuild: return "WRONG_BUILD";
    case NvpwrStateModuleNotFound: return "MODULE_NOT_FOUND";
    case NvpwrStateGpuNotFound: return "GPU_NOT_FOUND";
    case NvpwrStateContextInvalid: return "CONTEXT_INVALID";
    case NvpwrStatePreconditionNotReady: return "PRECONDITION_NOT_READY";
    case NvpwrStateStockBaseline: return "STOCK_BASELINE";
    default: return "UNKNOWN";
    }
}

static ULONG ParseProfile(const wchar_t* s)
{
    if (!s) return NvpwrProfileUnknown;
    if (_wcsicmp(s, L"5050") == 0) return NvpwrProfileRtx5050Laptop;
    if (_wcsicmp(s, L"5060") == 0) return NvpwrProfileRtx5060Laptop;
    if (_wcsicmp(s, L"5070") == 0) return NvpwrProfileRtx5070Laptop;
    if (_wcsicmp(s, L"5070ti") == 0) return NvpwrProfileRtx5070TiLaptop;
    if (_wcsicmp(s, L"5080") == 0) return NvpwrProfileRtx5080Laptop;
    if (_wcsicmp(s, L"5090") == 0) return NvpwrProfileRtx5090Laptop;
    if (_wcsicmp(s, L"4090") == 0) return NvpwrProfileRtx4090Laptop;
    if (_wcsicmp(s, L"4080") == 0) return NvpwrProfileRtx4080Laptop;
    if (_wcsicmp(s, L"4070") == 0) return NvpwrProfileRtx4070Laptop;
    if (_wcsicmp(s, L"4060") == 0) return NvpwrProfileRtx4060Laptop;
    if (_wcsicmp(s, L"4050") == 0) return NvpwrProfileRtx4050Laptop;
    return NvpwrProfileUnknown;
}

static void PrintPower(const char* label, ULONG v)
{
    if (v == 0xFFFFFFFFu) std::printf("%-20s : FFFFFFFF\n", label);
    else std::printf("%-20s : %lu (%.3f W)\n", label, v, v / 1000.0);
}

/*
    The power window the driver will actually accept.

    This used to be a table hardcoded here, one row per board, and it disagreed with
    the kernel side: the CLI capped the 5080 and 5090 at 225 W and the 4090 at 250 W,
    while POWER_HIGH_MAX and POWER_4090_MAX in driver.c are both POWER_CEILING_DEV,
    which is 350 W. The GUI never had the problem because it asks the driver --
    Driver.ProfileRange builds its window from the ceiling the driver reports.

    The effect of the disagreement was quiet and badly misleading. `set 5090 250`
    was rejected, so the value stayed at whatever had been set before, and a run
    that was meant to measure 250 W measured 225 W instead and looked like a plateau
    that does not exist. Nothing in the output said the request had been dropped.

    So the range now comes from the driver, which is the authority, and the numbers
    it returns are printed both in the error message and in status. The driver still
    validates the target itself; this is only so the CLI can refuse early and say why.
*/
struct PowerRange
{
    ULONG Lo, Hi, Ceiling, Session;
    bool FromDriver;
};

static PowerRange QueryRange(HANDLE h)
{
    PowerRange r{ 100000u, 350000u, 350000u, 350000u, false };

    NVPWR_STATUS s{};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_NVPWR_STATUS, nullptr, 0, &s, sizeof(s), &got, nullptr) || got < sizeof(s))
    {
        return r;
    }

    r.FromDriver = true;
    r.Ceiling = (s.CeilingMax != 0 && s.CeilingMax != 0xFFFFFFFFu) ? s.CeilingMax : 350000u;
    r.Session = (s.SessionMax != 0 && s.SessionMax != 0xFFFFFFFFu) ? s.SessionMax : r.Ceiling;
    r.Lo = (s.SupportedMin != 0 && s.SupportedMin != 0xFFFFFFFFu) ? s.SupportedMin : 100000u;
    r.Hi = (s.SupportedMax != 0 && s.SupportedMax != 0xFFFFFFFFu) ? s.SupportedMax : r.Ceiling;
    return r;
}

static bool ShowStatus(HANDLE h)
{
    NVPWR_STATUS s{};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_NVPWR_STATUS, nullptr, 0, &s, sizeof(s), &got, nullptr) || got < sizeof(s)) {
        std::printf("STATUS failed: Win32=%lu bytes=%lu\n", GetLastError(), got);
        return false;
    }

    std::printf("State                : %s (%lu)\n", StateName(s.State), s.State);
    std::printf("Detail               : %lu\n", s.Detail);
    std::printf("Last NTSTATUS        : 0x%08lX\n", (unsigned long)s.LastNtStatus);
    std::printf("Last NVIDIA status   : 0x%08lX\n", s.LastNvStatus);
    std::printf("TimeDateStamp        : 0x%08lX\n", s.TimeDateStamp);
    std::printf("SizeOfImage          : 0x%08lX\n", s.SizeOfImage);
    std::printf("Flags init/elig/amt  : %u / %u / %u\n", s.RootInitialized, s.Eligibility, s.AmountActive);
    std::printf("Active profile       : %lu\n", s.ActiveProfile);
    PrintPower("OEM baseline", s.OemBaseline);
    PrintPower("F7 input (+3D14)", s.CtgpTarget);
    PrintPower("amount (+3D18)", s.PpabAmount);
    PrintPower("UPPER (+3D24)", s.UpperBoundary);
    PrintPower("predicted F7", s.PredictedF7);
    PrintPower("MAX effective", s.MaxEffective);
    PrintPower("Current effective", s.CurrentEffective);
    PrintPower("Current F7", s.CurrentF7Value);
    PrintPower("Applied target", s.AppliedTarget);

    // The window and the two ceilings, as the driver reports them. Without these the
    // only way to learn the real limit was to binary-search the client's own table.
    if (s.SupportedMax != 0 && s.SupportedMax != 0xFFFFFFFFu)
        std::printf("Supported window     : %lu..%lu W\n", s.SupportedMin / 1000, s.SupportedMax / 1000);
    if (s.CeilingMax != 0 && s.CeilingMax != 0xFFFFFFFFu)
        std::printf("Driver ceiling       : %lu W\n", s.CeilingMax / 1000);
    if (s.SessionMax != 0 && s.SessionMax != 0xFFFFFFFFu)
        std::printf("Session ceiling      : %lu W\n", s.SessionMax / 1000);
    return true;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2 || argc > 4) {
        std::printf("Usage: NvpwrCtl status | set <4050|4060|4070|4080|4090|5050|5060|5070|5070ti|5080|5090> <watts> | restore\n");
        return 2;
    }

    HANDLE h = CreateFileW(NVPWR_DEVICE_WIN32, GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("Cannot open \\\\.\\Nvpwr: Win32=%lu\n", GetLastError());
        return 3;
    }

    int rc = 0;
    if (_wcsicmp(argv[1], L"status") == 0 && argc == 2) {
        rc = ShowStatus(h) ? 0 : 4;
    } else if (_wcsicmp(argv[1], L"restore") == 0 && argc == 2) {
        DWORD got = 0;
        if (!DeviceIoControl(h, IOCTL_NVPWR_RESTORE, nullptr, 0, nullptr, 0, &got, nullptr)) {
            std::printf("Restore failed: Win32=%lu\n", GetLastError());
            rc = 5;
        }
        ShowStatus(h);
    } else if (_wcsicmp(argv[1], L"set") == 0 && argc == 4) {
        ULONG profile = ParseProfile(argv[2]);
        wchar_t* end = nullptr;
        unsigned long watts = wcstoul(argv[3], &end, 10);

        PowerRange range = QueryRange(h);
        ULONGLONG targetMw = (ULONGLONG)watts * 1000ull;

        bool valid = end && *end == L'\0' && profile != NvpwrProfileUnknown;
        if (valid) {
            valid = targetMw >= range.Lo && targetMw <= range.Hi && (watts % 5) == 0;
        }
        if (!valid) {
            std::printf("This profile accepts %lu..%lu W in 5 W steps.\n",
                        range.Lo / 1000, range.Hi / 1000);
            std::printf("The window comes from the driver%s, the same one the GUI offers.\n",
                        range.FromDriver ? " (SupportedMin/SupportedMax)" : "; status was unreadable, using defaults");
            CloseHandle(h);
            return 2;
        }

        NVPWR_SET_POWER req{};
        req.Version = NVPWR_SET_VERSION;
        req.TargetMilliwatts = (ULONG)targetMw;
        req.Profile = profile;
        /*
            The session ceiling, which is what lets a target above the OEM rail through.
            The driver clamps it to its own POWER_CEILING_DEV. Leaving it at zero means
            "use the driver default", which also works; sending the reported ceiling keeps
            this on exactly the path the GUI takes.
        */
        req.MaxMilliwatts = range.Ceiling;
        req.Reserved = 0;

        DWORD got = 0;
        if (!DeviceIoControl(h, IOCTL_NVPWR_SET_POWER, &req, sizeof(req), nullptr, 0, &got, nullptr)) {
            std::printf("Set failed: Win32=%lu\n", GetLastError());
            rc = 6;
        }
        ShowStatus(h);
    } else {
        std::printf("Usage: NvpwrCtl status | set <4050|4060|4070|4080|4090|5050|5060|5070|5070ti|5080|5090> <watts> | restore\n");
        rc = 2;
    }

    CloseHandle(h);
    return rc;
}
