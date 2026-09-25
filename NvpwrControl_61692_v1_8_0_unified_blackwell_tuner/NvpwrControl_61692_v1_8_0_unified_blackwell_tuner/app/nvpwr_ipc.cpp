/*
    nvpwr_ipc.cpp — state persistence, service control and pipe client.
*/

#include "nvpwr_ipc.h"

#include <shlobj.h>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <cstdio>

namespace nvpwr {

/* ------------------------------------------------------------------ */
/* paths                                                              */
/* ------------------------------------------------------------------ */

std::wstring StateDirectory() {
    wchar_t base[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring dir = std::wstring(base) + L"\\NvpwrControl";
    CreateDirectoryW(dir.c_str(), nullptr);   /* already-exists is fine */
    return dir;
}

std::wstring StateFilePath() {
    std::wstring dir = StateDirectory();
    if (dir.empty()) return L"";
    return dir + L"\\state.ini";
}

/* ------------------------------------------------------------------ */
/* text key/value codec shared by the state file and the slot file     */
/* ------------------------------------------------------------------ */

namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

bool WriteAll(HANDLE h, const std::string& data) {
    DWORD total = 0;
    while (total < data.size()) {
        DWORD wrote = 0;
        if (!WriteFile(h, data.data() + total, (DWORD)(data.size() - total), &wrote, nullptr))
            return false;
        if (wrote == 0) return false;
        total += wrote;
    }
    return true;
}

bool ReadAll(HANDLE h, std::string& out, size_t maxBytes = 262144) {
    out.clear();
    char buf[1024];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf, sizeof(buf), &got, nullptr)) return !out.empty();
        if (got == 0) break;
        out.append(buf, got);
        if (out.size() > maxBytes) break;
    }
    return true;
}

typedef std::vector<std::pair<std::wstring, std::wstring> > KvList;

bool ReadKvFile(const std::wstring& path, KvList& out, bool& absent, std::wstring& error) {
    out.clear();
    absent = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) { absent = true; return true; }
        error = L"could not open " + path;
        return false;
    }
    std::string raw;
    ReadAll(h, raw);
    CloseHandle(h);

    std::wstring text = FromUtf8(raw);
    std::wistringstream in(text);
    std::wstring line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        out.push_back(std::make_pair(line.substr(0, eq), line.substr(eq + 1)));
    }
    return true;
}

bool WriteKvFile(const std::wstring& path, const std::wstring& header,
                 const std::vector<std::wstring>& lines, std::wstring& error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"could not open " + path + L" for writing";
        return false;
    }
    std::wstring text = header;
    for (size_t i = 0; i < lines.size(); ++i) { text += lines[i]; text += L"\r\n"; }

    std::string data = ToUtf8(text);
    bool ok = WriteAll(h, data);
    CloseHandle(h);
    if (!ok) error = L"could not write " + path;
    return ok;
}

long long ParseInt(const std::wstring& v) {
    errno = 0;
    long long n = _wcstoi64(v.c_str(), nullptr, 10);
    return (errno == 0) ? n : 0;
}

/*
    String lookup for the few non-numeric keys (the companion-tool path).

    Kept separate from the numeric pass rather than folded into it: the numeric
    pass uses "did we recognise any key" as its corruption test, and a free-form
    path must not be allowed to influence that.
*/
bool FindStringValue(const KvList& kv, const std::wstring& prefix,
                     const std::wstring& key, std::wstring& out)
{
    const std::wstring full = prefix + key;
    for (size_t i = 0; i < kv.size(); ++i) {
        if (kv[i].first == full) { out = kv[i].second; return true; }
    }
    return false;
}

/* The path is stored verbatim, so a Windows path with backslashes needs no
   escaping as long as newlines are removed: a newline would break the
   line-oriented format and silently truncate the path. */
std::wstring SanitizePathForStorage(const std::wstring& path) {
    std::wstring out = path;
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i] == L'\r' || out[i] == L'\n') out[i] = L' ';
    return out;
}

