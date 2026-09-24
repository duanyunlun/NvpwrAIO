/*
    nvpwr_telemetry.cpp — live environment sampling (read-only).
*/

#include "nvpwr_telemetry.h"

#include <dxgi.h>
#include <sstream>
#include <iomanip>
#include <vector>

#pragma comment(lib, "Dxgi.lib")

namespace nvpwr {

namespace {

/* ------------------------------------------------------------------ */
/* NVML (the library nvidia-smi itself uses)                           */
/* ------------------------------------------------------------------ */

typedef int nvmlReturn_t;
typedef void* nvmlDevice_t;
const nvmlReturn_t NVML_SUCCESS = 0;

/* Enum values are fixed by the NVML ABI; the header is not redistributed here so
   they are spelled out with their names in comments. */
const int NVML_TEMPERATURE_GPU = 0;
/* Sensor type 1 is MEMORY, 9 is MEMORY_JUNCTION. Both are probed best-effort:
   on the reference RTX 5090 Laptop / 616.92 driver they return NOT_SUPPORTED,
   so the UI must treat absence as normal rather than as a failure. */
const int NVML_TEMPERATURE_MEMORY = 1;
const int NVML_TEMPERATURE_MEMJUNCTION = 9;

/* nvmlTemperatureThresholds_t */
const int NVML_TEMP_THRESHOLD_SHUTDOWN     = 0;
const int NVML_TEMP_THRESHOLD_SLOWDOWN     = 1;
const int NVML_TEMP_THRESHOLD_GPU_MAX      = 3;

const int NVML_CLOCK_GRAPHICS = 0;
const int NVML_CLOCK_SM       = 1;
const int NVML_CLOCK_MEM      = 2;
const int NVML_CLOCK_VIDEO    = 3;

const int NVML_POWER_MANAGEMENT_ENABLED = 0;

/* NVML field ids read through nvmlDeviceGetFieldValues. */
const unsigned int NVML_FI_DEV_POWER_INSTANT = 94;
const unsigned int NVML_FI_DEV_POWER_AVERAGE = 95;
const unsigned int NVML_FI_DEV_TOTAL_ENERGY_CONSUMPTION = 84;

typedef int   (__cdecl *nvmlInit_t)(void);
typedef int   (__cdecl *nvmlShutdown_t)(void);
typedef int   (__cdecl *nvmlDeviceGetCount_t)(unsigned int*);
typedef int   (__cdecl *nvmlDeviceGetHandleByIndex_t)(unsigned int, nvmlDevice_t*);
typedef int   (__cdecl *nvmlDeviceGetName_t)(nvmlDevice_t, char*, unsigned int);
typedef int   (__cdecl *nvmlDeviceGetTemperature_t)(nvmlDevice_t, int, unsigned int*);
typedef int   (__cdecl *nvmlDeviceGetTemperatureThreshold_t)(nvmlDevice_t, int, unsigned int*);
typedef int   (__cdecl *nvmlDeviceGetClockInfo_t)(nvmlDevice_t, int, unsigned int*);
typedef int   (__cdecl *nvmlDeviceGetPowerUsage_t)(nvmlDevice_t, unsigned int*);
typedef int   (__cdecl *nvmlDeviceGetEnforcedPowerLimit_t)(nvmlDevice_t, unsigned int*);
typedef int   (__cdecl *nvmlDeviceGetUtilizationRates_t)(nvmlDevice_t, void*);
typedef int   (__cdecl *nvmlDeviceGetMemoryInfo_t)(nvmlDevice_t, void*);
typedef int   (__cdecl *nvmlDeviceGetFanSpeed_t)(nvmlDevice_t, unsigned int*);
typedef int   (__cdecl *nvmlDeviceGetCurrentClocksThrottleReasons_t)(nvmlDevice_t, unsigned long long*);
typedef int   (__cdecl *nvmlDeviceGetDriverVersion_t)(char*, unsigned int);
typedef int   (__cdecl *nvmlDeviceGetVbiosVersion_t)(nvmlDevice_t, char*, unsigned int);
typedef int   (__cdecl *nvmlDeviceGetPerformanceState_t)(nvmlDevice_t, int*);

/* Struct layouts as defined by the NVML ABI. */
struct nvmlUtilization_t { unsigned int gpu; unsigned int memory; };
struct nvmlMemory_t { unsigned long long total; unsigned long long free; unsigned long long used; };

struct NvmlApi {
    HMODULE module = nullptr;
    nvmlInit_t init = nullptr;
    nvmlShutdown_t shutdown = nullptr;
    nvmlDeviceGetCount_t getCount = nullptr;
    nvmlDeviceGetHandleByIndex_t getHandle = nullptr;
    nvmlDeviceGetName_t getName = nullptr;
    nvmlDeviceGetTemperature_t getTemp = nullptr;
    nvmlDeviceGetTemperatureThreshold_t getTempThreshold = nullptr;
    nvmlDeviceGetClockInfo_t getClock = nullptr;
    nvmlDeviceGetPowerUsage_t getPower = nullptr;
    nvmlDeviceGetEnforcedPowerLimit_t getLimit = nullptr;
    nvmlDeviceGetUtilizationRates_t getUtil = nullptr;
    nvmlDeviceGetMemoryInfo_t getMem = nullptr;
    nvmlDeviceGetFanSpeed_t getFan = nullptr;
    nvmlDeviceGetCurrentClocksThrottleReasons_t getThrottle = nullptr;
    nvmlDeviceGetDriverVersion_t getDriver = nullptr;
    nvmlDeviceGetVbiosVersion_t getVbios = nullptr;
    nvmlDeviceGetPerformanceState_t getPstate = nullptr;
    bool ready = false;

