/*
    target.h — 移植面。

    这个文件是驱动里【唯一】需要为一个新的 nvlddmkm.sys 而修改的地方。所有和具体
    构建绑定的数字都在这里，按构建分组存放在 g_Targets[] 里。

    为什么做成表而不是 #define：

      原先这些常量是 driver.c 顶部的 28 个 #define，全文件 64 处直接引用。加一个
      新驱动版本意味着改那 28 行，然后祈祷 64 个引用点都对 —— 而其中有些引用
      （比如结构偏移）错了不会崩溃，只会写到错误的字节上。

      做成表之后：
        · 移植 = 在 g_Targets[] 里加一个表项，不碰 driver.c 一行
        · 一份 Nvpwr.sys 可以同时支持多个驱动版本，用户升级驱动不必等新二进制
        · 每个版本带自己的签名，所以每个版本的正确性是被独立校验的
        · 校验失败时能指出【是哪一项】不匹配（见 g_SigNames），而不是笼统的
          "签名不匹配"

    如何新增一个版本见 docs/PORTING_TO_A_NEW_DRIVER.md。
*/

#pragma once

/* ---- 数组尺寸 ---------------------------------------------------------- */

#define NVPWR_TARGET_RVA_COUNT   8
#define NVPWR_TARGET_OFF_COUNT  22
#define NVPWR_TARGET_SIG_COUNT   6

/*
    签名里最长的一条是 21 字节（SetAmount）。留到 24 是为了将来加签名时不必同时
    改尺寸，代价是每个表项多 18 字节 —— 几个版本加起来也不到 1 KB。
*/
#define NVPWR_TARGET_SIG_MAXLEN 24

/* ---- 索引 -------------------------------------------------------------- */

/* 这些枚举的顺序【就是】下面每个表项里数组的顺序，改动时必须同步。 */
typedef enum _NVPWR_RVA_ID {
    NvpwrRvaGpuGlobal = 0,
    NvpwrRvaGpuRegistryFn,
    NvpwrRvaF7Generator,
    NvpwrRvaF7UpperLoad,
    NvpwrRvaF7Record,
    NvpwrRvaBoardType0Set,
    NvpwrRvaSetAmount,
    NvpwrRvaSetElig
} NVPWR_RVA_ID;

/*
    最后两项（步长与上限）严格说不是偏移，但它们是同一类"从逆向里读出来、会随
    构建变化"的数字，放在一起的好处是移植时不必在两个地方找。

    末尾三项是 board 对象【内部】的偏移。它们和 root 的偏移同样会随构建变化，
    但属于另一个对象 —— 放在同一个数组里是为了让移植面只有一个列表。名字里带
    Board 就是为了区分这一点。
*/
typedef enum _NVPWR_OFF_ID {
    NvpwrOffGlobalGpuTable = 0,
    NvpwrOffTableEntryPtr,
    NvpwrOffTableEntryGpuId,
    NvpwrOffTableCount,
    NvpwrOffGpuEntryStride,
    NvpwrOffGpuCountMax,
    NvpwrOffMajorPowerRoot,
    NvpwrOffRootRegistry,
    NvpwrOffRootLookupFn,
    NvpwrOffRootInit,
    NvpwrOffRootElig,
    NvpwrOffRootAmountActive,
    NvpwrOffRootCtgp,
    NvpwrOffRootAmount,
    NvpwrOffRootPolicyKey,
    NvpwrOffRootLower,
    NvpwrOffRootUpper,
    NvpwrOffRootAux28,
    NvpwrOffRootAux2C,
    NvpwrOffBoardSetFn,
    NvpwrOffSelector2,
    NvpwrOffSelector3
} NVPWR_OFF_ID;

typedef enum _NVPWR_SIG_ID {
    NvpwrSigGpuRegistry = 0,
    NvpwrSigUpperLoad,
    NvpwrSigF7Record,
    NvpwrSigBoardSet,
    NvpwrSigSetAmount,
    NvpwrSigSetElig
} NVPWR_SIG_ID;

/* ---- 一个受支持的构建 -------------------------------------------------- */

typedef struct _NVPWR_TARGET {
    ULONG       TimeDateStamp;      /* nvlddmkm.sys 的 PE TimeDateStamp */
    ULONG       SizeOfImage;        /* PE SizeOfImage */
    const char* Name;               /* 只用于日志，例如 "616.92" */

    ULONG       Rva[NVPWR_TARGET_RVA_COUNT];
    ULONG       Off[NVPWR_TARGET_OFF_COUNT];

    /*
        签名的长度单独存，而不是用 sizeof —— 表项里的数组是定长填充的，sizeof 永远
        是 SIG_MAXLEN，那样比对就会把填充的零也算进去，于是任何签名都匹配不上。
    */
    UCHAR       Sig[NVPWR_TARGET_SIG_COUNT][NVPWR_TARGET_SIG_MAXLEN];
    ULONG       SigLen[NVPWR_TARGET_SIG_COUNT];
} NVPWR_TARGET;

/* ---- 表 ---------------------------------------------------------------- */

