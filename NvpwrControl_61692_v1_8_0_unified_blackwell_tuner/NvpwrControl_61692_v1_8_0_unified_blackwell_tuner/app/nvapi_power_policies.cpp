/*
    nvapi_power_policies.cpp — rail voltage-limit control implementation.

    Every code path here is written so that an incomplete or wrong interface
    contract degrades to "voltage control unavailable", never to a blind write.
    See the contract block in nvapi_power_policies.h for what must be filled in.
*/

#include "nvapi_power_policies.h"

#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace nvpwr {

/* Zeroed on purpose — see the contract note in the header. */
NvVoltageContract g_contract{};

namespace {

/* ---- NVAPI entry points (public ids, published in the NVAPI SDK) ---- */
constexpr unsigned int NVAPI_INITIALIZE          = 0x0150E828u;
constexpr unsigned int NVAPI_ENUM_PHYSICAL_GPUS  = 0xE5AC921Fu;
constexpr unsigned int NVAPI_UNLOAD              = 0xD22BDD7Eu;
constexpr long         NVAPI_OK                  = 0;

/* Plausibility envelope for any rail voltage the driver may report. Used to
   reject a "successful" read that actually landed on unrelated memory. */
constexpr long long kVMinPlausibleUv = 200000;    /* 0.20 V */
constexpr long long kVMaxPlausibleUv = 2500000;   /* 2.50 V */

using QueryInterfaceFn = void* (__cdecl*)(unsigned int);
using InitFn           = long  (__cdecl*)();
using EnumGpuFn        = long  (__cdecl*)(void**, unsigned int*);
using BufferFn         = long  (__cdecl*)(void*, void*);

struct NvSession {
    HMODULE module = nullptr;
    QueryInterfaceFn qi = nullptr;
    InitFn unload = nullptr;
    void* gpu = nullptr;

    ~NvSession() {
        if (unload) unload();
        if (module) FreeLibrary(module);
    }

    bool Open(std::wstring& error) {
        module = LoadLibraryW(L"nvapi64.dll");
        if (!module) { error = L"nvapi64.dll could not be loaded"; return false; }
        qi = reinterpret_cast<QueryInterfaceFn>(
            GetProcAddress(module, "nvapi_QueryInterface"));
        if (!qi) { error = L"nvapi_QueryInterface is missing"; return false; }

        auto init = reinterpret_cast<InitFn>(qi(NVAPI_INITIALIZE));
        auto enumGpu = reinterpret_cast<EnumGpuFn>(qi(NVAPI_ENUM_PHYSICAL_GPUS));
        unload = reinterpret_cast<InitFn>(qi(NVAPI_UNLOAD));
        if (!init || !enumGpu) {
            error = L"NvAPI initialize/enumeration interfaces are missing";
            return false;
        }
        if (init() != NVAPI_OK) { error = L"NvAPI_Initialize failed"; return false; }

        void* gpus[64]{};
        unsigned int count = 0;
        if (enumGpu(gpus, &count) != NVAPI_OK || count == 0) {
            error = L"NvAPI_EnumPhysicalGPUs returned no GPU";
            return false;
        }
        gpu = gpus[0];
        return true;
    }

