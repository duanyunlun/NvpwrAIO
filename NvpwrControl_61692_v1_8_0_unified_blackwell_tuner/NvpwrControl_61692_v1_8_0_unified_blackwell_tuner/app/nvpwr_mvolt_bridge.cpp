/*
    nvpwr_mvolt_bridge.cpp — companion mVolt+ integration.

    Launches mVolt+ and reads its output. This file performs no GPU I/O of its
    own: everything it knows comes from mVolt+'s own command line, which keeps
    the trust boundary simple — if mVolt+ is absent, this module reports absence
    and the rest of the program carries on.
*/

#include "nvpwr_mvolt_bridge.h"

#include <sstream>
#include <vector>
#include <cstdlib>
#include <cstdio>

namespace nvpwr {

namespace {

std::wstring g_lastCommandLine;

/* ------------------------------------------------------------------ */
/* process launch with captured stdout                                */
/* ------------------------------------------------------------------ */

/*
    Runs a command and returns its stdout.

    Uses an inherited anonymous pipe. The timeout exists because mVolt+ is a GUI
    subsystem binary: invoked with a query flag it prints and exits, but a future
    release that changes that behaviour must not hang a service at boot.
*/
bool RunCapture(const std::wstring& commandLine, std::wstring& output,
                DWORD timeoutMs, std::wstring& error)
{
    output.clear();
    error.clear();
    g_lastCommandLine = commandLine;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        error = L"CreatePipe failed";
        return false;
    }
    if (!SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readEnd); CloseHandle(writeEnd);
        error = L"SetHandleInformation failed";
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(L'\0');

    BOOL started = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr,
                                  TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);   /* our copy; the child owns the other end */
    if (!started) {
        CloseHandle(readEnd);
        error = L"CreateProcess failed (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    /* Drain while the child runs so a full pipe buffer cannot deadlock it. */
    std::string raw;
    char buf[1024];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        raw.append(buf, got);
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        error = L"the companion tool did not exit in time";
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readEnd);

    if (wait == WAIT_TIMEOUT) return false;

    if (!raw.empty()) {
        int n = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
        std::wstring wide((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), &wide[0], n);
        output = wide;
    }