/*
    按加入顺序排列。查找按 PE 标识精确匹配，所以顺序不影响正确性，但把最常见的
    版本放前面可以少比几次。
*/
static const NVPWR_TARGET g_Targets[] = {

    /* ================================================================
       616.92 —— 参考机器的版本，也是这套逆向的原始目标。
       ================================================================ */
    {
        0x6A9B4070u,            /* TimeDateStamp */
        0x06D3E000u,            /* SizeOfImage   */
        "616.92",

        /* Rva[] —— 顺序同 NVPWR_RVA_ID */
        {
            0x013B2E18u,        /* GpuGlobal        */
            0x00107AC0u,        /* GpuRegistryFn    */
            0x004E3EF0u,        /* F7Generator      */
            0x004E3FBFu,        /* F7UpperLoad      */
            0x004E4088u,        /* F7Record         */
            0x008D6960u,        /* BoardType0Set    */
            0x004E4610u,        /* SetAmount        */
            0x004E4680u         /* SetElig          */
        },

        /* Off[] —— 顺序同 NVPWR_OFF_ID */
        {
            0x0208u,            /* GlobalGpuTable    */
            0x48A48u,           /* TableEntryPtr     */
            0x48A50u,           /* TableEntryGpuId   */
            0x48C48u,           /* TableCount        */
            0x10u,              /* GpuEntryStride    */
            32u,                /* GpuCountMax       */
            0x25B0u,            /* MajorPowerRoot    */
            0x1CC0u,            /* RootRegistry      */
            0x1CF8u,            /* RootLookupFn      */
            0x3D10u,            /* RootInit          */
            0x3D11u,            /* RootElig          */
            0x3D12u,            /* RootAmountActive  */
            0x3D14u,            /* RootCtgp          */
            0x3D18u,            /* RootAmount        */
            0x3D1Cu,            /* RootPolicyKey     */
            0x3D20u,            /* RootLower         */
            0x3D24u,            /* RootUpper         */
            0x3D28u,            /* RootAux28         */
            0x3D2Cu,            /* RootAux2C         */
            0x2D0u,             /* BoardSetFn        */
            0x104u,             /* Selector2         */
            0x1F4u              /* Selector3         */
        },

        /* Sig[][] —— 顺序同 NVPWR_SIG_ID */
        {
            { 0x48,0x8B,0x05,0x51,0xB3,0x2A,0x01,0x44,0x8B,0xD9,
              0x4C,0x8B,0x88,0x08,0x02,0x00,0x00 },                 /* GpuRegistry */
            { 0x44,0x8B,0x8B,0x24,0x3D,0x00,0x00 },                 /* UpperLoad   */
            { 0x66,0xC7,0x44,0x24,0x48,0xF7,0x03 },                 /* F7Record    */
            { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,
              0x48,0x89,0x74,0x24,0x18 },                           /* BoardSet    */
            { 0x48,0x83,0xEC,0x38,0x48,0x8B,0x81,0xB0,0x25,0x00,
              0x00,0x44,0x8B,0xC2,0x80,0xB8,0x10,0x3D,0x00,0x00,
              0x00 },                                               /* SetAmount   */
            { 0x48,0x83,0xEC,0x28,0x48,0x8B,0x81,0xB0,0x25,0x00,
              0x00,0x80,0xB8,0x10,0x3D,0x00,0x00,0x00 }            /* SetElig     */
        },

        /* SigLen[] —— 必须和上面每一行的实际字节数一致 */
        { 17u, 7u, 7u, 15u, 21u, 18u }
    }

    /* ====================================================================
       下一个构建加在这里。复制上面整块，改数字，把 Name 换成新驱动版本号。
       不需要改 driver.c 的任何一行。

       注意每一条签名的长度要和它的字节数对上 —— 长度错了不会编译失败，只会
       在校验时报"签名不匹配"，而那是移植时最容易被误判成 RVA 错误的一种表现。
       ==================================================================== */
};

#define NVPWR_TARGET_COUNT (sizeof(g_Targets) / sizeof(g_Targets[0]))

/* 没有任何表项匹配当前驱动的 PE 标识。用一个不可能的下标而不是 0，是因为 0 是
   一个合法的表项下标，用它当"没找到"会让诊断输出说"命中了 616.92"而当的没有。 */
#define NVPWR_TARGET_NONE  0xFFFFFFFFu

/* 所有签名都匹配时的掩码。 */
#define NVPWR_SIG_ALL      ((1u << NVPWR_TARGET_SIG_COUNT) - 1u)

/* ---- 结构性映射（对所有构建都一样，不属于移植面）---------------------- */

/* 每一条签名在哪个 RVA 上比对。顺序同 NVPWR_SIG_ID。 */
static const UCHAR g_SigRvaIndex[NVPWR_TARGET_SIG_COUNT] = {
    NvpwrRvaGpuRegistryFn,
    NvpwrRvaF7UpperLoad,
    NvpwrRvaF7Record,
    NvpwrRvaBoardType0Set,
    NvpwrRvaSetAmount,
    NvpwrRvaSetElig
};

/* 给日志和诊断用。移植时报告"哪一条签名不匹配"靠的就是这些名字。 */
static const char* const g_SigNames[NVPWR_TARGET_SIG_COUNT] = {
    "GpuRegistry",
    "UpperLoad",
    "F7Record",
    "BoardSet",
    "SetAmount",
    "SetElig"
};

/* 同上，用于 RVA 的诊断输出。 */
static const char* const g_RvaNames[NVPWR_TARGET_RVA_COUNT] = {
    "GpuGlobal",
    "GpuRegistryFn",
    "F7Generator",
    "F7UpperLoad",
    "F7Record",
    "BoardType0Set",
    "SetAmount",
    "SetElig"
};
