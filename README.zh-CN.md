# NvpwrAIO

> **NVIDIA 笔记本显卡的功耗墙解锁与电压/频率调节工具**，核心是一个直接改写 NVIDIA 驱动
> 内存中功率策略对象的内核驱动。

[English](README.md) · **中文**

---

## 这是什么

笔记本显卡的 TGP 上限不是写在一个文件里的，它存在于 NVIDIA 内核驱动的运行期状态中。
MSI Afterburner 这类工具能调核心和显存频率偏移，但在笔记本上，用户态没有任何办法把
TGP 上限抬到 OEM 出厂值以上 —— 因为那个设置根本不存在于任何可写的地方。

本项目增加一个小型内核驱动，它定位驱动在内存中的功率策略对象，修改其原生生成器所读取的
策略字段，然后调用 NVIDIA **自己的** setter —— 让改动走驱动本来就会走的那条代码路径。

驱动之上是一个 WPF 界面、一个命令行工具，以及一个在重启后重新下发设置的服务。

### 「设置不会保存」到底是什么含义

本项目改动的一切都存在于**驱动内存**里。重启会丢，**显示驱动重载**也会丢 —— 而后者在
不重启的情况下发生的频率比想象中高。参考机器上的实测：通过配套 CLI 下发核心 +200 MHz、
显存 +500 MHz，在显示设备重启后读回来是 **0**。

所以这不是功耗墙独有的性质，而是所有这一类设置的性质。**服务存在的全部理由就是这个**：
它记录一次出厂功耗墙，然后在桌面就绪后重新下发功耗、电压和频率。

---

## 组件

| 二进制 | 作用 |
|---|---|
| `Nvpwr.sys` | 内核驱动。在内存中定位 `nvlddmkm.sys` 并改写功率策略对象 |
| `NvpwrControl.exe` | WPF 界面。功耗、电压轨、频率偏移、存档、前置条件检查 |
| `NvpwrCtl.exe` | 命令行工具。同样的 IOCTL，适合脚本和诊断 |
| `NvpwrSvc.exe` | 后台服务。记录出厂功耗墙，登录后重放设置 |

界面和命令行都直接与 `\\.\Nvpwr` 通信，彼此之间不通信。**服务不是前两者工作的前提** ——
它只负责让设置活过一次重启。

---

## 前提条件

### 驱动版本 —— 先读这一段

**内核驱动绑定到 `nvlddmkm.sys` 的某一个确切构建。** 它在动任何内存之前校验 PE 标识和
六段机器码签名，不匹配就返回 `STATUS_REVISION_MISMATCH`。

| | |
|---|---|
| 已验证的驱动 | NVIDIA **616.92** |
| PE 时间戳 | `0x6A9B4070` |
| SizeOfImage | `0x06D3E000` |

**它在其它驱动版本上会拒绝运行，这是设计如此。** 移植到新版驱动是一次有明确清单的逆向
工程任务 —— 见 [docs/PORTING_TO_A_NEW_DRIVER.md](docs/PORTING_TO_A_NEW_DRIVER.md)。

### 机器前提

三条必须同时满足，界面底部用五个状态标签显示：

| 要求 | 为什么 |
|---|---|
| **Secure Boot 关闭** | 开启时 EfiGuard 装不上 |
| **VBS / 虚拟化安全关闭** | 开启时 DSE 补丁不生效 |
| **本次启动经过 EfiGuard** | 否则驱动加载不了 |

见 [docs/DEPLOYMENT_AND_EFIGUARD.md](docs/DEPLOYMENT_AND_EFIGUARD.md) —— **其中一部分必须每台机器自行构建
和安装**，包括 `EfiDSEFix.exe`，它**不能**取自 EfiGuard 的官方 release。

---

## 快速开始

```
1. 构建或取得发布包                    →  release/
2. 安装 Nvpwr.sys 的测试证书           →  右键那个 .cmd，以管理员身份运行
3. 把 EfiGuard 装到 ESP、添加启动项、从它启动
4. 以管理员身份运行 NvpwrControl.exe
5. 设定功耗目标、电压轨、频率，点「应用」
6. 想让它活过重启就安装服务
```

`docs/使用说明.txt` 是用户手册，`release/流程说明.md` 记录了各部分的配合方式。

---

## 仓库结构

```
src/          源码。driver/（内核）、gui/（WPF）、cli/、service/、app/（共享）、shared/
release/      完整可分发包，含二进制
tools/        构建、打包、部署、图标生成、3DMark 调优脚本
docs/         手册、前置条件说明、移植说明、调优实测数据
```

### 构建

```powershell
# 内核驱动 + 命令行 + 服务（需要 WDK 10.0.28000+ 与 MSVC 2022）
.\src\build.ps1

# 只构建命令行和服务 —— 跳过驱动链接，迭代更快
.\src\Build-Cli.ps1

# WPF 界面
dotnet build .\src\gui\NvpwrControl.csproj -c Release
```

`tools\package.ps1` 负责组装 `release/`。它的三个输入不是本仓库构建的产物，作为参数传入
—— 见该脚本顶部的注释。

---

## 安全

> 抬高笔记本显卡的功耗上限，会增加 VRM 的电流、核心与显存的发热，以及本为原上限设计的
> 散热系统的负担。

- 持续负载下盯住 **核心温度、热点、显存结温和 VRM**。
- 确认 **电源适配器** 真的供得上更高的功耗；用电池时上限本来也不会被兑现。
- 解锁会被重启撤销。一旦发现异常，重启。

驱动是**故意 fail-closed** 的：写之前先校验目标构建，转换失败时会恢复到本次会话第一次
写入之前捕获的基线。

### 已知的失败模式：TDR 风暴

在不稳定的超频下，GPU 可能彻底停止响应。Windows 记录 `nvlddmkm` 事件 153 并写出看门狗
dump，DXGI 报 `DEVICE_REMOVED`（`0x887A0005`），桌面进入半卡状态，而且**重启可能要好几分钟**
—— 因为关机流程在等一个已经不再应答的驱动。

参考机器上实测过一次：45 分钟内 16 次看门狗事件、12 个 dump，一次关机耗时 **3 分 05 秒**。
如果重启看起来卡住了，那就是这个现象 —— **不是本项目的进程造成的**。

功耗/分数曲线与产生它的设置见 [docs/tuning/](docs/tuning/)。

---

## 来源与许可

本项目衍生自
[**LevinAi-arch/rtx-5070ti-laptop-160w-power-limit**](https://github.com/LevinAi-arch/rtx-5070ti-laptop-160w-power-limit)
—— 最初的驱动工作、界面和分析都来自那里。**上游仓库没有许可证** —— 版权归其作者所有。

**由于上游作品未授权，这里也不授予任何许可。** 本仓库作为一份可读的实现记录发布，**不是**
一份权利授予。如果你打算复用其中任何部分，请联系上游作者。

`release/` 里还重新分发了若干第三方组件，它们**不在**上述任何声明覆盖范围内：

| 组件 | 作者 | 说明 |
|---|---|---|
| `mvolt+.exe` | 其作者 | 界面和服务都调用的电压轨 CLI |
| `bootx64.efi`、`EfiGuardDxe.efi` | [Mattiwatti/EfiGuard](https://github.com/Mattiwatti/EfiGuard) | 用于关闭 DSE 的引导器与 DXE 驱动 |
| `EfiDSEFix.exe` | 由同一 EfiGuard 源码树构建 | 需 commit `60a6a57` 或之后；官方 release 不可用 |

与 NVIDIA Corporation 无隶属、赞助或背书关系。NVIDIA、GeForce、RTX 是 NVIDIA Corporation
的商标。
