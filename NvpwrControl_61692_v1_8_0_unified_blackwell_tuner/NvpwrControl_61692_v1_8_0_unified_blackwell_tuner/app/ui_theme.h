#pragma once
/*
    ui_theme.h — presentation layer for the 1.9.0 "one tool" build.

    WHERE: compiled into NvpwrControl.exe only.
    WHAT:  colors, fonts, DPI helpers, owner-draw button painting, and the
           English / 简体中文 / Русский string table.
    WHY:   1.8.0 shipped an EN/RU table and translated by *scanning every window
           string for known English substrings* (`UiTranslate`). That approach
           cannot be extended to a third language safely: substring collisions
           grow, and partially translated concatenations produce mixed-language
           output. 1.9.0 addresses strings by ID instead, so a missing entry is
           a visible compile-time-known gap rather than silent corruption.

    This header performs no device, service or driver I/O.
*/

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <sstream>
#include <iomanip>

#include "nvpwr_ui_state.h"

/* ---------------- design tokens ---------------- */

static const COLORREF kUiBackground  = RGB(14, 18, 25);
static const COLORREF kUiCard        = RGB(22, 29, 41);
static const COLORREF kUiCardBorder  = RGB(42, 54, 76);
static const COLORREF kUiConsoleBg   = RGB(10, 13, 19);
static const COLORREF kUiNvidiaGreen = RGB(118, 185, 0);
static const COLORREF kUiCyan        = RGB(56, 189, 248);
static const COLORREF kUiAmber       = RGB(255, 170, 50);
static const COLORREF kUiCrimson     = RGB(220, 76, 92);
static const COLORREF kUiTextWhite   = RGB(248, 250, 252);
static const COLORREF kUiTextMuted   = RGB(148, 163, 184);
static const COLORREF kUiTextDim     = RGB(96, 112, 134);

/* ---------------- text ids ---------------- */

enum UiTextId {
    T_APP_TITLE = 0,
    T_APP_SUBTITLE,
    T_LANG_LABEL,

    T_NAV_POWER,
    T_NAV_STATUS,
    T_NAV_VOLTAGE,
    T_NAV_ADVANCED,
    T_NAV_SLOTS,
    T_NAV_LOG,
    T_NAV_SETTINGS,

    /* common */
    T_APPLY,
    T_RESTORE_OEM,
    T_REFRESH,
    T_RESET,
    T_ENABLE,
    T_DISABLE,
    T_READY,
    T_BUSY,
    T_NA,

    /* state labels */
    T_STATE_OEM,
    T_STATE_APPLIED,
    T_STATE_ARMED,
    T_STATE_MIXED,
    T_STATE_WRONG_BUILD,
    T_STATE_MODULE_MISSING,
    T_STATE_GPU_MISSING,
    T_STATE_CONTEXT_INVALID,
    T_STATE_PRECONDITION,
    T_STATE_UNKNOWN,

    /* power page */
    T_POWER_HEADING,
    T_POWER_HELP,
    T_POWER_TARGET,
    T_POWER_TARGET_HELP,
    T_POWER_CEILING,
    T_POWER_CEILING_HELP,
    T_POWER_QUICK,
    T_POWER_CURRENT,
    T_POWER_BASELINE,
    T_POWER_LOADED,
    T_POWER_MEASURED,
    T_POWER_CEILING_EFFECTIVE,
    T_POWER_APPLY,
    T_POWER_APPLYING,
    T_POWER_VERIFIED,
    T_POWER_EXPERIMENTAL,
    T_POWER_CONFIRM_TITLE,
    T_POWER_CONFIRM_BODY,
    T_POWER_CONFIRM_EXPERIMENTAL,
    T_POWER_INVALID_WATTS,
    T_POWER_OUT_OF_RANGE,
    T_POWER_ABOVE_CEILING,

    /* voltage page */
    T_VOLT_HEADING,
    T_VOLT_HELP,
    T_VOLT_NVVDD,
    T_VOLT_MSVDD,
    T_VOLT_VMIN,
    T_VOLT_REL,
    T_VOLT_ALT,
    T_VOLT_OV,
    T_VOLT_DEMAND,
    T_VOLT_DEMAND_CORE,
    T_VOLT_DEMAND_XBAR,
    T_VOLT_DEMAND_SYS,
    T_VOLT_DEMAND_VIDEO,
    T_VOLT_DEVICE_RANGE,
    T_VOLT_OFFSET_RANGE,
    T_VOLT_APPLY,
    T_VOLT_RESET,
    T_VOLT_CONFIRM_TITLE,
    T_VOLT_CONFIRM_BODY,
    T_VOLT_UNAVAILABLE,
    T_VOLT_NOTE_OFFSET,
    T_VOLT_IMPORT,
    T_VOLT_IMPORT_HELP,
    T_VOLT_BACKEND,
    T_VOLT_BACKEND_OK,
    T_VOLT_BACKEND_MISSING,
    T_VOLT_IMPORT_OK,
    T_VOLT_IMPORT_FAIL,
    T_VOLT_APPLY_FAIL,
    T_VOLT_ROUNDED,
    T_VOLT_NATIVE_UNAVAILABLE,