    BufferFn Fn(unsigned int id) const {
        if (!qi || id == 0u) return nullptr;   /* 0 = not bound, never a call */
        return reinterpret_cast<BufferFn>(qi(id));
    }
};

/* ---- little-endian field helpers over a byte buffer ---- */
unsigned int  GetU32(const std::vector<unsigned char>& b, size_t off) {
    unsigned int v = 0;
    if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
    return v;
}
long long GetI64(const std::vector<unsigned char>& b, size_t off) {
    long long v = 0;
    if (off + 8 <= b.size()) std::memcpy(&v, b.data() + off, 8);
    return v;
}
void PutU32(std::vector<unsigned char>& b, size_t off, unsigned int v) {
    if (off + 4 <= b.size()) std::memcpy(b.data() + off, &v, 4);
}
void PutI64(std::vector<unsigned char>& b, size_t off, long long v) {
    if (off + 8 <= b.size()) std::memcpy(b.data() + off, &v, 8);
}

bool PlausibleUv(long long uv) {
    return uv >= kVMinPlausibleUv && uv <= kVMaxPlausibleUv;
}

/*
    Session baseline. Captured independently per subsystem on the first
    mutation, exactly as nvapi_tuner.cpp does for clocks: a user may adjust
    NVVDD first and MSVDD later, and a single one-shot flag would lose the
    second subsystem's true pre-tuning state.
*/
struct Baseline {
    bool haveNvvdd = false; std::vector<unsigned char> nvvdd;
    bool haveMsvdd = false; std::vector<unsigned char> msvdd;
    bool haveDemand = false; std::vector<unsigned char> demand;

    bool Any() const { return haveNvvdd || haveMsvdd || haveDemand; }
    void Clear() { *this = Baseline{}; }
};
Baseline g_baseline;

/* ---- one rail ---- */

struct RailIo {
    bool haveInfo = false;
    bool haveStatus = false;
    bool canSet = false;

    long long deviceMinUv = 0;
    long long deviceMaxUv = 0;
    long long deviceStepUv = 0;

    long long vminUv = 0, relUv = 0, altUv = 0, ovUv = 0;
    long long effMinUv = 0, effMaxUv = 0;

