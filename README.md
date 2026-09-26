# NvpwrAIO

> **TGP unlocker and voltage/frequency tuner for NVIDIA laptop GPUs**, built around a kernel
> driver that edits NVIDIA's live power-policy objects in memory.

**English** · [中文](README.zh-CN.md)

---

## What this is

Laptop GPUs enforce their Total Graphics Power (TGP) ceiling inside the NVIDIA kernel driver,
not in a file. MSI Afterburner and similar tools can offset core and memory clocks, but on a
laptop nothing in user mode can raise the TGP ceiling past what the OEM shipped — the setting
does not exist anywhere writable.

This project adds a small kernel driver that finds the driver's live power-policy objects,
changes the policy fields its native generator reads, and then calls NVIDIA's own setters so the
change goes through the same code path the driver would use itself.

Around that driver sit a WPF interface, a command-line tool, and a service that re-applies the
settings after a reboot.

### What "the settings do not persist" actually means

Everything this project changes lives in driver memory. It is gone on reboot, and it is also
gone on a display-driver reload — which happens without a reboot more often than people expect.
Measured on the reference machine: core +200 MHz and memory +500 MHz applied through the
companion CLI read back as **0** after the display device was restarted, so this is not specific
to the power ceiling.

That is the whole reason the service exists. It records the factory wall once, then re-applies
power, voltage and clocks after the desktop is up.

---

## Components

| Binary | Role |
|---|---|
| `Nvpwr.sys` | Kernel driver. Resolves `nvlddmkm.sys` in memory and edits the power-policy objects. |
| `NvpwrControl.exe` | WPF front end. Power, voltage rails, clock offsets, presets, prerequisite checks. |
| `NvpwrCtl.exe` | Command-line tool. Same IOCTLs, useful for scripts and for diagnosis. |
| `NvpwrSvc.exe` | Background service. Records the factory wall, replays settings after logon. |

The GUI and the CLI both talk to `\\.\Nvpwr` directly. They do not talk to each other, and the
service is not required for either to work — it only makes settings survive a reboot.

---

## Requirements

### Driver version — read this first

**The kernel driver is pinned to one exact build of `nvlddmkm.sys`.** It validates the PE
identity and six machine-code signatures before it touches anything, and returns
`STATUS_REVISION_MISMATCH` if they do not match.

| | |
|---|---|
| Validated on | NVIDIA driver **616.92** |
| PE timestamp | `0x6A9B4070` |
| SizeOfImage | `0x06D3E000` |

**It will refuse to run on any other driver version, by design.** Run `NvpwrCtl.exe diagnose` first in
that case — it prints the actual PE identity, whether it matched a table entry, and if so
**which signature failed and at which RVA**.

 Porting it to a new driver is
a reverse-engineering task with a known checklist — see
[docs/PORTING_TO_A_NEW_DRIVER.md](docs/PORTING_TO_A_NEW_DRIVER.md).

### Machine prerequisites

All three must hold, and the interface shows them as five status chips on the status bar:

| Requirement | Why |
|---|---|
| **Secure Boot disabled** | EfiGuard cannot install while it is on |
| **VBS / Virtualization-Based Security disabled** | With VBS on, the DSE patch does not take effect |
| **Booted through EfiGuard** | The driver cannot load without DSE temporarily disabled |

See [docs/DEPLOYMENT_AND_EFIGUARD.md](docs/DEPLOYMENT_AND_EFIGUARD.md) — **parts of the setup
must be built and installed per machine**, including `EfiDSEFix.exe`, which cannot be taken from
EfiGuard's official release.

---

## Quick start

```
1. Build or obtain the release package          →  release/
2. Install the test certificate for Nvpwr.sys   →  right-click the .cmd, Run as administrator
3. Install EfiGuard on the ESP, add a boot entry, boot through it
4. Run NvpwrControl.exe as administrator
5. Set the power target, voltage rails and clocks, then click 应用
6. Install the service if you want it to survive a reboot
```

The interface is in Chinese. `docs/使用说明.txt` is the user manual, and
`release/流程说明.md` records how the pieces fit together.

---

## Repository layout

```
src/          Source. driver/ (kernel), gui/ (WPF), cli/, service/, app/ (shared), shared/
release/      The complete distributable package, binaries included
tools/        Build, package, deploy, icon generation, 3DMark tuning scripts
docs/         Manuals, prerequisite notes, porting notes, tuning measurements
```

### Building

```powershell
# Kernel driver + CLI + service  (needs WDK 10.0.28000+, MSVC 2022)
.\src\build.ps1

# CLI and service only — no driver link step, faster to iterate on
.\src\Build-Cli.ps1

# WPF interface
dotnet build .\src\gui\NvpwrControl.csproj -c Release
```

`tools\package.ps1` assembles `release/`. Three of its inputs are not built from this repository
and are taken as parameters — see the comment block at the top of that script.

---

## Safety

> Raising a laptop GPU's power ceiling increases current through the VRM, heat through the die
> and VRAM, and load on a cooling system that was sized for the original ceiling.

- Watch **GPU temperature, hotspot, memory junction and VRM** under sustained load.
- Confirm the **AC adapter** can actually supply the higher draw; on battery the ceiling will not
  be honoured anyway.
- The unlock is undone by a reboot. If anything looks wrong, reboot.

The driver is deliberately fail-closed: it verifies the target build before writing, and on a
failed transition it restores the baseline captured before its first write in that session.

### Known failure mode: a TDR storm

Under an unstable overclock the GPU can stop responding entirely. Windows logs
`nvlddmkm` event 153 and writes a watchdog dump, DXGI reports `DEVICE_REMOVED`
(`0x887A0005`), the desktop goes half-frozen, and **a reboot can take minutes** because the
shutdown path waits on a driver that is no longer answering.

Measured once on the reference machine: 16 watchdog events and 12 dumps over 45 minutes, and a
shutdown that took 3 minutes 5 seconds. If a reboot appears to hang, that is what is happening —
it is not this project's process.

See [docs/tuning/](docs/tuning/) for the measured power/score curve and the settings that
produced it.

---

## Credits and licence

This is a derivative of
[**LevinAi-arch/rtx-5070ti-laptop-160w-power-limit**](https://github.com/LevinAi-arch/rtx-5070ti-laptop-160w-power-limit),
which is where the original driver work, the interface, and the analysis came from. The
upstream repository carries **no licence** — all rights reserved by its authors.

**Because the upstream work is unlicensed, no licence is granted here either.** This repository
is published as a source-available record, not as a grant of rights. If you intend to reuse any
of it, contact the upstream authors.

Additional third-party components are redistributed in `release/` and are **not** covered by
anything here:

| Component | Author | Note |
|---|---|---|
| `mvolt+.exe` | its own author | Voltage-rail CLI the interface and the service both call |
| `bootx64.efi`, `EfiGuardDxe.efi` | [Mattiwatti/EfiGuard](https://github.com/Mattiwatti/EfiGuard) | Bootloader and DXE driver used to disable DSE |
| `EfiDSEFix.exe` | built from the same EfiGuard tree | Commit `60a6a57` or later; official releases do not work |

Not affiliated with, sponsored by, or endorsed by NVIDIA Corporation. NVIDIA, GeForce and RTX
are trademarks of NVIDIA Corporation.
