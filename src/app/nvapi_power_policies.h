#pragma once
/*
    nvapi_power_policies.h — rail voltage-limit control ("mVolt+ class")

    WHERE: user-mode module of NvpwrControl.exe. No kernel driver involved.
    WHAT:  reads and writes the driver's per-rail voltage-LIMIT policy
           (VMIN / REL / ALT-OP / OV offsets for NVVDD and MSVDD) plus the
           per-domain voltage-demand request, using the same NVAPI interface
           family that mVolt+ uses.

    WHY THIS MODULE EXISTS
      Nvprw's existing nvapi_tuner.cpp drives voltage through Pstates20. On the
      verified RTX 5090 Laptop / 616.92 machine Pstates20 reports
      `voltages=0` (measured, see the 1.9.0 notes), so that path is structurally
      incapable of touching voltage there — the control is greyed out because the
      driver exposes no voltage domain through that interface. mVolt+ reaches the
      same rails through the client power-policy / rail-limit interfaces instead.
      This module implements that second family so voltage tuning lives in the
      same tool as the power limit.

    ==========================================================================
    INTERFACE CONTRACT — READ THIS BEFORE ASSUMING THE FEATURE IS BROKEN
    ==========================================================================
    mVolt+ is distributed as a binary only; its GitHub repository contains the
    README and user guide but no source. The private interface ids below are
    therefore NOT reproduced from published source, and this build intentionally
    ships them ZEROED rather than guessed.

    Guessing a private interface id is not a harmless mistake: nvapi_QueryInterface
    returns a raw function pointer, and calling an unrelated entry point with a
    buffer laid out for a different structure is exactly the failure mode that
    corrupts driver state. A guess that "looks plausible" cannot be distinguished
    from a correct one without running it on the target machine.

    HOW TO COMPLETE THIS MODULE (one place, no other change required):
      1. Determine the real ids (e.g. from an authoritative reverse-engineered
         binding for the 596.xx/616.xx driver generation, or by tracing the
         mVolt+ process).
      2. Fill in g_contract below.
      3. Rebuild. The probe will then validate each interface against its
         expected version/status word at runtime.

    FAIL-SAFE BEHAVIOUR (implemented, and the reason zeroing is acceptable):
      * ProbeNvapiVoltage() binds each interface id; an id of 0 means "not
        available", not "call entry 0".
      * Every GET result is validated: the interface version word must equal the
        constant for that structure, and the returned device range must be
        self-consistent (min < max, step > 0, values in 0..2 V).
      * If validation fails, VoltageWritable stays false and the UI reports the
        feature as unavailable for this GPU/driver. Nothing is ever written.
      * A SET is only attempted for interfaces that already passed validation,
        is followed by a fresh GET, and is compared field by field. On mismatch
        or partial failure the whole captured baseline is restored.

      So the worst case with an unfilled contract is "voltage panel shows
      unavailable with a reason", never a blind write.

    UNITS
      Voltages are microvolts (uV) internally because that is what the driver
      reports; the UI converts to mV. All rail fields are OFFSETS against the
      driver's own baselines, never absolute setpoints.
*/

#include <windows.h>
#include <string>
#include <vector>

namespace nvpwr {

/* ------------------------------------------------------------------ */
/* Interface contract                                                 */
/* ------------------------------------------------------------------ */

/*
    Field map for one rail status/control buffer.

    This is part of the contract for the same reason the ids are: reading a
    voltage out of the wrong offset produces a plausible-looking number, and
    writing an offset to the wrong place is a real hazard. Keeping the layout in
    the contract means "we know the ids but not the layout" cannot silently
    degrade into "we guessed the layout".

    A field map of all zeros means "layout not established" and disables the
    rail rather than falling back to a guess.
*/
struct RailFieldMap {
    unsigned int offsetVminUv      = 0;  /* signed 32-bit */
    unsigned int offsetRelUv       = 0;
    unsigned int offsetAltUv       = 0;
    unsigned int offsetOvUv        = 0;
    unsigned int offsetDeviceMinUv = 0;
    unsigned int offsetDeviceMaxUv = 0;
    unsigned int offsetDeviceStepUv= 0;
    unsigned int offsetEffMinUv    = 0;
    unsigned int offsetEffMaxUv    = 0;