    std::vector<unsigned char> statusBuffer;
};

/*
    Reads and validates one rail.

    Validation order matters: bind -> call -> check version word -> check
    declared size -> parse through the contract's field map -> sanity-check the
    parsed values. Any failure leaves the rail unavailable.
*/
bool ReadRail(NvSession& s, const NvVoltageContract& c,
              unsigned int idGet, unsigned int idSet, unsigned int expectedVer,
              unsigned int expectedSize, const RailFieldMap& map,
              RailIo& out, std::wstring& why)
{
    if (!map.IsEstablished()) {
        why = L"rail field layout is not established in the interface contract";
        return false;
    }
    auto get = s.Fn(idGet);
    if (!get) {
        why = L"rail GET interface is not bound (contract id is 0)";
        return false;
    }
    auto set = s.Fn(idSet);
    out.canSet = (set != nullptr);

    if (expectedSize == 0u) {
        why = L"rail buffer size is not declared in the interface contract";
        return false;
    }

    std::vector<unsigned char> buf(expectedSize, 0);
    PutU32(buf, 0, expectedVer);
    if (get(s.gpu, buf.data()) != NVAPI_OK) {
        why = L"rail GET call failed";
        return false;
    }
    /* The driver echoes the structure version it filled in. A mismatch means we
       are not talking to the structure we think we are. */
    if (expectedVer != 0u && GetU32(buf, 0) != expectedVer) {
        why = L"rail GET version word does not match the contract";
        return false;
    }

    out.deviceMinUv = GetI64(buf, map.offsetDeviceMinUv);
    out.deviceMaxUv = GetI64(buf, map.offsetDeviceMaxUv);
    out.deviceStepUv = (map.offsetDeviceStepUv != 0u) ? GetI64(buf, map.offsetDeviceStepUv) : 5000;

    /* Self-consistency of the capability envelope. This is the check that
       catches a wrong field map: a misparsed range almost never satisfies
       min < max, a sane span and a sane step. */
    if (!PlausibleUv(out.deviceMinUv) || !PlausibleUv(out.deviceMaxUv) ||
        out.deviceMinUv >= out.deviceMaxUv || out.deviceStepUv <= 0 ||
        out.deviceStepUv > 100000) {
        why = L"rail device range failed the plausibility check";
        return false;
    }

    out.vminUv = GetI64(buf, map.offsetVminUv);
    out.relUv  = GetI64(buf, map.offsetRelUv);
    out.altUv  = (map.offsetAltUv != 0u) ? GetI64(buf, map.offsetAltUv) : 0;
    out.ovUv   = (map.offsetOvUv  != 0u) ? GetI64(buf, map.offsetOvUv)  : 0;
    out.effMinUv = (map.offsetEffMinUv != 0u) ? GetI64(buf, map.offsetEffMinUv) : 0;
    out.effMaxUv = (map.offsetEffMaxUv != 0u) ? GetI64(buf, map.offsetEffMaxUv) : 0;

    /* Offsets are small deltas; anything else means the map is wrong. */
    const long long kMaxOffsetUv = 500000;   /* 500 mV */
    if (std::llabs(out.vminUv) > kMaxOffsetUv || std::llabs(out.relUv) > kMaxOffsetUv ||
        std::llabs(out.altUv) > kMaxOffsetUv || std::llabs(out.ovUv) > kMaxOffsetUv) {
        why = L"rail offset fields failed the plausibility check";
        return false;
    }

    out.statusBuffer = buf;
    out.haveStatus = true;
    out.haveInfo = true;
    return true;
}

void CopyRailToState(const RailIo& io, bool writable, RailSupport& out) {
    out.available = io.haveStatus;
    out.writable = writable;
    out.deviceMinUv = io.deviceMinUv;
    out.deviceMaxUv = io.deviceMaxUv;
    out.deviceStepUv = io.deviceStepUv;
    out.vminUv = io.vminUv;
    out.relUv = io.relUv;
    out.altUv = io.altUv;
    out.ovUv = io.ovUv;
    /* The editable envelope is the device range expressed as an offset from the
       current applied value: the driver accepts offsets that keep the resulting
       limit inside the device range. Recomputed on every read so it tracks the
       driver's own moving baselines instead of being cached. */
    out.offsetMinUv = io.deviceMinUv;
    out.offsetMaxUv = io.deviceMaxUv;
    out.effectiveMinUv = io.effMinUv;
    out.effectiveMaxUv = io.effMaxUv;
}

/*
    Writes one rail's four offsets back through the SET interface, then re-reads
    and compares field by field. On any mismatch the rail is reverted from the
    caller's captured buffer.
*/
bool WriteRail(NvSession& s, const NvVoltageContract& c,
               unsigned int idGet, unsigned int idSet, unsigned int expectedVer,
               unsigned int expectedSize, const RailFieldMap& map,
               const RailOffsetRequest& req, RailIo& out, std::wstring& why)
{
    auto set = s.Fn(idSet);
    if (!set) { why = L"rail SET interface is not bound"; return false; }
    if (!out.haveStatus) { why = L"rail was not validated before the write"; return false; }

    std::vector<unsigned char> buf = out.statusBuffer;
    PutI64(buf, map.offsetVminUv, req.vminUv);
    PutI64(buf, map.offsetRelUv,  req.relUv);
    if (map.offsetAltUv != 0u) PutI64(buf, map.offsetAltUv, req.altUv);
    if (map.offsetOvUv  != 0u) PutI64(buf, map.offsetOvUv,  req.ovUv);
    PutU32(buf, 0, expectedVer);

    if (set(s.gpu, buf.data()) != NVAPI_OK) { why = L"rail SET call failed"; return false; }

    /* Fresh GET, not a replay of what we sent. */
    RailIo verify{};
    std::wstring verr;
    if (!ReadRail(s, c, idGet, idSet, expectedVer, expectedSize, map, verify, verr)) {
        why = L"rail readback failed: " + verr;
        return false;
    }
    if (verify.vminUv != req.vminUv || verify.relUv != req.relUv) {
        why = L"rail readback mismatch (offset not retained)";
        return false;
    }
    if (map.offsetAltUv != 0u && verify.altUv != req.altUv) {
        why = L"rail readback mismatch (ALT/OP offset not retained)";
        return false;
    }
    if (map.offsetOvUv != 0u && verify.ovUv != req.ovUv) {
        why = L"rail readback mismatch (OV offset not retained)";
        return false;
    }
    out = verify;
    return true;
}

/* ---- per-domain voltage demand ---- */

struct DemandIo {
    bool available = false;
    bool writable = false;
    long long minMv = 0, maxMv = 0;
    long long coreMv = 0, xbarMv = 0, sysMv = 0, videoMv = 0;
    std::vector<unsigned char> buffer;
};

/* Candidate layout for the demand block, validated by the same
   version+plausibility rules as the rails. */
constexpr size_t DEMAND_OFF_CORE  = 0x08;
constexpr size_t DEMAND_OFF_XBAR  = 0x0C;
constexpr size_t DEMAND_OFF_SYS   = 0x10;
constexpr size_t DEMAND_OFF_VIDEO = 0x14;
constexpr size_t DEMAND_OFF_MIN   = 0x18;
constexpr size_t DEMAND_OFF_MAX   = 0x1C;

bool ReadDemand(NvSession& s, const NvVoltageContract& c, DemandIo& out, std::wstring& why) {
    auto get = s.Fn(c.idVoltageDemandGet);
    if (!get) { why = L"voltage-demand GET interface is not bound (contract id is 0)"; return false; }
    out.writable = (s.Fn(c.idVoltageDemandSet) != nullptr);
    if (c.sizeDemand == 0u) { why = L"voltage-demand buffer size is not declared"; return false; }

    std::vector<unsigned char> buf(c.sizeDemand, 0);
    PutU32(buf, 0, c.verDemand);
    if (get(s.gpu, buf.data()) != NVAPI_OK) { why = L"voltage-demand GET failed"; return false; }
    if (c.verDemand != 0u && GetU32(buf, 0) != c.verDemand) {
        why = L"voltage-demand version word does not match the contract";
        return false;
    }
    if (buf.size() <= DEMAND_OFF_MAX + 4) { why = L"voltage-demand buffer is too small for its field map"; return false; }

    out.coreMv  = (long long)(int)GetU32(buf, DEMAND_OFF_CORE);
    out.xbarMv  = (long long)(int)GetU32(buf, DEMAND_OFF_XBAR);
    out.sysMv   = (long long)(int)GetU32(buf, DEMAND_OFF_SYS);
    out.videoMv = (long long)(int)GetU32(buf, DEMAND_OFF_VIDEO);
    out.minMv   = (long long)(int)GetU32(buf, DEMAND_OFF_MIN);
    out.maxMv   = (long long)(int)GetU32(buf, DEMAND_OFF_MAX);

    const long long kLimitMv = 500;
    if (out.minMv >= out.maxMv || std::llabs(out.minMv) > kLimitMv || std::llabs(out.maxMv) > kLimitMv) {
        why = L"voltage-demand range failed the plausibility check";
        return false;
    }
    if (std::llabs(out.coreMv) > kLimitMv || std::llabs(out.xbarMv) > kLimitMv ||
        std::llabs(out.sysMv) > kLimitMv || std::llabs(out.videoMv) > kLimitMv) {
        why = L"voltage-demand values failed the plausibility check";
        return false;
    }
    out.buffer = buf;
    out.available = true;
    return true;
}

void CopyDemandToState(const DemandIo& io, VoltageDemandState& out) {
    out.available = io.available;
    out.writable = io.writable;
    out.minMv = io.minMv;
    out.maxMv = io.maxMv;
    out.coreMv = io.coreMv;
    out.xbarMv = io.xbarMv;
    out.sysMv = io.sysMv;
    out.videoMv = io.videoMv;
}

} /* anonymous namespace */

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