void SerializeState(const DesiredState& st, const std::wstring& prefix,
                    std::vector<std::wstring>& out)
{
    std::wostringstream o;
    o << prefix << L"power_enabled=" << (st.powerEnabled ? 1 : 0);          out.push_back(o.str()); o.str(L"");
    o << prefix << L"power_mw=" << st.power.milliwatts;                      out.push_back(o.str()); o.str(L"");
    o << prefix << L"power_ceiling_mw=" << st.power.ceilingMw;              out.push_back(o.str()); o.str(L"");
    o << prefix << L"power_profile=" << st.power.profile;                   out.push_back(o.str()); o.str(L"");
    o << prefix << L"voltage_enabled=" << (st.voltage.enabled ? 1 : 0);     out.push_back(o.str()); o.str(L"");
    o << prefix << L"voltage_applier=" << (int)st.voltage.applier;          out.push_back(o.str()); o.str(L"");
    o << prefix << L"nvvdd_vmin_uv=" << st.voltage.nvvdd.vminUv;            out.push_back(o.str()); o.str(L"");
    o << prefix << L"nvvdd_rel_uv="  << st.voltage.nvvdd.relUv;             out.push_back(o.str()); o.str(L"");
    o << prefix << L"nvvdd_alt_uv="  << st.voltage.nvvdd.altUv;             out.push_back(o.str()); o.str(L"");
    o << prefix << L"nvvdd_ov_uv="   << st.voltage.nvvdd.ovUv;              out.push_back(o.str()); o.str(L"");
    o << prefix << L"msvdd_vmin_uv=" << st.voltage.msvdd.vminUv;            out.push_back(o.str()); o.str(L"");
    o << prefix << L"msvdd_rel_uv="  << st.voltage.msvdd.relUv;             out.push_back(o.str()); o.str(L"");
    o << prefix << L"msvdd_alt_uv="  << st.voltage.msvdd.altUv;             out.push_back(o.str()); o.str(L"");
    o << prefix << L"msvdd_ov_uv="   << st.voltage.msvdd.ovUv;              out.push_back(o.str()); o.str(L"");
    o << prefix << L"demand_core_mv="  << st.voltage.demand.coreMv;          out.push_back(o.str()); o.str(L"");
    o << prefix << L"demand_xbar_mv="  << st.voltage.demand.xbarMv;          out.push_back(o.str()); o.str(L"");
    o << prefix << L"demand_sys_mv="   << st.voltage.demand.sysMv;           out.push_back(o.str()); o.str(L"");
    o << prefix << L"demand_video_mv=" << st.voltage.demand.videoMv;        out.push_back(o.str()); o.str(L"");
    o << prefix << L"clock_enabled=" << (st.clock.enabled ? 1 : 0);          out.push_back(o.str()); o.str(L"");
    o << prefix << L"clock_core_mhz=" << st.clock.coreOffsetMhz;            out.push_back(o.str()); o.str(L"");
    o << prefix << L"clock_memory_mhz=" << st.clock.memoryOffsetMhz;        out.push_back(o.str()); o.str(L"");
    o << prefix << L"clock_xbar_mhz=" << st.clock.xbarOffsetMhz;            out.push_back(o.str()); o.str(L"");
    o << prefix << L"start_with_windows=" << (st.startWithWindows ? 1 : 0); out.push_back(o.str()); o.str(L"");
    o << prefix << L"start_minimized=" << (st.startMinimized ? 1 : 0);       out.push_back(o.str()); o.str(L"");
    /*
        The recorded factory wall. Both the GUI and the service read and write these,
        and the key names match the ones the C# side already used for power_floor_w —
        two spellings of one record would be worse than none, because each side would
        then keep filling in the other's blank.
    */
    o << prefix << L"power_floor_w=" << st.powerFloorW;                     out.push_back(o.str()); o.str(L"");
    o << prefix << L"power_floor_profile=" << st.powerFloorProfile;         out.push_back(o.str());
    /* Free-form string keys last, so the numeric block above stays contiguous
       and easy to read in the file. */
    out.push_back(prefix + L"mvolt_path=" + SanitizePathForStorage(st.mvoltPath));
}

