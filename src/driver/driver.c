#include <ntddk.h>
#include <ntimage.h>
#include <aux_klib.h>
#include "..\\shared\\nvpwr_ioctl.h"
#include "target.h"

/*
    NVPWR 1.5.0 EXPERIMENTAL — REVIEW / DEBUG NOTES

    WHERE:
      This driver never patches nvlddmkm.sys on disk and does not patch NVIDIA
      executable code. It resolves the already-loaded 616.92 kernel image,
      locates NVIDIA's live power-policy objects, changes the policy data fields
      used by the native generator, and calls NVIDIA's existing native setters.

    WHAT:
      root+3D14  = base / cTGP input
      root+3D18  = dynamic amount (our controlled model uses 25 W)
      root+3D24  = F7 saturation ceiling / UPPER
      selector2  = Board MAX (source FE)
      selector3  = CURRENT, including source F7 produced by NVIDIA's generator

    WHY TWO PHASES:
      Phase A changes base+amount while MAX/UPPER remain at the saved OEM
      ceiling. This proves that the real NVIDIA generator produces the expected
      staged state before the ceiling itself is raised.
      Phase B raises selector2/MAX and root+3D24/UPPER.
      Phase C re-runs NVIDIA's native generator and verifies that MAX, CURRENT,
      F7 and UPPER converge on the requested target.

    FAIL-CLOSED:
      Exact PE identity and machine-code signatures are checked before any
      mutation. On a failed transition we attempt to restore the exact baseline
      captured before the first write in this driver session.

    LOGGING:
      Kernel diagnostics use DbgPrintEx and are prefixed with [NVPWR].
      User-mode GUI activity is also written to
      C:\\ProgramData\\NvpwrControl\\nvpwr-control.log.
*/

#define NVPWR_LOG_INFO(...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[NVPWR][INFO] " __VA_ARGS__)
#define NVPWR_LOG_WARN(...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL, "[NVPWR][WARN] " __VA_ARGS__)
#define NVPWR_LOG_ERROR(...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[NVPWR][ERROR] " __VA_ARGS__)

#define NVPWR_TAG 'RWPN'

/*
    所有和具体 nvlddmkm 构建绑定的数字都在 target.h 里，按构建分组存放。

    原先它们是这里的 32 个 #define，全文件 64 处直接引用；加一个驱动版本意味着改
    32 行再核对 64 个引用点，而其中结构偏移写错了不会崩溃、只会写到错误的字节上。
    现在是表驱动：移植 = 在 g_Targets[] 里加一个表项，这个文件一行都不用改。

    下面这组没有搬走，因为它们描述的是【语义】而不是位置 —— 槽位结构的字段含义
    和功率范围不会随驱动构建变化。
*/

#define SLOT_MODE                0x00u
#define SLOT_COUNT               0x01u
#define SLOT_EFFECTIVE           0x04u
#define SLOT_SECONDARY           0x08u
#define SLOT_SOURCE_ID0          0x0Cu
#define SLOT_SOURCE_VALUE0       0x10u
#define SLOT_SOURCE_STRIDE       0x08u
#define SLOT_SOURCE_MAX          8u

#define SOURCE_FE                0xFEu
#define SOURCE_F7                0xF7u
#define SELECTOR_MAX             2u
#define SELECTOR_CURRENT         3u

#define POWER_LOW_STOCK           115000u
#define POWER_LOW_MIN             120000u
#define POWER_LOW_MAX             140000u
#define POWER_5070_MIN            145000u

/*
    PER-MACHINE OEM BASELINES ARE NO LONGER COMPILE-TIME CONSTANTS.
    WHERE: 1.8.0 defined POWER_5070_STOCK / POWER_4080_STOCK / POWER_4090_STOCK /
           POWER_4060_STOCK / POWER_4070_STOCK / POWER_4050_STOCK and compared
           the live OEM ceiling against them.
    WHAT:  those constants are gone. The OEM ceiling is now read from the live
           policy and accepted anywhere inside the POWER_OEM_MIN..POWER_OEM_MAX
           window defined below; each profile keeps only the lower bound of its
           own menu.
    WHY:   pinning the baseline to one machine's factory wattage is what made
           1.8.0 fail on the verified 175 W RTX 5090 Laptop: it could not
           classify stock, could not recognise a raised ceiling, and could not
           recover a partially applied state without a reboot.
    WHY NOT REMOVE THE LOW-POWER CHECK TOO: the 115 W class profiles keep an
           exact baseline test on purpose - that menu exists to unlock 120..140 W
           on machines NVIDIA ships at exactly 115 W, and treating a different
           ceiling as equivalent would ignore a different VRM/EC design.
*/

/*
    USER-SELECTABLE CEILING (1.9.0)
    WHERE: the delivered high bound of every per-profile target range.
    WHAT:  raised from the historical per-profile maxima (180/225/250 W) to a
           single absolute development ceiling of 350 W.
    WHY:   the previous builds hard-capped what a user could even request, so an
           already-unlocked machine could not be pushed further without editing
           and recompiling the driver. The requested value is now user data.

    SAFETY CONTRACT (unchanged and non-negotiable):
           Raising a bound only widens what may be *requested*. It does NOT relax
           the Phase A/Phase C readback verification. If NVIDIA's own generator
           does not converge on the requested value, or the EC/OEM ceiling refuses
           it, the transaction still fails closed and rolls back to the captured
           OEM baseline. "User responsibility" is implemented as "the user may ask
           for more", never as "we ignore a failed transition".
*/
#define POWER_CEILING_DEV         350000u
#define POWER_5070_MAX            POWER_CEILING_DEV
#define POWER_HIGH_MIN            175000u
#define POWER_HIGH_MAX            POWER_CEILING_DEV

/* RTX 40 Series Laptop Power Constants.
   Only the menu bounds remain; the stock baselines are read from the live
   policy (see the note above).

   The 4070 Laptop deliberately shares the 4060 bounds — both ship in the
   95..140 W class and the same 120..150 W menu applies, so a separate pair of
   constants would be two names for one number. */
#define POWER_4050_MIN            115000u
#define POWER_4050_MAX            140000u
#define POWER_4060_MIN            120000u
#define POWER_4060_MAX            150000u
#define POWER_4080_MIN            150000u
#define POWER_4080_MAX            POWER_CEILING_DEV
#define POWER_4090_MIN            150000u
#define POWER_4090_MAX            POWER_CEILING_DEV

#define POWER_ABSOLUTE_MIN        100000u
#define POWER_ABSOLUTE_MAX        POWER_CEILING_DEV
#define POWER_STEP                5000u
#define PPAB_FIXED               25000u
#define INVALID_POWER            0xFFFFFFFFu

/*
    GENERIC OEM-BASELINE SANITY WINDOW
    WHERE: coherent-stock and recovery classification.
    WHAT:  the OEM ceiling of a supported laptop is accepted anywhere in this
           window instead of being pinned to one machine's value (140 W).
    WHY:   1.8.0 hard-coded 140 W / 145..160 W in the recovery path, so the
           verified 175 W machine (RTX 5090 Laptop) could not be classified or
           recovered at all. The window is deliberately wide enough to cover
           75..250 W laptops but still rejects implausible policy values.
*/
#define POWER_OEM_MIN             75000u
#define POWER_OEM_MAX             250000u

/*
    当前生效的构建表项。由 ValidateBuild 按 PE 标识从 g_Targets[] 匹配后写入。

    初值不是 NULL，而是第 0 项 —— 这是必须的：FillStatusFromContext 里有几处
    (PUCHAR)Ctx->Board + g_T->Off[...] 的加法发生在 __try 之外，如果 g_T 为空，
    那里会是一次真正的内核空指针解引用，而不是被 __try 捕获的异常。指向表里第一
    项最坏情况是"用错误的偏移去读"，会被 __try 拦下；空指针不会。
*/
static const NVPWR_TARGET* g_T = &g_Targets[0];

static PDEVICE_OBJECT g_DeviceObject = NULL;
static UNICODE_STRING g_SymbolicLink;
static KMUTEX g_OperationMutex;
static volatile ULONG g_LastNvStatus = 0;
/* Tracks one controlled session. NVIDIA objects are not pinned; address equality
   is only a guard until explicit restore or reboot. */
static BOOLEAN g_MutationAttempted = FALSE;
static PVOID g_MutatedRoot = NULL;
static PVOID g_MutatedBoard = NULL;
static BOOLEAN g_SavedValid = FALSE;
static UCHAR g_SavedEligibility = 0;
static UCHAR g_SavedAmountActive = 0;
static ULONG g_SavedInput14 = 0;
static ULONG g_SavedAmount18 = 0;
static ULONG g_SavedUpper24 = 0;
static ULONG g_ActiveProfile = NvpwrProfileUnknown;
/*
    g_SessionMaxMw — the high bound authorized for the CURRENT request.
    Verification compares against this instead of a compile-time constant so a
    user-selected ceiling is honoured end to end without weakening the readback
    checks: a value above the ceiling is rejected up front, and a value at or
    below it must still converge exactly or the transaction rolls back.
*/
static ULONG g_SessionMaxMw = POWER_ABSOLUTE_MAX;
static BOOLEAN g_SessionMaxValid = FALSE;

typedef PVOID (*PFN_POLICY_LOOKUP)(PVOID Registry, ULONG Key);
typedef ULONG (*PFN_BOARD_SET)(
    PVOID Major,
    PVOID Root,
    PVOID Board,
    ULONG Selector,
    ULONG Source,
    ULONG Value);
typedef ULONG (*PFN_SET_AMOUNT)(PVOID Major, ULONG Amount);
typedef ULONG (*PFN_SET_ELIG)(PVOID Major, ULONG Eligible);