bool ProbeNvapiVoltage(VoltageState& out) {
    out = VoltageState{};

    NvSession s;
    if (!s.Open(out.error)) return false;
    out.nvapiReady = true;

    std::wstring why;
    RailIo nvvdd{}, msvdd{};
    if (ReadRail(s, g_contract, g_contract.idNvvddGetStatus, g_contract.idNvvddSetStatus,
                 g_contract.verRailStatus, g_contract.sizeRailStatus,
                 g_contract.railStatusMap, nvvdd, why)) {
        CopyRailToState(nvvdd, nvvdd.canSet, out.nvvdd);
    } else if (!why.empty()) {
        out.error += L"NVDD: " + why + L"; ";
    }

    why.clear();
    if (ReadRail(s, g_contract, g_contract.idMsvddGetStatus, g_contract.idMsvddSetStatus,
                 g_contract.verRailStatus, g_contract.sizeRailStatus,
                 g_contract.railStatusMap, msvdd, why)) {
        CopyRailToState(msvdd, msvdd.canSet, out.msvdd);
    } else if (!why.empty()) {
        out.error += L"MSVDD: " + why + L"; ";
    }

    why.clear();
    DemandIo demand{};
    if (ReadDemand(s, g_contract, demand, why)) {
        CopyDemandToState(demand, out.demand);
    } else if (!why.empty()) {
        out.error += L"Demand: " + why + L"; ";
    }

    if (!out.AnyAvailable() && out.error.empty())
        out.error = L"no rail or voltage-demand interface is available";

    return out.AnyAvailable();
}