    /* advanced page */
    T_ADV_HEADING,
    T_ADV_HELP,
    T_ADV_CORE,
    T_ADV_MEMORY,
    T_ADV_XBAR,
    T_ADV_RATIO,
    T_ADV_APPLY,
    T_ADV_RESET,
    T_ADV_AFTERBURNER_NOTE,

    /* log page */
    T_LOG_HEADING,
    T_LOG_COPY,
    T_LOG_OPEN_FOLDER,
    T_LOG_CLEAR,

    /* settings page */
    T_SET_HEADING,
    T_SET_LANGUAGE,
    T_SET_START_WINDOWS,
    T_SET_START_MIN,
    T_SET_CLOSE_TO_TRAY,
    T_SET_SERVICE_STATE,
    T_SET_SERVICE_INSTALL,
    T_SET_SERVICE_REMOVE,
    T_SET_SERVICE_RUNNING,
    T_SET_SERVICE_STOPPED,
    T_SET_SERVICE_ABSENT,
    T_SET_MODE,
    T_SET_MODE_UNLOCK,
    T_SET_MODE_STANDARD,
    T_SET_MODE_HELP,
    T_SET_MODE_CONFIRM,

    /* tray */
    T_TRAY_SHOW,
    T_TRAY_APPLY,
    T_TRAY_RESTORE,
    T_TRAY_EXIT,
    T_TRAY_BALLOON_TITLE,
    T_TRAY_BALLOON_BODY,

    /* status page */
    T_STATUS_HEADING,
    T_STATUS_HELP,
    T_STATUS_ADAPTER,
    T_STATUS_DRIVER,
    T_STATUS_VBIOS,
    T_STATUS_POWER_NOW,
    T_STATUS_POWER_LIMIT,
    T_STATUS_CORE_CLK,
    T_STATUS_MEM_CLK,
    T_STATUS_TEMP,
    T_STATUS_HOTSPOT,
    T_STATUS_THERMAL_HEADROOM,
    T_STATUS_TLIMIT,
    T_STATUS_UTIL,
    T_STATUS_VRAM,
    T_STATUS_FAN,
    T_STATUS_PSTATE,
    T_STATUS_THROTTLE,
    T_STATUS_LIMITER,
    T_STATUS_POWER_SRC,
    T_STATUS_AC,
    T_STATUS_BATTERY,
    T_STATUS_MUX,
    T_STATUS_MUX_DIRECT,
    T_STATUS_MUX_HYBRID,
    T_STATUS_ADAPTERS,
    T_STATUS_SOURCE,
    T_STATUS_NO_VOLTAGE_NOTE,
    T_STATUS_MUX_POWER_NOTE,

    /* safety */
    T_SAFETY_PENDING,
    T_SAFETY_KEEP,
    T_SAFETY_REVERT,
    T_SAFETY_NOTE,

    /* one-shot apply */
    T_APPLY_ALL,
    T_APPLY_ALL_HELP,
    T_APPLY_ALL_CONFIRM,
    T_APPLY_PARTIAL,

    /* slots */
    T_SLOT_SAVE,
    T_SLOT_LOAD,
    T_SLOT_CLEAR,
    T_SLOT_HEADING,
    T_SLOT_EMPTY,

    /* status / errors */
    T_ERR_NO_DRIVER,
    T_ERR_NOT_ADMIN,
    T_ERR_DRIVER_PATH,
    T_ERR_IOCTL,
    T_ERR_SERVICE_IPC,
    T_TECH_DETAILS,

    T_TEXT_COUNT
};

/* ---------------- string table ---------------- */

struct UiString { const wchar_t* en; const wchar_t* zh; const wchar_t* ru; };