typedef struct _NVPWR_CONTEXT {
    PUCHAR Base;
    ULONG ModuleSize;
    ULONG TimeDateStamp;
    ULONG SizeOfImage;
    PVOID DriverGlobal;
    PVOID GpuTable;
    ULONG RegistryCount;
    ULONG SelectedIndex;
    ULONG GpuId;
    PVOID Major;
    PVOID Root;
    PFN_POLICY_LOOKUP Lookup;
    PVOID Board;
    PFN_BOARD_SET BoardSet;
    PFN_SET_AMOUNT SetAmount;
    PFN_SET_ELIG SetEligibility;

    /*
        校验结果，留给诊断用。

        TargetIndex 是命中的 g_Targets[] 下标，0xFFFFFFFF 表示没有任何表项匹配这个
        PE 标识 —— 也就是"这个驱动版本还不支持"。
        SigMask 的第 i 位表示第 i 条签名是否匹配（顺序同 NVPWR_SIG_ID）。

        两者在失败时也是有效的，这正是诊断 IOCTL 能回答"失败在哪一步、哪一条"的原因。
        原先签名是一串 || 连起来的，第一个不匹配就短路返回，只知道"有东西不对"。
    */
    ULONG TargetIndex;
    ULONG SigMask;
} NVPWR_CONTEXT;

static BOOLEAN BytesEqual(const UCHAR* A, const UCHAR* B, SIZE_T N)
{
    SIZE_T i;
    for (i = 0; i < N; ++i) {
        if (A[i] != B[i]) return FALSE;
    }
    return TRUE;
}

static BOOLEAN IsKernelPointer(PVOID P)
{
    return P != NULL && (ULONG_PTR)P >= (ULONG_PTR)MmSystemRangeStart;
}

static BOOLEAN IsInsideImage(const NVPWR_CONTEXT* Ctx, PVOID P)
{
    ULONG_PTR p, b, e;
    if (!Ctx || !Ctx->Base || !P) return FALSE;
    p = (ULONG_PTR)P;
    b = (ULONG_PTR)Ctx->Base;
    e = b + Ctx->SizeOfImage;
    return p >= b && p < e;
}

static NTSTATUS FindNvlddmkm(PUCHAR* ImageBase, PULONG ImageSize)
{
    NTSTATUS status;
    ULONG bytes = 0;
    PAUX_MODULE_EXTENDED_INFO modules = NULL;
    ULONG count, i;
    ANSI_STRING wanted;

    if (!ImageBase || !ImageSize) return STATUS_INVALID_PARAMETER;
    *ImageBase = NULL;
    *ImageSize = 0;

    status = AuxKlibInitialize();
    if (!NT_SUCCESS(status)) return status;

    status = AuxKlibQueryModuleInformation(&bytes, sizeof(AUX_MODULE_EXTENDED_INFO), NULL);
    if (!NT_SUCCESS(status) || bytes == 0)
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;

    modules = (PAUX_MODULE_EXTENDED_INFO)ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, NVPWR_TAG);
    if (!modules) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(modules, bytes);

    status = AuxKlibQueryModuleInformation(&bytes, sizeof(AUX_MODULE_EXTENDED_INFO), modules);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(modules, NVPWR_TAG);
        return status;
    }

    RtlInitAnsiString(&wanted, "nvlddmkm.sys");
    count = bytes / sizeof(AUX_MODULE_EXTENDED_INFO);
    for (i = 0; i < count; ++i) {
        ANSI_STRING current;
        const CHAR* baseName = (const CHAR*)&modules[i].FullPathName[modules[i].FileNameOffset];
        RtlInitAnsiString(&current, baseName);
        if (RtlEqualString(&current, &wanted, TRUE)) {
            *ImageBase = (PUCHAR)modules[i].BasicInfo.ImageBase;
            *ImageSize = modules[i].ImageSize;
            NVPWR_LOG_INFO("FindNvlddmkm: base=%p moduleSize=0x%lX\n", *ImageBase, *ImageSize);
            ExFreePoolWithTag(modules, NVPWR_TAG);
            return STATUS_SUCCESS;
        }
    }

    ExFreePoolWithTag(modules, NVPWR_TAG);
    NVPWR_LOG_ERROR("FindNvlddmkm: nvlddmkm.sys not found in loaded module list\n");
    return STATUS_NOT_FOUND;
}