bool ApplyNvapiVoltage(const VoltageRequest& request, VoltageState& post,
                       std::wstring& error)
{
    error.clear();
    if (!request.AnySet()) { return ProbeNvapiVoltage(post); }

    NvSession s;
    if (!s.Open(error)) return false;

    /* ---- capture baselines before the first mutation of each subsystem ---- */
    std::wstring why;
    if (request.nvvdd.set && !g_baseline.haveNvvdd) {
        RailIo io{};
        if (ReadRail(s, g_contract, g_contract.idNvvddGetStatus, g_contract.idNvvddSetStatus,
                     g_contract.verRailStatus, g_contract.sizeRailStatus,
                     g_contract.railStatusMap, io, why)) {
            g_baseline.haveNvvdd = true;
            g_baseline.nvvdd = io.statusBuffer;
        }
    }
    if (request.msvdd.set && !g_baseline.haveMsvdd) {
        RailIo io{};
        if (ReadRail(s, g_contract, g_contract.idMsvddGetStatus, g_contract.idMsvddSetStatus,
                     g_contract.verRailStatus, g_contract.sizeRailStatus,
                     g_contract.railStatusMap, io, why)) {
            g_baseline.haveMsvdd = true;
            g_baseline.msvdd = io.statusBuffer;
        }
    }
    if (request.setDemand && !g_baseline.haveDemand) {
        DemandIo io{};
        if (ReadDemand(s, g_contract, io, why)) {
            g_baseline.haveDemand = true;
            g_baseline.demand = io.buffer;
        }
    }

    /* ---- NVVDD ---- */
    if (request.nvvdd.set) {
        RailIo io{};
        if (!ReadRail(s, g_contract, g_contract.idNvvddGetStatus, g_contract.idNvvddSetStatus,
                      g_contract.verRailStatus, g_contract.sizeRailStatus,
                      g_contract.railStatusMap, io, why)) {
            error = L"NVDD unavailable: " + why;
            ProbeNvapiVoltage(post);
            return false;
        }
        const RailOffsetRequest& r = request.nvvdd;
        if (r.vminUv < io.deviceMinUv || r.vminUv > io.deviceMaxUv ||
            r.relUv  < io.deviceMinUv || r.relUv  > io.deviceMaxUv) {
            error = L"NVDD offset is outside the driver-reported range";
            ProbeNvapiVoltage(post);
            return false;
        }
        if (!WriteRail(s, g_contract, g_contract.idNvvddGetStatus, g_contract.idNvvddSetStatus,
                       g_contract.verRailStatus, g_contract.sizeRailStatus,
                       g_contract.railStatusMap, r, io, why)) {
            error = L"NVDD write failed: " + why;
            ResetNvapiVoltage(post, why);
            return false;
        }
    }

    /* ---- MSVDD ---- */
    if (request.msvdd.set) {
        RailIo io{};
        if (!ReadRail(s, g_contract, g_contract.idMsvddGetStatus, g_contract.idMsvddSetStatus,
                      g_contract.verRailStatus, g_contract.sizeRailStatus,
                      g_contract.railStatusMap, io, why)) {
            error = L"MSVDD unavailable: " + why;
            ResetNvapiVoltage(post, why);
            return false;
        }
        const RailOffsetRequest& r = request.msvdd;
        if (r.vminUv < io.deviceMinUv || r.vminUv > io.deviceMaxUv ||
            r.relUv  < io.deviceMinUv || r.relUv  > io.deviceMaxUv) {
            error = L"MSVDD offset is outside the driver-reported range";
            ResetNvapiVoltage(post, why);
            return false;
        }
        if (!WriteRail(s, g_contract, g_contract.idMsvddGetStatus, g_contract.idMsvddSetStatus,
                       g_contract.verRailStatus, g_contract.sizeRailStatus,
                       g_contract.railStatusMap, r, io, why)) {
            error = L"MSVDD write failed: " + why;
            ResetNvapiVoltage(post, why);
            return false;
        }
    }

    /* ---- per-domain voltage demand ---- */
    if (request.setDemand) {
        DemandIo io{};
        if (!ReadDemand(s, g_contract, io, why)) {
            error = L"Voltage demand unavailable: " + why;
            ResetNvapiVoltage(post, why);
            return false;
        }
        if (!io.writable) {
            error = L"Voltage demand is read-only on this driver";
            ResetNvapiVoltage(post, why);
            return false;
        }
        if (request.coreMv < io.minMv || request.coreMv > io.maxMv ||
            request.xbarMv < io.minMv || request.xbarMv > io.maxMv ||
            request.sysMv  < io.minMv || request.sysMv  > io.maxMv ||
            request.videoMv< io.minMv || request.videoMv> io.maxMv) {
            error = L"Voltage demand is outside the driver-reported range";
            ResetNvapiVoltage(post, why);
            return false;
        }

        auto set = s.Fn(g_contract.idVoltageDemandSet);
        std::vector<unsigned char> buf = io.buffer;
        PutU32(buf, DEMAND_OFF_CORE,  (unsigned int)(int)request.coreMv);
        PutU32(buf, DEMAND_OFF_XBAR,  (unsigned int)(int)request.xbarMv);
        PutU32(buf, DEMAND_OFF_SYS,   (unsigned int)(int)request.sysMv);
        PutU32(buf, DEMAND_OFF_VIDEO, (unsigned int)(int)request.videoMv);
        PutU32(buf, 0, g_contract.verDemand);
        if (set(s.gpu, buf.data()) != NVAPI_OK) {
            error = L"Voltage demand SET failed";
            ResetNvapiVoltage(post, why);
            return false;
        }
        DemandIo verify{};
        if (!ReadDemand(s, g_contract, verify, why) ||
            verify.coreMv != request.coreMv || verify.xbarMv != request.xbarMv ||
            verify.sysMv != request.sysMv || verify.videoMv != request.videoMv) {
            error = L"Voltage demand readback mismatch";
            ResetNvapiVoltage(post, why);
            return false;
        }
    }

    ProbeNvapiVoltage(post);
    return true;
}

