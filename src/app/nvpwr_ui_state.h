#pragma once
/*
    nvpwr_ui_state.h — shared UI/session state model (1.9.0 "one tool" build)

    WHERE: included first by main.cpp (GUI) and by the service layer.
    WHAT:  the persisted "desired state" record plus the language/session enums
           that the 1.9.0 GUI, the background service and the CLI all agree on.
    WHY:   1.8.0 kept every setting in process globals, so closing the window
           silently lost the applied tuning and there was nothing for a service
           to replay after a reboot. This header is the single source of truth
           for what the user asked the GPU to do.

    PERSISTENCE SEMANTICS (important):
      GPU power/voltage policy lives in kernel memory owned by the NVIDIA driver.
      It does NOT survive a driver reload or a reboot. "Persist across reboot"
      therefore means "a service re-applies these values at boot", never "the
      GPU remembers". If the replay fails, the service must report why rather
      than silently leaving the machine at OEM limits.
*/

#include <string>
#include <vector>
#include <cstdint>

namespace nvpwr {

/* ---------------- language ---------------- */

enum class Lang : int {
    English = 0,
    Chinese = 1,
    Russian = 2,
};

/* Kept stable for settings.ini compatibility: 1.8.0 wrote [Interface] Russian=0/1. */
const wchar_t* LangToIniValue(Lang lang);
Lang LangFromIniValue(const std::wstring& value);

/* ---------------- power ---------------- */

struct PowerTarget {
    unsigned int milliwatts = 0;   /* 0 = OEM stock (no unlock) */
    unsigned int ceilingMw  = 0;   /* user-selected high bound; 0 = driver default */
    unsigned int profile    = 0;   /* NVPWR_GPU_PROFILE value detected for the GPU */

    bool IsStock() const { return milliwatts == 0u; }
};

/* ---------------- voltage (mVolt+-class rail offsets) ----------------
   These mirror the four per-rail offset controls that mVolt+ exposes and that
   the 616.92 driver accepts on this class of GPU. Units are the ones the
   driver reports: voltage in microvolts, clock offsets in kHz.
   All fields are OFFSETS, never absolute setpoints: the driver applies them
   against its own baselines and may clamp or ignore them.               */

struct RailOffsets {
    long long vminUv = 0;
    long long relUv  = 0;
    long long altUv  = 0;
    long long ovUv   = 0;

    bool IsZero() const {
        return vminUv == 0 && relUv == 0 && altUv == 0 && ovUv == 0;
    }
};

/* Per-domain voltage demand (Core / Xbar / SYS / Video), in millivolts. */
struct VoltageDemand {
    long long coreMv  = 0;
    long long xbarMv  = 0;
    long long sysMv   = 0;
    long long videoMv = 0;

    bool IsZero() const {
        return coreMv == 0 && xbarMv == 0 && sysMv == 0 && videoMv == 0;
    }
};

/*
    WHO APPLIES VOLTAGE — the one architectural decision worth stating plainly.

    This project cannot write voltage: the public NVAPI surface has no voltage
    setter, and Pstates20 carries no voltage domain on the reference GPU
    (measured: 0 domains, and a +25 mV rail change moved 0 bytes in the Pstates20
    buffer). mVolt+ reaches those rails through a reverse-engineered private
    interface.

    So the intended arrangement is: this program orchestrates, mVolt+ executes.
    The service replays the saved voltage at boot by invoking mVolt+'s CLI, which
    is what makes "one place to do everything" work without pretending this
    project can do something it cannot.
*/
enum class VoltageApplier : int {
    /* Do not touch voltage on this machine. */
    None = 0,
    /* Invoke the companion tool (mVolt+) with the stored offsets. Default. */
    CompanionTool = 1,
};

struct VoltageTuning {
    bool        enabled = false;
    RailOffsets nvvdd{};
    RailOffsets msvdd{};
    VoltageDemand demand{};

    /* How this tuning is meant to reach the GPU. Kept in the record so a saved
       slot is self-describing: a slot saved with CompanionTool cannot later be
       applied by a code path that has no companion available without saying so. */
    VoltageApplier applier = VoltageApplier::CompanionTool;

    bool IsZero() const {
        return nvvdd.IsZero() && msvdd.IsZero() && demand.IsZero();
    }
};

/* ---------------- clock offsets ----------------
   Kept in the same record as voltage so one "Apply" is one atomic intent.
   Overclocking of core/memory is deliberately NOT the primary path here (the
   user drives that with Afterburner/MSI Center); these exist so the panel can
   show and restore what is active, and so the service can replay it. */

struct ClockTuning {
    bool enabled = false;
    long coreOffsetMhz   = 0;
    long memoryOffsetMhz = 0;
    long xbarOffsetMhz   = 0;
    /* SYS / host-interface domain. Replayed through mVolt+ like the others, but it is the one
       the NVAPI ClockDomains path does not address, so it carries a field of its own. */
    long sysOffsetMhz    = 0;

