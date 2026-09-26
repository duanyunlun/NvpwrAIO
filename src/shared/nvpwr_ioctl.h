#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <Windows.h>
#include <winioctl.h>
#endif

#define NVPWR_DEVICE_WIN32 L"\\\\.\\Nvpwr"
#define NVPWR_STATUS_VERSION 8u
#define NVPWR_SET_VERSION 3u
#define NVPWR_DIAG_VERSION   1u

#define IOCTL_NVPWR_STATUS    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_NVPWR_SET_POWER CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_NVPWR_RESTORE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

/*
    IOCTL_NVPWR_DIAGNOSE —— 只读的构建诊断。

    为什么需要它，而不复用 STATUS：

      STATUS 走的是 ResolveContext，而那会在构建校验失败时提前返回。于是"校验失败
      了"这条信息能拿到，但"失败在哪一条签名、对应的 RVA 是多少"拿不到 —— 而那正是
      移植到新驱动时唯一想知道的事。

      这个 IOCTL 从 nvlddmkm 的 PE 头开始重做一遍匹配，【完全不写任何东西】，因此
      可以在一个不受支持的驱动上安全调用。它回答的是：

        · 实际读到的 PE 标识是什么
        · 它匹配上了 g_Targets[] 里的哪一项（或一项都没匹配上）
        · 每一签名的匹配情况（掩码），以及每条签名所处的 RVA

      有了这些，移植时的第一步就变成"跑一次 diagnose，把 RVA 抄进去"，而不是靠猜。
*/
#define IOCTL_NVPWR_DIAGNOSE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)

/* 每个表项的签名条数上限。驱动侧用它定长传输，避免变长缓冲的复杂度。 */
#define NVPWR_DIAG_SIG_MAX 8

typedef struct _NVPWR_DIAGNOSIS {
    ULONG Version;                  /* = NVPWR_DIAG_VERSION */

    /* 实际从 nvlddmkm 的 PE 头读到的 */
    ULONG TimeDateStamp;
    ULONG SizeOfImage;
    ULONG ModuleSize;

    /* 查表结果。TargetIndex == 0xFFFFFFFF 表示没有任何表项匹配。 */
    ULONG TargetIndex;
    ULONG TargetCount;

    /* 命中表项的期望值，只在 TargetIndex 有效时有意义 */
    ULONG ExpectedTimeDateStamp;
    ULONG ExpectedSizeOfImage;
    CHAR  TargetName[16];

    /* 第 i 位 = 第 i 条签名匹配。移位数即 NVPWR_SIG_ID 的顺序。 */
    ULONG SigMask;
    ULONG SigCount;

    /* 每条签名所处的 RVA 与长度 —— "第 3 条不匹配"要能直接翻译成一个地址 */
    ULONG SigRva[NVPWR_DIAG_SIG_MAX];
    ULONG SigLen[NVPWR_DIAG_SIG_MAX];

    ULONG Reserved[4];
} NVPWR_DIAGNOSIS;

typedef enum _NVPWR_GPU_PROFILE {
    NvpwrProfileUnknown = 0,
    NvpwrProfileRtx5070TiLaptop = 1,
    NvpwrProfileRtx5080Laptop = 2,
    NvpwrProfileRtx5090Laptop = 3,
    NvpwrProfileRtx5050Laptop = 4,
    NvpwrProfileRtx5060Laptop = 5,
    NvpwrProfileRtx5070Laptop = 6,
    NvpwrProfileRtx4090Laptop = 7,
    NvpwrProfileRtx4080Laptop = 8,
    NvpwrProfileRtx4070Laptop = 9,
    NvpwrProfileRtx4060Laptop = 10,
    NvpwrProfileRtx4050Laptop = 11
} NVPWR_GPU_PROFILE;

typedef enum _NVPWR_STATE {
    NvpwrStateUnknown = 0,
    NvpwrStateArmed = 1,
    NvpwrStateApplied = 2,
    NvpwrStateMixed = 3,
    NvpwrStateWrongBuild = 4,
    NvpwrStateModuleNotFound = 5,
    NvpwrStateGpuNotFound = 6,
    NvpwrStateContextInvalid = 7,
    NvpwrStatePreconditionNotReady = 8,
    NvpwrStateStockBaseline = 9
} NVPWR_STATE;

typedef enum _NVPWR_DETAIL {
    NvpwrDetailOk = 0,
    NvpwrDetailPeIdentity = 1,
    NvpwrDetailCodeSignature = 2,
    NvpwrDetailGlobalPointer = 3,
    NvpwrDetailGpuTable = 4,
    NvpwrDetailMajorObject = 5,
    NvpwrDetailPowerRoot = 6,
    NvpwrDetailLookupFunction = 7,
    NvpwrDetailBoardObject = 8,
    NvpwrDetailBoardHandler = 9,
    NvpwrDetailSelector2Layout = 10,
    NvpwrDetailSelector3F7 = 11,
    NvpwrDetailTestState = 12,
    NvpwrDetailBoardSet = 13,
    NvpwrDetailVerify = 14,
    NvpwrDetailTargetRange = 15
} NVPWR_DETAIL;

#pragma pack(push, 8)
typedef struct _NVPWR_SET_POWER {
    ULONG Version;
    ULONG TargetMilliwatts;
    ULONG Profile;
    /*
        MaxMilliwatts — user-selected high bound for this request (1.9.0).
        The driver clamps it to its own POWER_CEILING_DEV. It widens what may be
        REQUESTED only; the Phase A/Phase C convergence checks are unchanged, so
        a target the hardware will not honour still fails closed and rolls back.
        0 means "use the driver's default ceiling".
    */
    ULONG MaxMilliwatts;
    ULONG Reserved;
} NVPWR_SET_POWER;

typedef struct _NVPWR_STATUS {
    ULONG Version;
    ULONG State;
    LONG LastNtStatus;
    ULONG Detail;

    ULONGLONG ModuleBase;
    ULONG TimeDateStamp;
    ULONG SizeOfImage;

    ULONG RegistryCount;
    ULONG SelectedIndex;
    ULONG GpuId;
    ULONG Reserved0;

    ULONGLONG DriverGlobal;
    ULONGLONG GpuTable;
    ULONGLONG MajorObject;
    ULONGLONG PowerRoot;
    ULONGLONG LookupFunction;
    ULONGLONG BoardObject;
    ULONGLONG BoardSetFunction;

    UCHAR RootInitialized;
    UCHAR Eligibility;
    UCHAR AmountActive;
    UCHAR PolicyKey;
    ULONG CtgpTarget;
    ULONG PpabAmount;
    ULONG LowerBoundary;
    ULONG UpperBoundary;
    ULONG Aux28;
    ULONG Aux2C;

    UCHAR MaxMode;
    UCHAR MaxCount;
    UCHAR MaxSource0;
    UCHAR CurrentMode;
    UCHAR CurrentCount;
    UCHAR Reserved1[3];
    ULONG MaxEffective;
    ULONG MaxSecondary;
    ULONG MaxSource0Value;
    ULONG CurrentEffective;
    ULONG CurrentF7Value;
    ULONG PredictedF7;

    ULONG AppliedTarget;
    ULONG LastNvStatus;

    ULONG OemBaseline;
    ULONG ActiveProfile;
    ULONG SupportedMin;
    ULONG SupportedMax;

    /* 1.9.0 additions. Appended so the earlier field offsets stay stable. */
    ULONG CeilingMax;   /* compile-time ceiling compiled into this driver */
    ULONG SessionMax;   /* ceiling the user authorized for the current session */
} NVPWR_STATUS;
#pragma pack(pop)