static NTSTATUS ReadPeIdentity(PUCHAR ImageBase, PULONG TimeDateStamp, PULONG SizeOfImage)
{
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS64 nt;

    if (!ImageBase || !TimeDateStamp || !SizeOfImage) return STATUS_INVALID_PARAMETER;

    __try {
        dos = (PIMAGE_DOS_HEADER)ImageBase;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return STATUS_INVALID_IMAGE_FORMAT;
        nt = (PIMAGE_NT_HEADERS64)(ImageBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return STATUS_INVALID_IMAGE_FORMAT;
        *TimeDateStamp = nt->FileHeader.TimeDateStamp;
        *SizeOfImage = nt->OptionalHeader.SizeOfImage;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_SUCCESS;
}

static NTSTATUS ValidateBuild(NVPWR_CONTEXT* Ctx, PULONG Detail)
{
    NTSTATUS status;
    ULONG i, t;
    ULONG matched = NVPWR_TARGET_NONE;
    ULONG mask = 0;

    Ctx->TargetIndex = NVPWR_TARGET_NONE;
    Ctx->SigMask = 0;

    status = FindNvlddmkm(&Ctx->Base, &Ctx->ModuleSize);
    if (!NT_SUCCESS(status)) {
        NVPWR_LOG_ERROR("ValidateBuild: FindNvlddmkm failed NTSTATUS=0x%08X\n", status);
        return status;
    }

    status = ReadPeIdentity(Ctx->Base, &Ctx->TimeDateStamp, &Ctx->SizeOfImage);
    if (!NT_SUCCESS(status)) {
        if (Detail) *Detail = NvpwrDetailPeIdentity;
        return status;
    }

    NVPWR_LOG_INFO("ValidateBuild: timestamp=0x%08lX image=0x%08lX module=0x%08lX\n",
        Ctx->TimeDateStamp, Ctx->SizeOfImage, Ctx->ModuleSize);

    if (Ctx->ModuleSize < Ctx->SizeOfImage) {
        NVPWR_LOG_ERROR("ValidateBuild: module size 0x%08lX smaller than SizeOfImage 0x%08lX\n",
            Ctx->ModuleSize, Ctx->SizeOfImage);
        if (Detail) *Detail = NvpwrDetailPeIdentity;
        return STATUS_REVISION_MISMATCH;
    }

    /*
        按 PE 标识在表里找。这是整个移植模型的支点：支持一个新驱动 = 在 target.h 的
        g_Targets[] 里加一项，这里一行都不用改。
    */
    for (t = 0; t < NVPWR_TARGET_COUNT; ++t) {
        if (g_Targets[t].TimeDateStamp == Ctx->TimeDateStamp &&
            g_Targets[t].SizeOfImage == Ctx->SizeOfImage) {
            matched = t;
            break;
        }
    }

    if (matched == NVPWR_TARGET_NONE) {
        /*
            把已知的清单整个打出来。移植时这一条日志就是起点：它同时给出了"实际是
            什么"和"驱动认识什么"，而这两个数字正是 target.h 新表项的头两行。
        */
        NVPWR_LOG_ERROR("ValidateBuild: PE identity 0x%08lX/0x%08lX matches no known target\n",
            Ctx->TimeDateStamp, Ctx->SizeOfImage);
        for (i = 0; i < NVPWR_TARGET_COUNT; ++i) {
            NVPWR_LOG_ERROR("  known[%lu] %s timestamp=0x%08lX size=0x%08lX\n",
                i, g_Targets[i].Name, g_Targets[i].TimeDateStamp, g_Targets[i].SizeOfImage);
        }
        if (Detail) *Detail = NvpwrDetailPeIdentity;
        return STATUS_REVISION_MISMATCH;
    }

    g_T = &g_Targets[matched];
    Ctx->TargetIndex = matched;
    NVPWR_LOG_INFO("ValidateBuild: matched target %s (index %lu of %lu)\n",
        g_T->Name, matched, (ULONG)NVPWR_TARGET_COUNT);

    /*
        逐条校验签名，并且【不短路】。

        原先是一条 || 链：第一个不匹配就返回，只知道"有东西不对"。改成逐条记录掩码
        之后，失败时能说出是第几条，而每条签名对应一个明确的 RVA —— 移植时"哪个
        RVA 找错了"因此变成一个可以直接读出来的答案，而不是靠猜。

        掩码在失败路径上也写回 Ctx，诊断 IOCTL 靠它工作。
    */
    __try {
        for (i = 0; i < NVPWR_TARGET_SIG_COUNT; ++i) {
            const ULONG rva = g_T->Rva[g_SigRvaIndex[i]];
            if (BytesEqual(Ctx->Base + rva, g_T->Sig[i], g_T->SigLen[i])) {
                mask |= (1u << i);
            } else {
                NVPWR_LOG_ERROR("ValidateBuild: sig %lu/%lu %s MISMATCH at RVA 0x%08lX (len %lu)\n",
                    i + 1, (ULONG)NVPWR_TARGET_SIG_COUNT, g_SigNames[i], rva, g_T->SigLen[i]);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Ctx->SigMask = mask;
        NVPWR_LOG_ERROR("ValidateBuild: faulted while reading signatures mask=0x%02lX\n", mask);
        if (Detail) *Detail = NvpwrDetailCodeSignature;
        return GetExceptionCode();
    }

    Ctx->SigMask = mask;

    if (mask != NVPWR_SIG_ALL) {
        ULONG good = 0;
        for (i = 0; i < NVPWR_TARGET_SIG_COUNT; ++i) { if (mask & (1u << i)) ++good; }
        NVPWR_LOG_ERROR("ValidateBuild: only %lu of %lu signatures matched (mask=0x%02lX)\n",
            good, (ULONG)NVPWR_TARGET_SIG_COUNT, mask);
        if (Detail) *Detail = NvpwrDetailCodeSignature;
        return STATUS_REVISION_MISMATCH;
    }

    NVPWR_LOG_INFO("ValidateBuild: %s PE identity + %lu signatures PASSED\n",
        g_T->Name, (ULONG)NVPWR_TARGET_SIG_COUNT);
    return STATUS_SUCCESS;
}

/*
    只读的构建诊断。

    和 ValidateBuild 的区别是它【什么都不写】：不改 g_T，不改任何设备状态，也不
    要求构建校验通过。因此它可以在一个不受支持的 nvlddmkm 上安全调用 —— 而那种
    情况恰恰是最需要它的时候。

    它按 PE 标识在表里找，找到就把该表项每条签名所处的 RVA 和匹配情况一起报出去。
    移植到新驱动时，第一步就是跑一次这个：日志和结构里会直接列出"实际 PE 标识"
    和"每条签名在哪个 RVA、匹配没有"，而"哪条签名没匹配"等价于"哪个 RVA 找错了"。
*/
static VOID DiagnoseBuild(NVPWR_DIAGNOSIS* Out)
{
    PUCHAR base = NULL;
    ULONG moduleSize = 0, tstamp = 0, sizeOfImage = 0;
    ULONG i, t, matched = NVPWR_TARGET_NONE;
    NTSTATUS status;

    RtlZeroMemory(Out, sizeof(*Out));
    Out->Version = NVPWR_DIAG_VERSION;
    Out->TargetIndex = NVPWR_TARGET_NONE;
    Out->TargetCount = (ULONG)NVPWR_TARGET_COUNT;
    Out->SigCount = (ULONG)NVPWR_TARGET_SIG_COUNT;

    status = FindNvlddmkm(&base, &moduleSize);
    if (!NT_SUCCESS(status)) {
        NVPWR_LOG_ERROR("DiagnoseBuild: FindNvlddmkm failed NTSTATUS=0x%08X\n", status);
        return;
    }
    Out->ModuleSize = moduleSize;

    status = ReadPeIdentity(base, &tstamp, &sizeOfImage);
    if (!NT_SUCCESS(status)) {
        NVPWR_LOG_ERROR("DiagnoseBuild: ReadPeIdentity failed NTSTATUS=0x%08X\n", status);
        return;
    }
    Out->TimeDateStamp = tstamp;
    Out->SizeOfImage = sizeOfImage;

    NVPWR_LOG_INFO("DiagnoseBuild: actual timestamp=0x%08lX size=0x%08lX module=0x%08lX\n",
        tstamp, sizeOfImage, moduleSize);

    for (t = 0; t < NVPWR_TARGET_COUNT; ++t) {
        if (g_Targets[t].TimeDateStamp == tstamp && g_Targets[t].SizeOfImage == sizeOfImage) {
            matched = t;
            break;
        }
    }

    if (matched == NVPWR_TARGET_NONE) {
        /*
            没有可参照的表项，RVA 就无从谈起，签名也就没得比。这不是失败，是
            "这个构建还没被分析过"—— 而上面那行日志已经把该抄的两个数字给出去了。
        */
        NVPWR_LOG_ERROR("DiagnoseBuild: no table entry matches this build; porting required\n");
        for (i = 0; i < NVPWR_TARGET_COUNT; ++i) {
            NVPWR_LOG_ERROR("  known[%lu] %s timestamp=0x%08lX size=0x%08lX\n",
                i, g_Targets[i].Name, g_Targets[i].TimeDateStamp, g_Targets[i].SizeOfImage);
        }
        return;
    }

    Out->TargetIndex = matched;
    Out->ExpectedTimeDateStamp = g_Targets[matched].TimeDateStamp;
    Out->ExpectedSizeOfImage = g_Targets[matched].SizeOfImage;
    for (i = 0; i < sizeof(Out->TargetName) - 1 && g_Targets[matched].Name[i] != 0; ++i) {
        Out->TargetName[i] = g_Targets[matched].Name[i];
    }

    NVPWR_LOG_INFO("DiagnoseBuild: matches target %s (index %lu)\n", g_Targets[matched].Name, matched);

    __try {
        for (i = 0; i < NVPWR_TARGET_SIG_COUNT && i < NVPWR_DIAG_SIG_MAX; ++i) {
            const ULONG rva = g_Targets[matched].Rva[g_SigRvaIndex[i]];
            Out->SigRva[i] = rva;
            Out->SigLen[i] = g_Targets[matched].SigLen[i];
            if (BytesEqual(base + rva, g_Targets[matched].Sig[i], g_Targets[matched].SigLen[i])) {
                Out->SigMask |= (1u << i);
            } else {
                NVPWR_LOG_ERROR("DiagnoseBuild: sig %lu %s MISMATCH at RVA 0x%08lX (len %lu)\n",
                    i, g_SigNames[i], rva, g_Targets[matched].SigLen[i]);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        NVPWR_LOG_ERROR("DiagnoseBuild: faulted reading signatures mask=0x%02lX\n", Out->SigMask);
        return;
    }

    NVPWR_LOG_INFO("DiagnoseBuild: done mask=0x%02lX of 0x%02lX\n", Out->SigMask, NVPWR_SIG_ALL);
}

static BOOLEAN FindSource(PUCHAR Slot, UCHAR Source, PULONG Value, PUCHAR Index)
{
    UCHAR count, i;
    if (!Slot || !Value) return FALSE;

    __try {
        count = Slot[SLOT_COUNT];
        if (count > SLOT_SOURCE_MAX) return FALSE;
        for (i = 0; i < count; ++i) {
            PUCHAR e = Slot + SLOT_SOURCE_ID0 + ((ULONG)i * SLOT_SOURCE_STRIDE);
            if (e[0] == Source) {
                *Value = *(UNALIGNED ULONG*)(e + 4);
                if (Index) *Index = i;
                return TRUE;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
    return FALSE;
}

static ULONG ComputeStrictActiveF7(PVOID Root)
{
    ULONG c, a, b, u, pre;
    UCHAR amountActive, eligible;

    __try {
        amountActive = *(volatile UCHAR*)((PUCHAR)Root + g_T->Off[NvpwrOffRootAmountActive]);
        eligible = *(volatile UCHAR*)((PUCHAR)Root + g_T->Off[NvpwrOffRootElig]);
        c = *(volatile ULONG*)((PUCHAR)Root + g_T->Off[NvpwrOffRootCtgp]);
        a = *(volatile ULONG*)((PUCHAR)Root + g_T->Off[NvpwrOffRootAmount]);
        b = *(volatile ULONG*)((PUCHAR)Root + g_T->Off[NvpwrOffRootLower]);
        u = *(volatile ULONG*)((PUCHAR)Root + g_T->Off[NvpwrOffRootUpper]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return INVALID_POWER;
    }

    /* This is the exact 4E3EF0 finite-cTGP branch used by the planned proof:
       amountActive=1, eligibility=1, U>B, A<=U-B.
       It reduces to min(C, U-A) + A == min(C+A, U). */
    if (!amountActive || !eligible || c == INVALID_POWER || u <= b || a > (u - b))
        return INVALID_POWER;

    pre = u - a;
    if (c < pre) pre = c;
    if (pre > (0xFFFFFFFFu - a)) return INVALID_POWER;
    return pre + a;
}

static NTSTATUS ResolveContext(NVPWR_CONTEXT* Ctx, PULONG Detail)
{
    NTSTATUS status;
    ULONG i, count;
    PVOID globalState, table;

    if (!Ctx) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Ctx, sizeof(*Ctx));
    if (Detail) *Detail = NvpwrDetailOk;

    NVPWR_LOG_INFO("ResolveContext: begin\n");
    status = ValidateBuild(Ctx, Detail);
    if (!NT_SUCCESS(status)) {
        NVPWR_LOG_ERROR("ResolveContext: build validation failed NTSTATUS=0x%08X detail=%lu\n",
            status, Detail ? *Detail : 0u);
        return status;
    }

    __try {
        globalState = *(PVOID*)(Ctx->Base + g_T->Rva[NvpwrRvaGpuGlobal]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (Detail) *Detail = NvpwrDetailGlobalPointer;
        return GetExceptionCode();
    }
    if (!IsKernelPointer(globalState)) {
        if (Detail) *Detail = NvpwrDetailGlobalPointer;
        return STATUS_DEVICE_NOT_READY;
    }
    Ctx->DriverGlobal = globalState;

    __try {
        table = *(PVOID*)((PUCHAR)globalState + g_T->Off[NvpwrOffGlobalGpuTable]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (Detail) *Detail = NvpwrDetailGpuTable;
        return GetExceptionCode();
    }
    if (!IsKernelPointer(table)) {
        if (Detail) *Detail = NvpwrDetailGpuTable;
        return STATUS_DEVICE_NOT_READY;
    }
    Ctx->GpuTable = table;

    __try {
        count = *(volatile ULONG*)((PUCHAR)table + g_T->Off[NvpwrOffTableCount]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (Detail) *Detail = NvpwrDetailGpuTable;
        return GetExceptionCode();
    }
    Ctx->RegistryCount = count;
    NVPWR_LOG_INFO("ResolveContext: NVIDIA GPU registry count=%lu\n", count);
    if (count == 0 || count > g_T->Off[NvpwrOffGpuCountMax]) {
        if (Detail) *Detail = NvpwrDetailGpuTable;
        return STATUS_DEVICE_NOT_READY;
    }

    for (i = 0; i < count; ++i) {
        PVOID major = NULL, root = NULL, board = NULL, lookupRaw = NULL, boardSetRaw = NULL;
        ULONG gpuId = 0;
        UCHAR init = 0, key = 0;
        PFN_POLICY_LOOKUP lookup;

        __try {
            major = *(PVOID*)((PUCHAR)table + g_T->Off[NvpwrOffTableEntryPtr] + ((SIZE_T)i * g_T->Off[NvpwrOffGpuEntryStride]));
            gpuId = *(volatile ULONG*)((PUCHAR)table + g_T->Off[NvpwrOffTableEntryGpuId] + ((SIZE_T)i * g_T->Off[NvpwrOffGpuEntryStride]));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!IsKernelPointer(major)) continue;

        __try {
            root = *(PVOID*)((PUCHAR)major + g_T->Off[NvpwrOffMajorPowerRoot]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!IsKernelPointer(root)) continue;

        __try {
            init = *(volatile UCHAR*)((PUCHAR)root + g_T->Off[NvpwrOffRootInit]);
            key = *(volatile UCHAR*)((PUCHAR)root + g_T->Off[NvpwrOffRootPolicyKey]);
            lookupRaw = *(PVOID*)((PUCHAR)root + g_T->Off[NvpwrOffRootLookupFn]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (init != 1 || key >= 0x40 || !IsInsideImage(Ctx, lookupRaw)) continue;

        lookup = (PFN_POLICY_LOOKUP)lookupRaw;
        __try {
            board = lookup((PUCHAR)root + g_T->Off[NvpwrOffRootRegistry], (ULONG)key);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            board = NULL;
        }
        if (!IsKernelPointer(board)) continue;

        __try {
            boardSetRaw = *(PVOID*)((PUCHAR)board + g_T->Off[NvpwrOffBoardSetFn]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (boardSetRaw != (PVOID)(Ctx->Base + g_T->Rva[NvpwrRvaBoardType0Set])) continue;

        Ctx->SelectedIndex = i;
        Ctx->GpuId = gpuId;
        Ctx->Major = major;
        Ctx->Root = root;
        Ctx->Lookup = lookup;
        Ctx->Board = board;
        Ctx->BoardSet = (PFN_BOARD_SET)boardSetRaw;
        Ctx->SetAmount = (PFN_SET_AMOUNT)(Ctx->Base + g_T->Rva[NvpwrRvaSetAmount]);
        Ctx->SetEligibility = (PFN_SET_ELIG)(Ctx->Base + g_T->Rva[NvpwrRvaSetElig]);
        NVPWR_LOG_INFO(
            "ResolveContext: selected index=%lu gpuId=0x%08lX major=%p root=%p board=%p boardSet=%p\n",
            Ctx->SelectedIndex, Ctx->GpuId, Ctx->Major, Ctx->Root, Ctx->Board, Ctx->BoardSet);
        return STATUS_SUCCESS;
    }

    NVPWR_LOG_ERROR("ResolveContext: no compatible NVIDIA power-policy object found\n");
    if (Detail) *Detail = NvpwrDetailGpuTable;
    return STATUS_NOT_FOUND;
}

static VOID FillStatusFromContext(const NVPWR_CONTEXT* Ctx, NVPWR_STATUS* Out)
{
    PUCHAR maxSlot, currentSlot;
    ULONG f7 = INVALID_POWER;

    Out->ModuleBase = (ULONGLONG)(ULONG_PTR)Ctx->Base;
    Out->TimeDateStamp = Ctx->TimeDateStamp;
    Out->SizeOfImage = Ctx->SizeOfImage;
    Out->RegistryCount = Ctx->RegistryCount;
    Out->SelectedIndex = Ctx->SelectedIndex;
    Out->GpuId = Ctx->GpuId;
    Out->DriverGlobal = (ULONGLONG)(ULONG_PTR)Ctx->DriverGlobal;
    Out->GpuTable = (ULONGLONG)(ULONG_PTR)Ctx->GpuTable;
    Out->MajorObject = (ULONGLONG)(ULONG_PTR)Ctx->Major;
    Out->PowerRoot = (ULONGLONG)(ULONG_PTR)Ctx->Root;
    Out->LookupFunction = (ULONGLONG)(ULONG_PTR)Ctx->Lookup;
    Out->BoardObject = (ULONGLONG)(ULONG_PTR)Ctx->Board;
    Out->BoardSetFunction = (ULONGLONG)(ULONG_PTR)Ctx->BoardSet;

    maxSlot = (PUCHAR)Ctx->Board + g_T->Off[NvpwrOffSelector2];
    currentSlot = (PUCHAR)Ctx->Board + g_T->Off[NvpwrOffSelector3];

    __try {
        Out->RootInitialized = *(volatile UCHAR*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootInit]);
        Out->Eligibility = *(volatile UCHAR*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootElig]);
        Out->AmountActive = *(volatile UCHAR*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootAmountActive]);
        Out->PolicyKey = *(volatile UCHAR*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootPolicyKey]);
        Out->CtgpTarget = *(volatile ULONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootCtgp]);
        Out->PpabAmount = *(volatile ULONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootAmount]);
        Out->LowerBoundary = *(volatile ULONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootLower]);
        Out->UpperBoundary = *(volatile ULONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootUpper]);
        Out->Aux28 = *(volatile ULONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootAux28]);
        Out->Aux2C = *(volatile ULONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootAux2C]);

        Out->MaxMode = maxSlot[SLOT_MODE];
        Out->MaxCount = maxSlot[SLOT_COUNT];
        Out->MaxSource0 = maxSlot[SLOT_SOURCE_ID0];
        Out->MaxEffective = *(volatile ULONG*)(maxSlot + SLOT_EFFECTIVE);
        Out->MaxSecondary = *(volatile ULONG*)(maxSlot + SLOT_SECONDARY);
        Out->MaxSource0Value = *(volatile ULONG*)(maxSlot + SLOT_SOURCE_VALUE0);

        Out->CurrentMode = currentSlot[SLOT_MODE];
        Out->CurrentCount = currentSlot[SLOT_COUNT];
        Out->CurrentEffective = *(volatile ULONG*)(currentSlot + SLOT_EFFECTIVE);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Out->State = NvpwrStateContextInvalid;
        Out->Detail = NvpwrDetailBoardObject;
        Out->LastNtStatus = GetExceptionCode();
        return;
    }

    if (FindSource(currentSlot, SOURCE_F7, &f7, NULL))
        Out->CurrentF7Value = f7;
    else
        Out->CurrentF7Value = INVALID_POWER;

    Out->PredictedF7 = ComputeStrictActiveF7(Ctx->Root);
    Out->LastNvStatus = g_LastNvStatus;
    Out->ActiveProfile = g_ActiveProfile;
    if (g_SavedValid && Ctx->Root == g_MutatedRoot && Ctx->Board == g_MutatedBoard)
        Out->OemBaseline = g_SavedUpper24;
    if (g_ActiveProfile == NvpwrProfileRtx5050Laptop ||
        g_ActiveProfile == NvpwrProfileRtx5060Laptop || g_ActiveProfile == NvpwrProfileRtx5070Laptop) {
        Out->SupportedMin = POWER_LOW_MIN;
        Out->SupportedMax = POWER_LOW_MAX;
    } else if (g_ActiveProfile == NvpwrProfileRtx5070TiLaptop) {
        Out->SupportedMin = POWER_5070_MIN;
        Out->SupportedMax = POWER_5070_MAX;
    } else if (g_ActiveProfile == NvpwrProfileRtx5080Laptop || g_ActiveProfile == NvpwrProfileRtx5090Laptop) {
        Out->SupportedMin = POWER_HIGH_MIN;
        Out->SupportedMax = POWER_HIGH_MAX;
    } else if (g_ActiveProfile == NvpwrProfileRtx4090Laptop) {
        Out->SupportedMin = POWER_4090_MIN;
        Out->SupportedMax = POWER_4090_MAX;
    } else if (g_ActiveProfile == NvpwrProfileRtx4080Laptop) {
        Out->SupportedMin = POWER_4080_MIN;
        Out->SupportedMax = POWER_4080_MAX;
    } else if (g_ActiveProfile == NvpwrProfileRtx4070Laptop || g_ActiveProfile == NvpwrProfileRtx4060Laptop) {
        Out->SupportedMin = POWER_4060_MIN;
        Out->SupportedMax = POWER_4060_MAX;
    } else if (g_ActiveProfile == NvpwrProfileRtx4050Laptop) {
        Out->SupportedMin = POWER_4050_MIN;
        Out->SupportedMax = POWER_4050_MAX;
    }

    /* 1.9.0: expose both the compile-time ceiling and the ceiling the user
       actually authorized for this session, so the UI can show which bound is
       really in force. */
    Out->CeilingMax = POWER_CEILING_DEV;
    Out->SessionMax = g_SessionMaxValid ? g_SessionMaxMw : POWER_CEILING_DEV;

    if (Out->MaxMode != 0 || Out->MaxCount != 1 || Out->MaxSource0 != SOURCE_FE) {
        Out->State = NvpwrStateContextInvalid;
        Out->Detail = NvpwrDetailSelector2Layout;
        Out->LastNtStatus = STATUS_DATA_ERROR;
        return;
    }
    if (Out->CurrentF7Value == INVALID_POWER) {
        Out->State = NvpwrStateContextInvalid;
        Out->Detail = NvpwrDetailSelector3F7;
        Out->LastNtStatus = STATUS_NOT_FOUND;
        return;
    }

    /* General applied state: base=(target-25 W), amount=25 W, and the
       generator/board/current paths all agree on the same target.
       1.9.0: the bound is the session ceiling the user authorized, not a
       compile-time constant. The convergence requirements below are unchanged. */
    if (Out->UpperBoundary >= POWER_ABSOLUTE_MIN &&
        Out->UpperBoundary <= (g_SessionMaxValid ? g_SessionMaxMw : POWER_ABSOLUTE_MAX) &&
        (Out->UpperBoundary % POWER_STEP) == 0 &&
        Out->MaxEffective == Out->UpperBoundary &&
        Out->MaxSource0Value == Out->UpperBoundary &&
        Out->RootInitialized == 1 &&
        Out->Eligibility == 1 &&
        Out->AmountActive == 1 &&
        Out->PpabAmount == PPAB_FIXED &&
        Out->CtgpTarget == (Out->UpperBoundary - PPAB_FIXED) &&
        Out->CurrentF7Value == Out->UpperBoundary &&
        Out->CurrentEffective == Out->UpperBoundary &&
        Out->PredictedF7 == Out->UpperBoundary) {
        Out->State = NvpwrStateApplied;
        Out->AppliedTarget = Out->UpperBoundary;
        Out->OemBaseline = g_SavedValid ? g_SavedUpper24 : 0;
        Out->ActiveProfile = g_ActiveProfile;
        Out->LastNtStatus = STATUS_SUCCESS;
        return;
    }

    /* Internal staging state: keep the saved OEM ceiling while changing
       generator inputs. This is dynamic so the same 616.92 code path can be
       evaluated on higher-power Blackwell Laptop GPUs without assuming 140 W. */
    if (g_SavedValid && Ctx->Root == g_MutatedRoot && Ctx->Board == g_MutatedBoard &&
        Out->UpperBoundary == g_SavedUpper24 &&
        Out->MaxEffective == g_SavedUpper24 &&
        Out->MaxSource0Value == g_SavedUpper24 &&
        Out->RootInitialized == 1 &&
        Out->Eligibility == 1 &&
        Out->AmountActive == 1 &&
        Out->PpabAmount == PPAB_FIXED &&
        Out->CtgpTarget >= (POWER_ABSOLUTE_MIN - PPAB_FIXED) &&
        Out->CtgpTarget <= (POWER_ABSOLUTE_MAX - PPAB_FIXED) &&
        Out->CurrentF7Value == g_SavedUpper24 &&
        Out->CurrentEffective == g_SavedUpper24 &&
        Out->PredictedF7 == g_SavedUpper24) {
        Out->State = NvpwrStateArmed;
        Out->AppliedTarget = Out->CtgpTarget + PPAB_FIXED;
        Out->OemBaseline = g_SavedUpper24;
        Out->ActiveProfile = g_ActiveProfile;
        Out->LastNtStatus = STATUS_SUCCESS;
        return;
    }

    /* Generic coherent OEM baseline for this exact 616.92 KMD layout.
       No hard-coded 140 W assumption: all Board/F7 paths must agree and the
       generator's dynamic amount must be inactive. 1.9.0 widens the accepted
       window so 75..250 W laptops (including the verified 175 W RTX 5090
       Laptop) classify as stock instead of falling through to MIXED. */
    if (Out->UpperBoundary >= POWER_OEM_MIN && Out->UpperBoundary <= POWER_OEM_MAX &&
        Out->MaxEffective == Out->UpperBoundary &&
        Out->MaxSource0Value == Out->UpperBoundary &&
        Out->RootInitialized == 1 &&
        Out->Eligibility == 0 &&
        Out->AmountActive == 0 &&
        Out->CtgpTarget == Out->UpperBoundary &&
        Out->PpabAmount == 0 &&
        Out->LowerBoundary < Out->UpperBoundary &&
        Out->CurrentF7Value == Out->UpperBoundary &&
        Out->CurrentEffective == Out->UpperBoundary) {
        Out->State = NvpwrStateStockBaseline;
        Out->AppliedTarget = Out->UpperBoundary;
        Out->OemBaseline = Out->UpperBoundary;
        Out->ActiveProfile = g_ActiveProfile;
        Out->LastNtStatus = STATUS_SUCCESS;
        return;
    }

    if (g_SavedValid &&
        Out->UpperBoundary == g_SavedUpper24 &&
        Out->MaxEffective == g_SavedUpper24 &&
        Out->MaxSource0Value == g_SavedUpper24) {
        Out->State = NvpwrStatePreconditionNotReady;
        Out->Detail = NvpwrDetailTestState;
        Out->LastNtStatus = STATUS_SUCCESS;
        return;
    }

    Out->State = NvpwrStateMixed;
    Out->LastNtStatus = STATUS_SUCCESS;
}

static NTSTATUS QueryStatus(NVPWR_STATUS* Out)
{
    NVPWR_CONTEXT ctx;
    NTSTATUS status;
    ULONG detail = NvpwrDetailOk;

    if (!Out) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Out, sizeof(*Out));
    Out->Version = NVPWR_STATUS_VERSION;
    Out->CurrentF7Value = INVALID_POWER;
    Out->PredictedF7 = INVALID_POWER;

    status = ResolveContext(&ctx, &detail);
    if (!NT_SUCCESS(status)) {
        Out->Detail = detail;
        Out->LastNtStatus = status;
        if (status == STATUS_NOT_FOUND && ctx.Base == NULL)
            Out->State = NvpwrStateModuleNotFound;
        else if (status == STATUS_REVISION_MISMATCH)
            Out->State = NvpwrStateWrongBuild;
        else if (status == STATUS_NOT_FOUND)
            Out->State = NvpwrStateGpuNotFound;
        else
            Out->State = NvpwrStateContextInvalid;
        Out->ModuleBase = (ULONGLONG)(ULONG_PTR)ctx.Base;
        Out->TimeDateStamp = ctx.TimeDateStamp;
        Out->SizeOfImage = ctx.SizeOfImage;
        Out->RegistryCount = ctx.RegistryCount;
        return STATUS_SUCCESS; /* return diagnostics to user mode */
    }

    FillStatusFromContext(&ctx, Out);
    return STATUS_SUCCESS;
}

/*
    All Board writes go through NVIDIA's own type-0 Board setter.
    Logging both the request and NVIDIA's return code lets reviewers distinguish
    "we attempted a write" from "the vendor path accepted the write".
*/
static ULONG CallBoardSet(NVPWR_CONTEXT* Ctx, ULONG Selector, ULONG Source, ULONG Value)
{
    ULONG nv = 0xFFFFFFFFu;
    if (!Ctx || !Ctx->BoardSet) {
        NVPWR_LOG_ERROR("BoardSet: missing context/handler selector=%lu source=0x%02lX value=%lu\n",
            Selector, Source, Value);
        return nv;
    }
    NVPWR_LOG_INFO("BoardSet: selector=%lu source=0x%02lX value=%lu mW\n",
        Selector, Source, Value);
    __try {
        nv = Ctx->BoardSet(Ctx->Major, Ctx->Root, Ctx->Board, Selector, Source, Value);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        nv = 0xFFFFFFFFu;
    }
    InterlockedExchange((volatile LONG*)&g_LastNvStatus, (LONG)nv);
    if (nv == 0)
        NVPWR_LOG_INFO("BoardSet: accepted nvStatus=0x%08lX\n", nv);
    else
        NVPWR_LOG_ERROR("BoardSet: rejected/failed nvStatus=0x%08lX\n", nv);
    return nv;
}

static ULONG CallSetAmount(NVPWR_CONTEXT* Ctx, ULONG Amount)
{
    ULONG nv = 0xFFFFFFFFu;
    if (!Ctx || !Ctx->SetAmount) {
        NVPWR_LOG_ERROR("SetAmount: missing context/handler amount=%lu\n", Amount);
        return nv;
    }
    NVPWR_LOG_INFO("SetAmount: request amount=%lu mW\n", Amount);
    __try { nv = Ctx->SetAmount(Ctx->Major, Amount); }
    __except (EXCEPTION_EXECUTE_HANDLER) { nv = 0xFFFFFFFFu; }
    InterlockedExchange((volatile LONG*)&g_LastNvStatus, (LONG)nv);
    if (nv != 0) NVPWR_LOG_ERROR("SetAmount: nvStatus=0x%08lX\n", nv);
    else NVPWR_LOG_INFO("SetAmount: accepted\n");
    return nv;
}

static ULONG CallSetEligibility(NVPWR_CONTEXT* Ctx, ULONG Eligible)
{
    ULONG nv = 0xFFFFFFFFu;
    if (!Ctx || !Ctx->SetEligibility) {
        NVPWR_LOG_ERROR("SetEligibility: missing context/handler eligible=%lu\n", Eligible);
        return nv;
    }
    NVPWR_LOG_INFO("SetEligibility: request eligible=%lu (also re-runs NVIDIA F7 generator)\n", Eligible);
    __try { nv = Ctx->SetEligibility(Ctx->Major, Eligible); }
    __except (EXCEPTION_EXECUTE_HANDLER) { nv = 0xFFFFFFFFu; }
    InterlockedExchange((volatile LONG*)&g_LastNvStatus, (LONG)nv);
    if (nv != 0) NVPWR_LOG_ERROR("SetEligibility: nvStatus=0x%08lX\n", nv);
    else NVPWR_LOG_INFO("SetEligibility: accepted\n");
    return nv;
}

/*
    Capture the exact live OEM state before the first mutation.
    WHY: rollback must restore what this machine actually had, not a guessed
    universal wattage. Root/Board identity is remembered too, so we refuse to
    restore into a different NVIDIA object after an unexpected rebuild.
*/
static VOID SaveBaseline(const NVPWR_CONTEXT* Ctx, const NVPWR_STATUS* S)
{
    NVPWR_LOG_INFO(
        "SaveBaseline: root=%p board=%p elig=%u amountActive=%u base=%lu amount=%lu upper=%lu\n",
        Ctx->Root, Ctx->Board, S->Eligibility, S->AmountActive,
        S->CtgpTarget, S->PpabAmount, S->UpperBoundary);
    g_MutationAttempted = TRUE;
    g_MutatedRoot = Ctx->Root;
    g_MutatedBoard = Ctx->Board;
    g_SavedEligibility = S->Eligibility;
    g_SavedAmountActive = S->AmountActive;
    g_SavedInput14 = S->CtgpTarget;
    g_SavedAmount18 = S->PpabAmount;
    g_SavedUpper24 = S->UpperBoundary;
    g_SavedValid = TRUE;
}

static NTSTATUS RestoreSavedRoot(NVPWR_CONTEXT* Ctx)
{
    ULONG nv;
    if (!g_SavedValid || Ctx->Root != g_MutatedRoot || Ctx->Board != g_MutatedBoard) {
        NVPWR_LOG_ERROR("RestoreSavedRoot: saved context invalid or NVIDIA object identity changed\n");
        return STATUS_INVALID_DEVICE_STATE;
    }

    NVPWR_LOG_WARN(
        "RestoreSavedRoot: restoring base=%lu amount=%lu upper=%lu elig=%u amountActive=%u\n",
        g_SavedInput14, g_SavedAmount18, g_SavedUpper24,
        g_SavedEligibility, g_SavedAmountActive);

    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootUpper]), (LONG)g_SavedUpper24);
    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootCtgp]), (LONG)g_SavedInput14);
    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootAmount]), (LONG)g_SavedAmount18);
    *(volatile UCHAR*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootAmountActive]) = g_SavedAmountActive;
    KeMemoryBarrier();

    /* Native eligibility setter also invokes the real 4E3EF0 generator. */
    nv = CallSetEligibility(Ctx, g_SavedEligibility);
    return (nv == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS RestoreStock(VOID);

/* 1.9.0: user-selectable high bound. MaxRequestedMw is supplied by the caller
   (IOCTL field, clamped to POWER_CEILING_DEV). OEM-baseline validation is
   machine-generic rather than tied to one laptop's factory wattage. */
static BOOLEAN IsSupportedTarget(ULONG Profile, ULONG Target, ULONG OemBaseline, ULONG MaxRequestedMw)
{
    ULONG ceiling;

    if ((Target % POWER_STEP) != 0) return FALSE;

    ceiling = MaxRequestedMw;
    if (ceiling < POWER_ABSOLUTE_MIN) ceiling = POWER_ABSOLUTE_MIN;
    if (ceiling > POWER_CEILING_DEV) ceiling = POWER_CEILING_DEV;

    if (Profile == NvpwrProfileRtx5050Laptop ||
        Profile == NvpwrProfileRtx5060Laptop || Profile == NvpwrProfileRtx5070Laptop) {
        /*
            LOW-POWER PROFILE SAFETY GATE
            WHERE: 5050/5060/5070 Laptop requests.
            WHAT: authorize 120..ceiling only when the live coherent OEM ceiling
                  is exactly 115 W on this audited NVIDIA runtime layout.
            WHY: model-name matching is not permission to raise power.  An OEM
                 configured for a lower ceiling may have a different VRM/EC/
                 thermal design, so it fails closed instead of being treated as
                 an equivalent 115 W platform.
        */
        if (OemBaseline != POWER_LOW_STOCK) return FALSE;
        if (Target < POWER_LOW_MIN) return FALSE;
        return Target <= (ceiling < POWER_LOW_MAX ? POWER_LOW_MAX : ceiling);
    }

    if (Profile == NvpwrProfileRtx5070TiLaptop) {
        if (OemBaseline < POWER_OEM_MIN || OemBaseline > POWER_OEM_MAX) return FALSE;
        if (Target < POWER_5070_MIN) return FALSE;
        return Target <= ceiling;
    }

    if (Profile == NvpwrProfileRtx5080Laptop || Profile == NvpwrProfileRtx5090Laptop) {
        /* High-power profiles are intentionally accepted only when the live OEM
           baseline is already in the 150 W+ class. This prevents selecting a
           5080/5090 profile on a 115/140 W machine. */
        if (OemBaseline < 150000u || OemBaseline > POWER_OEM_MAX) return FALSE;
        if (Target < POWER_HIGH_MIN) return FALSE;
        return Target <= ceiling;
    }

    if (Profile == NvpwrProfileRtx4090Laptop) {
        /* RTX 4090 Laptop GPU: OEM baseline typically 115..175 W depending on OEM mode (e.g. Balanced 130 W / Extreme 175 W). */
        if (OemBaseline < 115000u || OemBaseline > POWER_OEM_MAX) return FALSE;
        if (Target < POWER_4090_MIN) return FALSE;
        return Target <= ceiling;
    }

    if (Profile == NvpwrProfileRtx4080Laptop) {
        /* RTX 4080 Laptop GPU: OEM baseline typically 115..175 W. */
        if (OemBaseline < 115000u || OemBaseline > POWER_OEM_MAX) return FALSE;
        if (Target < POWER_4080_MIN) return FALSE;
        return Target <= ceiling;
    }

    if (Profile == NvpwrProfileRtx4070Laptop || Profile == NvpwrProfileRtx4060Laptop) {
        /* RTX 4060 / 4070 Laptop GPU: OEM baseline typically 95..140 W. */
        if (OemBaseline < 95000u || OemBaseline > 140000u) return FALSE;
        if (Target < POWER_4060_MIN) return FALSE;
        return Target <= (ceiling < POWER_4060_MAX ? POWER_4060_MAX : ceiling);
    }

    if (Profile == NvpwrProfileRtx4050Laptop) {
        /* RTX 4050 Laptop GPU: OEM baseline typically 75..140 W. */
        if (OemBaseline < 75000u || OemBaseline > 140000u) return FALSE;
        if (Target < POWER_4050_MIN) return FALSE;
        return Target <= (ceiling < POWER_4050_MAX ? POWER_4050_MAX : ceiling);
    }

    return FALSE;
}

static NTSTATUS RestoreContextToBaseline(NVPWR_CONTEXT* Ctx)
{
    NVPWR_STATUS s;
    NTSTATUS status;
    ULONG nv;

    if (!Ctx || !g_SavedValid) return STATUS_INVALID_DEVICE_STATE;
    NVPWR_LOG_WARN("Rollback: begin restore to captured OEM baseline=%lu mW\n", g_SavedUpper24);
    status = RestoreSavedRoot(Ctx);
    if (!NT_SUCCESS(status)) return status;

    nv = CallBoardSet(Ctx, SELECTOR_MAX, SOURCE_FE, g_SavedUpper24);
    if (nv != 0) return STATUS_UNSUCCESSFUL;

    RtlZeroMemory(&s, sizeof(s));
    s.Version = NVPWR_STATUS_VERSION;
    FillStatusFromContext(Ctx, &s);
    if (s.State == NvpwrStateStockBaseline) {
        NVPWR_LOG_INFO("Rollback: VERIFIED OEM baseline restored upper=%lu current=%lu\n",
            s.UpperBoundary, s.CurrentEffective);
        return STATUS_SUCCESS;
    }
    NVPWR_LOG_ERROR("Rollback: verification FAILED state=%lu upper=%lu max=%lu current=%lu f7=%lu\n",
        s.State, s.UpperBoundary, s.MaxEffective, s.CurrentEffective, s.CurrentF7Value);
    return STATUS_DATA_ERROR;
}

static NTSTATUS StageTargetAtStockCeiling(NVPWR_CONTEXT* Ctx, ULONG Target, ULONG Stock)
{
    NVPWR_STATUS s;
    ULONG nv;
    ULONG base;

    if (!Ctx || Target < POWER_ABSOLUTE_MIN || Target > POWER_ABSOLUTE_MAX || Stock == 0)
        return STATUS_INVALID_PARAMETER;

    base = Target - PPAB_FIXED;

    /*
        PHASE A — STAGE UNDER OEM CEILING
        WHAT: base=(target-25 W), amount=25 W, eligibility=1.
        WHY: MAX/UPPER stay at the saved OEM ceiling first so NVIDIA's own F7
             generator must prove the split is coherent before the ceiling moves.
    */
    NVPWR_LOG_INFO(
        "Phase A: target=%lu base=%lu amount=%lu savedOemCeiling=%lu\n",
        Target, base, PPAB_FIXED, Stock);

    /* Keep the saved OEM ceiling while changing generator inputs. */
    nv = CallBoardSet(Ctx, SELECTOR_MAX, SOURCE_FE, Stock);
    if (nv != 0) return STATUS_UNSUCCESSFUL;
    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootUpper]), (LONG)Stock);
    KeMemoryBarrier();

    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootCtgp]), (LONG)base);
    KeMemoryBarrier();

    nv = CallSetAmount(Ctx, PPAB_FIXED);
    if (nv != 0) return STATUS_UNSUCCESSFUL;

    nv = CallSetEligibility(Ctx, 1);
    if (nv != 0) return STATUS_UNSUCCESSFUL;

    RtlZeroMemory(&s, sizeof(s));
    s.Version = NVPWR_STATUS_VERSION;
    FillStatusFromContext(Ctx, &s);
    NVPWR_LOG_INFO(
        "Phase A verify: state=%lu stagedTarget=%lu upper=%lu max=%lu current=%lu f7=%lu predicted=%lu\n",
        s.State, s.AppliedTarget, s.UpperBoundary, s.MaxEffective,
        s.CurrentEffective, s.CurrentF7Value, s.PredictedF7);
    if (s.State != NvpwrStateArmed || s.AppliedTarget != Target) {
        NVPWR_LOG_ERROR("Phase A: verification FAILED\n");
        return STATUS_DATA_ERROR;
    }

    NVPWR_LOG_INFO("Phase A: PASSED\n");
    return STATUS_SUCCESS;
}

