#pragma once
/*
    nvpwr_mvolt_bridge.h — optional companion-tool integration (mVolt+).

    WHERE: user-mode. Used by the GUI (manual actions) and by NvpwrSvc (boot
           replay). No kernel involvement.
    WHAT:  a thin, defensive wrapper around mVolt+'s command-line interface, so
           this project can drive voltage adjustment instead of asking the user
           to open a second program and click Apply every boot.

    DESIGN DECISION — WHY AN EXTERNAL PROCESS AND NOT OUR OWN IMPLEMENTATION
      Voltage rails cannot be written from this project at all: the public NVAPI
      surface has no voltage setter, and Pstates20 carries no voltage domain on
      the reference GPU (measured: 0 voltage domains, and a +25 mV rail change
      moved 0 bytes in the Pstates20 buffer). mVolt+ reaches those rails through
      a reverse-engineered private interface. So this project orchestrates and
      mVolt+ executes. That is a deliberate split, not a stopgap.

    STORAGE FORMAT — DELIBERATELY mVolt+'S OWN UNITS
      Values are stored as the four signed microvolt offsets mVolt+ itself
      reports (vmin / rel / alt / ov, in uV), so no conversion loss and no
      ambiguity about what "voltage" means. The CLI takes millivolts, so the
      bridge divides by 1000 exactly once, at the call site, with rounding made
      explicit.

    FAILURE POLICY
      A missing or misbehaving mVolt+ must never block the power-limit path.
      Every function returns a bool plus a reason, and callers are expected to
      continue with the stages that do work. Nothing here writes to the GPU
      itself; it only launches a process and reads its output.
*/

#include <windows.h>
#include <string>
#include <vector>

#include "nvpwr_ui_state.h"

namespace nvpwr {

/* The CLI name this project looks for next to itself and in a few known spots. */
constexpr wchar_t kMVoltExeName[] = L"mVolt+.exe";

/* ------------------------------------------------------------------ */
/* discovery                                                          */
/* ------------------------------------------------------------------ */

/*
    Resolves the mVolt+ executable.

    Order: explicit user override, then next to this program, then the project
    layout, then the user's Documents/Downloads. Returns an empty string when
    nothing is found, so the UI can say "not found" rather than failing later
    with a confusing launch error.
*/
std::wstring FindMVoltExecutable(const std::wstring& explicitPath = L"");

/* True when the resolved path actually exists and is a file. */
bool MVoltAvailable(const std::wstring& explicitPath = L"");

/* ------------------------------------------------------------------ */
/* read                                                               */
/* ------------------------------------------------------------------ */

/*
    Runs `mVolt+.exe --status` and parses the JSON it prints.

    Only the fields this project cares about are extracted; the parser is
    written to tolerate unknown/added keys so a future mVolt+ release does not
    break it. Returns false with `error` set when the process cannot be started
    or its output cannot be understood.
*/
struct MVoltSnapshot {
    bool  ok = false;

    /* Rail offsets in microvolts, exactly as mVolt+ reports them. */
    RailOffsets nvvdd{};
    RailOffsets msvdd{};

    /* Editable envelope reported by the driver, microvolts. */
    long long nvvddDeviceMinUv = 0;
    long long nvvddDeviceMaxUv = 0;
    long long nvvddDeviceStepUv = 0;
    long long msvddDeviceMinUv = 0;
    long long msvddDeviceMaxUv = 0;
    long long msvddDeviceStepUv = 0;

    /* Per-domain voltage request, millivolts. */
    VoltageDemand demand{};
    long long demandMinMv = 0;
    long long demandMaxMv = 0;

    /* Identity / context, for display and for detecting a different GPU. */
    std::wstring gpuName;
    std::wstring vbios;

    /* Power view, cross-checked against our own reading where useful. */
    bool  hasEnforcedPower = false;
    unsigned int enforcedMw = 0;
    bool  hasDefaultPower = false;
    unsigned int defaultMw = 0;

    std::wstring error;
    std::wstring rawJson;
};

bool QueryMVoltStatus(MVoltSnapshot& out, const std::wstring& explicitPath = L"");

/*
    Reads mVolt+'s *current* rail offsets into a VoltageTuning and returns it.

    This is what makes the workflow workable: the user fine-tunes voltage in
    mVolt+, then imports the result here so it can be saved into a slot and
    replayed at boot. Without an import step the user would have to retype four
    numbers per rail, which is exactly the friction this is meant to remove.
*/
bool ImportVoltageFromMVolt(VoltageTuning& out, std::wstring& error,
                            const std::wstring& explicitPath = L"");

/* ------------------------------------------------------------------ */
/* write                                                              */
/* ------------------------------------------------------------------ */

/*
    Applies rail offsets through mVolt+'s CLI.

    Only the rails named in `tuning` are touched; the other rail is left alone,
    which matches mVolt+'s own "unspecified controls are left untouched"
    contract. Values are converted from uV to mV with rounding to the nearest
    millivolt, and the caller is told when rounding occurred so the UI can warn
    instead of silently applying a slightly different number.
*/
bool ApplyVoltageViaMVolt(const VoltageTuning& tuning, std::wstring& error,
                          bool* rounded = nullptr,
                          const std::wstring& explicitPath = L"");
/*
    Applies clock offsets through the companion tool.

    Separate from the voltage call because mVolt+ takes them as separate options and leaves
    whatever is not named alone — so a replay that must not disturb the clocks passes neither,
    and one that must not disturb the rails passes only these.

    WHY the service replays these at all: until now it replayed power and voltage and stopped,
    so the core/memory/XBar/SYS offsets were written to the state file at every apply and then
    ignored at boot. After a reboot the user got the power ceiling back and silently lost the
    clocks. This closes that.

    SYS goes through the same CLI as the rest even though the GUI cannot touch it through NVAPI:
    for a replay the transport does not matter, only that the stored value reaches the driver.
*/
bool ApplyClocksViaMVolt(const ClockTuning& tuning, std::wstring& error,
                         const std::wstring& explicitPath = L"");

/* Resets both rails to zero offsets through mVolt+. */
bool ResetVoltageViaMVolt(std::wstring& error,
                          const std::wstring& explicitPath = L"");

/* ------------------------------------------------------------------ */
/* diagnostics                                                        */
/* ------------------------------------------------------------------ */

/* Last command line the bridge ran, for the activity log. Empty until used. */
std::wstring LastMVoltCommandLine();

/*
    Runs the executable with `--version` to prove it can actually be launched
    from this context (an interactive GUI and a LocalSystem service are different
    environments). Returns the version string on success.
*/
bool TestMVoltLaunch(std::wstring& versionOut, std::wstring& error,
                     const std::wstring& explicitPath = L"");

} /* namespace nvpwr */
