# Porting the kernel driver to a new `nvlddmkm.sys`

**English** · [中文见下方](#中文)

> The driver is pinned to one exact build of NVIDIA's kernel display driver. This document is
> the complete list of what is pinned and how to re-derive each item.

---

## Why it is pinned at all

`Nvpwr.sys` never patches `nvlddmkm.sys` on disk and never patches NVIDIA executable code. What
it does is locate NVIDIA's **live power-policy objects** in memory, change the policy **data**
fields its native generator reads, and then call NVIDIA's **existing** setters.

That approach depends on four things that only exist at specific addresses and offsets inside a
specific build:

1. Where the functions are (RVAs).
2. Where the objects are, relative to what you start from (struct offsets).
3. What the fields mean.
4. That the functions are still the functions you think they are (machine-code signatures).

Every one of those is checked before the first write. A mismatch returns
`STATUS_REVISION_MISMATCH` and nothing is touched.

---

## The complete list

All of these live at the top of `src/driver/driver.c`.

### 1. PE build identity — 2 values

```c
#define NVPWR_EXPECTED_TIMESTAMP 0x6A9B4070u   // nvlddmkm.sys PE TimeDateStamp
#define NVPWR_EXPECTED_SIZE      0x06D3E000u   // SizeOfImage
```

Read them straight out of the new binary's PE header. They change on every build.

### 2. Function RVAs — 8 values

```c
#define RVA_GPU_GLOBAL           0x013B2E18u
#define RVA_GPU_REGISTRY_FN      0x00107AC0u
#define RVA_F7_GENERATOR         0x004E3EF0u   // the power-policy generator itself
#define RVA_F7_UPPER_LOAD        0x004E3FBFu
#define RVA_F7_RECORD            0x004E4088u
#define RVA_BOARD_TYPE0_SET      0x008D6960u
#define RVA_SET_AMOUNT           0x004E4610u
#define RVA_SET_ELIG             0x004E4680u
```

**These always change.** They are file offsets from the image base.

The practical method is differential: load the known-good 616.92 image and the new one side by
side in IDA or Ghidra, and find each function by its body. The functions themselves are stable
across versions — the generator a few hundred bytes long, the setters small and distinctive —
so matching on shape and on the string/constant references they make is reliable. Do **not**
assume the relative order of the functions in the file is preserved.

### 3. Object field offsets — 18 values

```c
#define OFF_GLOBAL_GPU_TABLE     0x0208u
#define OFF_TABLE_ENTRY_PTR      0x48A48u
#define OFF_TABLE_ENTRY_GPUID    0x48A50u
#define OFF_TABLE_COUNT          0x48C48u
#define GPU_ENTRY_STRIDE         0x10u
#define GPU_COUNT_MAX            32u

#define OFF_MAJOR_POWER_ROOT     0x25B0u
#define OFF_ROOT_REGISTRY        0x1CC0u
#define OFF_ROOT_LOOKUP_FN       0x1CF8u
#define OFF_ROOT_INIT            0x3D10u
#define OFF_ROOT_ELIG            0x3D11u
#define OFF_ROOT_AMOUNT_ACTIVE   0x3D12u
#define OFF_ROOT_CTGP            0x3D14u
#define OFF_ROOT_AMOUNT          0x3D18u
#define OFF_ROOT_POLICY_KEY      0x3D1Cu
#define OFF_ROOT_LOWER           0x3D20u
#define OFF_ROOT_UPPER           0x3D24u
#define OFF_ROOT_AUX28           0x3D28u
```

**These are the most likely to survive a version bump, and the most dangerous to assume.**

The `OFF_ROOT_*` block is a single struct that is densely packed — `0x3D10` through `0x3D2C`,
one byte apart in places. NVIDIA has kept its layout across the builds this project has seen,
which is why the driver can be recognised at all; but a field inserted anywhere in that range
shifts everything after it, and the failure would be a silent write to the wrong byte rather
than a crash. **Verify it, do not assume it.**

The way to verify: find the generator function, and read which offsets off its context argument
it actually loads and stores. The roles are:

| Offset | Role |
|---|---|
| `+3D10` | root initialised flag |
| `+3D11` | eligibility flag |
| `+3D12` | amount-active flag |
| `+3D14` | base / cTGP input |
| `+3D18` | dynamic amount |
| `+3D1C` | policy key |
| `+3D20` | lower bound |
| `+3D24` | F7 saturation ceiling (UPPER) |
| `+3D28` | auxiliary 28 |

### 4. Machine-code signatures — 6 arrays, 85 bytes

```c
static const UCHAR g_SigGpuRegistry[]  // 17 bytes
static const UCHAR g_SigUpperLoad[]    //  7 bytes
static const UCHAR g_SigF7Record[]     //  7 bytes
static const UCHAR g_SigBoardSet[]     // 15 bytes
static const UCHAR g_SigSetAmount[]    // 21 bytes
static const UCHAR g_SigSetElig[]      // 18 bytes
```

Each is compared byte for byte against `imageBase + RVA`:

```c
if (!BytesEqual(Ctx->Base + RVA_GPU_REGISTRY_FN, g_SigGpuRegistry, sizeof(g_SigGpuRegistry)) ||
    !BytesEqual(Ctx->Base + RVA_F7_UPPER_LOAD,    g_SigUpperLoad,   sizeof(g_SigUpperLoad))   ||
    ...
    return STATUS_REVISION_MISMATCH;
```

These exist because an RVA alone is not proof. A wrong RVA that happens to land in valid code
would otherwise be called. **Copy the first N bytes of each newly located function**, keeping
the length the same as the old signature so the arrays stay comparable in review.

Choose bytes that are **stable** across compiler versions where you can — a sequence that
encodes a relative branch or an absolute address will differ between builds even when the
function is unchanged, and you want the check to fail on a real change rather than on
recompilation noise. The existing signatures are mostly register operations and small
immediates for this reason.

---

## Procedure

```
 1. Obtain the new nvlddmkm.sys.          From DriverStore\FileRepository\nv_dispi.inf_*\,
                                          or from the running system via its loaded base.
 2. Read its PE TimeDateStamp and SizeOfImage.
                                          → NVPWR_EXPECTED_TIMESTAMP, NVPWR_EXPECTED_SIZE
 3. Open it and the 616.92 image side by side in IDA/Ghidra.
 4. Locate the 8 functions by body shape.
                                          → the 8 RVA_* values
 5. Confirm the 18 struct offsets against what the generator actually reads.
                                          → the 18 OFF_* values
 6. Copy the opening bytes of each function.
                                          → the 6 g_Sig* arrays
 7. Rebuild Nvpwr.sys.
 8. Re-sign it with the test certificate, and import the certificate on the target machine.
 9. Update the table in README.md and the version note at the top of driver.c.
```

## Validating the port

The driver fails closed, so the first signal is `STATUS_REVISION_MISMATCH` from
`NvpwrCtl.exe status` — that means step 2, 4 or 6 is wrong, in that order of likelihood.

Once it loads, the three-phase sequence in `SetPowerTarget` is the functional test. Phase A
stages `base + amount` **while the ceiling is still at the saved OEM value** and then re-runs
NVIDIA's generator to prove it converged; only then does Phase B raise the ceiling. If Phase A
fails, the offsets are wrong even though the signatures matched, and the driver restores the
baseline it captured before its first write.

A useful cross-check while porting: `OFF_ROOT_CTGP` and `OFF_ROOT_UPPER` should read back as
plausible milliwatt values in `NvpwrCtl.exe status` — on the reference machine 175000 for both
at stock. If they read as zero or as garbage, the offsets have shifted.

---

## 中文

> 驱动绑定到 NVIDIA 内核显示驱动的某一个确切构建。本文是**完整的绑定清单**以及每一项的
> 重新推导方法。

### 为什么要绑定

`Nvpwr.sys` **从不**修改磁盘上的 `nvlddmkm.sys`，**也从不**修改 NVIDIA 的可执行代码。它
做的是：在内存中定位 NVIDIA 的**活的**功率策略对象，修改其原生生成器所读取的策略**数据**
字段，然后调用 NVIDIA **现成的** setter。

这个做法依赖四件只存在于特定构建里的事：

1. 函数在哪（RVA）
2. 对象相对于起点在哪（结构偏移）
3. 那些字段是什么含义
4. 那些函数**仍然是你以为的那个函数**（机器码签名）

**这四类在第一次写入之前全部会被检查。** 不匹配就返回 `STATUS_REVISION_MISMATCH`，什么都
不碰。

### 清单（共 28 项）

| 类别 | 数量 | 是否必然要重做 |
|---|---|---|
| **PE 构建标识** | 2 | ✅ 每次构建都变 |
| **函数 RVA** | 8 | ✅ 必然要重做 |
| **对象字段偏移** | 18 | ⚠️ **最可能稳定，但最危险** |
| **机器码签名** | 6（85 字节） | ✅ 必然要重做 |

### 关于字段偏移的警告

`OFF_ROOT_*` 那一块是**一个紧凑排布的结构体** —— `0x3D10` 到 `0x3D2C`，有些字段只差
一个字节。NVIDIA 在本项目见过的构建里保持了它的布局，**但在这个范围内任何位置插入一个字
段，后面全部会平移**，而失败的形态是**写到错误的字节**，不是崩溃。

**所以：要验证，不要假设。** 验证方法是从生成器函数里读出它实际加载和存储了哪些偏移。

### 关于签名的建议

**尽量挑选编译器版本之间稳定的字节。** 编码了相对跳转或绝对地址的序列，即使函数没变也会
随重新编译而不同 —— 那样校验就会因为编译噪声而失败，而不是因为真正的改动。现有签名主要
是寄存器操作和小立即数，正是出于这个原因。

### 验证移植

驱动是 fail-closed 的，所以第一个信号是 `NvpwrCtl.exe status` 返回 `STATUS_REVISION_MISMATCH`
—— 那说明第 2、4 或 6 步有错（按可能性排序）。

能加载之后，`SetPowerTarget` 的三阶段序列就是功能测试。**Phase A 在墙仍处于 OEM 值时就
去构建 `base + amount`，然后重跑 NVIDIA 的生成器证明它收敛了**；只有这之后 Phase B 才抬墙。
Phase A 失败说明偏移错了（尽管签名匹配），此时驱动会恢复到第一次写入前捕获的基线。

移植时一个有用的交叉验证：`NvpwrCtl.exe status` 里的 `OFF_ROOT_CTGP` 和 `OFF_ROOT_UPPER`
读出来应该是合理的毫瓦值 —— 参考机器出厂时两者都是 175000。**如果读成 0 或乱码，说明偏移
已经移位了。**