    bool IsEstablished() const {
        /* Minimum viable layout: the four editable offsets plus the device
           range that constrains them. */
        return offsetVminUv != 0 && offsetRelUv != 0 &&
               offsetDeviceMinUv != 0 && offsetDeviceMaxUv != 0;
    }
};

/*
    Expected structure versions. These are validated against the version word
    the driver writes back, so a wrong value here degrades to "unavailable"
    rather than to a bad write. Fill in together with the ids.
*/
struct NvVoltageContract {
    /* --- ids handed to nvapi_QueryInterface; 0 = not bound --- */
    unsigned int idNvvddInfo      = 0;
    unsigned int idNvvddGetStatus = 0;
    unsigned int idNvvddSetStatus = 0;
    unsigned int idMsvddInfo      = 0;
    unsigned int idMsvddGetStatus = 0;
    unsigned int idMsvddSetStatus = 0;
    unsigned int idVoltageDemandGet = 0;
    unsigned int idVoltageDemandSet = 0;

    /* --- expected version words for each returned structure --- */
    unsigned int verRailInfo    = 0;
    unsigned int verRailStatus  = 0;
    unsigned int verDemand      = 0;

    /* --- declared buffer sizes; the driver reports its own size too, and a
           mismatch is treated as "unavailable" --- */
    unsigned int sizeRailInfo   = 0;
    unsigned int sizeRailStatus = 0;
    unsigned int sizeDemand     = 0;

    /* --- field layouts; all-zero disables the corresponding control --- */
    RailFieldMap railStatusMap{};
    RailFieldMap railInfoMap{};
};

/* The single place to complete this module. Every field zeroed on purpose —
   see the contract note above. */
extern NvVoltageContract g_contract;

/* ------------------------------------------------------------------ */
/* Observed state                                                     */
/* ------------------------------------------------------------------ */

struct RailSupport {
    bool available = false;         /* interface bound AND validated */
    bool writable  = false;         /* SET interface present and validated */

    /* device capabilities */
    long long deviceMinUv = 0;
    long long deviceMaxUv = 0;
    long long deviceStepUv = 0;

    /* currently applied offsets */
    long long vminUv = 0;
    long long relUv  = 0;
    long long altUv  = 0;
    long long ovUv   = 0;

    /* editable offset envelope reported for this rail */
    long long offsetMinUv = 0;
    long long offsetMaxUv = 0;

    /* driver-evaluated resulting limit, read-only */
    long long effectiveMinUv = 0;
    long long effectiveMaxUv = 0;
};

struct VoltageDemandState {
    bool available = false;
    bool writable  = false;
    long long minMv = 0;
    long long maxMv = 0;
    long long coreMv  = 0;
    long long xbarMv  = 0;
    long long sysMv   = 0;
    long long videoMv = 0;
};

struct VoltageState {
    bool nvapiReady = false;
    std::wstring error;

    RailSupport nvvdd{};
    RailSupport msvdd{};
    VoltageDemandState demand{};

    /* True only when at least one rail or the demand block is usable. */
    bool AnyAvailable() const {
        return nvvdd.available || msvdd.available || demand.available;
    }
    bool AnyWritable() const {
        return nvvdd.writable || msvdd.writable || demand.writable;
    }
    /* Human-readable explanation of what is missing, for the UI. */
    std::wstring MissingReason() const;
};

/* ------------------------------------------------------------------ */
/* Requests                                                           */
/* ------------------------------------------------------------------ */

struct RailOffsetRequest {
    bool      set = false;
    long long vminUv = 0;
    long long relUv  = 0;
    long long altUv  = 0;
    long long ovUv   = 0;
};

struct VoltageRequest {
    RailOffsetRequest nvvdd{};
    RailOffsetRequest msvdd{};

    bool setDemand = false;
    long long coreMv  = 0;
    long long xbarMv  = 0;
    long long sysMv   = 0;
    long long videoMv = 0;

    bool AnySet() const {
        return nvvdd.set || msvdd.set || setDemand;
    }
};

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

/*
    Read-only. Never issues a SET. Safe to call on any machine; on an
    unsupported driver it returns false with `out.error` explaining why.
*/
bool ProbeNvapiVoltage(VoltageState& out);

/*
    Applies only the offsets explicitly requested. Every interface is
    re-validated immediately before use, each value is range-checked against the
    driver-reported envelope, the SET is followed by a fresh GET and an exact
    field comparison, and any failure restores the baseline captured on the
    first mutation of this process.

    Returns false and populates `error` without having changed anything when the
    request cannot be satisfied.
*/
bool ApplyNvapiVoltage(const VoltageRequest& request, VoltageState& post,
                       std::wstring& error);

/*
    Restores the rail/demand values captured before the first successful voltage
    mutation in this process. No-op (followed by a probe) when nothing was
    mutated.
*/
bool ResetNvapiVoltage(VoltageState& post, std::wstring& error);

/*
    True when a voltage mutation has been made in this process and a usable
    baseline exists, i.e. when ResetNvapiVoltage can do real work.
*/
bool HasVoltageBaseline();

/* Formatted multi-line report for the Technical details pane. */
std::wstring FormatNvapiVoltage(const VoltageState& s);

} /* namespace nvpwr */