/*
    Applies the key/value pairs for one state payload.

    `prefix` is stripped before matching. Unknown keys are ignored so an older or
    newer writer does not break the reader; malformed numeric values are also
    skipped rather than aborting the whole load, but `recognised` counts only
    keys we actually understood, which is what the caller uses to detect a
    genuinely unreadable file.
*/
void DeserializeState(const KvList& kv, const std::wstring& prefix,
                      size_t begin, size_t end, DesiredState& out, unsigned int& recognised)
{
    for (size_t i = begin; i < end && i < kv.size(); ++i) {
        std::wstring key = kv[i].first;
        if (key.size() < prefix.size() || key.compare(0, prefix.size(), prefix) != 0) continue;
        key = key.substr(prefix.size());
        long long n = ParseInt(kv[i].second);
        ++recognised;

        if      (key == L"schema")             out.schema = (unsigned int)n;
        else if (key == L"power_enabled")      out.powerEnabled = (n != 0);
        else if (key == L"power_mw")           out.power.milliwatts = (unsigned int)n;
        else if (key == L"power_ceiling_mw")   out.power.ceilingMw = (unsigned int)n;
        else if (key == L"power_profile")      out.power.profile = (unsigned int)n;
        else if (key == L"voltage_enabled")    out.voltage.enabled = (n != 0);
        else if (key == L"voltage_applier")    out.voltage.applier =
            (n == (int)VoltageApplier::CompanionTool) ? VoltageApplier::CompanionTool
                                                      : VoltageApplier::None;
        else if (key == L"nvvdd_vmin_uv")      out.voltage.nvvdd.vminUv = n;
        else if (key == L"nvvdd_rel_uv")       out.voltage.nvvdd.relUv = n;
        else if (key == L"nvvdd_alt_uv")       out.voltage.nvvdd.altUv = n;
        else if (key == L"nvvdd_ov_uv")        out.voltage.nvvdd.ovUv = n;
        else if (key == L"msvdd_vmin_uv")      out.voltage.msvdd.vminUv = n;
        else if (key == L"msvdd_rel_uv")       out.voltage.msvdd.relUv = n;
        else if (key == L"msvdd_alt_uv")       out.voltage.msvdd.altUv = n;
        else if (key == L"msvdd_ov_uv")        out.voltage.msvdd.ovUv = n;
        else if (key == L"demand_core_mv")     out.voltage.demand.coreMv = n;
        else if (key == L"demand_xbar_mv")     out.voltage.demand.xbarMv = n;
        else if (key == L"demand_sys_mv")      out.voltage.demand.sysMv = n;
        else if (key == L"demand_video_mv")    out.voltage.demand.videoMv = n;
        else if (key == L"clock_enabled")      out.clock.enabled = (n != 0);
        else if (key == L"clock_core_mhz")     out.clock.coreOffsetMhz = (long)n;
        else if (key == L"clock_memory_mhz")   out.clock.memoryOffsetMhz = (long)n;
        else if (key == L"clock_xbar_mhz")     out.clock.xbarOffsetMhz = (long)n;
        else if (key == L"start_with_windows") out.startWithWindows = (n != 0);
        else if (key == L"start_minimized")    out.startMinimized = (n != 0);
        else if (key == L"power_floor_w")      out.powerFloorW = (unsigned int)n;
        else if (key == L"power_floor_profile") out.powerFloorProfile = (unsigned int)n;
        else --recognised;   /* not a key we know: do not count it */
    }
}

std::wstring LocalTimestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t b[64]{};
    swprintf_s(b, L"%04u-%02u-%02u %02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return b;
}

} /* anonymous namespace */

bool SaveDesiredState(const DesiredState& st, std::wstring& error) {
    std::wstring path = StateFilePath();
    if (path.empty()) { error = L"ProgramData is not available"; return false; }

    std::vector<std::wstring> lines;
    SerializeState(st, L"", lines);
    return WriteKvFile(path,
        L"; Nvpwr Control desired state. Written by the GUI, replayed by the service.\r\n"
        L"; The GPU does not remember these values across a driver reload or reboot;\r\n"
        L"; the service re-applies them at boot. See nvpwr_ui_state.h.\r\n",
        lines, error);
}

bool LoadDesiredState(DesiredState& out, std::wstring& error) {
    error.clear();
    out = DesiredState{};

    std::wstring path = StateFilePath();
    if (path.empty()) { error = L"ProgramData is not available"; return false; }

    KvList kv;
    bool absent = false;
    if (!ReadKvFile(path, kv, absent, error)) return false;
    if (absent) return true;   /* first run: nothing configured yet */

    unsigned int recognised = 0;
    DeserializeState(kv, L"", 0, kv.size(), out, recognised);
    /* Non-numeric keys are read separately so they cannot affect the
       "did we understand this file" test above. */
    FindStringValue(kv, L"", L"mvolt_path", out.mvoltPath);

    /* A file that exists but yielded nothing recognisable is a real problem:
       silently treating it as "nothing configured" would look like the user's
       settings vanished. */
    if (recognised == 0) {
        error = L"state file at " + path + L" could not be parsed";
        return false;
    }
    return true;
}

std::wstring ProfileFilePath() {
    std::wstring dir = StateDirectory();
    if (dir.empty()) return L"";
    return dir + L"\\profiles.ini";
}

