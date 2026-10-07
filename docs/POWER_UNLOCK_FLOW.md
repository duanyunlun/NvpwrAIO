# 软解功耗墙 —— 完整流程（已验证）

状态：**核心链路实测通过**（175 W → 200 W，驱动卸载后仍生效）
最后验证：2026-09-24

---

## 一、核心机制

```
功耗上限写在 NVIDIA 驱动（nvlddmkm.sys）的运行时内存里
  ├─ 与我们的 Nvpwr.sys 无关
  │    → 所以我们的驱动卸载后，值仍然生效
  │    → 所以我们的程序关闭后，值仍然生效
  └─ 但它是运行时状态
       → 重启后丢失，每次开机都要重新写一遍
```

**这就是视频里"关掉软件还能玩游戏、但重启就没了"的原因。**

---

## 二、三个必须理解的机制

### 1. 为什么需要 EfiGuard

`Nvpwr.sys` 是自签名的。Windows 默认拒绝加载无 WHQL 签名的内核驱动（DSE，驱动签名强制）。

EfiGuard 在**启动阶段**修补内核，让我们可以**临时**关闭 DSE。相比测试签名模式：

| | 测试签名模式 | EfiGuard |
|---|---|---|
| 实现 | `bcdedit /set testsigning on` | 启动期修补内核 |
| 需要重启 | ✅ 是（启动配置） | ❌ 否（运行时改） |
| 桌面水印 | ✅ 显示"测试模式" | ❌ 无 |
| BCD 标志 | ✅ 写着 testsigning=on | ❌ 不写 |
| 反作弊检测 | ❌ 一眼看穿 | ✅ 与正常启动无异 |

### 2. 为什么 DSE 只关极短时间

**DSE 只在"驱动加载那一刻"被检查。** 驱动一旦装载完成，DSE 就可以恢复，驱动照常运行。

所以整个流程里 DSE 只关闭**几秒钟**：
```
EfiDSEFix -d  →  加载驱动  →  下发设置  →  EfiDSEFix -e  →  卸载驱动
   ↑ 关                                                    ↑ 恢复
   └────────────── 全程几秒 ──────────────────────────────┘
```

### 3. 为什么驱动用完就卸载

- 系统里不留未签名驱动 → 反作弊（ACE 等）更友好
- 功耗值已写在 NVIDIA 内存里，卸载驱动不影响

---

## 三、硬前提：VBS 必须彻底关闭

**实测结论**：

| VBS 状态 | KDU 路径 | EfiGuard 路径 |
|---|---|---|
| **2（运行中）** | ❌ **两次都 bugcheck** | ❌ 会 bugcheck |
| **0（未启用）** | 未测 | ✅ **成功** |

**原因**（EfiGuard 官方文档原文）：

> Both types of DSE bypass are rendered useless by HVCI: the boot time patch has no effect because the kernel defers to the secure kernel for integrity checks, and the SetVariable hook will cause a **`SECURE_KERNEL_ERROR`** bugcheck if it is used to write to `g_CiOptions`.

**只要安全内核在跑，写 `g_CiOptions` 就会直接 bugcheck。**

### VBS 为什么关不掉（重要）

只设 `EnableVirtualizationBasedSecurity=0` 和 `hypervisorlaunchtype off` **不够** —— 固件级特性会独立启动 hypervisor：

| 来源 | 层级 | 处理 |
|---|---|---|
| Windows Hello 增强登录安全性 | 注册表 | `Scenarios\WindowsHello\Enabled = 0` |
| `RequireMicrosoftSignedBootChain` | 注册表 | 设为 0 |
| **内核 DMA 保护** | **BIOS** | 需在 BIOS 关闭 |
| **UEFI 代码只读 / SMM 缓解** | **BIOS** | 需在 BIOS 关闭 |

**判定标准**：
```
VBS 状态（WMI）= 0
HypervisorPresent = False
hvservice 驱动 = 已停止
```

---

## 四、流程

### A. 一次性安装

```
1. 检查并修正前提条件
   ├─ Secure Boot        必须关闭（BIOS）
   ├─ VBS / hypervisor   必须关闭（注册表 + BIOS DMA 保护）
   ├─ 驱动阻止列表       必须关闭（注册表）
   ├─ WDAC CI 策略       必须删除（EFI 分区 + Windows 目录，带备份）
   └─ 以上任一不满足 → 提示用户，不继续

2. 安装 EfiGuard 到 ESP
   ├─ 复制 EfiGuardDxe.efi 和 Loader（改名 bootx64.efi）到 \EFI\Boot\
   └─ 添加 UEFI 启动项，并设为固件默认启动项

3. 安装我们的服务（NvpwrSvc），设为开机自启

4. 提示重启
```

### B. 启动引导

```
BIOS → EfiGuard Loader（bootx64.efi）
     → 修补 bootmgfw.efi / winload.efi / ntoskrnl.exe
     → 启动 Windows
```

**判定 EfiGuard 是否生效**：`EfiDSEFix.exe -c` 返回 0

### C. 开机自动应用（服务）

