# Porting the kernel driver to a new `nvlddmkm.sys`

**English** · [中文见下方](#中文)

> The driver is pinned to specific builds of NVIDIA's kernel display driver. This document is
> the complete list of what is pinned, where it lives, and how to add a new build.

---

## Why it is pinned at all

`Nvpwr.sys` never patches `nvlddmkm.sys` on disk and never patches NVIDIA executable code. It
locates NVIDIA's **live power-policy objects** in memory, changes the policy **data** fields its
native generator reads, and then calls NVIDIA's **existing** setters.

That depends on four things that only exist at specific addresses inside a specific build:

1. Where the functions are (RVAs).
2. Where the objects are, relative to what you start from (struct offsets).
3. What the fields mean.
4. That the functions are still the functions you think they are (machine-code signatures).

All four are checked before the first write. A mismatch returns `STATUS_REVISION_MISMATCH` and
nothing is touched.

---

## How a build is described

Everything build-specific lives in **one file**: `src/driver/target.h`. It holds a table of
supported builds:

```c
static const NVPWR_TARGET g_Targets[] = {
    { 0x6A9B4070u, 0x06D3E000u, "616.92", { Rva[8] }, { Off[22] }, { Sig[6][24] }, { SigLen[6] } }
    /* the next build goes here */
};
```

The driver reads the actual PE identity from the loaded image, looks it up in this table, and
uses **that entry's** RVAs, offsets and signatures. `ValidateBuild` in `driver.c` does the
lookup and never needs to change again.

**Adding a build is adding a table entry.** `driver.c` is not touched.

That also means one `Nvpwr.sys` can support several driver versions at once, each verified
against its own signatures — so a user who updates their display driver does not have to wait
for a new binary.

---

## Start here: run the diagnosis

Before reverse-engineering anything, ask the driver what it sees:

```
NvpwrCtl.exe diagnose
```

On a build that is already in the table it prints, per signature, the RVA and whether the bytes
matched:

```
Matched target       : 616.92 (index 0)
Signature mask       : 0x3F of 0x3F (6 of 6 matched)

Per-signature (a mismatch means that RVA is wrong for this build):
  [0] GpuReg   RVA 0x00107AC0 len 17  ok
  [1] UpperLd  RVA 0x004E3FBF len 7   ok
  [2] F7Rec    RVA 0x004E4088 len 7   ok
  [3] BoardSet RVA 0x008D6960 len 15  ok
  [4] SetAmt   RVA 0x004E4610 len 21  ok
  [5] SetElig  RVA 0x004E4680 len 18  ok
```

On a build that is **not** in the table yet it prints the two numbers the new table entry needs
first:

```
Actual timestamp     : 0xXXXXXXXX
Actual SizeOfImage   : 0xYYYYYYYY
Matched target       : NONE
```

`NvpwrCtl.exe status` also reports the driver's `Detail` code with a sentence, which says
whether the failure was the PE identity (add a table entry) or a signature (an RVA in an
existing entry is wrong).

---

## The complete list

### 1. PE build identity — 2 values

```c
TimeDateStamp;   /* nvlddmkm.sys PE TimeDateStamp */
SizeOfImage;     /* PE SizeOfImage */
```

Read them from the new binary's PE header. They change on every build. `diagnose` prints both.

### 2. Function RVAs — 8 values

```c
NvpwrRvaGpuGlobal        0x013B2E18u
NvpwrRvaGpuRegistryFn    0x00107AC0u
NvpwrRvaF7Generator      0x004E3EF0u   /* the power-policy generator itself */
NvpwrRvaF7UpperLoad      0x004E3FBFu
NvpwrRvaF7Record         0x004E4088u
NvpwrRvaBoardType0Set    0x008D6960u
NvpwrRvaSetAmount        0x004E4610u
NvpwrRvaSetElig          0x004E4680u
```

**These always change.** They are file offsets from the image base.

The practical method is differential: open the known-good 616.92 image and the new one side by
side in IDA or Ghidra and find each function by its body. The functions are stable across
versions — the generator is a few hundred bytes long and the setters are small and distinctive —
so matching on shape and on the constants they reference is reliable. Do **not** assume the
relative order of the functions in the file is preserved.

### 3. Structure offsets — 22 values

```c
NvpwrOffGlobalGpuTable      0x0208u
NvpwrOffTableEntryPtr       0x48A48u
NvpwrOffTableEntryGpuId     0x48A50u
NvpwrOffTableCount          0x48C48u
NvpwrOffGpuEntryStride      0x10u      /* a stride, not an offset */
NvpwrOffGpuCountMax         32u        /* a count, not an offset */

NvpwrOffMajorPowerRoot      0x25B0u
NvpwrOffRootRegistry        0x1CC0u
NvpwrOffRootLookupFn        0x1CF8u
NvpwrOffRootInit            0x3D10u
NvpwrOffRootElig            0x3D11u
NvpwrOffRootAmountActive    0x3D12u
NvpwrOffRootCtgp            0x3D14u
NvpwrOffRootAmount          0x3D18u
NvpwrOffRootPolicyKey       0x3D1Cu
NvpwrOffRootLower           0x3D20u
NvpwrOffRootUpper           0x3D24u
NvpwrOffRootAux28           0x3D28u
NvpwrOffRootAux2C           0x3D2Cu

NvpwrOffBoardSetFn          0x2D0u     /* inside the board object, not the root */
NvpwrOffSelector2           0x104u     /* likewise */
NvpwrOffSelector3           0x1F4u     /* likewise */
```

**These are the most likely to survive a version bump and the most dangerous to assume.**

The `Root*` block is one densely packed struct — `0x3D10` through `0x3D2C`, one byte apart in
places. NVIDIA has kept the layout across the builds this project has seen, which is why the
driver can recognise them at all; but a field inserted anywhere in that range shifts everything
after it, and the failure would be a **silent write to the wrong byte** rather than a crash.

**Verify, do not assume.** Find the generator function and read which offsets off its context
argument it actually loads and stores. The roles:

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
| `+3D2C` | auxiliary 2C |

### 4. Machine-code signatures — 6 arrays

```
NvpwrSigGpuRegistry   17 bytes
NvpwrSigUpperLoad      7 bytes
NvpwrSigF7Record       7 bytes
NvpwrSigBoardSet      15 bytes
NvpwrSigSetAmount     21 bytes
NvpwrSigSetElig       18 bytes
```

Each is compared byte for byte against `imageBase + Rva[g_SigRvaIndex[i]]`, and the result is
recorded in a **mask** rather than short-circuited — so a failure names the signature and its
RVA instead of just saying something is wrong.

**Copy the opening bytes of each newly located function.** Keep the length the same as the old
signature so the two stay comparable in review, and update `SigLen[]` to match — a wrong length
does not fail to compile, it just reports a mismatch, which is the single most misleading
outcome during a port.

Where you have a choice, pick bytes that are **stable** across compiler versions. A sequence
encoding a relative branch or an absolute address differs between builds even when the function
is unchanged, and you want the check to fail on a real change rather than on recompilation
noise. The current signatures are mostly register operations and small immediates for this
reason.

---

## Procedure

```
 1. Obtain the new nvlddmkm.sys.          From DriverStore\FileRepository\nv_dispi.inf_*\,
                                          or from the running system via its loaded base.
 2. Run NvpwrCtl.exe diagnose.
                                          → the actual PE identity, and whether any
                                            existing table entry matches
 3. Read its PE TimeDateStamp and SizeOfImage.
                                          → TimeDateStamp, SizeOfImage in the new entry
 4. Open it and the 616.92 image side by side in IDA/Ghidra.
 5. Locate the 8 functions by body shape. → the 8 Rva[] values
 6. Confirm the 22 offsets.               → the 22 Off[] values
 7. Copy the opening bytes of each function.
                                          → the 6 Sig[][] rows and their SigLen[] values
 8. Add the entry to g_Targets[] in src/driver/target.h. Copy the 616.92 block, change
    the numbers, change Name. driver.c is not touched.
 9. Rebuild Nvpwr.sys.
10. Re-sign it with the test certificate, and import the certificate on the target machine.
11. Run diagnose again: all six signatures should report ok.
12. Update the version note at the top of driver.c and the table in README.md.
```

## Validating the port

The driver fails closed, so the first signal is `STATUS_REVISION_MISMATCH` from
`NvpwrCtl.exe status`, with a `Detail` code that says whether it was the PE identity or a
signature:

| Detail | Meaning | Where to look |
|---|---|---|
| 1 | PE identity not in the table | step 3 |
| 2 | A signature does not match | steps 5 and 7 |
| 3–13 | The build matched but an object or its semantics moved | step 6 |

`diagnose` then names the failing signature and its RVA, which turns "something is wrong" into
"this one RVA is wrong".

Once it loads, the three-phase sequence in `SetPowerTarget` is the functional test. **Phase A
stages `base + amount` while the ceiling is still at the saved OEM value** and re-runs NVIDIA's
generator to prove it converged; only then does Phase B raise the ceiling. If Phase A fails, the
offsets are wrong even though the signatures matched, and the driver restores the baseline it
captured before its first write.

A useful cross-check while porting: `NvpwrCtl.exe status` should show its `F7 input` and
`UPPER` lines as plausible milliwatt values — on the reference machine, 175000 for both at
stock. **If they read as zero or as garbage, the offsets have shifted.**

---

## 中文

> 驱动绑定到 NVIDIA 内核显示驱动的若干具体构建。本文是**完整的绑定清单**、它们存放的位置，
> 以及如何新增一个构建。

### 为什么必须绑定

`Nvpwr.sys` **从不**修改磁盘上的 `nvlddmkm.sys`，**也从不**修改 NVIDIA 的可执行代码。它做的
是：在内存中定位 NVIDIA 的**活的**功率策略对象，修改其原生生成器所读取的策略**数据**字段，
然后调用 NVIDIA **现成的** setter。

这依赖四件只存在于特定构建里的事：函数在哪（RVA）、对象在哪（结构偏移）、字段是什么含义、
以及那些函数**仍然是你以为的那个函数**（机器码签名）。**这四类在第一次写入之前全部会被
检查。**

### 一个构建是怎么被描述的

**所有和构建绑定的东西都在一个文件里：`src/driver/target.h`。** 它是一张受支持构建的表。

驱动从已加载的映像读出实际的 PE 标识，在表里查，然后用**那一项的** RVA、偏移和签名。
`driver.c` 里的 `ValidateBuild` 负责查表，**从此不再需要改动**。

**新增一个构建 = 新增一个表项。** `driver.c` 一行都不用碰。

这也意味着一份 `Nvpwr.sys` 可以同时支持多个驱动版本，每个都用**它自己的**签名校验 ——
用户升级显示驱动后不必等新的二进制。

### 第一步：先跑诊断

**在动手逆向之前，先问驱动它看到了什么：**

```
NvpwrCtl.exe diagnose
```

对已经在表里的构建，它会逐条列出签名的 RVA 和是否匹配。对**还不在表里**的构建，它会打印
新表项首先需要的两个数字：`Actual timestamp` 和 `Actual SizeOfImage`。

`NvpwrCtl.exe status` 也会把驱动的 `Detail` 码连同一句话报出来，用来说明失败在 PE 标识
（去加表项）还是某条签名（表项里某个 RVA 抄错了）。

### 清单（共 32 项）

| 类别 | 数量 | 是否必然要重做 |
|---|---|---|
| **PE 构建标识** | 2 | ✅ 每次构建都变 |
| **函数 RVA** | 8 | ✅ 必然要重做 |
| **结构偏移** | 22 | ⚠️ **最可能稳定，但最危险** |
| **机器码签名** | 6（85 字节） | ✅ 必然要重做 |

### 关于字段偏移的警告

`Root*` 那一块是**一个紧凑排布的结构体** —— `0x3D10` 到 `0x3D2C`，有些字段只差一个字节。
NVIDIA 在本项目见过的构建里保持了它的布局，**但在这个范围内任何位置插入一个字段，后面
全部会平移**，而失败的形态是**写到错误的字节**，不是崩溃。

**所以：要验证，不要假设。** 方法是从生成器函数里读出它实际加载和存储了哪些偏移。

### 关于签名的建议

- **逐条比对而不是短路** —— 现在失败会说出是哪一条、在哪个 RVA，"有东西不对"因此变成
  "这一个 RVA 错了"。
- **`SigLen[]` 要和字节数对上。** 长度写错**不会编译失败**，只会报"签名不匹配"，而那是
  移植过程中最容易误导人的一种表现。
- **尽量挑编译器版本之间稳定的字节。** 编码了相对跳转或绝对地址的序列，即使函数没变也会
  随重新编译而不同。

### 验证移植

| Detail | 含义 | 去看 |
|---|---|---|
| 1 | PE 标识不在表里 | 第 3 步 |
| 2 | 某条签名不匹配 | 第 5、7 步 |
| 3–13 | 构建匹配了，但对象或其语义移动了 | 第 6 步 |

能加载之后，`SetPowerTarget` 的三阶段序列就是功能测试。**Phase A 在墙仍处于 OEM 值时
就去构建 `base + amount`，然后重跑 NVIDIA 的生成器证明它收敛了**；只有这之后 Phase B
才抬墙。Phase A 失败说明偏移错了（尽管签名匹配），此时驱动会恢复到第一次写入前捕获的基线。

移植时一个有用的交叉验证：`status` 输出里的 `F7 input` 和 `UPPER` 应该是合理的毫瓦值 ——
参考机器出厂时都是 175000。**如果读成 0 或乱码，说明偏移已经移位了。**
