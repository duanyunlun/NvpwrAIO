#pragma once
/*
    nvpwr_telemetry.h — live environment status panel data.

    WHERE: user-mode, read-only. Loads NVML (nvidia-smi's own library) and falls
           back to DXGI. Never writes anything and never depends on the kernel
           helper, so the panel still works when the driver is not loaded.
    WHY:   tuning blind is how people conclude "the unlock did nothing". The
           numbers that answer that question - actual board power versus the
           enforced ceiling, the throttle reason, temperature headroom, and
           whether the discrete GPU is driving the panel - are all readable
           without touching the tuning paths.

    HONESTY RULES for this module:
      * A reading the driver does not provide is reported unavailable, never as
        zero and never as a guess. `has*` flags exist precisely so the UI can
        print "N/A" instead of a plausible-looking number.
      * Every unavailable field carries the reason it is unavailable, so a gap
        is never mistaken for a bug in this tool.

    WHY THERE IS NO HOTSPOT OR MEMORY-JUNCTION TEMPERATURE HERE
      Measured on the reference RTX 5090 Laptop / 616.92 machine: NVML supports
      exactly one temperature sensor there. nvmlDeviceGetTemperature returns
      SUCCESS for sensor type 0 (edge) only; sensor types 1..12 - including
      MEMORY (1) and MEMORY_JUNCTION (9) - all return NVML_ERROR_NOT_SUPPORTED.
      nvidia-smi agrees: `temperature.gpu` reports a value while
      `temperature.gpu.hotspot` is not a valid query field, and
      `temperature.memory` / `temperature.gpu_max` report N/A.

      Tools that DO show hotspot (HWiNFO, GPU-Z) read NVIDIA's private ADC /
      thermal interface - the same family of private interfaces this project
      deliberately does not guess at (see nvapi_power_policies.h). So on this
      class of GPU there is no public source for those sensors, and reporting an
      invented number would be worse than reporting nothing.

      What replaces it: the thermal LIMITS are available
      (nvmlDeviceGetTemperatureThreshold), and those are what actually answer
      "how close am I to throttling". Thermal headroom = speed threshold minus
      current edge temperature.
*/

#include <windows.h>
#include <string>

namespace nvpwr {

/*
    GPU throttle reason bits (nvidia-smi `clocks_throttle_reasons`).
    A noisy, repeated "SW Power Cap" at a stable clock means the card is
    power-limited; "SW Thermal" means it is thermally limited and raising the
    power limit will not help. Distinguishing those two is the whole point of
    showing this.
*/
enum ThrottleBit : unsigned long long {
    ThrottleGpuIdle          = 1ULL << 0,
    ThrottleApplicationsClk  = 1ULL << 1,
    ThrottleSwPowerCap       = 1ULL << 2,
    ThrottleHwSlowdown       = 1ULL << 3,
    ThrottleSyncBoost        = 1ULL << 4,
    ThrottleSwThermal        = 1ULL << 5,
    ThrottleHwThermal        = 1ULL << 6,
    ThrottleHwPowerBrake     = 1ULL << 7,
    ThrottleDisplayClk       = 1ULL << 8,
    ThrottleReliability      = 1ULL << 9,
    ThrottleBoardLimit       = 1ULL << 10,
    ThrottleLowUtilization   = 1ULL << 11,
};

struct EnvStatus {
    /* --- identity --- */
    std::wstring gpuName;
    std::wstring driverVersion;
    std::wstring vbiosVersion;

    /* --- live readings --- */
    bool  hasPowerDraw = false;      double powerDrawW = 0.0;
    bool  hasPowerLimit = false;     double enforcedLimitW = 0.0;
    bool  hasCoreClock = false;      double coreClockMhz = 0.0;
    bool  hasMemoryClock = false;    double memoryClockMhz = 0.0;
    bool  hasTemp = false;           double tempC = 0.0;
    bool  hasHotspot = false;        double hotspotC = 0.0;
    bool  hasUtilization = false;    double utilizationPct = 0.0;
    bool  hasMemoryUsed = false;     double memoryUsedMb = 0.0;
    bool  hasMemoryTotal = false;    double memoryTotalMb = 0.0;
    bool  hasFanPct = false;         double fanPct = 0.0;
    bool  hasPstate = false;         unsigned int pstate = 0;

    /*
        Thermal limits. These are the substitute for a hotspot reading: they say
        how much headroom exists before the card throttles, which is the actual
        question behind "will a higher power limit be usable".
    */
    bool  hasSpeedThreshold = false; double speedThresholdC = 0.0;   /* SW slowdown */
    bool  hasShutdownThreshold = false; double shutdownThresholdC = 0.0;
    bool  hasMaxOperatingTemp = false; double maxOperatingC = 0.0;   /* VBIOS target */

    /* --- limiting --- */
    bool  hasThrottle = false;       unsigned long long throttleBits = 0;

    /* --- platform --- */
    bool  onAcPower = false;         bool acKnown = false;
    bool  batteryPctKnown = false;   double batteryPct = 0.0;

    /*
        Discrete-direct (MUX) state.

        Detection is DXGI-based: the NVIDIA adapter is enumerated and checked for
        an attached output. In hybrid mode the iGPU owns the panel and the dGPU
        has no attached output; in discrete-direct mode the dGPU drives it. That
        is a driver-agnostic signal and reliable for the common configurations.

        The one caveat worth knowing: Advanced Optimus can reassign outputs at
        runtime, so a sample taken mid-switch can lag by up to one refresh
        interval. The value is re-sampled on the UI timer and self-corrects.
    */
    bool  discreteDirectKnown = false;
    bool  discreteDirect = false;
    unsigned int adapterCount = 0;

    /* Which backend produced the readings, for the technical log. */
    std::wstring source;
    std::wstring error;

    /* Reason a specific reading is absent, e.g. hotspot. Empty when everything
       requested was available. */
    std::wstring unavailableNote;

    bool AnyReading() const {
        return hasPowerDraw || hasCoreClock || hasTemp || hasThrottle;
    }

    /* Degrees of headroom before the software slowdown threshold, when known. */
    bool HasThermalHeadroom() const {
        return hasSpeedThreshold && hasTemp;
    }
    double ThermalHeadroomC() const {
        return hasSpeedThreshold ? (speedThresholdC - tempC) : 0.0;
    }
};

/* Read-only sample. Safe to call from the UI thread; it is expected to be called
   on a timer, so it must stay cheap. */
bool SampleEnvironment(EnvStatus& out);

/* Human-readable throttle reason derived from the raw bits. */
std::wstring DescribeThrottle(unsigned long long bits);

/*
    One-line interpretation of what is currently limiting the GPU. This is the
    sentence that tells the user whether raising the power limit can help at all.
*/
std::wstring DescribeLimiter(const EnvStatus& s);

} /* namespace nvpwr */