static NTSTATUS SetPowerTarget(ULONG Profile, ULONG Target, ULONG MaxRequestedMw)
{
    NVPWR_CONTEXT ctx;
    NVPWR_STATUS s;
    NTSTATUS status;
    ULONG detail = NvpwrDetailOk;
    ULONG nv;
    ULONG ceiling;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;

    /* Resolve the effective ceiling for this request before anything else. 0 (or
       a value below the absolute minimum) means "use the compiled default". */
    ceiling = MaxRequestedMw;
    if (ceiling == 0u || ceiling < POWER_ABSOLUTE_MIN) ceiling = POWER_ABSOLUTE_MAX;
    if (ceiling > POWER_CEILING_DEV) {
        NVPWR_LOG_WARN("SetPowerTarget: requested ceiling %lu exceeds driver ceiling %lu; clamping\n",
            MaxRequestedMw, POWER_CEILING_DEV);
        ceiling = POWER_CEILING_DEV;
    }

    NVPWR_LOG_INFO("SetPowerTarget: request profile=%lu target=%lu mW ceiling=%lu mW\n",
        Profile, Target, ceiling);
    status = ResolveContext(&ctx, &detail);
    if (!NT_SUCCESS(status)) return status;

    RtlZeroMemory(&s, sizeof(s));
    s.Version = NVPWR_STATUS_VERSION;
    FillStatusFromContext(&ctx, &s);

    {
        ULONG baseline = g_SavedValid ? g_SavedUpper24 :
            ((s.State == NvpwrStateStockBaseline) ? s.UpperBoundary : 0);
        NVPWR_LOG_INFO("SetPowerTarget: liveState=%lu baseline=%lu activeProfile=%lu savedValid=%u\n",
            s.State, baseline, g_ActiveProfile, g_SavedValid);
        if (!IsSupportedTarget(Profile, Target, baseline, ceiling)) {
            NVPWR_LOG_ERROR(
                "SetPowerTarget: target/profile rejected profile=%lu target=%lu baseline=%lu ceiling=%lu\n",
                Profile, Target, baseline, ceiling);
            return STATUS_INVALID_PARAMETER;
        }
    }

    /* Authorize the ceiling only after the request passed validation. */
    g_SessionMaxMw = ceiling;
    g_SessionMaxValid = TRUE;

    if (s.State == NvpwrStateApplied && s.AppliedTarget == Target)
        return STATUS_SUCCESS;

    if (!g_SavedValid) {
        if (s.State != NvpwrStateStockBaseline)
            return STATUS_INVALID_DEVICE_STATE;
        SaveBaseline(&ctx, &s);
        g_ActiveProfile = Profile;
    } else {
        if (g_ActiveProfile != Profile) return STATUS_INVALID_DEVICE_STATE;
        if (ctx.Root != g_MutatedRoot || ctx.Board != g_MutatedBoard)
            return STATUS_INVALID_DEVICE_STATE;
        if (s.State != NvpwrStateStockBaseline &&
            s.State != NvpwrStateArmed &&
            s.State != NvpwrStateApplied)
            return STATUS_INVALID_DEVICE_STATE;
    }

    /* Phase A: prove the requested split under the saved OEM ceiling. */
    NVPWR_LOG_INFO("SetPowerTarget: entering PHASE A (stage under OEM ceiling)\n");
    status = StageTargetAtStockCeiling(&ctx, Target, g_SavedUpper24);
    if (!NT_SUCCESS(status)) {
        NTSTATUS rollback;
        NVPWR_LOG_ERROR("SetPowerTarget: PHASE A failed NTSTATUS=0x%08X; rolling back\n", status);
        rollback = RestoreContextToBaseline(&ctx);
        NVPWR_LOG_WARN("SetPowerTarget: rollback result=0x%08X\n", rollback);
        return status;
    }

    /*
        PHASE B — RAISE THE TWO CEILINGS
        selector2/source FE is Board MAX; root+3D24 is the saturation ceiling
        consumed by F7. Raising only one is not enough for the proven path.
    */
    NVPWR_LOG_INFO("SetPowerTarget: entering PHASE B target=%lu\n", Target);
    nv = CallBoardSet(&ctx, SELECTOR_MAX, SOURCE_FE, Target);
    if (nv != 0) {
        NTSTATUS rollback;
        NVPWR_LOG_ERROR("SetPowerTarget: PHASE B Board MAX failed nvStatus=0x%08lX; rolling back\n", nv);
        rollback = RestoreContextToBaseline(&ctx);
        NVPWR_LOG_WARN("SetPowerTarget: rollback result=0x%08X\n", rollback);
        return STATUS_UNSUCCESSFUL;
    }

    NVPWR_LOG_INFO("Phase B: writing root+3D24 UPPER=%lu mW\n", Target);
    InterlockedExchange((volatile LONG*)((PUCHAR)ctx.Root + g_T->Off[NvpwrOffRootUpper]), (LONG)Target);
    KeMemoryBarrier();

    /*
        PHASE C — REGENERATE + VERIFY
        Calling the native eligibility setter with 1 re-enters NVIDIA's real
        generator. selector3/CURRENT is not fabricated by Nvpwr here.
    */
    NVPWR_LOG_INFO("SetPowerTarget: entering PHASE C (native generator + final readback)\n");
    nv = CallSetEligibility(&ctx, 1);
    if (nv != 0) {
        NTSTATUS rollback;
        NVPWR_LOG_ERROR("SetPowerTarget: PHASE C generator failed nvStatus=0x%08lX; rolling back\n", nv);
        rollback = RestoreContextToBaseline(&ctx);
        NVPWR_LOG_WARN("SetPowerTarget: rollback result=0x%08X\n", rollback);
        return STATUS_UNSUCCESSFUL;
    }

    RtlZeroMemory(&s, sizeof(s));
    s.Version = NVPWR_STATUS_VERSION;
    FillStatusFromContext(&ctx, &s);
    NVPWR_LOG_INFO(
        "Final verify: state=%lu target=%lu base=%lu amount=%lu upper=%lu max=%lu current=%lu f7=%lu predicted=%lu\n",
        s.State, s.AppliedTarget, s.CtgpTarget, s.PpabAmount, s.UpperBoundary,
        s.MaxEffective, s.CurrentEffective, s.CurrentF7Value, s.PredictedF7);
    if (s.State != NvpwrStateApplied || s.AppliedTarget != Target) {
        NTSTATUS rollback;
        NVPWR_LOG_ERROR("SetPowerTarget: FINAL verification FAILED; rolling back\n");
        rollback = RestoreContextToBaseline(&ctx);
        NVPWR_LOG_WARN("SetPowerTarget: rollback result=0x%08X\n", rollback);
        return STATUS_DATA_ERROR;
    }

    NVPWR_LOG_INFO("SetPowerTarget: SUCCESS target=%lu mW\n", Target);
    return STATUS_SUCCESS;
}

