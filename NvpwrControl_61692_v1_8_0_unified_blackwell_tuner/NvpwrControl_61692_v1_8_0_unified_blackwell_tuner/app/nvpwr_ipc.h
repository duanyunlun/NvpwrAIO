#pragma once
/*
    nvpwr_ipc.h — transport between the tray GUI and the background service.

    WHERE: shared by NvpwrControl.exe (client) and NvpwrSvc.exe (server).
    WHAT:  a small line-oriented named-pipe protocol plus the settings file
           reader/writer both sides use.
    WHY:   the GUI must work whether or not the service is installed. In 1.8.0
           the GUI owned the device handle and the driver service itself, so
           closing the window dropped all state. 1.9.0 splits duties:

             service present -> GUI is a thin client, state survives GUI exit
                                and is replayed at boot / resume
             service absent  -> GUI falls back to driving the kernel device
                                directly, exactly as 1.8.0 did

           So a user who never installs the service loses nothing.

    PROTOCOL (text, UTF-8, one request line -> one response, then close):
        Request : "CMD <verb> [key=value ...]\r\n"
        Response: "OK\r\n" or "ERR <message>\r\n", optionally followed by
                  "key=value\r\n" status lines terminated by "END\r\n"

    Verbs: STATUS, APPLY, RESTORE, PING, SHUTDOWN
*/

#include <windows.h>
#include <string>

#include "nvpwr_ui_state.h"

namespace nvpwr {
constexpr wchar_t kServiceName[] = L"NvpwrSvc";
constexpr wchar_t kServiceDisplayName[] = L"Nvpwr Control Background Service";

/* Where the desired state lives. ProgramData so the service (SYSTEM) and the
   GUI (elevated user) can both reach it. */
std::wstring StateFilePath();
std::wstring StateDirectory();

/* ---------------- desired-state persistence ---------------- */

/*
    Serialisation is a flat INI-style key/value text file rather than binary or
    JSON: it must be inspectable by a human when something goes wrong at boot,
    and it must round-trip through the same reader on both sides.

    Returns false with `error` populated when the file exists but cannot be
    parsed, so a corrupt state file is reported instead of silently reset.
*/
bool LoadDesiredState(DesiredState& out, std::wstring& error);
bool SaveDesiredState(const DesiredState& state, std::wstring& error);

/* ---------------- configuration slots ----------------
   Stored separately from the active state so resetting the live settings never
   destroys saved experiments. */

std::wstring ProfileFilePath();

bool LoadProfileStore(ProfileStore& out, std::wstring& error);
bool SaveProfileStore(const ProfileStore& store, std::wstring& error);

/* Slot indices are 0-based; slot 0 is presented to the user as "槽位 1". */
bool SaveSlot(int index, const std::wstring& name, const DesiredState& state,
              std::wstring& error);
bool LoadSlot(int index, DesiredState& out, std::wstring& error);
bool ClearSlot(int index, std::wstring& error);
bool RenameSlot(int index, const std::wstring& name, std::wstring& error);

/* Records the outcome of the last apply so the UI can mark slots that are known
   to misbehave. Persisted, because the point is to survive a reboot. */
bool MarkSlotApplyResult(int index, bool ok, const std::wstring& note,
                         std::wstring& error);

/* ---------------- safety: restore points ---------------- */

/*
    A restore point is the complete live tuning captured immediately before a
    mutation. It is what "undo the last thing I did" means, which is the single
    most-used operation during iterative tuning.

    Kept in memory only: a restore point that survives a reboot would be
    replaying a state the GPU no longer holds anyway.
*/
struct RestorePoint {
    bool         valid = false;
    std::wstring label;
    DesiredState state{};
};

void        SetRestorePoint(const std::wstring& label, const DesiredState& state);
bool        GetRestorePoint(RestorePoint& out);
void        ClearRestorePoint();

/* ---------------- service control (used by the GUI) ---------------- */

enum class ServiceStatus { Absent, Stopped, Running, Unknown };

ServiceStatus QueryServiceStatus();
bool InstallService(std::wstring& error);   /* register + start, auto-start type */
bool RemoveService(std::wstring& error);    /* stop + unregister */
bool StartServiceNow(std::wstring& error);

/* Auto-start registration for the GUI itself (tray), separate from the service. */
bool SetGuiAutoStart(bool enable, std::wstring& error);
bool GetGuiAutoStart();

} /* namespace nvpwr */