bool SaveProfileStore(const ProfileStore& store, std::wstring& error) {
    std::wstring path = ProfileFilePath();
    if (path.empty()) { error = L"ProgramData is not available"; return false; }

    std::vector<std::wstring> lines;
    lines.push_back(L"schema=" + std::to_wstring(store.schema));
    lines.push_back(L"slot_count=" + std::to_wstring(kSlotCount));

    for (int i = 0; i < kSlotCount; ++i) {
        const ConfigSlot& s = store.slots[i];
        if (!s.used) continue;
        const std::wstring p = L"slot" + std::to_wstring(i + 1) + L"_";

        lines.push_back(p + L"used=1");
        /* Newlines would break the line-oriented format; a slot name is a short
           label, so folding them to spaces is lossless enough and keeps the file
           parseable by hand. */
        std::wstring name = s.name;
        for (size_t k = 0; k < name.size(); ++k)
            if (name[k] == L'\r' || name[k] == L'\n') name[k] = L' ';
        lines.push_back(p + L"name=" + name);
        lines.push_back(p + L"saved_at=" + s.savedAt);
        lines.push_back(p + L"last_ok=" + std::to_wstring(s.lastApplyOk ? 1 : 0));
        std::wstring note = s.lastApplyNote;
        for (size_t k = 0; k < note.size(); ++k)
            if (note[k] == L'\r' || note[k] == L'\n') note[k] = L' ';
        lines.push_back(p + L"last_note=" + note);

        std::vector<std::wstring> payload;
        SerializeState(s.state, p, payload);
        lines.insert(lines.end(), payload.begin(), payload.end());
    }

    return WriteKvFile(path,
        L"; Nvpwr Control saved tuning slots. Written by the GUI.\r\n"
        L"; Slots survive resetting the live settings on purpose: they are the\r\n"
        L"; record of which experiments were worth keeping.\r\n",
        lines, error);
}

bool LoadProfileStore(ProfileStore& out, std::wstring& error) {
    error.clear();
    out = ProfileStore{};

    std::wstring path = ProfileFilePath();
    if (path.empty()) { error = L"ProgramData is not available"; return false; }

    KvList kv;
    bool absent = false;
    if (!ReadKvFile(path, kv, absent, error)) return false;
    if (absent) return true;

    /* Locate each slot's block by walking its prefixed keys. Seeking by prefix
       rather than assuming a fixed line layout keeps this readable if a slot
       gains a field later. */
    for (int i = 0; i < kSlotCount; ++i) {
        const std::wstring p = L"slot" + std::to_wstring(i + 1) + L"_";
        size_t first = kv.size();
        size_t last = 0;
        for (size_t k = 0; k < kv.size(); ++k) {
            if (kv[k].first.size() >= p.size() && kv[k].first.compare(0, p.size(), p) == 0) {
                if (first == kv.size()) first = k;
                last = k;
            }
        }
        if (first == kv.size()) continue;   /* slot not present */

        ConfigSlot& s = out.slots[i];
        unsigned int recognised = 0;
        DeserializeState(kv, p, first, kv.size(), s.state, recognised);
        /* A slot carries the companion path it was saved with, so loading a slot
           on a machine whose mVolt+ moved produces a clear error rather than
           silently using a different binary. */
        FindStringValue(kv, p, L"mvolt_path", s.state.mvoltPath);

        /* Read the slot metadata (not part of the tuning payload). */
        for (size_t k = first; k <= last && k < kv.size(); ++k) {
            const std::wstring& key = kv[k].first;
            if (key.size() < p.size() || key.compare(0, p.size(), p) != 0) continue;
            std::wstring tail = key.substr(p.size());
            if      (tail == L"used")      s.used = ParseInt(kv[k].second) != 0;
            else if (tail == L"name")      s.name = kv[k].second;
            else if (tail == L"saved_at")  s.savedAt = kv[k].second;
            else if (tail == L"last_ok")   s.lastApplyOk = ParseInt(kv[k].second) != 0;
            else if (tail == L"last_note") s.lastApplyNote = kv[k].second;
        }
        if (s.name.empty()) s.name = L"Slot " + std::to_wstring(i + 1);
    }
    return true;
}