static const UiString kUiStrings[T_TEXT_COUNT] = {
    /* T_APP_TITLE            */ { L"NVPWR CONTROL", L"NVPWR 显卡功耗控制", L"NVPWR CONTROL" },
    /* T_APP_SUBTITLE         */ { L"Laptop GPU power & voltage console", L"笔记本显卡功耗与电压控制台", L"Консоль питания и напряжения GPU ноутбука" },
    /* T_LANG_LABEL           */ { L"Language", L"界面语言", L"Язык" },

    /* T_NAV_POWER            */ { L"Power", L"功耗", L"Питание" },
    /* T_NAV_STATUS           */ { L"Live status", L"运行状态", L"Состояние" },
    /* T_NAV_VOLTAGE          */ { L"Voltage", L"电压", L"Напряжение" },
    /* T_NAV_ADVANCED         */ { L"Clocks", L"频率", L"Частоты" },
    /* T_NAV_SLOTS            */ { L"Slots", L"配置槽位", L"Слоты" },
    /* T_NAV_LOG              */ { L"Log", L"日志", L"Журнал" },
    /* T_NAV_SETTINGS         */ { L"Settings", L"设置", L"Настройки" },

    /* T_APPLY                */ { L"Apply", L"应用", L"Применить" },
    /* T_RESTORE_OEM          */ { L"Restore OEM", L"恢复出厂", L"Вернуть OEM" },
    /* T_REFRESH              */ { L"Refresh", L"刷新", L"Обновить" },
    /* T_RESET                */ { L"Reset", L"重置", L"Сбросить" },
    /* T_ENABLE               */ { L"Enable", L"启用", L"Включить" },
    /* T_DISABLE              */ { L"Disable", L"停用", L"Отключить" },
    /* T_READY                */ { L"Ready", L"就绪", L"Готово" },
    /* T_BUSY                 */ { L"Working...", L"处理中…", L"Выполняется…" },
    /* T_NA                   */ { L"N/A", L"不可用", L"Н/Д" },

    /* T_STATE_OEM            */ { L"OEM STOCK", L"出厂状态", L"ШТАТНЫЙ OEM" },
    /* T_STATE_APPLIED        */ { L"APPLIED", L"已生效", L"ПРИМЕНЕНО" },
    /* T_STATE_ARMED          */ { L"ARMED / staging", L"准备中 / 过渡态", L"ПОДГОТОВЛЕНО" },
    /* T_STATE_MIXED          */ { L"MIXED — restore OEM", L"状态混合 — 请恢复出厂", L"СМЕШАННОЕ — вернуть OEM" },
    /* T_STATE_WRONG_BUILD    */ { L"UNSUPPORTED NVIDIA DRIVER", L"不支持的 NVIDIA 驱动版本", L"НЕПОДДЕРЖИВАЕМЫЙ ДРАЙВЕР" },
    /* T_STATE_MODULE_MISSING */ { L"nvlddmkm.sys not loaded", L"未加载 nvlddmkm.sys", L"nvlddmkm.sys не загружен" },
    /* T_STATE_GPU_MISSING    */ { L"NVIDIA GPU not found", L"未找到 NVIDIA 显卡", L"GPU NVIDIA не найден" },
    /* T_STATE_CONTEXT_INVALID*/ { L"driver context invalid", L"驱动上下文无效", L"Контекст драйвера некорректен" },
    /* T_STATE_PRECONDITION   */ { L"preconditions not met", L"前提条件未满足", L"Условия не выполнены" },
    /* T_STATE_UNKNOWN        */ { L"UNKNOWN", L"未知", L"НЕИЗВЕСТНО" },

    /* T_POWER_HEADING        */ { L"POWER LIMIT", L"功耗上限", L"ЛИМИТ ПИТАНИЯ" },
    /* T_POWER_HELP           */ { L"Enter any target the GPU will accept, then click Apply. The driver verifies the result against NVIDIA's own policy and rolls back if it does not match.",
                                  L"填入显卡能接受的目标值后点击“应用”。驱动会与 NVIDIA 自身策略比对回读结果，不一致会自动回滚。",
                                  L"Введите целевое значение и нажмите «Применить». Драйвер сверит результат и откатится при несовпадении." },
    /* T_POWER_TARGET         */ { L"Target (W)", L"目标功耗 (W)", L"Цель (Вт)" },
    /* T_POWER_TARGET_HELP    */ { L"Free input, 5 W steps. Bounds come from the GPU profile, not from a fixed menu.",
                                  L"自由输入，5 W 步进。范围由显卡型号决定，不再是固定菜单。",
                                  L"Свободный ввод, шаг 5 Вт. Диапазон задаёт профиль GPU." },
    /* T_POWER_CEILING        */ { L"Session ceiling (W)", L"本次会话上限 (W)", L"Потолок сессии (Вт)" },
    /* T_POWER_CEILING_HELP   */ { L"Upper bound this tool may request. The driver never exceeds its own compiled ceiling.",
                                  L"本工具允许申请的最高值。驱动自身编译上限不会被突破。",
                                  L"Максимум, который разрешено запросить." },
    /* T_POWER_QUICK          */ { L"Quick pick", L"快速选择", L"Быстрый выбор" },
    /* T_POWER_CURRENT        */ { L"Active limit", L"当前生效", L"Действующий лимит" },
    /* T_POWER_BASELINE       */ { L"OEM baseline", L"出厂基线", L"Штатный OEM" },
    /* T_POWER_LOADED         */ { L"Requested", L"已申请", L"Запрошено" },
    /* T_POWER_MEASURED       */ { L"Measured draw", L"实测功耗", L"Измеренное потребление" },
    /* T_POWER_CEILING_EFFECTIVE */ { L"Effective ceiling", L"生效上限", L"Действующий потолок" },
    /* T_POWER_APPLY          */ { L"Apply power limit", L"应用功耗上限", L"Применить лимит" },
    /* T_POWER_APPLYING       */ { L"Applying...", L"正在应用…", L"Применение…" },
    /* T_POWER_VERIFIED       */ { L"verified on hardware", L"已实机验证", L"проверено на железе" },
    /* T_POWER_EXPERIMENTAL   */ { L"experimental / unvalidated", L"实验性 / 未验证", L"экспериментально" },
    /* T_POWER_CONFIRM_TITLE  */ { L"Confirm power target", L"确认功耗目标", L"Подтвердите лимит" },
    /* T_POWER_CONFIRM_BODY   */ { L"Applying a higher power limit increases electrical and thermal load on the GPU, VRM, VRAM and the laptop cooling system. The adapter may not be able to sustain it.",
                                  L"提高功耗上限会增加 GPU、供电模块、显存和整机散热的电气与热负荷。电源适配器可能无法长期支撑。",
                                  L"Повышение лимита увеличивает электрическую и тепловую нагрузку." },
    /* T_POWER_CONFIRM_EXPERIMENTAL */ { L"This value has NOT been load-validated on this GPU profile.",
                                  L"该数值未在此显卡型号上做过负载验证。",
                                  L"Это значение НЕ проверено под нагрузкой." },
    /* T_POWER_INVALID_WATTS  */ { L"Enter a whole number of watts.", L"请输入整数瓦数。", L"Введите целое число ватт." },
    /* T_POWER_OUT_OF_RANGE   */ { L"Target is outside this GPU profile's range.", L"目标值超出该显卡型号的范围。", L"Значение вне диапазона профиля GPU." },
    /* T_POWER_ABOVE_CEILING  */ { L"Target is above the session ceiling you set.", L"目标值高于你设置的本次会话上限。", L"Значение выше потолка сессии." },

    /* T_VOLT_HEADING         */ { L"VOLTAGE", L"电压调节", L"НАПРЯЖЕНИЕ" },
    /* T_VOLT_HELP            */ { L"Rail offsets are applied against the driver's own baselines. They loosen or tighten voltage LIMITS; they do not force a voltage. Fine-tuning voltage is what lets an unlocked power limit actually be drawn.",
                                  L"各轨偏移基于驱动自身基线，放宽或收紧的是电压“限值”，不会强制电压。细调电压正是让解锁后的功耗真正被吃满的关键。",
                                  L"Смещения применяются к базовым линиям драйвера и меняют ПРЕДЕЛЫ напряжения." },
    /* T_VOLT_NVVDD           */ { L"NVDD (core rail)", L"NVDD（核心轨）", L"NVDD (ядро)" },
    /* T_VOLT_MSVDD           */ { L"MSVDD (fabric rail)", L"MSVDD（显存/互联轨）", L"MSVDD (фабрика)" },
    /* T_VOLT_VMIN            */ { L"VMIN", L"最低电压偏移 VMIN", L"VMIN" },
    /* T_VOLT_REL             */ { L"REL", L"可靠电压偏移 REL", L"REL" },
    /* T_VOLT_ALT             */ { L"ALT / OP", L"工作电压偏移 ALT/OP", L"ALT / OP" },
    /* T_VOLT_OV              */ { L"OV", L"过压上限偏移 OV", L"OV" },
    /* T_VOLT_DEMAND          */ { L"Per-domain voltage demand (mV)", L"各域电压需求 (mV)", L"Запрос напряжения по доменам (мВ)" },
    /* T_VOLT_DEMAND_CORE     */ { L"Core", L"核心 Core", L"Core" },
    /* T_VOLT_DEMAND_XBAR     */ { L"XBAR", L"XBAR", L"XBAR" },
    /* T_VOLT_DEMAND_SYS      */ { L"SYS", L"SYS", L"SYS" },
    /* T_VOLT_DEMAND_VIDEO    */ { L"Video", L"视频 Video", L"Video" },
    /* T_VOLT_DEVICE_RANGE    */ { L"Device range", L"器件范围", L"Диапазон устройства" },
    /* T_VOLT_OFFSET_RANGE    */ { L"Editable offset range", L"可编辑偏移范围", L"Диапазон смещений" },
    /* T_VOLT_APPLY           */ { L"Apply voltage", L"应用电压", L"Применить напряжение" },
    /* T_VOLT_RESET           */ { L"Reset voltage", L"重置电压", L"Сбросить напряжение" },
    /* T_VOLT_CONFIRM_TITLE   */ { L"Confirm voltage change", L"确认电压修改", L"Подтвердите напряжение" },
    /* T_VOLT_CONFIRM_BODY    */ { L"Voltage changes can make the GPU unstable, reset the display driver, or in the worst case damage the rail. A driver that accepts the value is not proof of stability.",
                                  L"电压修改可能导致显卡不稳定、驱动重置，极端情况会损坏供电轨。驱动接受了数值不等于稳定。",
                                  L"Изменение напряжения может привести к нестабильности драйвера или повреждению." },
    /* T_VOLT_UNAVAILABLE     */ { L"This GPU/driver does not expose editable voltage limits.", L"当前显卡/驱动未开放可编辑的电压限值。", L"Данный GPU/драйвер не предоставляет редактируемые лимиты напряжения." },
    /* T_VOLT_NOTE_OFFSET     */ { L"Positive raises the limit, negative lowers it. Units are mV.", L"正值放宽限值，负值收紧限值。单位 mV。", L"Плюс — выше предел, минус — ниже. Единица: мВ." },
    /* T_VOLT_IMPORT          */ { L"Read current from mVolt+", L"从 mVolt+ 读取当前值", L"Прочитать из mVolt+" },
    /* T_VOLT_IMPORT_HELP     */ { L"Fine-tune voltage in mVolt+, then read the result back here. Saved values are re-applied automatically at boot.",
                                  L"在 mVolt+ 里细调电压后，回到这里点“读取当前值”把结果导入。保存后开机会自动重新下发。",
                                  L"Настройте напряжение в mVolt+, затем импортируйте значения сюда." },
    /* T_VOLT_BACKEND         */ { L"Voltage backend", L"电压执行器", L"Исполнитель напряжения" },
    /* T_VOLT_BACKEND_OK      */ { L"mVolt+ found and ready", L"mVolt+ 已就绪", L"mVolt+ готов" },
    /* T_VOLT_BACKEND_MISSING */ { L"mVolt+ not found — place it next to this program, or set its path in Settings",
                                  L"未找到 mVolt+ —— 请放到本程序同目录，或在「设置」里指定路径",
                                  L"mVolt+ не найден — укажите путь в настройках" },
    /* T_VOLT_IMPORT_OK       */ { L"Imported from mVolt+", L"已从 mVolt+ 导入", L"Импортировано" },
    /* T_VOLT_IMPORT_FAIL     */ { L"Could not read from mVolt+", L"读取 mVolt+ 失败", L"Ошибка чтения" },
    /* T_VOLT_APPLY_FAIL      */ { L"mVolt+ could not apply the values", L"mVolt+ 应用失败", L"Не удалось применить" },
    /* T_VOLT_ROUNDED         */ { L"Values were rounded to whole millivolts.", L"数值已取整到整毫伏。", L"Значения округлены до мВ." },
    /* T_VOLT_NATIVE_UNAVAILABLE */ { L"This tool cannot write voltage itself: NVIDIA exposes no public voltage interface. mVolt+ performs the write; this program stores, replays and verifies it.",
                                  L"本工具无法直接写电压：NVIDIA 未提供公开的电压接口。由 mVolt+ 执行写入，本程序负责保存、开机重放与校验。",
                                  L"Запись напряжения выполняет mVolt+, эта программа сохраняет и воспроизводит её." },

    /* T_ADV_HEADING          */ { L"CLOCK OFFSETS", L"频率偏移", L"СМЕЩЕНИЯ ЧАСТОТ" },
    /* T_ADV_HELP             */ { L"Not exposed: GPU core/memory overclocking is out of scope for this tool. Use MSI Afterburner or MSI Center for that, or set offsets here if you prefer a single console.",
                                  L"本工具不主攻超频。核心/显存超频请用 MSI Afterburner 或 MSI Center；如果你想在一个界面里完成，也可以在这里设置偏移。",
                                  L"Разгон ядра/памяти не является целью этого инструмента." },
    /* T_ADV_CORE             */ { L"Core offset (MHz)", L"核心频率偏移 (MHz)", L"Смещение ядра (МГц)" },
    /* T_ADV_MEMORY           */ { L"Memory offset (MHz)", L"显存频率偏移 (MHz)", L"Смещение памяти (МГц)" },
    /* T_ADV_XBAR             */ { L"XBAR offset (MHz)", L"XBAR 频率偏移 (MHz)", L"Смещение XBAR (МГц)" },
    /* T_ADV_RATIO            */ { L"GPC:XBAR ratio", L"GPC:XBAR 比例", L"Коэффициент GPC:XBAR" },
    /* T_ADV_APPLY            */ { L"Apply clocks", L"应用频率", L"Применить частоты" },
    /* T_ADV_RESET            */ { L"Reset clocks", L"重置频率", L"Сбросить частоты" },
    /* T_ADV_AFTERBURNER_NOTE */ { L"If Afterburner or MSI Center also writes clock offsets, the last writer wins. Keep one tool in charge.",
                                  L"若 Afterburner / MSI Center 也在写频率偏移，后写入者生效。建议只让一个工具负责。",
                                  L"Если Afterburner тоже пишет смещения, побеждает последняя запись." },

    /* T_LOG_HEADING          */ { L"ACTIVITY LOG", L"运行日志", L"ЖУРНАЛ" },
    /* T_LOG_COPY             */ { L"Copy log", L"复制日志", L"Копировать" },
    /* T_LOG_OPEN_FOLDER      */ { L"Open folder", L"打开所在目录", L"Открыть папку" },
    /* T_LOG_CLEAR            */ { L"Clear view", L"清空显示", L"Очистить" },

    /* T_SET_HEADING          */ { L"SETTINGS", L"设置", L"НАСТРОЙКИ" },
    /* T_SET_LANGUAGE         */ { L"Interface language", L"界面语言", L"Язык интерфейса" },
    /* T_SET_START_WINDOWS    */ { L"Start with Windows", L"开机自动启动", L"Запуск с Windows" },
    /* T_SET_START_MIN        */ { L"Start minimized to tray", L"启动时最小化到托盘", L"Запуск свёрнутым в трей" },
    /* T_SET_CLOSE_TO_TRAY    */ { L"Closing the window keeps it in the tray", L"关闭窗口后保留在托盘", L"Закрытие окна оставляет в трее" },
    /* T_SET_SERVICE_STATE    */ { L"Background service", L"后台服务", L"Фоновая служба" },
    /* T_SET_SERVICE_INSTALL  */ { L"Install & start service", L"安装并启动服务", L"Установить службу" },
    /* T_SET_SERVICE_REMOVE   */ { L"Stop & remove service", L"停止并移除服务", L"Удалить службу" },
    /* T_SET_SERVICE_RUNNING  */ { L"running", L"运行中", L"работает" },
    /* T_SET_SERVICE_STOPPED  */ { L"stopped", L"已停止", L"остановлена" },
    /* T_SET_SERVICE_ABSENT   */ { L"not installed", L"未安装", L"не установлена" },
    /* T_SET_MODE             */ { L"Compatibility mode", L"兼容模式", L"Режим совместимости" },
    /* T_SET_MODE_UNLOCK      */ { L"Unlock mode (driver loaded)", L"解锁模式（驱动已加载）", L"Режим разблокировки" },
    /* T_SET_MODE_STANDARD    */ { L"Standard mode (driver removed)", L"标准模式（已卸载驱动）", L"Стандартный режим" },
    /* T_SET_MODE_HELP        */ { L"Standard mode stops the service and removes the helper so kernel anti-cheat games that require Secure Boot can run. Your settings are kept and re-applied when you return to unlock mode.",
                                  L"标准模式会停止服务并卸载驱动，以便运行要求开启 Secure Boot 的内核级反作弊游戏。你的设置会被保留，切回解锁模式时自动重新应用。",
                                  L"Стандартный режим останавливает службу и удаляет драйвер для игр с античитом." },
    /* T_SET_MODE_CONFIRM     */ { L"Switch to standard mode now? Power limits and tuning revert to OEM until you switch back.",
                                  L"现在切换到标准模式？功耗与频率设置会恢复到出厂状态，直到你切回解锁模式。",
                                  L"Переключиться в стандартный режим?" },

    /* T_TRAY_SHOW            */ { L"Show window", L"显示主界面", L"Показать окно" },
    /* T_TRAY_APPLY           */ { L"Apply saved settings", L"应用已保存设置", L"Применить настройки" },
    /* T_TRAY_RESTORE         */ { L"Restore OEM", L"恢复出厂", L"Вернуть OEM" },
    /* T_TRAY_EXIT            */ { L"Exit", L"退出", L"Выход" },
    /* T_TRAY_BALLOON_TITLE   */ { L"Nvpwr Control", L"Nvpwr 控制台", L"Nvpwr Control" },
    /* T_TRAY_BALLOON_BODY    */ { L"Still running in the background. Settings stay active.", L"仍在后台运行，设置保持生效。", L"Работает в фоне, настройки активны." },

    /* T_STATUS_HEADING       */ { L"LIVE STATUS", L"运行状态", L"ТЕКУЩЕЕ СОСТОЯНИЕ" },
    /* T_STATUS_HELP          */ { L"Read-only readings sampled from NVML (or nvidia-smi). Unavailable values are shown as N/A rather than estimated. There is no per-rail voltage reading here: the GPU does not expose board rail voltage through these interfaces.",
                                  L"以下为经由 NVML（或 nvidia-smi）读取的实时数据，只读。读取不到的项显示为“不可用”，不做估算。此处不提供分轨电压：显卡不通过这些接口暴露板级轨电压。",
                                  L"Только чтение через NVML или nvidia-smi. Недоступные значения показаны как Н/Д." },
    /* T_STATUS_ADAPTER       */ { L"Adapter", L"显卡", L"Адаптер" },
    /* T_STATUS_DRIVER        */ { L"Driver", L"驱动版本", L"Драйвер" },
    /* T_STATUS_VBIOS         */ { L"VBIOS", L"VBIOS 版本", L"VBIOS" },
    /* T_STATUS_POWER_NOW     */ { L"Board power now", L"当前实际功耗", L"Текущее потребление" },
    /* T_STATUS_POWER_LIMIT   */ { L"Enforced ceiling", L"当前生效上限", L"Действующий лимит" },
    /* T_STATUS_CORE_CLK      */ { L"Core clock", L"核心频率", L"Частота ядра" },
    /* T_STATUS_MEM_CLK       */ { L"Memory clock", L"显存频率", L"Частота памяти" },
    /* T_STATUS_TEMP          */ { L"GPU temperature", L"核心温度", L"Температура GPU" },
    /* T_STATUS_HOTSPOT       */ { L"Hotspot", L"热点温度", L"Hotspot" },
    /* T_STATUS_THERMAL_HEADROOM */ { L"Thermal headroom", L"温度余量", L"Запас по температуре" },
    /* T_STATUS_TLIMIT        */ { L"Thermal limit (T.Limit)", L"温度墙 (T.Limit)", L"Тепловой предел" },
    /* T_STATUS_UTIL          */ { L"GPU utilization", L"GPU 占用率", L"Загрузка GPU" },
    /* T_STATUS_VRAM          */ { L"VRAM used", L"显存占用", L"Использование VRAM" },
    /* T_STATUS_FAN           */ { L"Fan", L"风扇转速", L"Вентилятор" },
    /* T_STATUS_PSTATE        */ { L"P-state", L"性能状态 P-state", L"P-state" },
    /* T_STATUS_THROTTLE      */ { L"Throttle reasons", L"限流原因", L"Причины троттлинга" },
    /* T_STATUS_LIMITER       */ { L"What is limiting it now", L"当前瓶颈判断", L"Что ограничивает" },
    /* T_STATUS_POWER_SRC     */ { L"Power source", L"供电状态", L"Источник питания" },
    /* T_STATUS_AC            */ { L"AC adapter", L"电源适配器", L"Адаптер питания" },
    /* T_STATUS_BATTERY       */ { L"Battery", L"电池电量", L"Батарея" },
    /* T_STATUS_MUX           */ { L"Discrete GPU direct output", L"独显直连", L"Прямой вывод dGPU" },
    /* T_STATUS_MUX_DIRECT    */ { L"ON (dGPU drives the display)", L"已开启（独显直接输出）", L"ВКЛ" },
    /* T_STATUS_MUX_HYBRID    */ { L"OFF (hybrid / iGPU drives the display)", L"未开启（混合模式，核显输出）", L"ВЫКЛ" },
    /* T_STATUS_ADAPTERS      */ { L"Display adapters", L"显示适配器数量", L"Видеоадаптеров" },
    /* T_STATUS_SOURCE        */ { L"Reading source", L"数据来源", L"Источник данных" },
    /* T_STATUS_NO_VOLTAGE_NOTE */ { L"No rail-voltage or per-rail current reading is shown: the GPU does not expose them through NVML/NVAPI. Voltage LIMITS are on the Voltage page.",
                                  L"不显示分轨电压/分轨电流：显卡未通过 NVML/NVAPI 暴露这些数据。电压“限值”请见「电压」页。",
                                  L"Напряжение по рельсам недоступно через NVML/NVAPI." },
    /* T_STATUS_MUX_POWER_NOTE */ { L"Discrete-direct output usually raises whole-machine power draw. If the adapter is already at its limit, an unlock can be cancelled out by CPU/EC throttling.",
                                  L"独显直连通常会明显提高整机功耗。若电源适配器已接近上限，解锁的收益可能被 CPU/EC 限流抵消。",
                                  L"Прямой вывод повышает общее потребление; при слабом адаптере выигрыш может исчезнуть." },

    /* T_SAFETY_PENDING       */ { L"Provisional change — confirm to keep it", L"新设置已下发，待你确认保留", L"Изменение не подтверждено" },
    /* T_SAFETY_KEEP          */ { L"Keep it", L"确认保留", L"Оставить" },
    /* T_SAFETY_REVERT        */ { L"Revert now", L"立即回滚", L"Откатить" },
    /* T_SAFETY_NOTE          */ { L"Nothing is reverted automatically. If the display goes black or the driver resets, reboot: the machine returns to OEM limits.",
                                  L"不会自动回滚。若出现黑屏或驱动重置，直接重启即可回到出厂状态。",
                                  L"Автоотката нет. При чёрном экране перезагрузите систему." },
    /* T_APPLY_ALL            */ { L"Apply all changes", L"一键应用全部", L"Применить всё" },
    /* T_APPLY_ALL_HELP       */ { L"Sends power limit, then voltage offsets, then clock offsets in one action and reports exactly which stage failed.",
                                  L"按顺序一次性下发：功耗上限 → 电压偏移 → 频率偏移，并精确报告是哪一环失败。",
                                  L"Отправляет лимит питания, напряжение и частоты одной операцией." },
    /* T_APPLY_ALL_CONFIRM    */ { L"Apply power, voltage and clocks together?", L"确认一次性应用功耗、电压与频率？", L"Применить питание, напряжение и частоты?" },
    /* T_APPLY_PARTIAL        */ { L"Partially applied: the earlier stages are live, the listed stage failed.", L"部分生效：前面的环节已生效，后列环节失败。", L"Применено частично." },
    /* T_SLOT_SAVE            */ { L"Save", L"保存", L"Сохранить" },
    /* T_SLOT_LOAD            */ { L"Load", L"载入", L"Загрузить" },
    /* T_SLOT_CLEAR           */ { L"Clear", L"清空", L"Очистить" },
    /* T_SLOT_HEADING         */ { L"TUNING SLOTS", L"调参槽位", L"СЛОТЫ НАСТРОЕК" },
    /* T_SLOT_EMPTY           */ { L"empty", L"空", L"пусто" },

    /* T_ERR_NO_DRIVER        */ { L"Nvpwr driver is not open.", L"Nvpwr 驱动未连接。", L"Драйвер Nvpwr не открыт." },
    /* T_ERR_NOT_ADMIN        */ { L"Run as administrator.", L"请以管理员身份运行。", L"Запустите от имени администратора." },
    /* T_ERR_DRIVER_PATH      */ { L"Nvpwr.sys was not found next to the app or in dist\\driver build folders.", L"未在程序目录或 dist\\driver 构建目录中找到 Nvpwr.sys。", L"Nvpwr.sys не найден." },
    /* T_ERR_IOCTL            */ { L"Driver rejected the request.", L"驱动拒绝了该请求。", L"Драйвер отклонил запрос." },
    /* T_ERR_SERVICE_IPC      */ { L"Background service is not reachable; using direct driver access.", L"后台服务不可达，已回退为直接访问驱动。", L"Служба недоступна, используется прямой доступ." },
    /* T_TECH_DETAILS         */ { L"Technical details", L"技术详情", L"Технические сведения" },
};