bool ResetNvapiVoltage(VoltageState& post, std::wstring& error) {
    error.clear();

    if (!g_baseline.Any()) {
        ProbeNvapiVoltage(post);
        return true;
    }

    NvSession s;
    if (!s.Open(error)) return false;

    bool ok = true;
    std::wstring all;

    if (g_baseline.haveNvvdd) {
        auto set = s.Fn(g_contract.idNvvddSetStatus);
        if (!set) { ok = false; all += L"NVDD restore: SET interface unavailable; "; }
        else {
            std::vector<unsigned char> buf = g_baseline.nvvdd;
            PutU32(buf, 0, g_contract.verRailStatus);
            if (set(s.gpu, buf.data()) != NVAPI_OK) {
                ok = false; all += L"NVDD restore: SET call failed; ";
            }
        }
    }
    if (g_baseline.haveMsvdd) {
        auto set = s.Fn(g_contract.idMsvddSetStatus);
        if (!set) { ok = false; all += L"MSVDD restore: SET interface unavailable; "; }
        else {
            std::vector<unsigned char> buf = g_baseline.msvdd;
            PutU32(buf, 0, g_contract.verRailStatus);
            if (set(s.gpu, buf.data()) != NVAPI_OK) {
                ok = false; all += L"MSVDD restore: SET call failed; ";
            }
        }
    }
    if (g_baseline.haveDemand) {
        auto set = s.Fn(g_contract.idVoltageDemandSet);
        if (!set) { ok = false; all += L"Demand restore: SET interface unavailable; "; }
        else {
            std::vector<unsigned char> buf = g_baseline.demand;
            PutU32(buf, 0, g_contract.verDemand);
            if (set(s.gpu, buf.data()) != NVAPI_OK) {
                ok = false; all += L"Demand restore: SET call failed; ";
            }
        }
    }

    if (ok) {
        /* Only clear the baseline when the restore actually succeeded; keeping
           it lets the user retry instead of losing the pre-tuning values. */
        g_baseline.Clear();
    } else {
        error = all;
    }

    ProbeNvapiVoltage(post);
    return ok;
}