bool SaveSlot(int index, const std::wstring& name, const DesiredState& state,
              std::wstring& error)
{
    if (index < 0 || index >= kSlotCount) { error = L"slot index out of range"; return false; }
    ProfileStore store{};
    if (!LoadProfileStore(store, error)) return false;

    ConfigSlot& s = store.slots[index];
    s.used = true;
    s.name = name.empty() ? (L"Slot " + std::to_wstring(index + 1)) : name;
    if (s.name.size() > (size_t)kSlotNameMax) s.name.resize(kSlotNameMax);
    s.savedAt = LocalTimestamp();
    s.state = state;
    /* A freshly saved slot has no recorded outcome yet. */
    s.lastApplyOk = false;
    s.lastApplyNote.clear();
    return SaveProfileStore(store, error);
}

bool LoadSlot(int index, DesiredState& out, std::wstring& error) {
    if (index < 0 || index >= kSlotCount) { error = L"slot index out of range"; return false; }
    ProfileStore store{};
    if (!LoadProfileStore(store, error)) return false;
    if (!store.slots[index].used) { error = L"slot is empty"; return false; }
    out = store.slots[index].state;
    return true;
}

bool ClearSlot(int index, std::wstring& error) {
    if (index < 0 || index >= kSlotCount) { error = L"slot index out of range"; return false; }
    ProfileStore store{};
    if (!LoadProfileStore(store, error)) return false;
    store.slots[index] = ConfigSlot{};
    return SaveProfileStore(store, error);
}

bool RenameSlot(int index, const std::wstring& name, std::wstring& error) {
    if (index < 0 || index >= kSlotCount) { error = L"slot index out of range"; return false; }
    ProfileStore store{};
    if (!LoadProfileStore(store, error)) return false;
    if (!store.slots[index].used) { error = L"slot is empty"; return false; }
    store.slots[index].name = name.empty() ? (L"Slot " + std::to_wstring(index + 1)) : name;
    if (store.slots[index].name.size() > (size_t)kSlotNameMax)
        store.slots[index].name.resize(kSlotNameMax);
    return SaveProfileStore(store, error);
}

bool MarkSlotApplyResult(int index, bool ok, const std::wstring& note,
                         std::wstring& error)
{
    if (index < 0 || index >= kSlotCount) return false;   /* best-effort bookkeeping */
    ProfileStore store{};
    if (!LoadProfileStore(store, error)) return false;
    if (!store.slots[index].used) return true;
    store.slots[index].lastApplyOk = ok;
    store.slots[index].lastApplyNote = note;
    return SaveProfileStore(store, error);
}

/* ------------------------------------------------------------------ */
/* safety: restore point                                              */
/* ------------------------------------------------------------------ */

namespace {
RestorePoint g_restorePoint;
}

void SetRestorePoint(const std::wstring& label, const DesiredState& state) {
    g_restorePoint.valid = true;
    g_restorePoint.label = label;
    g_restorePoint.state = state;
}

bool GetRestorePoint(RestorePoint& out) {
    if (!g_restorePoint.valid) return false;
    out = g_restorePoint;
    return true;
}

void ClearRestorePoint() {
    g_restorePoint = RestorePoint{};
}

/* ------------------------------------------------------------------ */
/* service control                                                    */
/* ------------------------------------------------------------------ */

ServiceStatus QueryServiceStatus() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return ServiceStatus::Unknown;

    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
    if (!svc) {
        DWORD e = GetLastError();
        CloseServiceHandle(scm);
        return (e == ERROR_SERVICE_DOES_NOT_EXIST) ? ServiceStatus::Absent
                                                   : ServiceStatus::Unknown;
    }
    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    ServiceStatus result = ServiceStatus::Unknown;
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed)) {
        result = (ssp.dwCurrentState == SERVICE_RUNNING) ? ServiceStatus::Running
                                                         : ServiceStatus::Stopped;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return result;
}