    ~NvmlApi() {
        if (ready && shutdown) shutdown();
        if (module) FreeLibrary(module);
    }

    bool Open() {
        if (ready) return true;
        module = LoadLibraryW(L"nvml.dll");
        if (!module) {
            /* A driver install sometimes leaves NVML only under System32. */
            module = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
            if (!module) return false;
        }
        #define BIND(name, field) field = reinterpret_cast<name##_t>(GetProcAddress(module, #name))
        BIND(nvmlInit, init);
        BIND(nvmlShutdown, shutdown);
        BIND(nvmlDeviceGetCount, getCount);
        BIND(nvmlDeviceGetHandleByIndex, getHandle);
        BIND(nvmlDeviceGetName, getName);
        BIND(nvmlDeviceGetTemperature, getTemp);
        BIND(nvmlDeviceGetTemperatureThreshold, getTempThreshold);
        BIND(nvmlDeviceGetClockInfo, getClock);
        BIND(nvmlDeviceGetPowerUsage, getPower);
        BIND(nvmlDeviceGetEnforcedPowerLimit, getLimit);
        BIND(nvmlDeviceGetUtilizationRates, getUtil);
        BIND(nvmlDeviceGetMemoryInfo, getMem);
        BIND(nvmlDeviceGetFanSpeed, getFan);
        BIND(nvmlDeviceGetCurrentClocksThrottleReasons, getThrottle);
        BIND(nvmlDeviceGetDriverVersion, getDriver);
        BIND(nvmlDeviceGetVbiosVersion, getVbios);
        BIND(nvmlDeviceGetPerformanceState, getPstate);
        #undef BIND

        if (!init || !getCount || !getHandle || init() != NVML_SUCCESS) return false;
        ready = true;
        return true;
    }
};

std::wstring WidenAscii(const char* s) {
    if (!s) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring w((size_t)(n - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

/* ------------------------------------------------------------------ */
/* nvidia-smi fallback                                                 */
/* ------------------------------------------------------------------ */

bool RunNvidiaSmi(const std::wstring& args, std::wstring& output) {
    output.clear();

    wchar_t sys[MAX_PATH]{};
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return false;
    std::wstring exe = std::wstring(sys) + L"\\nvidia-smi.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    std::wstring cmd = L"\"" + exe + L"\" " + args;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return false;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) { CloseHandle(readEnd); return false; }

    /* The child writes bytes, not wide characters, so accumulate as narrow and
       convert once at the end. nvidia-smi output is ASCII for the CSV queries
       used here, so CP_ACP is the right code page. */
    std::string raw;
    char buf[512];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0)
        raw.append(buf, got);

    WaitForSingleObject(pi.hProcess, 4000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readEnd);

    if (!raw.empty()) {
        int n = MultiByteToWideChar(CP_ACP, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
        std::wstring wide((size_t)n, L'\0');
        MultiByteToWideChar(CP_ACP, 0, raw.c_str(), (int)raw.size(), &wide[0], n);
        output = wide;
    }

    return exitCode == 0 && !output.empty();
}

std::vector<std::wstring> SplitCsv(const std::wstring& line) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : line) {
        if (c == L',') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    for (std::wstring& s : out) {
        size_t b = s.find_first_not_of(L" \t\r\n");
        size_t e = s.find_last_not_of(L" \t\r\n");
        s = (b == std::wstring::npos) ? L"" : s.substr(b, e - b + 1);
    }
    return out;
}

bool ParseDouble(const std::wstring& s, double& out) {
    if (s.empty() || s == L"N/A" || s == L"[N/A]" || s == L"[Not Supported]") return false;
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    out = v;
    return true;
}

/* ------------------------------------------------------------------ */
/* display topology (MUX / discrete-direct state)                      */
/* ------------------------------------------------------------------ */

/*
    A laptop is in discrete-direct state when the NVIDIA adapter is currently the
    one driving a display output. In hybrid mode the iGPU owns the panel and the
    dGPU has no attached output. This is a reliable, driver-agnostic signal and
    needs no vendor extension.
*/
void SampleDisplayTopology(EnvStatus& out) {
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))) || !factory)
        return;

    unsigned int adapters = 0;
    bool nvidiaHasOutput = false;
    bool someoneHasOutput = false;
    bool sawNvidia = false;

    for (UINT i = 0;; ++i) {
        IDXGIAdapter* adapter = nullptr;
        if (factory->EnumAdapters(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;
        ++adapters;

        DXGI_ADAPTER_DESC desc{};
        bool isNvidia = false;
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            isNvidia = (desc.VendorId == 0x10DE);
            if (isNvidia && out.gpuName.empty()) out.gpuName = desc.Description;
        }
        if (isNvidia) sawNvidia = true;

        for (UINT o = 0;; ++o) {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
            if (!output) break;
            someoneHasOutput = true;
            if (isNvidia) nvidiaHasOutput = true;
            output->Release();
            break;   /* one output is enough to classify this adapter */
        }
        adapter->Release();
    }
    factory->Release();

    out.adapterCount = adapters;
    if (sawNvidia) {
        out.discreteDirectKnown = true;
        out.discreteDirect = nvidiaHasOutput || !someoneHasOutput;
    }
}