    bool IsZero() const {
        return coreOffsetMhz == 0 && memoryOffsetMhz == 0 &&
               xbarOffsetMhz == 0 && sysOffsetMhz == 0;
    }
};

/* ---------------- full desired state ---------------- */

struct DesiredState {
    /* schema version so a future build can migrate an older state file */
    unsigned int schema = 2;

    bool         powerEnabled = false;
    PowerTarget  power{};
    VoltageTuning voltage{};
    ClockTuning  clock{};

    /*
        Companion tool used for voltage (mVolt+).

        Path is stored explicitly rather than re-discovered at boot: a service
        starting before the user logs in has a different notion of "next to the
        program" than an interactive shell, and silently picking a different
        binary than the one the user tested would be worse than failing loudly.
        Empty means "search the usual locations".
    */
    std::wstring mvoltPath;

    /*
        The factory power wall, in watts. Recorded ONCE, then never rewritten.

        WHY this has to be a record and not a live reading: a live reading of the
        power limit cannot tell a factory wall from one this program raised — both
        are just "UPPER" in driver memory. The only way to know what the card
        shipped with is to write it down while the answer is still certain, before
        anything has touched the wall, and then refuse to touch that record again.

        WHY both entry points carry it: the service starts at boot and the GUI is
        launched by hand, and either can be the first to run on a given boot. The
        window in which the value is knowable is exactly "before the first apply",
        so whichever process starts first while the field is still absent fills it
        in. Leaving it to the GUI meant a boot where the service replayed a raised
        wall first, after which the GUI could only read a value already lost.

        powerFloorProfile is the GPU profile the value was captured for. A different
        card, or a VBIOS change, invalidates the record — and that comparison is the
        ONLY reason it is ever rewritten.
    */
    unsigned int powerFloorW = 0;
    unsigned int powerFloorProfile = 0;

    /* Auto-start behaviour (mirrors the tray menu) */
    bool startWithWindows = false;
    bool startMinimized   = false;

    bool IsEmpty() const {
        return !powerEnabled && !voltage.enabled && !clock.enabled;
    }
};

/* ---------------- configuration slots ----------------
   The dominant workflow for this tool is iterative tuning: apply, measure,
   adjust, apply again. Without slots every experiment has to be reconstructed
   by hand, and a bad experiment has no way back except a reboot. A slot is a
   complete, named tuning setup that can be saved from the live controls, loaded
   back, or reset to defaults in one action.                                     */

constexpr int kSlotCount = 6;          /* 1..6 user slots */
constexpr int kSlotNameMax = 24;       /* characters, excluding terminator */

struct ConfigSlot {
    bool         used = false;
    std::wstring name;                 /* user-visible label */
    std::wstring savedAt;              /* ISO-ish local timestamp, display only */
    DesiredState state{};              /* the full tuning payload */

    /* Result of the last attempt to apply this slot. Kept so the UI can show
       "known good" vs "last attempt failed" per slot instead of making the user
       remember which one misbehaved. */
    bool         lastApplyOk = false;
    std::wstring lastApplyNote;
};

struct ProfileStore {
    unsigned int schema = 1;
    ConfigSlot   slots[kSlotCount]{};
};

/*
    A conservative, explicitly safe configuration: power returned to OEM and all
    voltage/clock offsets zeroed. This is what "one-click restore to defaults"
    applies, and what the safety watchdog falls back to.

    Deliberately NOT "the user's previous settings": when someone reaches for
    defaults after a bad experiment they want a known-good state, not a replay of
    whatever produced the problem.
*/
inline DesiredState DefaultState() {
    DesiredState s{};
    s.schema = 2;
    s.powerEnabled = false;
    s.power = PowerTarget{};          /* 0 mW == OEM stock */
    s.voltage.enabled = false;
    s.voltage.nvvdd = RailOffsets{};
    s.voltage.msvdd = RailOffsets{};
    s.voltage.demand = VoltageDemand{};
    s.voltage.applier = VoltageApplier::CompanionTool;
    s.clock.enabled = false;
    s.clock.coreOffsetMhz = 0;
    s.clock.memoryOffsetMhz = 0;
    s.clock.xbarOffsetMhz = 0;
    return s;
}

/* ---------------- safety (fail-closed) ----------------
   Every mutation path in this project is already fail-closed at the driver and
   NvAPI level: a value that does not converge is rolled back rather than
   accepted. What the tuning workflow adds is the case the low-level checks
   cannot see - a setting that IS accepted by the driver but makes the machine
   unusable (black screen, driver reset loop). The guard for that is a
   confirmation watchdog: after an apply, the new state is provisional until the
   user confirms it, and an unconfirmed state is reverted automatically.       */

enum class SafetyMode {
    /* Revert unless the user confirms within the timeout. Used for voltage and
       clock changes, which can take the display down. */
    ConfirmRequired,
    /* Power-limit only: the value cannot hang the display on its own, so no
       confirmation window is imposed. Still rolled back on a failed readback. */
    Immediate,
};

constexpr unsigned int kConfirmTimeoutSec = 15;
constexpr unsigned int kConfirmTimeoutSecVoltage = 10;

} /* namespace nvpwr */