static BOOLEAN IsRecoverableExternalModifiedState(const NVPWR_STATUS* S)
{
    if (!S) return FALSE;

    /*
        RECOGNIZED RAISED-CEILING FAMILY (1.9.0 — machine-generic)
        WHERE: driver loaded after an older helper already raised the ceiling, so
               this driver has no captured baseline of its own.
        WHAT:  accept any coherent, believable raised-ceiling policy in the
               POWER_OEM_MIN..POWER_CEILING_DEV window where Board MAX, generator
               output and both F7 projections all agree, the generator is
               initialized, eligibility and the dynamic amount are active, and
               the amount is a plausible Dynamic Boost share of the ceiling.
        WHY:   1.8.0 pinned this to "145..160 W and PPAB==25/30/40 W", i.e. the
               RTX 5070 Ti proof machine only. On the verified 175 W RTX 5090
               Laptop a raised ceiling could therefore never be recognized, and a
               partially applied state could only be cleared by rebooting. The
               window is now derived from the live policy values instead of one
               machine's constants.

        The recovery this enables (ForceKnownStockBaseline) only ever writes a
        clean, stock-consistent policy; it is not a general "write anything" path.
    */
    if (S->UpperBoundary < POWER_OEM_MIN || S->UpperBoundary > POWER_CEILING_DEV)
        return FALSE;
    if ((S->UpperBoundary % POWER_STEP) != 0)
        return FALSE;
    if (S->MaxMode != 0 || S->MaxCount != 1 || S->MaxSource0 != SOURCE_FE)
        return FALSE;
    if (S->MaxEffective != S->UpperBoundary ||
        S->MaxSource0Value != S->UpperBoundary ||
        S->CurrentEffective != S->UpperBoundary ||
        S->CurrentF7Value != S->UpperBoundary ||
        S->PredictedF7 != S->UpperBoundary)
        return FALSE;
    if (S->RootInitialized != 1 || S->Eligibility != 1 || S->AmountActive != 1)
        return FALSE;
    /* Dynamic Boost share must be a sane fraction of the ceiling. The historical
       proof splits translate to 20% (25 W of 125 W), 20% (30 of 150) and 25%
       (40 of 160); current builds use 25 W. Accept 1..40 % and a 5 W step so the
       verified 25 W model and the earlier proof splits all qualify. */
    if (S->PpabAmount < 1000u || S->PpabAmount > (S->UpperBoundary * 40u) / 100u)
        return FALSE;
    if ((S->PpabAmount % 1000u) != 0)
        return FALSE;
    return TRUE;
}