/* ------------------------------------------------------------------ */
/* platform power                                                      */
/* ------------------------------------------------------------------ */

void SamplePlatform(EnvStatus& out) {
    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps)) {
        if (sps.ACLineStatus == 0 || sps.ACLineStatus == 1) {
            out.acKnown = true;
            out.onAcPower = (sps.ACLineStatus == 1);
        }
        if (sps.BatteryLifePercent != 255) {
            out.batteryPctKnown = true;
            out.batteryPct = sps.BatteryLifePercent;
        }
    }
}

} /* anonymous namespace */

bool SampleEnvironment(EnvStatus& out) {
    out = EnvStatus{};

    SamplePlatform(out);
    SampleDisplayTopology(out);

    /* --- preferred path: NVML --- */
    static NvmlApi nvml;
    bool gotReadings = false;

    if (nvml.Open()) {
        out.source = L"NVML";
        unsigned int count = 0;
        if (nvml.getCount && nvml.getCount(&count) == NVML_SUCCESS && count > 0) {
            nvmlDevice_t dev = nullptr;
            /* Index 0 is the first NVIDIA device; on a single-dGPU laptop that is
               the discrete GPU, which is the one being tuned. */
            if (nvml.getHandle && nvml.getHandle(0, &dev) == NVML_SUCCESS && dev) {
                char name[128]{};
                if (nvml.getName && nvml.getName(dev, name, sizeof(name)) == NVML_SUCCESS) {
                    std::wstring n = WidenAscii(name);
                    if (!n.empty()) out.gpuName = n;
                }
                char drv[80]{};
                if (nvml.getDriver && nvml.getDriver(drv, sizeof(drv)) == NVML_SUCCESS) {
                    std::wstring d = WidenAscii(drv);
                    if (!d.empty()) out.driverVersion = d;
                }
                char vb[80]{};
                if (nvml.getVbios && nvml.getVbios(dev, vb, sizeof(vb)) == NVML_SUCCESS) {
                    std::wstring v = WidenAscii(vb);
                    if (!v.empty()) out.vbiosVersion = v;
                }

                unsigned int u = 0;
                if (nvml.getPower && nvml.getPower(dev, &u) == NVML_SUCCESS) {
                    out.hasPowerDraw = true; out.powerDrawW = u / 1000.0;
                }
                if (nvml.getLimit && nvml.getLimit(dev, &u) == NVML_SUCCESS) {
                    out.hasPowerLimit = true; out.enforcedLimitW = u / 1000.0;
                }
                if (nvml.getClock && nvml.getClock(dev, NVML_CLOCK_GRAPHICS, &u) == NVML_SUCCESS) {
                    out.hasCoreClock = true; out.coreClockMhz = u;
                }
                if (nvml.getClock && nvml.getClock(dev, NVML_CLOCK_MEM, &u) == NVML_SUCCESS) {
                    out.hasMemoryClock = true; out.memoryClockMhz = u;
                }
                if (nvml.getTemp && nvml.getTemp(dev, NVML_TEMPERATURE_GPU, &u) == NVML_SUCCESS) {
                    out.hasTemp = true; out.tempC = u;
                }

                /*
                    Thermal limits. These are what substitute for a hotspot
                    reading: they answer "how close am I to throttling", which is
                    the question that decides whether a higher power limit is
                    usable at all.
                */
                if (nvml.getTempThreshold) {
                    unsigned int t = 0;
                    if (nvml.getTempThreshold(dev, NVML_TEMP_THRESHOLD_SLOWDOWN, &t) == NVML_SUCCESS && t > 0) {
                        out.hasSpeedThreshold = true; out.speedThresholdC = t;
                    }
                    t = 0;
                    if (nvml.getTempThreshold(dev, NVML_TEMP_THRESHOLD_SHUTDOWN, &t) == NVML_SUCCESS && t > 0) {
                        out.hasShutdownThreshold = true; out.shutdownThresholdC = t;
                    }
                    t = 0;
                    if (nvml.getTempThreshold(dev, NVML_TEMP_THRESHOLD_GPU_MAX, &t) == NVML_SUCCESS && t > 0) {
                        out.hasMaxOperatingTemp = true; out.maxOperatingC = t;
                    }
                }

                /*
                    Hotspot / memory-junction: probe, then record WHY it is
                    absent. On the reference RTX 5090 Laptop every sensor except
                    the edge sensor returns NOT_SUPPORTED, so the honest answer is
                    "this GPU does not expose it via NVML" rather than a blank
                    field that looks like a bug.
                */
                if (nvml.getTemp) {
                    unsigned int h = 0;
                    if (nvml.getTemp(dev, NVML_TEMPERATURE_MEMJUNCTION, &h) == NVML_SUCCESS && h > 0) {
                        out.hasHotspot = true; out.hotspotC = h;
                    } else {
                        unsigned int m = 0;
                        if (nvml.getTemp(dev, NVML_TEMPERATURE_MEMORY, &m) == NVML_SUCCESS && m > 0) {
                            out.hasHotspot = true; out.hotspotC = m;
                        }
                    }
                    if (!out.hasHotspot) {
                        out.unavailableNote =
                            L"NVML exposes only the edge temperature sensor on this GPU; "
                            L"the hotspot and memory-junction sensors return NOT_SUPPORTED "
                            L"for every device sensor type. Tools that show hotspot use "
                            L"NVIDIA's private ADC interface, which this build does not "
                            L"bind (see nvapi_power_policies.h).";
                    }
                }
                nvmlUtilization_t util{};
                if (nvml.getUtil && nvml.getUtil(dev, &util) == NVML_SUCCESS) {
                    out.hasUtilization = true; out.utilizationPct = util.gpu;
                }
                nvmlMemory_t mem{};
                if (nvml.getMem && nvml.getMem(dev, &mem) == NVML_SUCCESS) {
                    out.hasMemoryTotal = true; out.memoryTotalMb = mem.total / (1024.0 * 1024.0);
                    out.hasMemoryUsed = true;  out.memoryUsedMb  = mem.used  / (1024.0 * 1024.0);
                }
                if (nvml.getFan && nvml.getFan(dev, &u) == NVML_SUCCESS) {
                    out.hasFanPct = true; out.fanPct = u;
                }
                unsigned long long reasons = 0;
                if (nvml.getThrottle && nvml.getThrottle(dev, &reasons) == NVML_SUCCESS) {
                    out.hasThrottle = true; out.throttleBits = reasons;
                }
                int ps = 0;
                if (nvml.getPstate && nvml.getPstate(dev, &ps) == NVML_SUCCESS) {
                    out.hasPstate = true; out.pstate = (unsigned int)ps;
                }
                gotReadings = out.hasPowerDraw || out.hasCoreClock || out.hasTemp;
            }
        }
    }

    /* --- fallback: nvidia-smi, which is present on every driver install --- */
    if (!gotReadings) {
        std::wstring csv;
        if (RunNvidiaSmi(L"--query-gpu=name,driver_version,power.draw,enforced.power.limit,"
                         L"clocks.current.graphics,clocks.current.memory,temperature.gpu,"
                         L"utilization.gpu,memory.used,memory.total,fan.speed,pstate,"
                         L"clocks_throttle_reasons.active,temperature.gpu.tlimit --format=csv,noheader,nounits", csv)) {
            out.source = L"nvidia-smi";
            std::wistringstream in(csv);
            std::wstring line;
            if (std::getline(in, line)) {
                std::vector<std::wstring> f = SplitCsv(line);
                if (f.size() >= 13) {
                    if (!f[0].empty()) out.gpuName = f[0];
                    out.driverVersion = f[1];
                    double d = 0;
                    if (ParseDouble(f[2], d))  { out.hasPowerDraw = true;  out.powerDrawW = d; }
                    if (ParseDouble(f[3], d))  { out.hasPowerLimit = true; out.enforcedLimitW = d; }
                    if (ParseDouble(f[4], d))  { out.hasCoreClock = true;  out.coreClockMhz = d; }
                    if (ParseDouble(f[5], d))  { out.hasMemoryClock = true;out.memoryClockMhz = d; }
                    if (ParseDouble(f[6], d))  { out.hasTemp = true;       out.tempC = d; }
                    if (ParseDouble(f[7], d))  { out.hasUtilization = true;out.utilizationPct = d; }
                    if (ParseDouble(f[8], d))  { out.hasMemoryUsed = true; out.memoryUsedMb = d; }
                    if (ParseDouble(f[9], d))  { out.hasMemoryTotal = true;out.memoryTotalMb = d; }
                    if (ParseDouble(f[10], d)) { out.hasFanPct = true;     out.fanPct = d; }
                    if (!f[11].empty()) {
                        out.hasPstate = true;
                        if (f[11].size() >= 2 && (f[11][0] == L'P' || f[11][0] == L'p'))
                            out.pstate = (unsigned int)wcstoul(f[11].c_str() + 1, nullptr, 10);
                    }
                    /* nvidia-smi prints the throttle mask as 0x.... */
                    unsigned long long bits = wcstoull(f[12].c_str(), nullptr, 16);
                    out.hasThrottle = true;
                    out.throttleBits = bits;
                    /* T.Limit: the driver's own thermal limit for this moment.
                       It moves with operating conditions, which is exactly why it
                       is more useful than a fixed wall number. */
                    if (f.size() >= 14 && ParseDouble(f[13], d) && d > 0) {
                        out.hasSpeedThreshold = true;
                        out.speedThresholdC = d;
                    }
                }
            }
        } else {
            out.error = L"neither NVML nor nvidia-smi could provide readings";
        }
    }

    /* Thermal headroom line, so the UI never has to compute it. */
    if (!out.hasSpeedThreshold && out.hasShutdownThreshold) {
        /* No software slowdown threshold: fall back to the shutdown limit. */
        out.hasSpeedThreshold = true;
        out.speedThresholdC = out.shutdownThresholdC;
    }

    return out.AnyReading();
}