    /* A non-zero exit with no output is still worth reporting, but mVolt+ returns
       non-zero on some query paths, so output presence is the stronger signal. */
    if (output.empty() && exitCode != 0) {
        error = L"the companion tool returned no output (exit " +
                std::to_wstring(exitCode) + L")";
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* a very small JSON reader                                           */
/* ------------------------------------------------------------------ */

/*
    Deliberately minimal: this only needs to pull numbers and strings out of the
    flat-ish object mVolt+ prints. It tolerates unknown keys and nested arrays,
    and returns "not found" rather than throwing, so a future mVolt+ that adds
    fields keeps working.
*/
class JsonView {
public:
    explicit JsonView(const std::wstring& text) : text_(text) {}

    /* Finds "key" anywhere in the text and returns the value that follows the
       colon. Searching by key rather than walking the tree keeps this robust
       against layout changes, at the cost of not distinguishing duplicate keys —
       acceptable here because mVolt+ emits unique, descriptive names. */
    bool FindRaw(const std::wstring& key, std::wstring& value) const {
        const std::wstring needle = L"\"" + key + L"\"";
        size_t pos = text_.find(needle);
        while (pos != std::wstring::npos) {
            size_t colon = text_.find(L':', pos + needle.size());
            if (colon == std::wstring::npos) return false;
            size_t i = colon + 1;
            while (i < text_.size() && (text_[i] == L' ' || text_[i] == L'\t' ||
                                        text_[i] == L'\r' || text_[i] == L'\n')) ++i;
            if (i >= text_.size()) return false;

            if (text_[i] == L'"') {
                size_t end = text_.find(L'"', i + 1);
                if (end == std::wstring::npos) return false;
                value = text_.substr(i + 1, end - i - 1);
                return true;
            }
            /* Number, bool or null: read to the next delimiter. */
            size_t end = i;
            while (end < text_.size() && text_[end] != L',' && text_[end] != L'}' &&
                   text_[end] != L']' && text_[end] != L'\r' && text_[end] != L'\n') ++end;
            std::wstring raw = text_.substr(i, end - i);
            while (!raw.empty() && (raw.back() == L' ' || raw.back() == L'\t')) raw.pop_back();
            if (raw == L"null") { value.clear(); return false; }
            value = raw;
            return true;
        }
        return false;
    }

    bool GetLongLong(const std::wstring& key, long long& out) const {
        std::wstring v;
        if (!FindRaw(key, v) || v.empty()) return false;
        wchar_t* end = nullptr;
        long long n = _wcstoi64(v.c_str(), &end, 10);
        if (end == v.c_str()) return false;
        out = n;
        return true;
    }

    bool GetWString(const std::wstring& key, std::wstring& out) const {
        return FindRaw(key, out);
    }

private:
    const std::wstring& text_;
};

/*
    Text-window rail parser.

    mVolt+ prints the rail blocks inline, so the reliable approach is to locate
    the rail's own key, take the following object text, and read the four keys
    inside that window. This avoids ambiguity between the two rails' identical
    inner key names ("vmin" appears in both).
*/
bool ParseRail(const std::wstring& text, const std::wstring& railKey, RailOffsets& out) {
    const std::wstring needle = L"\"" + railKey + L"\"";
    size_t pos = text.find(needle);
    if (pos == std::wstring::npos) return false;
    size_t open = text.find(L'{', pos);
    if (open == std::wstring::npos) return false;
    size_t close = text.find(L'}', open);
    if (close == std::wstring::npos) return false;

    JsonView inner(text.substr(open, close - open + 1));
    long long v = 0;
    bool any = false;
    if (inner.GetLongLong(L"vmin", v)) { out.vminUv = v; any = true; }
    if (inner.GetLongLong(L"rel",  v)) { out.relUv  = v; any = true; }
    if (inner.GetLongLong(L"alt",  v)) { out.altUv  = v; any = true; }
    if (inner.GetLongLong(L"ov",   v)) { out.ovUv   = v; any = true; }
    return any;
}

/* Reads a nested {minimum,maximum,step} object that follows the given key. */
bool ParseRange(const std::wstring& text, const std::wstring& railKey,
                long long& mn, long long& mx, long long& step)
{
    const std::wstring needle = L"\"" + railKey + L"\"";
    size_t pos = text.find(needle);
    if (pos == std::wstring::npos) return false;
    size_t open = text.find(L'{', pos);
    if (open == std::wstring::npos) return false;
    size_t close = text.find(L'}', open);
    if (close == std::wstring::npos) return false;

    JsonView inner(text.substr(open, close - open + 1));
    bool a = inner.GetLongLong(L"minimum", mn);
    bool b = inner.GetLongLong(L"maximum", mx);
    bool c = inner.GetLongLong(L"step", step);
    return a && b && c;
}

/* ------------------------------------------------------------------ */
/* command-line formatting                                            */
/* ------------------------------------------------------------------ */

std::wstring QuoteArg(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

/* uV -> mV with explicit rounding, reporting whether rounding happened. */
long long UvToMvRounded(long long uv, bool& rounded) {
    const long long sign = (uv < 0) ? -1 : 1;
    const long long mag = (uv < 0) ? -uv : uv;
    const long long mv = (mag + 500) / 1000;   /* nearest millivolt */
    if (mv * 1000 != mag) rounded = true;
    return sign * mv;
}

} /* anonymous namespace */

/* ------------------------------------------------------------------ */
/* discovery                                                          */
/* ------------------------------------------------------------------ */

std::wstring FindMVoltExecutable(const std::wstring& explicitPath) {
    if (!explicitPath.empty()) {
        DWORD a = GetFileAttributesW(explicitPath.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
            return explicitPath;
        /* An explicit path that does not resolve is still returned so the caller
           can report exactly what the user configured rather than silently
           falling back and hiding the mistake. */
        return explicitPath;
    }

    std::vector<std::wstring> roots;

    wchar_t self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH)) {
        std::wstring dir(self);
        size_t p = dir.find_last_of(L"\\/");
        if (p != std::wstring::npos) roots.push_back(dir.substr(0, p));
    }
    {
        wchar_t base[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
            roots.push_back(std::wstring(base) + L"\\NvpwrControl");
        if (GetEnvironmentVariableW(L"USERPROFILE", base, MAX_PATH)) {
            roots.push_back(std::wstring(base) + L"\\Downloads");
            roots.push_back(std::wstring(base) + L"\\Documents");
            roots.push_back(std::wstring(base) + L"\\Desktop");
        }
    }

    for (const std::wstring& root : roots) {
        std::wstring dir = root;
        for (int depth = 0; depth < 4 && !dir.empty(); ++depth) {
            const std::wstring candidates[] = {
                dir + L"\\" + kMVoltExeName,
                dir + L"\\dist\\" + kMVoltExeName,
            };
            for (const std::wstring& c : candidates) {
                DWORD a = GetFileAttributesW(c.c_str());
                if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
                    return c;
            }
            size_t p = dir.find_last_of(L"\\/");
            if (p == std::wstring::npos || p < 3) break;
            dir = dir.substr(0, p);
        }
    }
    return L"";
}

bool MVoltAvailable(const std::wstring& explicitPath) {
    std::wstring path = FindMVoltExecutable(explicitPath);
    if (path.empty()) return false;
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ------------------------------------------------------------------ */
/* read                                                               */
/* ------------------------------------------------------------------ */

bool QueryMVoltStatus(MVoltSnapshot& out, const std::wstring& explicitPath) {
    out = MVoltSnapshot{};

    std::wstring exe = FindMVoltExecutable(explicitPath);
    if (exe.empty()) {
        out.error = L"mVolt+ was not found";
        return false;
    }
    if (!MVoltAvailable(explicitPath)) {
        out.error = L"the configured mVolt+ path does not exist: " + exe;
        return false;
    }

    std::wstring output, err;
    if (!RunCapture(QuoteArg(exe) + L" --status", output, 20000, err)) {
        out.error = L"could not query mVolt+: " + err;
        return false;
    }
    if (output.empty()) {
        out.error = L"mVolt+ returned no status output";
        return false;
    }
    out.rawJson = output;

    /* The CLI prints one JSON object; find its span so stray banner text cannot
       be mistaken for data. */
    size_t open = output.find(L'{');
    size_t close = output.rfind(L'}');
    if (open == std::wstring::npos || close == std::wstring::npos || close <= open) {
        out.error = L"mVolt+ output did not contain a JSON object";
        return false;
    }
    const std::wstring json = output.substr(open, close - open + 1);
    JsonView view(json);

    view.GetWString(L"gpu", out.gpuName);
    view.GetWString(L"vbios", out.vbios);

    /* Rail offsets and their editable envelopes. */
    if (!ParseRail(json, L"nvvdd_offsets_uv", out.nvvdd)) {
        out.error = L"mVolt+ did not report nvvdd_offsets_uv";
        return false;
    }
    ParseRail(json, L"msvdd_offsets_uv", out.msvdd);
    ParseRange(json, L"nvvdd_device_range_uv", out.nvvddDeviceMinUv,
               out.nvvddDeviceMaxUv, out.nvvddDeviceStepUv);
    ParseRange(json, L"msvdd_device_range_uv", out.msvddDeviceMinUv,
               out.msvddDeviceMaxUv, out.msvddDeviceStepUv);

    /* Per-domain voltage request. */
    {
        const std::wstring needle = L"\"voltage_demand_mv\"";
        size_t pos = json.find(needle);
        if (pos != std::wstring::npos) {
            size_t o = json.find(L'{', pos);
            size_t c = json.find(L'}', o);
            if (o != std::wstring::npos && c != std::wstring::npos) {
                JsonView inner(json.substr(o, c - o + 1));
                long long v = 0;
                if (inner.GetLongLong(L"Core",  v)) out.demand.coreMv  = v;
                if (inner.GetLongLong(L"Xbar",  v)) out.demand.xbarMv  = v;
                if (inner.GetLongLong(L"SYS",   v)) out.demand.sysMv   = v;
                if (inner.GetLongLong(L"Video", v)) out.demand.videoMv = v;
            }
        }
        view.GetLongLong(L"voltage_demand_limit_min_mv", out.demandMinMv);
        view.GetLongLong(L"voltage_demand_limit_max_mv", out.demandMaxMv);
    }

    /* Power view. mVolt+ reports null for "not requested", which GetLongLong
       treats as absent. */
    long long mw = 0;
    if (view.GetLongLong(L"enforced_mw", mw)) { out.hasEnforcedPower = true; out.enforcedMw = (unsigned int)mw; }
    if (view.GetLongLong(L"default_mw", mw))  { out.hasDefaultPower = true;  out.defaultMw = (unsigned int)mw; }

    out.ok = true;
    return true;
}

bool ImportVoltageFromMVolt(VoltageTuning& out, std::wstring& error,
                            const std::wstring& explicitPath)
{
    MVoltSnapshot snap{};
    if (!QueryMVoltStatus(snap, explicitPath)) {
        error = snap.error;
        return false;
    }
    out = VoltageTuning{};
    out.nvvdd = snap.nvvdd;
    out.msvdd = snap.msvdd;
    out.demand = snap.demand;
    /* An all-zero import is a valid result (nothing applied), but it must not be
       reported as "enabled" or the boot replay would write zeros every start. */
    out.enabled = !out.IsZero();
    return true;
}

/* ------------------------------------------------------------------ */
/* write                                                              */
/* ------------------------------------------------------------------ */

bool ApplyVoltageViaMVolt(const VoltageTuning& tuning, std::wstring& error,
                          bool* rounded, const std::wstring& explicitPath)
{
    error.clear();
    if (rounded) *rounded = false;

    std::wstring exe = FindMVoltExecutable(explicitPath);
    if (exe.empty()) { error = L"mVolt+ was not found"; return false; }
    if (!MVoltAvailable(explicitPath)) {
        error = L"the configured mVolt+ path does not exist: " + exe;
        return false;
    }

    bool anyRound = false;
    std::wstring cmd = QuoteArg(exe);

    /* Only the rails explicitly requested are passed, matching mVolt+'s contract
       that unspecified controls are left untouched. */
    auto railArg = [&](const wchar_t* option, const RailOffsets& r) {
        bool rr = false;
        const long long vmin = UvToMvRounded(r.vminUv, rr);
        const long long rel  = UvToMvRounded(r.relUv,  rr);
        const long long alt  = UvToMvRounded(r.altUv,  rr);
        const long long ov   = UvToMvRounded(r.ovUv,   rr);
        if (rr) anyRound = true;
        std::wstringstream a;
        a << L" " << option << L" " << vmin << L"," << rel << L"," << alt << L"," << ov;
        cmd += a.str();
    };

    bool useNvvdd = !tuning.nvvdd.IsZero();
    bool useMsvdd = !tuning.msvdd.IsZero();

    if (useNvvdd) railArg(L"--nvvdd-offsets", tuning.nvvdd);
    if (useMsvdd) railArg(L"--msvdd-offsets", tuning.msvdd);

    if (!useNvvdd && !useMsvdd) {
        error = L"no non-zero rail offsets were requested";
        return false;
    }
    if (rounded) *rounded = anyRound;

    std::wstring output, err;
    /* mVolt+ applies immediately for tuning options, so a generous timeout is
       used to cover driver-side settling. */
    if (!RunCapture(cmd, output, 30000, err)) {
        error = L"mVolt+ could not be run: " + err;
        return false;
    }

    /* Confirm by reading back rather than trusting the exit code: mVolt+'s CLI
       prints a status object on success, and the rail values in it are the
       authoritative result. */
    MVoltSnapshot check{};
    if (QueryMVoltStatus(check, explicitPath)) {
        if (useNvvdd && (check.nvvdd.vminUv != tuning.nvvdd.vminUv ||
                         check.nvvdd.relUv  != tuning.nvvdd.relUv)) {
            std::wstringstream e;
            e << L"mVolt+ did not retain the requested NVDD offsets (asked rel="
              << (tuning.nvvdd.relUv / 1000) << L" mV, read back rel="
              << (check.nvvdd.relUv / 1000) << L" mV)";
            error = e.str();
            return false;
        }
        if (useMsvdd && (check.msvdd.vminUv != tuning.msvdd.vminUv ||
                         check.msvdd.relUv  != tuning.msvdd.relUv)) {
            error = L"mVolt+ did not retain the requested MSVDD offsets";
            return false;
        }
    }
    /* If the readback itself failed we still consider the write successful: the
       CLI ran, and a missing --status is not evidence that the apply failed. */
    return true;
}

bool ResetVoltageViaMVolt(std::wstring& error, const std::wstring& explicitPath) {
    error.clear();
    VoltageTuning zero{};
    /* Both rails explicitly zeroed: this is a reset, so touching both is correct
       even though a general apply only touches what was requested. */
    zero.nvvdd = RailOffsets{};
    zero.msvdd = RailOffsets{};
    zero.enabled = true;

    std::wstring exe = FindMVoltExecutable(explicitPath);
    if (exe.empty()) { error = L"mVolt+ was not found"; return false; }

    std::wstring cmd = QuoteArg(exe) + L" --nvvdd-offsets 0,0,0,0 --msvdd-offsets 0,0,0,0";
    std::wstring output, err;
    if (!RunCapture(cmd, output, 30000, err)) {
        error = L"mVolt+ could not be run: " + err;
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* diagnostics                                                        */
/* ------------------------------------------------------------------ */

std::wstring LastMVoltCommandLine() {
    return g_lastCommandLine;
}

bool TestMVoltLaunch(std::wstring& versionOut, std::wstring& error,
                     const std::wstring& explicitPath)
{
    versionOut.clear();
    error.clear();

    std::wstring exe = FindMVoltExecutable(explicitPath);
    if (exe.empty()) { error = L"mVolt+ was not found"; return false; }
    if (!MVoltAvailable(explicitPath)) {
        error = L"the configured mVolt+ path does not exist: " + exe;
        return false;
    }

    std::wstring output, err;
    if (!RunCapture(QuoteArg(exe) + L" --version", output, 15000, err)) {
        error = L"launch test failed: " + err;
        return false;
    }
    while (!output.empty() && (output.back() == L'\r' || output.back() == L'\n'))
        output.pop_back();
    versionOut = output;
    if (versionOut.empty()) { error = L"mVolt+ produced no version output"; return false; }
    return true;
}

} /* namespace nvpwr */