/*
    FORCED STOCK RECONSTRUCTION (1.9.0 — machine-generic)
    WHERE: a raised-ceiling policy was left behind by an earlier helper or by a
           previous driver load, and this driver has no captured baseline.
    WHAT:  write a clean, internally consistent stock policy using the ceiling
           OBSERVED ON THIS MACHINE, re-run NVIDIA's own generator, then require
           the generic coherent-stock classifier to accept the result.
    WHY:   1.8.0 wrote the compile-time constant POWER_5070_STOCK (140 W). On the
           verified 175 W RTX 5090 Laptop that wrote the wrong ceiling and the
           verification below then failed, leaving the user with no recovery path
           short of a reboot.

    FAIL-CLOSED: the observed ceiling must be inside the plausible OEM window and
    aligned to the power step, and the post-write classification must succeed.
    If anything is off, this returns an error and changes nothing.
*/
static NTSTATUS ForceKnownStockBaseline(NVPWR_CONTEXT* Ctx)
{
    NVPWR_STATUS s;
    ULONG nv;
    ULONG stock;

    if (!Ctx) return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(&s, sizeof(s));
    s.Version = NVPWR_STATUS_VERSION;
    FillStatusFromContext(Ctx, &s);

    stock = s.UpperBoundary;
    if (stock < POWER_OEM_MIN || stock > POWER_CEILING_DEV || (stock % POWER_STEP) != 0) {
        NVPWR_LOG_ERROR(
            "ForceKnownStockBaseline: observed ceiling %lu mW is not a plausible OEM baseline\n",
            stock);
        return STATUS_INVALID_DEVICE_STATE;
    }

    NVPWR_LOG_WARN("ForceKnownStockBaseline: reconstructing stock from observed ceiling=%lu mW\n", stock);

    /* Order mirrors the verified saved-state rollback: restore generator
       inputs/ceiling, regenerate through NVIDIA's native eligibility setter,
       then restore the board MAX source FE. */
    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootUpper]), (LONG)stock);
    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootCtgp]), (LONG)stock);
    InterlockedExchange((volatile LONG*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootAmount]), 0);
    *(volatile UCHAR*)((PUCHAR)Ctx->Root + g_T->Off[NvpwrOffRootAmountActive]) = 0;
    KeMemoryBarrier();

    nv = CallSetEligibility(Ctx, 0);
    if (nv != 0) return STATUS_UNSUCCESSFUL;

    nv = CallBoardSet(Ctx, SELECTOR_MAX, SOURCE_FE, stock);
    if (nv != 0) return STATUS_UNSUCCESSFUL;

    RtlZeroMemory(&s, sizeof(s));
    s.Version = NVPWR_STATUS_VERSION;
    FillStatusFromContext(Ctx, &s);
    if (s.State != NvpwrStateStockBaseline) {
        NVPWR_LOG_ERROR(
            "ForceKnownStockBaseline: verification FAILED state=%lu upper=%lu max=%lu f7=%lu\n",
            s.State, s.UpperBoundary, s.MaxEffective, s.CurrentF7Value);
        return STATUS_DATA_ERROR;
    }

    NVPWR_LOG_INFO("ForceKnownStockBaseline: SUCCESS stock ceiling=%lu mW restored\n", s.UpperBoundary);

    g_MutationAttempted = FALSE;
    g_MutatedRoot = NULL;
    g_MutatedBoard = NULL;
    g_SavedValid = FALSE;
    g_SavedEligibility = 0;
    g_SavedAmountActive = 0;
    g_SavedInput14 = 0;
    g_SavedAmount18 = 0;
    g_SavedUpper24 = 0;
    g_ActiveProfile = NvpwrProfileUnknown;
    g_SessionMaxValid = FALSE;
    g_SessionMaxMw = POWER_ABSOLUTE_MAX;
    return STATUS_SUCCESS;
}