bool InstallService(std::wstring& error) {
    /* The service binary is expected next to the GUI. */
    wchar_t self[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) {
        error = L"could not resolve the application path";
        return false;
    }
    std::wstring dir(self);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) { error = L"could not resolve the application directory"; return false; }
    dir = dir.substr(0, slash);
    std::wstring svcPath = dir + L"\\NvpwrSvc.exe";

    if (GetFileAttributesW(svcPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"NvpwrSvc.exe was not found next to the application (expected " + svcPath + L")";
        return false;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr,
                                   SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) { error = L"OpenSCManager failed - run as administrator"; return false; }

    SC_HANDLE svc = CreateServiceW(
        scm, kServiceName, kServiceDisplayName,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, svcPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);

    if (!svc) {
        DWORD e = GetLastError();
        if (e == ERROR_SERVICE_EXISTS) {
            svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
            if (svc) {
                ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                                     SERVICE_NO_CHANGE, svcPath.c_str(),
                                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            }
        }
        if (!svc) {
            error = L"CreateService failed (" + std::to_wstring(e) + L")";
            CloseServiceHandle(scm);
            return false;
        }
    }

    if (!StartServiceW(svc, 0, nullptr)) {
        DWORD e = GetLastError();
        if (e != ERROR_SERVICE_ALREADY_RUNNING) {
            error = L"service registered but StartService failed (" + std::to_wstring(e) + L")";
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return false;
        }
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

bool RemoveService(std::wstring& error) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { error = L"OpenSCManager failed - run as administrator"; return false; }

    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
    if (!svc) {
        DWORD e = GetLastError();
        CloseServiceHandle(scm);
        if (e == ERROR_SERVICE_DOES_NOT_EXIST) return true;   /* already gone */
        error = L"OpenService failed (" + std::to_wstring(e) + L")";
        return false;
    }

    SERVICE_STATUS st{};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);

    /* Wait briefly for the stop so the image is not left locked. */
    for (int i = 0; i < 50; ++i) {
        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed)) break;
        if (ssp.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(100);
    }

    bool ok = DeleteService(svc) != FALSE;
    if (!ok) error = L"DeleteService failed (" + std::to_wstring(GetLastError()) + L")";
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

bool StartServiceNow(std::wstring& error) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { error = L"OpenSCManager failed"; return false; }
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_START);
    if (!svc) {
        error = L"service is not installed";
        CloseServiceHandle(scm);
        return false;
    }
    bool ok = StartServiceW(svc, 0, nullptr) != FALSE;
    if (!ok) {
        DWORD e = GetLastError();
        if (e != ERROR_SERVICE_ALREADY_RUNNING) {
            error = L"StartService failed (" + std::to_wstring(e) + L")";
            ok = false;
        } else ok = true;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

/* ------------------------------------------------------------------ */
/* GUI auto-start (tray)                                              */
/* ------------------------------------------------------------------ */

namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"NvpwrControl";
}

bool SetGuiAutoStart(bool enable, std::wstring& error) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        error = L"could not open the Run registry key";
        return false;
    }
    bool ok = true;
    if (enable) {
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(self) + L"\" --tray";
        LONG r = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(cmd.c_str()),
                                (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        if (r != ERROR_SUCCESS) { error = L"could not write the Run value"; ok = false; }
    } else {
        LONG r = RegDeleteValueW(key, kRunValue);
        if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) {
            error = L"could not remove the Run value"; ok = false;
        }
    }
    RegCloseKey(key);
    return ok;
}

bool GetGuiAutoStart() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[1024]{};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    LONG r = RegQueryValueExW(key, kRunValue, nullptr, &type,
                              reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_SZ;
}

/* ------------------------------------------------------------------ */
/* pipe client                                                        */
/* ------------------------------------------------------------------ */

bool IpcCall(const std::wstring& request, std::wstring& response, std::wstring& error) {
    response.clear();
    error.clear();

    /* Never block the UI thread for long: a missing service must fail fast so
       the caller can fall back to direct device access. */
    if (!WaitNamedPipeW(kPipeName, 300)) {
        error = L"background service is not listening";
        return false;
    }
    HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"could not open the service pipe (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    std::string req = ToUtf8(request + L"\r\n");
    if (!WriteAll(h, req)) {
        error = L"could not send the request";
        CloseHandle(h);
        return false;
    }
    FlushFileBuffers(h);

    std::string raw;
    ReadAll(h, raw);
    CloseHandle(h);

    response = FromUtf8(raw);
    if (response.empty()) { error = L"empty response from the service"; return false; }
    if (response.rfind(L"ERR ", 0) == 0 || response.rfind(L"ERR\r\n", 0) == 0) {
        error = response;
        return false;
    }
    return true;
}

bool IpcPing(std::wstring& error) {
    std::wstring resp;
    if (!IpcCall(L"CMD PING", resp, error)) return false;
    return resp.rfind(L"OK", 0) == 0;
}

bool IpcApplyPower(unsigned int milliwatts, unsigned int ceilingMw, unsigned int profile,
                   std::wstring& error)
{
    std::wstringstream req;
    req << L"CMD APPLY power_mw=" << milliwatts
        << L" ceiling_mw=" << ceilingMw
        << L" profile=" << profile;
    std::wstring resp;
    return IpcCall(req.str(), resp, error);
}

bool IpcRestore(std::wstring& error) {
    std::wstring resp;
    return IpcCall(L"CMD RESTORE", resp, error);
}

} /* namespace nvpwr */