```
服务启动
  ↓
① 检查 EfiGuard 是否生效（EfiDSEFix -c）
   ├─ 否 → 记日志"本次未经 EfiGuard 引导"，跳过 → 功耗墙保持出厂值（fail-safe）
   └─ 是 → 继续
  ↓
② 读取保存的设置（state.ini）
   ├─ 功耗 / 电压 / 频率
   └─ 全为零 → 什么都不做，退出
  ↓
③ try {
       EfiDSEFix -d                  关闭 DSE
       sc create/start Nvpwr         加载驱动
       NvpwrCtl set <profile> <W>    下发功耗
       （电压/频率走各自通道）
   }
   finally {
       EfiDSEFix -e                  恢复 DSE  ← 无论成败都必须执行
       sc stop Nvpwr                 卸载驱动  ← 无论成败都必须执行
   }
  ↓
④ 记录结果到日志
```

### D. 手动调整（GUI）

与 C 的 ③ 相同，只是触发源是用户点「应用」。

### E. 失效条件

| 事件 | 结果 |
|---|---|
| 重启 | 功耗墙回到出厂值，服务在开机时重放（若开机时为电池供电则不重放，见下） |
| 未经 EfiGuard 引导 | 服务跳过，功耗墙保持出厂值 |
| 点「恢复默认」 | 清空 state.ini 设置，下次开机不再重放 |
| 卸载服务 | 不再自动重放 |
| **AC → DC（拔电/断电）** | **驱动按电池策略重算，出现"墙还抬着、生成器掉回 base+amount"的部分应用态（State=MIXED）。服务不动作，保持出厂值。** |
| **DC → AC（恢复供电）** | **服务在电源事件安静 20 秒后重放：先 RESTORE 回出厂基线，再 SET_POWER。约 20 秒内恢复。** |

#### 为什么电源切换会破坏状态，以及为什么必须先 RESTORE

NVIDIA 驱动对 AC 与 DC 维护两套独立的功耗策略，切换时会重算生成器的输出，
但**不会**恢复本程序写入的天花板（那是驱动内存里的数据，被原样带过）。于是：

```
UPPER (+3D24)     275 W    ← 我们写的，保留
F7 input (+3D14)  175 W    ← 驱动按当前电源重算，掉回出厂
predicted F7      200 W    ← = 175 + 25
Current effective 200 W    ← 实际只生效 200 W
Applied target      0      ← 目标没了
```

天花板侧自洽、生成器侧三方一致但低于天花板 —— 这正是 `NvpwrStateMixed` 的定义。

而 `SetPowerTarget` 的入口门只接受 `StockBaseline` / `Armed` / `Applied`
（`driver.c` 的 `SetPowerTarget`，`g_SavedValid` 分支），**MIXED 会被直接拒绝**，
表现为 `Win32 22`。实测确认：从 MIXED 直接 `SET_POWER` 返回 `Win32=22`，策略未被改动。

所以重放必须先 `RESTORE`。这一条对两种场景都成立：

- **MIXED（电源切换后）**：RESTORE 走 `IsRecoverableExternalModifiedState` 分支，
  该判定接受"天花板自洽 + 生成器三方一致且不超过天花板"的形状，正是 MIXED 的形状。
- **STOCK_BASELINE（正常开机）**：`RestoreStock` 在发现已是干净出厂基线时立即返回成功，
  是幂等的空操作。

因此在重放路径里无条件先 RESTORE 既修好了坏状态，也不影响好状态。

#### 为什么只在恢复供电时重放

抬墙的语义就是"允许消耗超过固件上限的功率"。用电池时这么做既不是用户设置时
的意图（他设的是一个插电时使用的值），也会最快掏空电池。所以：

- **AC → DC**：不重放
- **DC → AC**：重放
- **开机时是电池**：不重放，等来电（服务在开机路径里也检查电源来源）

#### 去抖

一次断电不会只产生一个通知。参考机器的日志显示一次切换会带来两个 `AcDcBurst`
（13:43:12 断开 → 13:43:14、13:43:18 → 13:44:15 来电），而驱动在这期间一直在重算策略。
在重算中途写入天花板正是 `service_main.cpp` 头部注释警告过的、曾把机器挂死的时机。
所以通知只用来重置定时器，安静满 `kPowerSettleMs`（20 秒）后才执行重放。

---

## 五、验证过的命令链与实测输出

```powershell
# 1. 关闭 DSE
EfiDSEFix.exe -d                 # 退出码 0

# 2. 加载驱动
sc.exe create Nvpwr type= kernel binPath= D:\ProgramFiles\nvpwrcontrol\Nvpwr.sys
sc.exe start Nvpwr               # STATE: 4 RUNNING

# 3. 下发功耗（5090 = 本机型号）
NvpwrCtl.exe set 5090 200
#   State              : APPLIED (2)
#   Last NTSTATUS      : 0x00000000
#   Last NVIDIA status : 0x00000000
#   amount (+3D18)     : 25000 (25 W)
#   UPPER (+3D24)      : 200000 (200 W)
#   MAX effective      : 200000 (200 W)
# nvidia-smi 确认: 175.00 W → 200.00 W   ✅

# 4. 恢复 DSE
EfiDSEFix.exe -e                 # 退出码 0

# 5. 卸载驱动
sc.exe stop Nvpwr                # STATE: 1 STOPPED
NvpwrCtl.exe status              # Cannot open \\.\Nvpwr  ← 确认关闭

# 6. ★ 关键验证 ★
nvidia-smi --query-gpu=power.max_limit --format=csv,noheader
# → 200.00 W   ← 驱动卸载后仍然生效！
```

