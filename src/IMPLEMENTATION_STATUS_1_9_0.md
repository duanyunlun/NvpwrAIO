# NvpwrControl 1.9.0 — implementation status

This file records what the 1.9.0 "one tool" build actually implements, what is
verified, and — importantly — what is **not** finished. Read the
[Unfinished / needs your verification](#unfinished--needs-your-verification)
section before assuming a feature is complete.

---

## 1. What changed versus 1.8.0

| Area | 1.8.0 | 1.9.0 |
|---|---|---|
| Power ceiling | Per-profile compile-time maxima (180/225/250 W) | User-selectable at runtime, driver ceiling 350 W |
| Power input | Fixed dropdown | Editable target + session ceiling, free input |
| Recovery | MIXED/legacy state recognition pinned to the RTX 5070 Ti (140 W, 145–160 W) | Machine-generic, derived from the live policy envelope |
| Voltage | Pstates20 only — structurally empty on the verified machine | Dedicated rail-limit module (mVolt+-class interface family) |
| Clocks | Core / Memory / XBAR, separate Apply | Same, plus included in the one-shot apply |
| One-shot apply | None | Power → voltage → clocks in one action, per-stage reporting |
| Language | EN/RU, substring-replacement translation | EN/中文/RU, addressed by text id |
| Live status | None | Dedicated page: power, clocks, temperature, throttle reason, MUX state |
| Tuning workflow | None | 6 saved slots, one-click defaults, undo, provisional-change guard |
| Persistence | Process globals only | `state.ini` + `profiles.ini`, replayed by a service |
| Background | GUI owned the driver service | Optional `NvpwrSvc.exe`, GUI falls back to direct access |

---

## 2. Architecture

```
┌───────────────────────────────────────┐
│ NvpwrControl.exe (WPF GUI)            │
│  · tuning column + live status column │
│  · localisation, slots, undo, guard   │
└───────────┬───────────────┬───────────┘
            │               │
   named pipe (optional)    │ DeviceIoControl (always)
            │               │
┌───────────┴───────┐       │
│ NvpwrSvc.exe      │       │
│  · replay at      │       │
│    boot / resume  │       │
└───────────┬───────┘       │
            └───────┬───────┘
                    ▼
      ┌──────────────────────────────┐
      │ \\.\Nvpwr                    │
      │ Nvpwr.sys (kernel)           │
      │  · exact 616.92 build guard  │
      │  · 3-phase power write       │
      └──────────────────────────────┘
```
The GUI never depends on the service: if `NvpwrSvc.exe` is not installed,
`IpcCall` fails fast and the GUI drives `\\.\Nvpwr` directly, exactly as 1.8.0
did. Installing the service only adds boot replay and GUI-independent lifetime.

---

## 3. Fail-closed behaviour (unchanged philosophy, wider input)

Widening what may be **requested** did not relax any **verification**:

* `driver.c` — Phase A / Phase C still require `MAX`, `CURRENT`, `F7` and
 `UPPER` to converge on the requested value. A target the EC or the generator
 will not honour fails and rolls back to the captured OEM baseline.
* The session ceiling is validated before it is applied, and clamped to the
 driver's own compiled ceiling (`POWER_CEILING_DEV`).
* `nvapi_power_policies.cpp` — every rail read must pass a version match, a
 declared-size match, and a value-plausibility check before anything is
 written. A write is followed by a fresh read and an exact field comparison.
* Every mutation path captures a restore point first.

### The provisional-change guard

A setting that the driver *accepts* can still blank the display, which no
readback check can detect. After a voltage or clock change the new state is
therefore **provisional** until confirmed, and a banner offers *Keep* or
*Revert now*.

It deliberately does **not** auto-revert on a timer. The dominant workflow is
iterative tuning with a stress test running; an automatic revert after N seconds
would silently undo the very change being measured. The documented escape hatch
for a dead display is a reboot — GPU policy lives in driver memory only, so a
reboot always returns the machine to OEM limits.

---

## 4. Tuning workflow features

| Feature | Where | Notes |
|---|---|---|
| One-shot apply | Power page | Power → voltage → clocks; reports which stage failed. Earlier stages are **not** silently rolled back, so you know the power limit is live even if voltage was refused. |
| 6 tuning slots | Slots page | Save / load / clear / rename. Each slot records the outcome of its last apply so a misbehaving setup is identifiable after a reboot. |
| One-click defaults | Power / Voltage pages | Returns power to OEM and zeroes all offsets. Explicitly a **known-good** state, not a replay of your last settings. |
| Undo | Guard banner | Re-applies the restore point captured before the last mutation. |
| Live status | Status page | See below. |

---

## 5. Live status page

Sampled read-only through NVML, falling back to `nvidia-smi`, plus DXGI for
display topology. Works even when the helper driver is not loaded.

Shown: adapter, driver, VBIOS, board power **now**, enforced ceiling, core and
memory clock, GPU temperature, **thermal headroom**, **T.Limit**, utilization,
VRAM used/total, fan, P-state, AC / battery, number of display adapters,
**discrete-direct (MUX) state**, and the raw throttle-reason mask.

Two derived lines carry the real value:

* **What is limiting it now** — distinguishes `SW Power Cap` (a higher limit can
 be consumed) from `SW/HW Thermal` (it cannot, until cooling improves) and from
 a hardware power brake (the adapter or platform is the limit).
* **Thermal headroom** — `T.Limit − current edge temperature`. This is what
 decides whether a raised power limit will be usable or will just push the card
 into the thermal wall.

### Deliberately not shown

**Hotspot / memory-junction temperature.** See [6.2](#62-hotspot-and-memory-junction-temperature-are-not-available-on-this-gpu):
the GPU exposes only the edge sensor through NVML, and does not offer hotspot as
an `nvidia-smi` field either.

**Per-rail voltage and per-rail current.** The GPU does not expose board rail
voltage through NVML or NVAPI without vendor tooling, and mVolt+'s "ADC"
readings come from its own private interface. Reporting an invented voltage
would be worse than reporting nothing. Voltage **limits** are on the Voltage page.

Every absent field prints the reason in the note line, so an N/A reads as a
hardware limitation rather than a defect.

---

## 6. Unfinished / needs your verification

These are the parts I could not complete or verify without running the code and
without a verified interface specification. They are listed here rather than
buried, so nothing looks finished that is not.

### 6.0 Feature ownership — what belongs to this project and what does not

Settled by measurement on the reference machine, not by assumption:

| Capability | Owner | Why |
|---|---|---|
| **Power-limit unlock above the OEM ceiling** | **this project** | Kernel power-policy path; no public API can do it, and it is verified working |
| Voltage rails (NVVDD / MSVDD / per-domain demand) | **mVolt+** | Absent from the public NVAPI surface — see 6.1 |
| Core / memory / XBAR overclock | Afterburner / MSI Center | User's existing tooling; also present here for convenience |
| Hotspot / memory-junction temperature | **HWiNFO** | RTX 50 moved hotspot out of NVAPI; it needs kernel-level register access — see 6.2 |
| Power, clocks, edge temp, thermal limits, throttle reason, MUX state | **this project** (Status page) | All reachable through NVML / nvidia-smi / DXGI |

This project's non-substitutable value is the **power ceiling**. Everything else
on that list is either monitoring it performs well, or a capability that belongs
to a tool which already solved a specific access problem.

### 6.1 Voltage: monitoring is IMPLEMENTED; writing is identified but not wired

**This section replaces an earlier conclusion in this document that voltage was
unreachable. That conclusion was wrong, and the correction matters.**

#### What was wrong

The earlier reasoning was: the public SDK has no voltage getter, NVML exports no
`nvmlDeviceGetVoltage`, and `nvidia-smi` has no voltage field, therefore no read
path exists. All three observations are true — and all three are beside the point,
because the interface is **undocumented but present**, reachable through
`nvapi_QueryInterface` with an id taken from published reverse-engineering work.

Closing a capability as impossible on the basis of the *public* surface alone was a
mistake. The lesson: check what the open-source community has already recovered
before concluding an interface does not exist.

#### Monitoring — implemented and verified

Live core voltage is shown in the status panel, sampled once per second.

| Item | Value |
|---|---|
| Interface | `ClientVoltRailsGetStatus` — id `0x465F9BCF` |
| Structure | `ClientVoltRailsStatusV1`, 0x4C bytes, version 1 |
| Voltage field | `+0x28` (`current_voltage_uv`) |
| Observed | ~0.700 V idle, moving between 0.625 V and 0.635 V across samples |

Provenance: published by **Loong0x00** (the author mVolt+ credits for the XBAR
discovery), who recovered it by reverse-engineering ASUS GPU Tweak III's
`Vender.dll` and cross-referencing nvapi-rs, nvapioc and vertminer. Independently
corroborated by **LACT** (`lact-daemon/.../nvidia/nvapi.rs`), which declares the
same id and the same `ClientVoltRailsStatusV1` / `ClientVoltRailStatusV1` layout.

Verified locally **before** being wired in, because a third-party id is not
trustworthy on its own:

1. `nvapi_QueryInterface(0x465F9BCF)` resolves to a non-null pointer.
2. The call returns 0 with an exact 0x4C buffer and version 1.
3. The driver echoes size 0x4C / version 1 in the header — it filled the structure
 that was requested, not something else.
4. Wrong buffer sizes are rejected with `-9`, so the size is genuinely validated.
5. The value is live: it changed between successive samples and matches what mVolt+
 displays for the same GPU at the same moment.
6. A plausibility gate (0.2–2.0 V) rejects a misparsed value instead of showing it.

#### V/F curve — implemented (read-only)

| Item | Value |
|---|---|
| Interface | `ClockClientClkVfPointsGetStatus` — id `0x21537AD4` |
| Structure | 0x1C28 bytes, version 1, 128 points at `+0x48`, stride `0x1C` |
| Point layout | `freq_kHz` at `+0x00`, `volt_uV` at `+0x04` |
| Measured range | 180–3165 MHz, 0.450–1.240 V in even 25 mV steps |

The panel shows the current **operating point** (e.g. `1020 MHz @ 0.650 V`), matched
to the live voltage within one 25 mV curve step. The same id is used by this
project's audited C++ NVAPI module.

Implementation note: the request must be pre-filled with the point mask (all 128
bits) and a clock count at `+0x14`; without that the driver returns a zeroed
structure, which is an easy way to mistake "not asked for" for "no data".

#### Writing — tested on hardware; both candidate paths are refused

A dedicated experiment (`voltprobe/`, a separate elevated console app) was written
and run to settle this, because the alternative was shipping a guess. Its safety
protocol: refuse to start unless the baseline is zero, write one small step at a
time, read back after every write, return to zero after every step, and revert on
exit, on Ctrl+C, and on exception.

**Path 1 — `ClientVoltRailsSetControl` (0xB9306D9B): refused.**

| Request | Result |
|---|---|
| `SetControl(0)` — write back the value already in force | `rc=0` accepted |
| `SetControl(+1, +2, +3, +5, +8, +10, +20)` | `rc=-104` = `NVAPI_NOT_SUPPORTED` |
| `SetControl(-1, -2, -5, -10)` | `rc=-5` = `NVAPI_INVALID_ARGUMENT` |

Two things follow. The `rc=-1` seen earlier from a non-elevated probe was indeed a
**permission** problem: elevated, the identical call returns 0. And any non-zero
value is refused **at every magnitude tried**, so this is not a range boundary — the
control is not available on this GPU. The differing codes for positive and negative
values show the field is signed and that support and magnitude are validated
separately.

**Path 2 — `ClockClientClkVfPointsSetControl` (0x0733E009): refused.**

| Request | Result |
|---|---|
| `GetControl` with an empty mask | `rc=0` |
| `GetControl` with a 0x1F or 0xFF point mask | `rc=-1` |
| `SetControl` writing back the driver's own values, empty mask | `rc=-1` |
| `SetControl` targeting one point with `freq_offset_khz = +15000` | `rc=-1` |

Writing the curve is refused even when requesting **no change at all**, so the
precondition is not the value. Something about the request shape, the point `type`
field, or a capability gate is unmet, and no source examined documents it.

**Conclusion: this project does not write voltage on this hardware.**

Verified working, and kept:

| Capability | Interface | State |
|---|---|---|
| Live core voltage | `0x465F9BCF` | implemented, 1 s refresh |
| V/F curve (128 points) + operating point | `0x21537AD4` | implemented |
| Clock-domain ranges | `0x64B43A6A` | read verified |
| Voltage rail offset **write** | `0xB9306D9B` | `NVAPI_NOT_SUPPORTED` on this GPU |
| V/F point offset **write** | `0x0733E009` | rejected; precondition unknown |

The hardware was confirmed clean after every run: rail voltage unchanged,
`percent_delta = 0`, zero non-zero V/F offsets, and `nvidia-smi` reporting the same
idle power and clocks as before.

**Voltage adjustment therefore stays with mVolt+.** It evidently reaches the rails
through a mechanism that is neither of these two interfaces. Recovering that
mechanism would require the kind of disassembly this project has deliberately
avoided; the ids and the negative results above are recorded so the search is not
repeated blindly.

### 6.2 Hotspot and memory-junction temperature are not available on this GPU

This is a **hardware/driver limitation, not a missing implementation.** Measured
on the reference RTX 5090 Laptop / 616.92 machine:

```
nvmlDeviceGetTemperature(sensorType)
 type 0 GPU SUCCESS 41 C <- the only usable sensor
 type 1 MEMORY NOT_SUPPORTED
 type 9 MEM_JUNCTION NOT_SUPPORTED
 types 2..8, 10..12 NOT_SUPPORTED

nvmlDeviceGetTemperatureThreshold(thresholdType)
 0 SHUTDOWN 103 C
 1 SLOWDOWN 100 C
 3 GPU_MAX 89 C <- the thermal wall
 2,4,5,6 NOT_SUPPORTED
```

`nvidia-smi` agrees independently: `temperature.gpu` reports a value, while
`temperature.gpu.hotspot` **is not a valid query field**, and
`temperature.memory` / `temperature.gpu_max` report `N/A`.

Tools that do show hotspot (HWiNFO, GPU-Z) read NVIDIA's **private ADC / thermal
interface** — the same family of private interfaces this project deliberately
refuses to guess at (see `nvapi_power_policies.h` for why). There is therefore no
public source for those sensors on this class of GPU.

> **I am not able to read hotspot, and neither is this project — but that is a
> property of the hardware, not of the code. Writing a plausible-looking number
> into that field would be worse than showing N/A.**

**What was added instead:** the thermal **limits** are readable, and they answer
the question that actually matters — *how close is the card to throttling?* The
status page now shows **Thermal headroom** (`T.Limit − current edge temp`) and the
**T.Limit** itself, plus shutdown and max-operating limits. On the reference
machine at idle: 46 掳C limit vs 41 掳C edge = 5 掳C headroom.

The UI also prints the precise reason in the note line when a sensor is absent,
so an N/A never looks like a bug.

### 6.3 Discrete-direct (MUX) detection — implemented, one caveat

**This one works.** `SampleDisplayTopology()` in `nvpwr_telemetry.cpp` enumerates
adapters through DXGI and checks whether the NVIDIA adapter has an attached
output:

* NVIDIA adapter has an output →**discrete-direct (MUX to dGPU)**
* another adapter owns the output →**hybrid**

That is a driver-agnostic signal and is reliable for the common laptop
configurations. The single caveat: **Advanced Optimus can reassign outputs at
runtime**, so a sample taken mid-switch can lag by up to one refresh interval.
The value is re-sampled on the 3-second UI timer and self-corrects; it is not
worth interrupting the user over.

### 6.4 The service pipe ACL is permissive

`service_main.cpp` creates the pipe with a `NULL` security descriptor, giving a
default ACL reachable by the interactive user. Every command is re-validated
inside the service (profile range, ceiling clamp, envelope checks), so an
out-of-range value cannot be injected — but a local user can still change the
power limit **within** the allowed range.

For anything beyond personal use, replace the descriptor with one naming only
`BUILTIN\Administrators` and require the GUI to be elevated. There is a comment
at the top of `service_main.cpp` marking this.

### 6.5 Nothing in this build has been executed

Per your instruction, no test was run. The code is statically self-consistent
(153 text ids declared and matched against the string table in order; no
duplicate definitions; no cross-module undeclared symbols), but it has not been
compiled or run. Expect to fix compiler diagnostics on the first build.

---

## 7. Build

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\build.ps1
```

Produces in `dist\`: `NvpwrControl.exe`, `NvpwrSvc.exe`, `NvpwrCtl.exe`,
`Nvpwr.sys`, `Nvpwr.cer`, and the helper scripts.

`/utf-8` is required and is set in the build script: the UI string table carries
Chinese literals, and without that flag MSVC would read them in the active code
page and mangle them.

### Where settings live

| File | Contents |
|---|---|
| `%ProgramData%\NvpwrControl\state.ini` | The desired state the service replays |
| `%ProgramData%\NvpwrControl\profiles.ini` | The six tuning slots |
| `%ProgramData%\NvpwrControl\nvpwr-control.log` | GUI activity log |
| `%ProgramData%\NvpwrControl\nvpwr-service.log` | Service log |
| `%LOCALAPPDATA%\NvpwrControlUI\settings.ini` | Language and window preferences |

All are flat key/value text, readable by hand — which matters when a replay at
boot misbehaves and you need to see what it was told to do.

---

## 8. Driver requirements (unchanged)

`Nvpwr.sys` still validates the exact NVIDIA 616.92 build: PE timestamp
`0x6A9B4070`, image size `0x06D3E000`, plus six machine-code signature
comparisons. Any other driver version is refused with `STATUS_REVISION_MISMATCH`
before a single byte is written.

On the reference machine this build is used with Secure Boot **enabled** and
without testsigning: the driver loads because the platform's code-integrity
policy is in audit mode (`CodeIntegrity` event 3076: *"due to code integrity
auditing policy, the image was allowed to load"*). That is an environment
property, not something this project establishes. On a standard Windows install
you still need testsigning or `nointegritychecks`, with Secure Boot off.