/* ---------------- state ---------------- */

static nvpwr::Lang g_uiLang = nvpwr::Lang::Chinese;
static int  g_uiDpi = 96;
static HFONT g_uiFont = nullptr, g_uiHeadingFont = nullptr, g_uiStateFont = nullptr;
static HFONT g_uiMetricFont = nullptr, g_uiButtonFont = nullptr, g_uiMonoFont = nullptr, g_uiSmallFont = nullptr;
static HBRUSH g_uiBackground = nullptr, g_uiCard = nullptr, g_uiConsole = nullptr;
static HPEN g_uiCardPen = nullptr;

static const wchar_t* UiText(UiTextId id) {
    if (id < 0 || id >= T_TEXT_COUNT) return L"";
    const UiString& s = kUiStrings[id];
    switch (g_uiLang) {
    case nvpwr::Lang::Chinese: return s.zh;
    case nvpwr::Lang::Russian: return s.ru;
    default:                   return s.en;
    }
}

/* Chinese needs a face that actually carries CJK glyphs. Segoe UI relies on
   registry FontLink fallbacks, which are absent on non-Chinese Windows and on
   some LTSC/Server images, producing tofu boxes. Pick the face explicitly. */
static const wchar_t* UiFontFace() {
    return (g_uiLang == nvpwr::Lang::Chinese) ? L"Microsoft YaHei UI" : L"Segoe UI";
}