**实测环境**：MSI Raider A18 HX A9WJG，RTX 5090 Laptop，驱动 616.92，Windows 11 26H2 build 26300

---

## 六、EfiDSEFix 必须用我们自己的构建

**他们 Release 里的 `EfiDSEFix.exe` 在新 Windows 上不能用：**

| 版本 | `-r`（读 g_CiOptions） |
|---|---|
| 他们的（21504 bytes） | ❌ `0xC0000225` STATUS_NOT_FOUND |
| **我们的（22016 bytes，HEAD 60a6a57）** | ✅ 退出码 0 |

**原因**：EfiGuard 仓库的 `60a6a57`（2026-06-17）提交是
`EfiDSEFix: FindCiOptions: match arbitrary r32 immediates` —— 专门适配新版 Windows 上
`g_CiOptions` 的指令模式变化。他们的二进制早于这个修复。

**构建方法**（本机无 v145 工具集，需降级）：
```powershell
# 前置：稀疏拉取 edk2 头文件
git clone --depth 1 --filter=blob:none --sparse https://ghproxy.net/https://github.com/tianocore/edk2.git D:\Work\Github\edk2
git -C D:\Work\Github\edk2 sparse-checkout set MdePkg/Include MdeModulePkg/Include
# 建立 EfiDSEFix 期望的路径（$(SolutionDir)../MdePkg）
mklink /J D:\Work\Github\MdePkg        D:\Work\Github\edk2\MdePkg
mklink /J D:\Work\Github\MdeModulePkg  D:\Work\Github\edk2\MdeModulePkg

# 编译（必须显式传 SolutionDir，否则 include 路径解析错误）
msbuild EfiDSEFix.vcxproj /p:Configuration=Release /p:Platform=x64 `
        /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.26100.0 `
        "/p:SolutionDir=D:\Work\Github\EfiGuard\"
```

---

## 七、已发现待修 bug

### bug-1：出厂功耗墙会被自己的写入污染 ★

```csharp
// MainWindow.ReadPowerFloorW() 现状
if (Driver.QueryStatus(...))
    return status.OemBaseline / 1000;              // ← 驱动加载时正确
...
return (int)Math.Round(envSample.EnforcedLimitW);  // ← 驱动未加载时读"当前生效上限"！
```

**现象**：下发 200 W 后，驱动卸载，界面「出厂功耗墙」显示 **200 W**（应为 175 W）。

**根因**：从"会被我们自己改写的数据"里读"出厂值"。
`nvidia-smi` 的 `max_limit` 在写入后从 175 变成 200，信息已丢失。

**修法**（与电压基线 `baseline_max_mv` 同一模式）：
```
1. state.ini 增加 power_floor_w
2. 驱动加载时读到 OemBaseline → 记录并持久化
3. 之后一律用记录值，不再从 NVML 反推
4. 只有记录为空时才捕获一次
```

**同类问题回顾**：电压基线已用同样方式修过（`baseline_min_mv`/`baseline_max_mv`）。

---

## 八、整合清单（待做）

| # | 内容 | 依赖 |
|---|---|---|
| 1 | 修 bug-1（出厂功耗墙持久化） | 无 |
| 2 | 前提条件检查增加「VBS/hypervisor 是否真关」 | 无 |
| 3 | CI 策略删除/还原（带备份） | 无 |
| 4 | EfiGuard 安装/检测（ESP 文件 + UEFI 启动项） | 无 |
| 5 | EfiDSEFix 封装（`-c`/`-d`/`-e`） | 无 |
| 6 | 加载链路封装（含 finally 保证 DSE 恢复） | 5 |
| 7 | 服务开机自动重放 | 6 |
| 8 | 捆绑二进制（我们的 EfiDSEFix + 他们的引导文件） | 无 |

---

## 九、文件位置

```
D:\Work\Github\rtx-5070ti-laptop-160w-power-limit\   本项目（WPF GUI）
D:\Work\Github\EfiGuard\                              EfiGuard 源码
    Application\EfiDSEFix\bin\EfiDSEFix.exe           ★ 我们的构建（含修复）
D:\Work\Github\edk2\                                  edk2 头文件（稀疏检出）
D:\Work\Github\_backup\efiguard-theirs\               他们的 EfiGuard 二进制（备份）
D:\ProgramFiles\nvpwrcontrol\                         部署目录
Z:\EFI\Boot\                                          ESP 上的 EfiGuard（已安装）
```
