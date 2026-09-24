# NvpwrControl 1.9.0 — Unified Laptop Tuner (Power + Voltage + Clocks)

One UI/source tree combining the power-control work, coherent XMG-style high-power contract, Blackwell clock/rail tuning, telemetry, recovery and the DPI/Unicode UI work.

> **Read `IMPLEMENTATION_STATUS_1_9_0.md` first.** It records what this build
> implements, what is verified, and — importantly — that the voltage interface
> contract is deliberately unfilled, which leaves the Voltage page reporting
> "unavailable" until the private interface ids and field layouts are supplied.
> It also carries a per-item list of what still needs verification.

## What 1.9.0 adds over 1.8.0

- **User-selectable power ceiling.** The per-profile maxima are no longer
  compile-time constants, so an already-unlocked machine can be pushed further
  without editing and rebuilding the driver. Requesting more does **not** relax
  the driver's convergence checks: a target the hardware will not honour still
  fails closed and rolls back.
- **One-shot apply.** Power limit → voltage offsets → clock offsets in a single
  action, reporting exactly which stage failed.
- **Tuning workflow.** Six saved slots, one-click restore to defaults, undo, and
  a provisional-change guard with an explicit *Keep* / *Revert now* choice.
- **Live status page.** Board power, enforced ceiling, clocks, temperature,
  utilization, VRAM, P-state, throttle reason and MUX state, sampled read-only
  through NVML (falling back to `nvidia-smi`). The throttle-reason line answers
  the question that matters: whether the card is power-limited or thermally
  limited.
- **Chinese interface** alongside English and Russian, addressed by text id
  rather than by substring replacement.
- **Background service** (`NvpwrSvc.exe`) that replays the saved state at boot.
  Optional — without it the GUI drives the driver directly, as 1.8.0 did.
- **Machine-generic recovery.** The MIXED/legacy-state logic no longer assumes
  the RTX 5070 Ti's 140 W ceiling, so a 175 W machine can classify and recover.

## GPU profiles / TGP

| GPU | Power menu | Backend |
|---|---:|---|
| RTX 5050 Laptop | OEM 115 W + 120–140 W | Nvpwr two-phase runtime policy; experimental; exact coherent 115 W OEM gate |
| RTX 5060 Laptop | OEM 115 W + 120–140 W | same |
| RTX 5070 Laptop | OEM 115 W + 120–140 W | same |
| RTX 5070 Ti Laptop | 145 W up to the session ceiling | Nvpwr; 145/150/160 W physically validated on the reference 616.92 machine; other values experimental |
| RTX 5080 Laptop | 175 W up to the session ceiling | Nvpwr experimental |
| RTX 5090 Laptop | 175 W up to the session ceiling | Nvpwr experimental |
| RTX 5080/5090 compatible XMG platform | 250 W | coherent XMG v8 19-target backend (Core/NVPCF + Entry13 + Entry14) |

The 250 W backend does not fall back to MAX/UPPER/F7. It requires the XMG v8
semantic runtime and the exact profile/capability state. The XMG driver binary is
not redistributed in this project.

The delivered high bound is `POWER_CEILING_DEV` (350 W). Bounds above the values
physically validated on a given model are marked experimental in the UI rather
than silently treated as equal.

## Advanced tuning

The GUI contains a single Advanced Tuning panel.

- **Core offset** — Pstates20, driver-reported min/max, SET + GET readback.
- **Memory offset** — Pstates20, driver-reported min/max. `+5000 MHz` is available only if the installed GPU/driver reports it. On the saved reference RTX 5070 Ti / 616.92 capture the actual range is `-1000..+3000 MHz`.
- **NVVDD offset** — enabled only when Pstates20 exposes an editable voltage-delta range. It is not fabricated when the GPU/driver reports no such control. On the reference machine Pstates20 reports `voltages=0`, so this control is inert there; the rail-limit module exists for that case.
- **XBAR offset** — ClockDomains private NvAPI. Full buffer read, dynamic layout discovery, audited `0x304` entry stride requirement, SET, exact readback, rollback.
- **XBAR-domain MSVDD request offset** — same ClockDomains transaction; UI gated to RTX 5070 Ti / 5080 / 5090 profiles and only after the live XBAR layout validates. Absolute software guard `-100..+100 mV`.
- **GPC→XBAR propagation ratio** — validates the semantic GPC(0)→XBAR(1), bidirectional relationship before SET; range `0.0..2.0`, U16.16 readback.
- **V/F information** — read-only capability/control-table telemetry. This build does not expose a generic V/F writer because the current 616.92 status layout differs from the smaller public reference layout and a blanket point write would not be fail-closed.
- **ADC / rail information** — read-only info/status capability telemetry. Physical MSVDD ADC is not claimed.
- **Physical XBAR clock** — read with CLK_MEASURE_FREQ when available.

Every writable OC stage does `GET -> validate -> SET -> GET/readback`. Each writable subsystem captures its own session baseline before its first mutation. A failed later stage attempts to restore every captured subsystem baseline.

## UI / recovery

- Unicode Win32 UI, `/utf-8`, Segoe UI (Microsoft YaHei UI when Chinese is selected — Segoe UI relies on registry FontLink fallbacks that are absent on non-Chinese Windows).
- EN / 中文 / RU interface, addressed by text id.
- Per-Monitor DPI Awareness V2 and `WM_DPICHANGED` relayout.
- **Restart Nvpwr driver**: refuses to unload a non-stock helper unless OEM restore succeeds first.
- **Restart NVIDIA device**: first restores Nvpwr OEM power plus the captured NvAPI tuning baseline, then launches the supported Windows PnP disable/enable helper. The screen can go black during the reset.
- User log: `C:\ProgramData\NvpwrControl\nvpwr-control.log` (service log alongside it).
- Kernel log prefix: `[NVPWR]`.
- `collect-debug.ps1` packages the app log, service state, BCD state, signatures, Nvpwr status, NVIDIA power/clock data and display-device state.

## Driver signing

This project does not implement DSE/Secure-Boot bypasses. `Nvpwr.sys` remains a test-signed development driver. The supported development flow is Secure Boot disabled manually in UEFI + Windows TESTSIGNING enabled + reboot, or a legitimate production signing path.

On the reference machine the driver loads with Secure Boot **enabled** and
testsigning **off**, because that platform's code-integrity policy is in audit
mode. That is an environment property, not something this project establishes.

The user-mode NvAPI tuning module is independent of whether `Nvpwr.sys` loads.

## Build

Run elevated PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
cd C:\NvpwrControl_61692_v1_8_0_unified_blackwell_tuner
.\build.ps1
```

Output:

```text
dist\NvpwrControl.exe
dist\NvpwrSvc.exe
dist\NvpwrCtl.exe
dist\Nvpwr.sys
dist\restart-nvidia-device.ps1
dist\collect-debug.ps1
```

The source is statically audited in this environment, but Windows/WDK compilation must be performed on Windows.