static NTSTATUS RestoreStock(VOID)
{
    NVPWR_CONTEXT ctx;
    NVPWR_STATUS s;
    NTSTATUS status;
    ULONG detail = NvpwrDetailOk;
    ULONG nv;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;

    NVPWR_LOG_WARN("RestoreStock: request received\n");
    status = ResolveContext(&ctx, &detail);
    if (!NT_SUCCESS(status)) return status;

    RtlZeroMemory(&s, sizeof(s));
    s.Version = NVPWR_STATUS_VERSION;
    FillStatusFromContext(&ctx, &s);

    if (s.State == NvpwrStateStockBaseline) {
        NVPWR_LOG_INFO("RestoreStock: already at coherent OEM baseline=%lu mW\n", s.UpperBoundary);
        /* Returning to OEM also releases any session ceiling the user raised. */
        g_SessionMaxValid = FALSE;
        g_SessionMaxMw = POWER_ABSOLUTE_MAX;
        return STATUS_SUCCESS;
    }

    /* Normal same-session restore: use the state captured before our write. */
    if (g_MutationAttempted && g_SavedValid) {
        NVPWR_LOG_WARN("RestoreStock: using same-session captured baseline=%lu mW\n", g_SavedUpper24);
        if (ctx.Root != g_MutatedRoot || ctx.Board != g_MutatedBoard) {
            NVPWR_LOG_ERROR("RestoreStock: NVIDIA object identity changed; refusing blind restore\n");
            return STATUS_INVALID_DEVICE_STATE;
        }

        status = RestoreSavedRoot(&ctx);
        if (!NT_SUCCESS(status)) return status;

        nv = CallBoardSet(&ctx, SELECTOR_MAX, SOURCE_FE, g_SavedUpper24);
        if (nv != 0) return STATUS_UNSUCCESSFUL;

        RtlZeroMemory(&s, sizeof(s));
        s.Version = NVPWR_STATUS_VERSION;
        FillStatusFromContext(&ctx, &s);
        if (s.State != NvpwrStateStockBaseline)
            return STATUS_DATA_ERROR;

        g_MutationAttempted = FALSE;
        g_MutatedRoot = NULL;
        g_MutatedBoard = NULL;
        g_SavedValid = FALSE;
        g_SavedEligibility = 0;
        g_SavedAmountActive = 0;
        g_SavedInput14 = 0;
        g_SavedAmount18 = 0;
        g_SavedUpper24 = 0;
        g_ActiveProfile = NvpwrProfileUnknown;
        g_SessionMaxValid = FALSE;
        g_SessionMaxMw = POWER_ABSOLUTE_MAX;
        NVPWR_LOG_INFO("RestoreStock: SUCCESS captured OEM baseline restored\n");
        return STATUS_SUCCESS;
    }

    /* Driver was loaded after an older proof helper had already changed the
       616.92 runtime policy. Recover only if the live state matches that very
       narrow verified family; arbitrary MIXED states are still rejected. */
    if (IsRecoverableExternalModifiedState(&s)) {
        NVPWR_LOG_WARN("RestoreStock: recognized legacy 5070 Ti proof state; using narrow known-stock recovery\n");
        return ForceKnownStockBaseline(&ctx);
    }

    NVPWR_LOG_ERROR("RestoreStock: live state is not a known safe restore family; refusing mutation\n");
    return STATUS_INVALID_DEVICE_STATE;
}