std::wstring DescribeThrottle(unsigned long long bits) {
    if (bits == 0) return L"—";

    struct Entry { unsigned long long bit; const wchar_t* name; };
    static const Entry kEntries[] = {
        { ThrottleGpuIdle,        L"GPU Idle" },
        { ThrottleApplicationsClk,L"App Clock Setting" },
        { ThrottleSwPowerCap,     L"SW Power Cap" },
        { ThrottleHwSlowdown,     L"HW Slowdown" },
        { ThrottleSyncBoost,      L"Sync Boost" },
        { ThrottleSwThermal,      L"SW Thermal" },
        { ThrottleHwThermal,      L"HW Thermal" },
        { ThrottleHwPowerBrake,   L"HW Power Brake" },
        { ThrottleDisplayClk,     L"Display Clock" },
        { ThrottleReliability,    L"Reliability Voltage" },
        { ThrottleBoardLimit,     L"Board Limit" },
        { ThrottleLowUtilization, L"Low Utilization" },
    };
    std::wstring result;
    for (const Entry& e : kEntries) {
        if ((bits & e.bit) == 0) continue;
        if (!result.empty()) result += L" + ";
        result += e.name;
    }
    if (result.empty()) {
        wchar_t b[32]{};
        swprintf_s(b, L"0x%llX", bits);
        result = b;
    }
    return result;
}

std::wstring DescribeLimiter(const EnvStatus& s) {
    if (!s.hasThrottle) return L"";
    const unsigned long long b = s.throttleBits;

    if (b & ThrottleHwThermal || b & ThrottleSwThermal) {
        return L"THERMAL: the GPU is limited by temperature. Raising the power "
               L"limit will not increase performance until cooling improves.";
    }
    if (b & ThrottleSwPowerCap || b & ThrottleBoardLimit) {
        return L"POWER: the GPU is limited by its power ceiling. This is the case "
               L"where a higher limit can actually be consumed.";
    }
    if (b & ThrottleHwSlowdown || b & ThrottleHwPowerBrake) {
        return L"EXTERNAL: a hardware power brake is active (adapter or platform "
               L"limit). The GPU is being throttled below its own policy.";
    }
    if (b & ThrottleReliability) {
        return L"RELIABILITY: the voltage reliability limit is active.";
    }
    if (b & ThrottleGpuIdle || b & ThrottleLowUtilization) {
        return L"IDLE: no meaningful load, so no limit is engaged. Load the GPU to "
               L"measure the real limiter.";
    }
    return L"";
}

} /* namespace nvpwr */