bool HasVoltageBaseline() {
    return g_baseline.Any();
}

std::wstring VoltageState::MissingReason() const {
    if (AnyWritable()) return std::wstring();
    if (!nvapiReady) return L"NvAPI is not initialised; " + error;
    if (!AnyAvailable()) {
        return L"no voltage-limit interface is exposed by this GPU/driver. "
               L"The private interface ids and field layouts in "
               L"nvapi_power_policies.h have not been established for this "
               L"driver generation, so no write is attempted. Detail: " + error;
    }
    return L"voltage limits are readable but not writable on this driver. Detail: " + error;
}

std::wstring FormatNvapiVoltage(const VoltageState& s) {
    std::wstringstream ss;
    ss << L"\r\n=== Voltage limits (rail offsets) ===\r\n";
    if (!s.nvapiReady) {
        ss << L"NvAPI: unavailable";
        if (!s.error.empty()) ss << L" - " << s.error;
        ss << L"\r\n";
        return ss.str();
    }

    auto rail = [&](const wchar_t* name, const RailSupport& r) {
        ss << name << L": ";
        if (!r.available) { ss << L"N/A\r\n"; return; }
        ss << (r.writable ? L"WRITE" : L"read-only") << L"\r\n";
        ss << L"    offsets  VMIN " << (r.vminUv / 1000) << L" mV"
           << L" | REL " << (r.relUv / 1000) << L" mV"
           << L" | ALT/OP " << (r.altUv / 1000) << L" mV"
           << L" | OV " << (r.ovUv / 1000) << L" mV\r\n";
        ss << L"    device   " << (r.deviceMinUv / 1000) << L" .. "
           << (r.deviceMaxUv / 1000) << L" mV (step "
           << (r.deviceStepUv / 1000) << L" mV)\r\n";
    };
    rail(L"NVDD", s.nvvdd);
    rail(L"MSVDD", s.msvdd);

    ss << L"Voltage demand: ";
    if (!s.demand.available) ss << L"N/A\r\n";
    else {
        ss << (s.demand.writable ? L"WRITE" : L"read-only") << L"\r\n";
        ss << L"    Core " << s.demand.coreMv << L" | XBAR " << s.demand.xbarMv
           << L" | SYS " << s.demand.sysMv << L" | Video " << s.demand.videoMv
           << L" (mV), range " << s.demand.minMv << L" .. " << s.demand.maxMv << L"\r\n";
    }

    if (!s.error.empty()) ss << L"Notes: " << s.error << L"\r\n";
    return ss.str();
}

} /* namespace nvpwr */