static NTSTATUS DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS DispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    ULONG code;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;

    UNREFERENCED_PARAMETER(DeviceObject);
    stack = IoGetCurrentIrpStackLocation(Irp);
    code = stack->Parameters.DeviceIoControl.IoControlCode;

    switch (code) {
    case IOCTL_NVPWR_STATUS:
        if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(NVPWR_STATUS)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = QueryStatus((NVPWR_STATUS*)Irp->AssociatedIrp.SystemBuffer);
        information = sizeof(NVPWR_STATUS);
        break;

    case IOCTL_NVPWR_SET_POWER:
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(NVPWR_SET_POWER)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            NVPWR_SET_POWER* request = (NVPWR_SET_POWER*)Irp->AssociatedIrp.SystemBuffer;
            if (request->Version != NVPWR_SET_VERSION) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            NVPWR_LOG_INFO("IOCTL_SET_POWER: profile=%lu target=%lu mW ceiling=%lu mW\n",
                request->Profile, request->TargetMilliwatts, request->MaxMilliwatts);
            KeWaitForSingleObject(&g_OperationMutex, Executive, KernelMode, FALSE, NULL);
            status = SetPowerTarget(request->Profile, request->TargetMilliwatts,
                request->MaxMilliwatts);
            KeReleaseMutex(&g_OperationMutex, FALSE);
            if (NT_SUCCESS(status))
                NVPWR_LOG_INFO("IOCTL_SET_POWER: completed NTSTATUS=0x%08X\n", status);
            else
                NVPWR_LOG_ERROR("IOCTL_SET_POWER: failed NTSTATUS=0x%08X\n", status);
        }
        break;

    case IOCTL_NVPWR_RESTORE:
        NVPWR_LOG_WARN("IOCTL_RESTORE: requested\n");
        KeWaitForSingleObject(&g_OperationMutex, Executive, KernelMode, FALSE, NULL);
        status = RestoreStock();
        KeReleaseMutex(&g_OperationMutex, FALSE);
        if (NT_SUCCESS(status))
            NVPWR_LOG_INFO("IOCTL_RESTORE: completed NTSTATUS=0x%08X\n", status);
        else
            NVPWR_LOG_ERROR("IOCTL_RESTORE: failed NTSTATUS=0x%08X\n", status);
        break;

    case IOCTL_NVPWR_DIAGNOSE:
        if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(NVPWR_DIAGNOSIS)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        /*
            只读，因此【不取】g_OperationMutex。它不碰任何可变的设备状态，也就不该
            被一个正在进行的 SET_POWER 挡住 —— 而诊断最常见的用法恰恰是"设置失败了，
            看看为什么"，那时候去等一个可能正卡住的锁只会让情况更糟。
        */
        DiagnoseBuild((NVPWR_DIAGNOSIS*)Irp->AssociatedIrp.SystemBuffer);
        information = sizeof(NVPWR_DIAGNOSIS);
        status = STATUS_SUCCESS;
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    NVPWR_LOG_WARN("DriverUnload: unloading. No implicit NVIDIA restore is performed.\n");

    /* No implicit calls into NVIDIA on unload. Explicit restore is separate.
       If restore has not succeeded, reboot to reconstruct stock state. */

    IoDeleteSymbolicLink(&g_SymbolicLink);
    if (g_DeviceObject) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    UNICODE_STRING deviceName;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    NVPWR_LOG_INFO("DriverEntry: Nvpwr 1.5.0 experimental review/debug build loading\n");
    KeInitializeMutex(&g_OperationMutex, 0);
    RtlInitUnicodeString(&deviceName, L"\\Device\\Nvpwr");
    RtlInitUnicodeString(&g_SymbolicLink, L"\\DosDevices\\Nvpwr");

    status = IoCreateDevice(
        DriverObject,
        0,
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &g_DeviceObject);
    if (!NT_SUCCESS(status)) return status;

    status = IoCreateSymbolicLink(&g_SymbolicLink, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
        return status;
    }

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
        DriverObject->MajorFunction[i] = DispatchCreateClose;

    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;
    DriverObject->DriverUnload = DriverUnload;
    g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    NVPWR_LOG_INFO("DriverEntry: device \\Device\\Nvpwr and \\DosDevices\\Nvpwr ready\n");
    return STATUS_SUCCESS;
}