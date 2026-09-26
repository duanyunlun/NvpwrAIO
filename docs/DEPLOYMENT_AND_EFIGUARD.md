# Deployment requirements and EfiGuard

**English** · [中文见下方](#中文)

> What has to be true about the machine before the driver can load, and **which parts must be
> built and installed per machine rather than taken from a release.**

---

## Why EfiGuard is involved at all

`Nvpwr.sys` is not signed by a certificate Windows trusts. Loading it requires Driver Signature
Enforcement (DSE) to be off.

There are two ways to get there, and this project uses the second:

| Approach | Problem |
|---|---|
| `bcdedit /set testsigning on` | Permanently marks the machine as test-signed. The desktop watermark never goes away, and **kernel anti-cheat refuses to run** while it is set. |
| **EfiGuard** | Patches the kernel's DSE check during boot, so DSE is off for that boot only. Nothing about the installed system changes. |

EfiGuard is a UEFI application plus a DXE driver. It hooks `SetVariable` early enough in boot to
neutralise the code-integrity check, and the hook is also how the project **detects** that this
boot went through it — `DseControl.IsBooted()` asks the hook a question and sees whether it
answers. Everything else is gated on that answer.

---

## Prerequisites

| Requirement | Why | Where it is checked |
|---|---|---|
| **Secure Boot disabled** | EfiGuard cannot install or run while it is on | BIOS/UEFI setup |
| **VBS / Virtualization-Based Security disabled** | With VBS on, the bootloader patch does not take effect | 虚拟化安全 chip |
| **Booted through EfiGuard** | Without it the driver will not load | EfiGuard chip |

The interface shows these as five status chips, and refuses to attempt anything while a blocking
one is unmet — the failure would otherwise be a driver load that silently does nothing.

> On the reference machine the firmware itself has VBS disabled, which means the registry setting
> is forced back to zero on every boot. That case is detected by reading `Kernel-Boot` event 153
> — the only way to learn the reason, because the UEFI variable is refused from user mode with
> error 1314.

---

## What must be per-machine

**This is the part people get wrong.** Not everything can be copied from a release.

| Item | Per-machine? | Why |
|---|---|---|
| `Nvpwr.sys` | No | Machine-independent |
| `Nvpwr.cer` | **Import** into the local certificate store | The file is generic, the install is not |
| **`EfiDSEFix.exe`** | **Build it yourself** | See below |
| `bootx64.efi`, `EfiGuardDxe.efi` | **Copy to this machine's ESP and add a boot entry** | ESP and boot entries are per-machine |
| **UEFI boot entry** | **Add it** | The machine has to know to boot EfiGuard |
| The two passwords/BIOS settings | This machine's firmware | — |

### `EfiDSEFix.exe` cannot come from EfiGuard's official release

From `src/gui/UnlockChain.cs`:

```csharp
/// It must be the build from EfiGuard commit 60a6a57 or later. Earlier builds cannot
/// find g_CiOptions on current Windows and return STATUS_NOT_FOUND for every call.
```

The official release binaries predate that commit. They install and run, and then every single
call fails with `STATUS_NOT_FOUND`, which reads like a driver problem and is not one. Clone
[Mattiwatti/EfiGuard](https://github.com/Mattiwatti/EfiGuard), check out `60a6a57` or later,
and build `Application/EfiDSEFix`.

`tools/package.ps1` takes the path to it as a parameter (`-EfiFix`) for exactly this reason.

### Installing the boot stage

The outline:

```
1. Mount the EFI System Partition.        mountvol S: /s     (or the standard ESP mount)
2. Copy bootx64.efi and EfiGuardDxe.efi to it, e.g. \EFI\EfiGuard\
3. Add a UEFI boot entry pointing at bootx64.efi.
4. Reboot and select that entry.
5. The EfiGuard chip in the interface should now read 已生效.
```

EfiGuard's own README covers the mechanics and the various loader modes; the important thing
here is that **the boot entry is what makes `IsBooted()` true**, not the presence of the files.

---

## When you are done

Returning the machine to a normal, anti-cheat-compatible state:

```
1. Uninstall the service and stop using the tuner.
2. Reboot (the power ceiling reverts on its own — it only ever lived in driver memory).
3. Remove the EfiGuard boot entry, or move it down the boot order.
4. Re-enable Secure Boot in BIOS if you had turned it off.
```

Nothing about the installed Windows system was modified, so there is no `testsigning` flag to
clear and no watermark to remove.

---

## 中文

> 驱动能加载之前，机器必须满足什么条件；以及**哪些部分必须每台机器自行构建和安装**，
> 而不是从发布包里拿。

### 为什么要用 EfiGuard

`Nvpwr.sys` 没有 Windows 信任的签名，加载它要求关闭驱动签名强制（DSE）。

| 做法 | 问题 |
|---|---|
| `bcdedit /set testsigning on` | 会把机器**永久**标记为测试签名。桌面水印去不掉，而且**内核反作弊会拒绝运行** |
| **EfiGuard** | 在启动时给内核的 DSE 检查打补丁，**只对这一次启动生效**。已安装的系统没有任何改变 |

EfiGuard 由一个 UEFI 程序和一个 DXE 驱动组成。它在启动早期就挂钩 `SetVariable`，从而中和
代码完整性检查；而这个钩子**同时也是本项目检测「本次是否经过 EfiGuard」的手段** ——
`DseControl.IsBooted()` 向钩子提一个问题，看它答不答。**其余一切都以这个答案为前提。**

### 必须每机器做的部分

**这是最容易搞错的地方。**

| 项目 | 是否每机器 | 原因 |
|---|---|---|
| `Nvpwr.sys` | ❌ 通用 | 与硬件无关 |
| `Nvpwr.cer` | ⚠️ **导入**本机证书存储 | 文件通用，安装动作不通用 |
| **`EfiDSEFix.exe`** | ✅ **必须自己构建** | 见下 |
| `bootx64.efi`、`EfiGuardDxe.efi` | ⚠️ **复制到本机 ESP 并加启动项** | ESP 和启动项都是每机器的 |
| **UEFI 启动项** | ✅ **必须添加** | 机器得知道要从 EfiGuard 启动 |

### `EfiDSEFix.exe` 不能取自官方 release

`src/gui/UnlockChain.cs` 里写得很清楚：**必须是 EfiGuard commit `60a6a57` 或之后构建的版本。
更早的构建在当前 Windows 上找不到 `g_CiOptions`，每次调用都返回 `STATUS_NOT_FOUND`。**

官方 release 的二进制早于那个 commit。它们能装上、能运行，然后**每一次调用都失败**，症状
看起来像驱动问题，实际不是。正确做法：clone
[Mattiwatti/EfiGuard](https://github.com/Mattiwatti/EfiGuard)，切到 `60a6a57` 之后，编译
`Application/EfiDSEFix`。

`tools/package.ps1` 正是因为这一点才把它的路径做成参数（`-EfiFix`）。

### 引导阶段的安装轮廓

```
1. 挂载 EFI 系统分区          mountvol S: /s
2. 把 bootx64.efi 与 EfiGuardDxe.efi 复制进去，例如 \EFI\EfiGuard\
3. 添加一个指向 bootx64.efi 的 UEFI 启动项
4. 重启并从那个启动项进入
5. 界面上的 EfiGuard 标签应显示「已生效」
```

具体机制和几种加载模式见 EfiGuard 自己的 README。这里要强调的是：**让 `IsBooted()` 为真
的是那个启动项，不是文件在不在。**

### 用完怎么恢复

```
1. 卸载服务，停止使用调参工具
2. 重启（功耗墙自己就回去了 —— 它本来就只存在于驱动内存里）
3. 删除 EfiGuard 启动项，或把它调到启动顺序后面
4. 如果你之前关了 Secure Boot，重新打开
```

**已安装的 Windows 系统本身没有任何改动**，所以没有 `testsigning` 标记要清，也没有水印。