static int UiScale(int value) { return MulDiv(value, g_uiDpi, 96); }

static void UiCreateFonts() {
    if (g_uiFont) DeleteObject(g_uiFont);
    if (g_uiHeadingFont) DeleteObject(g_uiHeadingFont);
    if (g_uiMetricFont) DeleteObject(g_uiMetricFont);
    if (g_uiStateFont) DeleteObject(g_uiStateFont);
    if (g_uiButtonFont) DeleteObject(g_uiButtonFont);
    if (g_uiMonoFont) DeleteObject(g_uiMonoFont);
    if (g_uiSmallFont) DeleteObject(g_uiSmallFont);

    const wchar_t* face = UiFontFace();
    /* Chinese glyphs are visually denser; add one point of leading so the
       extra height does not clip inside the fixed-height owner-draw buttons. */
    const int bump = (g_uiLang == nvpwr::Lang::Chinese) ? 1 : 0;
    const wchar_t* mono = (g_uiLang == nvpwr::Lang::Chinese) ? L"Consolas" : L"Consolas";

    g_uiFont       = CreateFontW(-UiScale(14 + bump), 0,0,0, FW_NORMAL,   FALSE,FALSE,FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    g_uiHeadingFont= CreateFontW(-UiScale(22 + bump), 0,0,0, FW_BOLD,     FALSE,FALSE,FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    g_uiMetricFont = CreateFontW(-UiScale(28 + bump), 0,0,0, FW_BOLD,     FALSE,FALSE,FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    g_uiStateFont  = CreateFontW(-UiScale(15 + bump), 0,0,0, FW_SEMIBOLD, FALSE,FALSE,FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    g_uiButtonFont = CreateFontW(-UiScale(14 + bump), 0,0,0, FW_SEMIBOLD, FALSE,FALSE,FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    g_uiMonoFont   = CreateFontW(-UiScale(13), 0,0,0, FW_NORMAL,   FALSE,FALSE,FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, mono);
    g_uiSmallFont  = CreateFontW(-UiScale(12 + bump), 0,0,0, FW_NORMAL,   FALSE,FALSE,FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void UiFont(HWND h, HFONT font = nullptr) {
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)(font ? font : g_uiFont), TRUE);
}

static void UiSetDpi(UINT dpi) {
    if (!dpi) return;
    g_uiDpi = (int)dpi;
    UiCreateFonts();
}

/* ---------------- settings persistence ----------------
   Stored under %LOCALAPPDATA%\NvpwrControlUI\settings.ini.
   Language moved from the 1.8.0 "[Interface] Russian=0/1" key to a
   three-valued Language key; the old key is still read for migration. */

static std::wstring UiSettingsPath() {
    wchar_t base[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring folder = std::wstring(base) + L"\\NvpwrControlUI";
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return L"";
    return folder + L"\\settings.ini";
}

/* ---------------- small helpers ---------------- */

static void UiSetTextRaw(HWND h, const std::wstring& value) {
    if (h) SetWindowTextW(h, value.c_str());
}

static void UiSetTextId(HWND h, UiTextId id) {
    if (h) SetWindowTextW(h, UiText(id));
}

static std::wstring UiFormatWatts(unsigned int mw) {
    if (mw == 0xFFFFFFFFu) return UiText(T_NA);
    wchar_t b[64]{};
    swprintf_s(b, L"%.1f W", mw / 1000.0);
    return b;
}

static std::wstring UiFormatMv(long long uv) {
    wchar_t b[64]{};
    swprintf_s(b, L"%+.0f mV", uv / 1000.0);
    return b;
}

static std::wstring UiFormatMhz(long khz) {
    wchar_t b[64]{};
    swprintf_s(b, L"%+ld MHz", khz / 1000);
    return b;
}

static void UiDestroy() {
    HFONT fonts[] = { g_uiFont, g_uiHeadingFont, g_uiMetricFont, g_uiStateFont,
                      g_uiButtonFont, g_uiMonoFont, g_uiSmallFont };
    for (HFONT f : fonts) if (f) DeleteObject(f);
    if (g_uiBackground) DeleteObject(g_uiBackground);
    if (g_uiCard) DeleteObject(g_uiCard);
    if (g_uiConsole) DeleteObject(g_uiConsole);
    if (g_uiCardPen) DeleteObject(g_uiCardPen);
}

static void UiInitBrushes() {
    g_uiBackground = CreateSolidBrush(kUiBackground);
    g_uiCard       = CreateSolidBrush(kUiCard);
    g_uiConsole    = CreateSolidBrush(kUiConsoleBg);
    g_uiCardPen    = CreatePen(PS_SOLID, 1, kUiCardBorder);
}
