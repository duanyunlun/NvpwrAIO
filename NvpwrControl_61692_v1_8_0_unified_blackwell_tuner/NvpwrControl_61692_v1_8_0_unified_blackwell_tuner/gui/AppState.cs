using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.Eventing.Reader;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace NvpwrControl
{
    /// <summary>
    /// Rail offsets in microvolts, exactly as mVolt+ reports them. Storing the
    /// companion tool's own units avoids any rounding or ambiguity about what
    /// "voltage" means; the conversion to millivolts happens once, at the CLI
    /// boundary.
    /// </summary>
    internal sealed class RailOffsets
    {
        public long VminUv, RelUv, AltUv, OvUv;

        public bool IsZero { get { return VminUv == 0 && RelUv == 0 && AltUv == 0 && OvUv == 0; } }
        public RailOffsets Clone() { return (RailOffsets)MemberwiseClone(); }
    }

    internal sealed class VoltageTuning
    {
        public bool Enabled;
        public RailOffsets Nvvdd = new RailOffsets();
        public RailOffsets Msvdd = new RailOffsets();
        public long DemandCoreMv, DemandXbarMv, DemandSysMv, DemandVideoMv;

        /// <summary>
        /// The factory voltage limits, in mV, captured once while every offset was zero.
        ///
        /// These have to be remembered rather than derived from the live readings. The obvious
        /// derivation — reported limit minus the offset on that rail — only holds while OV is
        /// untouched, because an active OV offset changes the reported maximum without appearing
        /// in the REL value. Deriving it after the ceiling had been pulled to 1020 produced a
        /// baseline of 1020, and asking for 1025 then wrote REL +5 against the real baseline of
        /// 1025, landing on 1030.
        /// </summary>
        public long BaselineMinMv, BaselineMaxMv;

        public bool DemandIsZero
        {
            get { return DemandCoreMv == 0 && DemandXbarMv == 0 && DemandSysMv == 0 && DemandVideoMv == 0; }
        }
        public bool IsZero { get { return Nvvdd.IsZero && Msvdd.IsZero && DemandIsZero; } }
    }

    internal sealed class ClockTuning
    {
        public bool Enabled;
        public long CoreOffsetMhz, MemoryOffsetMhz, XbarOffsetMhz;

        /// <summary>
        /// SYS / host-interface domain offset.
        ///
        /// Applied through mVolt+ rather than the NVAPI path the other three use. Core and memory
        /// ride Pstates20, XBAR goes through the ClockDomains transaction, and SYS has no
        /// equivalent there — the ClockDomains reader resolves a single domain by design, and the
        /// XBAR index is baked into it. mVolt+ exposes --sys-offset and reports the range, so that
        /// is where this one goes. Different mechanism, same "Apply" button.
        /// </summary>
        public long SysOffsetMhz;

        public bool IsZero { get { return CoreOffsetMhz == 0 && MemoryOffsetMhz == 0 && XbarOffsetMhz == 0 && SysOffsetMhz == 0; } }
    }

    /// <summary>
    /// The complete "what the user wants the GPU to do" record.
    ///
    /// Written to %ProgramData%\NvpwrControl\state.ini in the same flat key/value
    /// format the C++ service reads, so either program can produce a state file
    /// the other understands. That compatibility is deliberate: the service owns
    /// boot replay, and it must not care which UI wrote the file.
    /// </summary>
    internal sealed class DesiredState
    {
        public int Schema = 2;
        public bool PowerEnabled;
        public uint PowerMw;
        public uint CeilingMw;
        public uint Profile;

        /// <summary>
        /// The factory power wall in watts, recorded once and then trusted.
        ///
        /// It cannot be read back on demand, because the value any source would return is the
        /// one the GPU currently enforces — and changing that is this program's job. Measured
        /// directly: raising the ceiling to 200 W moved both the NVML maximum and the driver
        /// upper boundary to 200, after which the interface announced a factory wall of 200 W.
        ///
        /// The driver OemBaseline is authoritative while it is loaded and untouched, so that is
        /// what gets recorded. The boot service runs before anything has been applied, which is
        /// the one moment a live reading is guaranteed to be the factory value.
        ///
        /// Written once and never rewritten. A live reading cannot tell a factory wall from one
        /// this program raised, so the record is the only thing that can — and a record that any
        /// later reading may overwrite is not a record at all. The one measured failure: a helper
        /// loaded after the ceiling had moved reports the moved value as its baseline, and the
        /// write-through that used to be here replaced the correct 175 with 250.
        /// </summary>
        public int PowerFloorW;

        /// <summary>
        /// The GPU profile PowerFloorW was captured for.
        ///
        /// A different profile means different hardware or a different VBIOS, and therefore a
        /// different factory wall — it is the only thing that may invalidate the record besides
        /// the record being absent. The service writes both fields together; this side must
        /// carry the profile through unchanged, or the service would find its own value paired
        /// with a zero profile on the next start and re-capture every time.
        /// </summary>
        public uint PowerFloorProfile;

        /// <summary>
        /// The system boot time at which PowerFloorW was recorded, as ticks.
        ///
        /// Audit only — nothing branches on it. It answers "when was this captured", which is the
        /// first question when a factory wall looks wrong, and it distinguishes a record made on
        /// a boot where the wall was still factory from one made later.
        /// </summary>
        public long PowerFloorBoot;
        public VoltageTuning Voltage = new VoltageTuning();
        public ClockTuning Clock = new ClockTuning();
        public string MvoltPath = "";
        public bool StartWithWindows;
        public bool StartMinimized;

        public bool IsEmpty
        {
            get { return !PowerEnabled && !Voltage.Enabled && !Clock.Enabled; }
        }

        public static DesiredState Default()
        {
            DesiredState s = new DesiredState();
            s.PowerEnabled = false;
            s.PowerMw = 0;
            s.CeilingMw = Driver.DefaultCeilingMw;
            s.Voltage.Enabled = false;
            s.Clock.Enabled = false;
            return s;
        }

        public DesiredState Clone()
        {
            DesiredState s = (DesiredState)MemberwiseClone();
            s.Voltage = new VoltageTuning
            {
                Enabled = Voltage.Enabled,
                Nvvdd = Voltage.Nvvdd.Clone(),
                Msvdd = Voltage.Msvdd.Clone(),
                DemandCoreMv = Voltage.DemandCoreMv,
                DemandXbarMv = Voltage.DemandXbarMv,
                DemandSysMv = Voltage.DemandSysMv,
                DemandVideoMv = Voltage.DemandVideoMv
            };
            s.Clock = new ClockTuning
            {
                Enabled = Clock.Enabled,
                CoreOffsetMhz = Clock.CoreOffsetMhz,
                MemoryOffsetMhz = Clock.MemoryOffsetMhz,
                XbarOffsetMhz = Clock.XbarOffsetMhz
            };
            return s;
        }
    }

    internal sealed class ConfigSlot
    {
        public bool Used;
        public string Name = "";
        public string SavedAt = "";
        public DesiredState State = DesiredState.Default();
        public bool LastApplyOk;
        public string LastApplyNote = "";
    }

    /// <summary>
    /// Persistence for the desired state, the six tuning slots, and the restore
    /// point.
    ///
    /// Format notes: the state file must stay readable by the C++ service, so the
    /// keys are copied verbatim from nvpwr_ipc.cpp. Slots live in a separate file
    /// because resetting the live settings must never destroy the record of which
    /// experiments were worth keeping.
    /// </summary>
    internal static class Store
    {
        public const int SlotCount = 6;

        public static string Dir
        {
            get
            {
                string programData = Environment.GetEnvironmentVariable("ProgramData");
                if (string.IsNullOrEmpty(programData)) programData = @"C:\ProgramData";
                string dir = Path.Combine(programData, "NvpwrControl");
                try { Directory.CreateDirectory(dir); } catch { }
                return dir;
            }
        }

        public static string StatePath { get { return Path.Combine(Dir, "state.ini"); } }
        public static string ProfilePath { get { return Path.Combine(Dir, "profiles.ini"); } }
        public static string LogPath { get { return Path.Combine(Dir, "nvpwr-control.log"); } }

        // ---------------------------------------------------------------- log

        private static readonly object LogLock = new object();

        public static void Log(string text)
        {
            try
            {
                lock (LogLock)
                {
                    string line = "[" + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff") + "] " + text + Environment.NewLine;
                    File.AppendAllText(LogPath, line, new UTF8Encoding(false));
                }
            }
            catch { /* logging must never take the app down */ }
        }

        // ------------------------------------------------------- state file

        private static string Sanitize(string s)
        {
            if (s == null) return "";
            return s.Replace('\r', ' ').Replace('\n', ' ');
        }

        public static bool SaveState(DesiredState s, out string error)
        {
            error = null;
            try
            {
                StringBuilder b = new StringBuilder();
                b.AppendLine("; Nvpwr Control desired state.");
                b.AppendLine("; The GPU does not remember these across a driver reload or reboot;");
                b.AppendLine("; the background service re-applies them at boot.");
                b.AppendLine("schema=" + s.Schema.ToString(CultureInfo.InvariantCulture));
                WriteStateKeys(b, "", s);
                File.WriteAllText(StatePath, b.ToString(), new UTF8Encoding(false));
                return true;
            }
            catch (Exception ex)
            {
                error = "无法写入状态文件: " + ex.Message;
                return false;
            }
        }

        private static void WriteStateKeys(StringBuilder b, string p, DesiredState s)
        {
            b.AppendLine(p + "power_enabled=" + (s.PowerEnabled ? 1 : 0));
            b.AppendLine(p + "power_mw=" + s.PowerMw);
            b.AppendLine(p + "power_ceiling_mw=" + s.CeilingMw);
            b.AppendLine(p + "power_profile=" + s.Profile);
            b.AppendLine(p + "power_floor_w=" + s.PowerFloorW);
            b.AppendLine(p + "power_floor_profile=" + s.PowerFloorProfile);
            b.AppendLine(p + "power_floor_boot=" + s.PowerFloorBoot);
            b.AppendLine(p + "voltage_enabled=" + (s.Voltage.Enabled ? 1 : 0));
            b.AppendLine(p + "voltage_applier=1");   // 1 = companion tool
            b.AppendLine(p + "nvvdd_vmin_uv=" + s.Voltage.Nvvdd.VminUv);
            b.AppendLine(p + "nvvdd_rel_uv=" + s.Voltage.Nvvdd.RelUv);
            b.AppendLine(p + "nvvdd_alt_uv=" + s.Voltage.Nvvdd.AltUv);
            b.AppendLine(p + "nvvdd_ov_uv=" + s.Voltage.Nvvdd.OvUv);
            b.AppendLine(p + "baseline_min_mv=" + s.Voltage.BaselineMinMv);
            b.AppendLine(p + "baseline_max_mv=" + s.Voltage.BaselineMaxMv);
            b.AppendLine(p + "msvdd_vmin_uv=" + s.Voltage.Msvdd.VminUv);
            b.AppendLine(p + "msvdd_rel_uv=" + s.Voltage.Msvdd.RelUv);
            b.AppendLine(p + "msvdd_alt_uv=" + s.Voltage.Msvdd.AltUv);
            b.AppendLine(p + "msvdd_ov_uv=" + s.Voltage.Msvdd.OvUv);
            b.AppendLine(p + "demand_core_mv=" + s.Voltage.DemandCoreMv);
            b.AppendLine(p + "demand_xbar_mv=" + s.Voltage.DemandXbarMv);
            b.AppendLine(p + "demand_sys_mv=" + s.Voltage.DemandSysMv);
            b.AppendLine(p + "demand_video_mv=" + s.Voltage.DemandVideoMv);
            b.AppendLine(p + "clock_enabled=" + (s.Clock.Enabled ? 1 : 0));
            b.AppendLine(p + "clock_core_mhz=" + s.Clock.CoreOffsetMhz);
            b.AppendLine(p + "clock_memory_mhz=" + s.Clock.MemoryOffsetMhz);
            b.AppendLine(p + "clock_xbar_mhz=" + s.Clock.XbarOffsetMhz);
            b.AppendLine(p + "clock_sys_mhz=" + s.Clock.SysOffsetMhz);
            b.AppendLine(p + "start_with_windows=" + (s.StartWithWindows ? 1 : 0));
            b.AppendLine(p + "start_minimized=" + (s.StartMinimized ? 1 : 0));
            b.AppendLine(p + "mvolt_path=" + Sanitize(s.MvoltPath));
        }

        public static DesiredState LoadState(out string error)
        {
            error = null;
            DesiredState s = DesiredState.Default();
            if (!File.Exists(StatePath)) return s;   // first run is not an error

            try
            {
                Dictionary<string, string> kv = ReadKv(StatePath);
                if (kv.Count == 0)
                {
                    error = "状态文件存在但无法解析: " + StatePath;
                    return s;
                }
                ApplyKeys(s, kv, "");
                return s;
            }
            catch (Exception ex)
            {
                error = "读取状态文件失败: " + ex.Message;
                return s;
            }
        }

        private static void ApplyKeys(DesiredState s, Dictionary<string, string> kv, string p)
        {
            Func<string, long> num = key =>
            {
                string v;
                long n;
                if (kv.TryGetValue(p + key, out v) && long.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out n))
                    return n;
                return 0;
            };
            Func<string, bool> has = key => kv.ContainsKey(p + key);

            if (has("schema")) s.Schema = (int)num("schema");
            s.PowerEnabled = num("power_enabled") != 0;
            s.PowerMw = (uint)num("power_mw");
            uint ceiling = (uint)num("power_ceiling_mw");
            s.CeilingMw = ceiling == 0 ? Driver.DefaultCeilingMw : ceiling;
            s.Profile = (uint)num("power_profile");
            s.PowerFloorW = (int)num("power_floor_w");
            s.PowerFloorProfile = (uint)num("power_floor_profile");
            s.PowerFloorBoot = num("power_floor_boot");

            s.Voltage.Enabled = num("voltage_enabled") != 0;
            s.Voltage.Nvvdd.VminUv = num("nvvdd_vmin_uv");
            s.Voltage.Nvvdd.RelUv = num("nvvdd_rel_uv");
            s.Voltage.Nvvdd.AltUv = num("nvvdd_alt_uv");
            s.Voltage.Nvvdd.OvUv = num("nvvdd_ov_uv");
            s.Voltage.BaselineMinMv = num("baseline_min_mv");
            s.Voltage.BaselineMaxMv = num("baseline_max_mv");
            s.Voltage.Msvdd.VminUv = num("msvdd_vmin_uv");
            s.Voltage.Msvdd.RelUv = num("msvdd_rel_uv");
            s.Voltage.Msvdd.AltUv = num("msvdd_alt_uv");
            s.Voltage.Msvdd.OvUv = num("msvdd_ov_uv");
            s.Voltage.DemandCoreMv = num("demand_core_mv");
            s.Voltage.DemandXbarMv = num("demand_xbar_mv");
            s.Voltage.DemandSysMv = num("demand_sys_mv");
            s.Voltage.DemandVideoMv = num("demand_video_mv");

            s.Clock.Enabled = num("clock_enabled") != 0;
            s.Clock.CoreOffsetMhz = num("clock_core_mhz");
            s.Clock.MemoryOffsetMhz = num("clock_memory_mhz");
            s.Clock.XbarOffsetMhz = num("clock_xbar_mhz");
            s.Clock.SysOffsetMhz = num("clock_sys_mhz");

            s.StartWithWindows = num("start_with_windows") != 0;
            s.StartMinimized = num("start_minimized") != 0;

            string path;
            if (kv.TryGetValue(p + "mvolt_path", out path)) s.MvoltPath = path;
        }

        private static Dictionary<string, string> ReadKv(string path)
        {
            Dictionary<string, string> kv = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (string raw in File.ReadAllLines(path, Encoding.UTF8))
            {
                string line = raw.TrimEnd('\r');
                if (line.Length == 0 || line[0] == ';' || line[0] == '#') continue;
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                kv[line.Substring(0, eq)] = line.Substring(eq + 1);
            }
            return kv;
        }

        // -------------------------------------------------------- slot file

        public static ConfigSlot[] LoadSlots(out string error)
        {
            error = null;
            ConfigSlot[] slots = new ConfigSlot[SlotCount];
            for (int i = 0; i < SlotCount; i++) slots[i] = new ConfigSlot();
            if (!File.Exists(ProfilePath)) return slots;

            try
            {
                Dictionary<string, string> kv = ReadKv(ProfilePath);
                for (int i = 0; i < SlotCount; i++)
                {
                    string p = "slot" + (i + 1) + "_";
                    ConfigSlot slot = slots[i];
                    string v;
                    slot.Used = kv.TryGetValue(p + "used", out v) && v == "1";
                    if (kv.TryGetValue(p + "name", out v)) slot.Name = v;
                    if (kv.TryGetValue(p + "saved_at", out v)) slot.SavedAt = v;
                    slot.LastApplyOk = kv.TryGetValue(p + "last_ok", out v) && v == "1";
                    if (kv.TryGetValue(p + "last_note", out v)) slot.LastApplyNote = v;
                    if (slot.Used) ApplyKeys(slot.State, kv, p);
                }
                return slots;
            }
            catch (Exception ex)
            {
                error = "读取槽位文件失败: " + ex.Message;
                return slots;
            }
        }

        public static bool SaveSlots(ConfigSlot[] slots, out string error)
        {
            error = null;
            try
            {
                StringBuilder b = new StringBuilder();
                b.AppendLine("; Nvpwr Control saved tuning slots.");
                b.AppendLine("; Slots deliberately survive resetting the live settings: they are the");
                b.AppendLine("; record of which experiments were worth keeping.");
                b.AppendLine("schema=1");
                b.AppendLine("slot_count=" + SlotCount);

                for (int i = 0; i < SlotCount && i < slots.Length; i++)
                {
                    ConfigSlot s = slots[i];
                    if (!s.Used) continue;
                    string p = "slot" + (i + 1) + "_";
                    b.AppendLine(p + "used=1");
                    b.AppendLine(p + "name=" + Sanitize(s.Name));
                    b.AppendLine(p + "saved_at=" + Sanitize(s.SavedAt));
                    b.AppendLine(p + "last_ok=" + (s.LastApplyOk ? 1 : 0));
                    b.AppendLine(p + "last_note=" + Sanitize(s.LastApplyNote));
                    WriteStateKeys(b, p, s.State);
                }
                File.WriteAllText(ProfilePath, b.ToString(), new UTF8Encoding(false));
                return true;
            }
            catch (Exception ex)
            {
                error = "写入槽位文件失败: " + ex.Message;
                return false;
            }
        }

        // --------------------------------------------- restore point (memory)

        private static DesiredState _restorePoint;
        private static string _restoreLabel = "";

        public static void SetRestorePoint(string label, DesiredState state)
        {
            _restorePoint = state.Clone();
            _restoreLabel = label;
        }

        public static DesiredState GetRestorePoint(out string label)
        {
            label = _restoreLabel;
            return _restorePoint == null ? null : _restorePoint.Clone();
        }

        public static void ClearRestorePoint()
        {
            _restorePoint = null;
            _restoreLabel = "";
        }
    }

    /// <summary>One reading of mVolt+'s status output.</summary>
    internal sealed class MVoltSnapshot
    {
        public bool Ok;
        public string Gpu = "", Vbios = "", Error = "";
        public RailOffsets Nvvdd = new RailOffsets();
        public RailOffsets Msvdd = new RailOffsets();
        public long DemandCoreMv, DemandXbarMv, DemandSysMv, DemandVideoMv;

        /// <summary>
        /// The factory voltage limits, in mV, captured once while every offset was zero.
        ///
        /// These have to be remembered rather than derived from the live readings. The obvious
        /// derivation — reported limit minus the offset on that rail — only holds while OV is
        /// untouched, because an active OV offset changes the reported maximum without appearing
        /// in the REL value. Deriving it after the ceiling had been pulled to 1020 produced a
        /// baseline of 1020, and asking for 1025 then wrote REL +5 against the real baseline of
        /// 1025, landing on 1030.
        /// </summary>
        public long BaselineMinMv, BaselineMaxMv;
        // Two different things, and conflating them produced a wrong reading in the UI:
        //   NvvddMinUv/MaxUv        the DEVICE range the rail can be set within (445–1280 mV)
        //   NvvddLimitMinMv/MaxMv   the limits currently in force (625/1025 mV), which move
        //                           when a limit offset is applied
        public long NvvddMinUv, NvvddMaxUv, NvvddStepUv;
        public long NvvddLimitMinMv, NvvddLimitMaxMv;
        public long MsvddMinUv, MsvddMaxUv, MsvddStepUv;
        // MSVDD 当前生效的上下限，和 NvvddLimitMinMv/MaxMv 一个意思 —— 没有它就只能把
        // MSVDD 滑块做成偏移，做不成绝对值。
        public long MsvddLimitMinMv, MsvddLimitMaxMv;
        /// <summary>mVolt+ 报告的 BoostLock 状态。</summary>
        public bool BoostLocked;

        /// <summary>
        /// The SYS domain's current offset and writable range, as mVolt+ reports them.
        ///
        /// SYS is the one clock domain this program has no direct interface for: the ClockDomains
        /// transaction resolves XBAR specifically. mVolt+ does expose it, so the row is filled
        /// from here and applied through the companion rather than through NVAPI.
        /// </summary>
        public long SysOffsetMhz, SysLimitMinMhz, SysLimitMaxMhz;
        public bool HasEnforced;
        public uint EnforcedMw;
    }

    /// <summary>
    /// Companion-tool integration.
    ///
    /// This program cannot write voltage rails itself: the public NVAPI surface
    /// has no voltage setter, and Pstates20 carries no voltage domain on the
    /// target GPU. mVolt+ reaches those rails through a reverse-engineered
    /// private interface, so the arrangement is "this program orchestrates,
    /// mVolt+ executes" — including at boot, where the service replays by
    /// invoking the same CLI.
    ///
    /// Parsing is done with small regexes over the JSON text rather than a JSON
    /// library: the shape is flat and stable, and tolerating unknown or reordered
    /// keys matters more than strict deserialisation.
    /// </summary>
    internal static class MVolt
    {
        public const string ExeName = "mVolt+.exe";

        public static string LastCommandLine { get; private set; }

        public static string Find(string explicitPath)
        {
            if (!string.IsNullOrEmpty(explicitPath)) return explicitPath;

            List<string> roots = new List<string>();
            string exeDir = Path.GetDirectoryName(System.Reflection.Assembly.GetExecutingAssembly().Location);
            if (!string.IsNullOrEmpty(exeDir)) roots.Add(exeDir);

            string programData = Environment.GetEnvironmentVariable("ProgramData");
            if (!string.IsNullOrEmpty(programData))
                roots.Add(Path.Combine(programData, "NvpwrControl"));

            string profile = Environment.GetEnvironmentVariable("USERPROFILE");
            if (!string.IsNullOrEmpty(profile))
            {
                roots.Add(Path.Combine(profile, "Downloads"));
                roots.Add(Path.Combine(profile, "Documents"));
                roots.Add(Path.Combine(profile, "Desktop"));
            }

            foreach (string root in roots)
            {
                string dir = root;
                for (int depth = 0; depth < 4 && !string.IsNullOrEmpty(dir); depth++)
                {
                    string c1 = Path.Combine(dir, ExeName);
                    if (File.Exists(c1)) return c1;
                    string c2 = Path.Combine(Path.Combine(dir, "dist"), ExeName);
                    if (File.Exists(c2)) return c2;
                    DirectoryInfo parent = Directory.GetParent(dir);
                    if (parent == null) break;
                    dir = parent.FullName;
                }
            }
            return "";
        }

        public static bool Available(string explicitPath)
        {
            string p = Find(explicitPath);
            return !string.IsNullOrEmpty(p) && File.Exists(p);
        }

        private static string Run(string exe, string args, int timeoutMs, out string error)
        {
            error = null;
            string cmd = "\"" + exe + "\" " + args;
            LastCommandLine = cmd;

            try
            {
                ProcessStartInfo psi = new ProcessStartInfo(exe, args)
                {
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true,
                    StandardOutputEncoding = Encoding.UTF8,
                    StandardErrorEncoding = Encoding.UTF8
                };
                using (Process p = Process.Start(psi))
                {
                    if (p == null) { error = "无法启动 mVolt+"; return null; }
                    string stdout = p.StandardOutput.ReadToEnd();
                    p.StandardError.ReadToEnd();
                    if (!p.WaitForExit(timeoutMs))
                    {
                        try { p.Kill(); } catch { }
                        error = "mVolt+ 未在预期时间内退出";
                        return null;
                    }
                    return stdout;
                }
            }
            catch (Exception ex)
            {
                error = "运行 mVolt+ 失败: " + ex.Message;
                return null;
            }
        }

        private static bool TryLong(string json, string key, out long value)
        {
            value = 0;
            Match m = Regex.Match(json, "\"" + Regex.Escape(key) + "\"\\s*:\\s*(-?\\d+)");
            if (!m.Success) return false;
            return long.TryParse(m.Groups[1].Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out value);
        }

        private static void ReadRail(string json, string railKey, RailOffsets target)
        {
            Match block = Regex.Match(json, "\"" + Regex.Escape(railKey) + "\"\\s*:\\s*\\{([^}]*)\\}");
            if (!block.Success) return;
            string inner = block.Groups[1].Value;
            long v;
            if (TryLong(inner, "vmin", out v)) target.VminUv = v;
            if (TryLong(inner, "rel", out v)) target.RelUv = v;
            if (TryLong(inner, "alt", out v)) target.AltUv = v;
            if (TryLong(inner, "ov", out v)) target.OvUv = v;
        }

        private static void ReadRange(string json, string key, out long min, out long max, out long step)
        {
            min = max = step = 0;
            Match block = Regex.Match(json, "\"" + Regex.Escape(key) + "\"\\s*:\\s*\\{([^}]*)\\}");
            if (!block.Success) return;
            TryLong(block.Groups[1].Value, "minimum", out min);
            TryLong(block.Groups[1].Value, "maximum", out max);
            TryLong(block.Groups[1].Value, "step", out step);
        }

        public static MVoltSnapshot QueryStatus(string explicitPath)
        {
            MVoltSnapshot snap = new MVoltSnapshot();

            string exe = Find(explicitPath);
            if (string.IsNullOrEmpty(exe)) { snap.Error = "未找到 mVolt+"; return snap; }
            if (!File.Exists(exe)) { snap.Error = "配置的 mVolt+ 路径不存在: " + exe; return snap; }

            string error;
            string output = Run(exe, "--status", 20000, out error);
            if (output == null) { snap.Error = error; return snap; }

            int open = output.IndexOf('{');
            int close = output.LastIndexOf('}');
            if (open < 0 || close <= open) { snap.Error = "mVolt+ 未返回有效状态"; return snap; }
            string json = output.Substring(open, close - open + 1);
            if (json.IndexOf("nvvdd_offsets_uv", StringComparison.Ordinal) < 0)
            {
                snap.Error = "mVolt+ 状态中缺少 nvvdd_offsets_uv";
                return snap;
            }

            Match gpu = Regex.Match(json, "\"gpu\"\\s*:\\s*\"([^\"]*)\"");
            if (gpu.Success) snap.Gpu = gpu.Groups[1].Value;
            Match vb = Regex.Match(json, "\"vbios\"\\s*:\\s*\"([^\"]*)\"");
            if (vb.Success) snap.Vbios = vb.Groups[1].Value;

            ReadRail(json, "nvvdd_offsets_uv", snap.Nvvdd);
            ReadRail(json, "msvdd_offsets_uv", snap.Msvdd);
            ReadRange(json, "nvvdd_device_range_uv", out snap.NvvddMinUv, out snap.NvvddMaxUv, out snap.NvvddStepUv);
            ReadRange(json, "msvdd_device_range_uv", out snap.MsvddMinUv, out snap.MsvddMaxUv, out snap.MsvddStepUv);

            Match demand = Regex.Match(json, "\"voltage_demand_mv\"\\s*:\\s*\\{([^}]*)\\}");
            if (demand.Success)
            {
                long v;
                string inner = demand.Groups[1].Value;
                if (TryLong(inner, "Core", out v)) snap.DemandCoreMv = v;
                if (TryLong(inner, "Xbar", out v)) snap.DemandXbarMv = v;
                if (TryLong(inner, "SYS", out v)) snap.DemandSysMv = v;
                if (TryLong(inner, "Video", out v)) snap.DemandVideoMv = v;
            }

            // The limits in force, as opposed to the device range parsed just above.
            long lim;
            if (TryLong(json, "nvvdd_min_mv", out lim)) snap.NvvddLimitMinMv = lim;
            if (TryLong(json, "nvvdd_max_mv", out lim)) snap.NvvddLimitMaxMv = lim;
            if (TryLong(json, "msvdd_min_mv", out lim)) snap.MsvddLimitMinMv = lim;
            if (TryLong(json, "msvdd_max_mv", out lim)) snap.MsvddLimitMaxMv = lim;
            // BoostLock 是布尔，TryLong 解不了，直接找字面量。
            if (json.IndexOf("\"boost_lock\":true", StringComparison.Ordinal) >= 0) snap.BoostLocked = true;

            long sysv;
            if (TryLong(json, "sys_offset_mhz", out sysv)) snap.SysOffsetMhz = sysv;
            if (TryLong(json, "sys_offset_mhz_limit_min", out sysv)) snap.SysLimitMinMhz = sysv;
            if (TryLong(json, "sys_offset_mhz_limit_max", out sysv)) snap.SysLimitMaxMhz = sysv;

            long mw;
            if (TryLong(json, "enforced_mw", out mw)) { snap.HasEnforced = true; snap.EnforcedMw = (uint)mw; }

            snap.Ok = true;
            return snap;
        }

        /// <summary>
        /// Applies rail offsets. Only rails with non-zero values are passed, which
        /// matches mVolt+'s own contract that unspecified controls are untouched.
        /// The result is read back and compared, so a true return means verified.
        /// </summary>
        public static bool Apply(string explicitPath, VoltageTuning tuning, out string error, out bool rounded)
        {
            error = null;
            rounded = false;

            string exe = Find(explicitPath);
            if (string.IsNullOrEmpty(exe)) { error = "未找到 mVolt+"; return false; }
            if (!File.Exists(exe)) { error = "配置的 mVolt+ 路径不存在: " + exe; return false; }

            /*
                Two different voltage controls, both via mVolt+:

                  --core-vdemand N          shifts the voltage the core ASKS FOR. This is
                                            the direct "change my core voltage" knob, and it
                                            is the one mVolt+ has enabled by default. Its
                                            allowed span is -25…+50 mV.

                  --nvvdd-offsets ...       shifts the driver's voltage LIMITS (VMIN / REL /
                                            ALT / OV). Verified on hardware: REL +25 moved the
                                            reported ceiling from 625/1025 to 625/1050 mV. It
                                            bounds what the rail may reach rather than
                                            requesting a voltage directly.

                The demand is what the UI drives; the limits remain supported for callers that
                set them.
            */
            List<string> args = new List<string>();
            bool useNvvdd = !tuning.Nvvdd.IsZero;
            bool useMsvdd = !tuning.Msvdd.IsZero;
            bool useDemand = tuning.DemandCoreMv != 0;

            if (useDemand) args.Add("--core-vdemand " + UvToMv(tuning.DemandCoreMv * 1000, ref rounded));
            if (useNvvdd) args.Add("--nvvdd-offsets " + RailArg(tuning.Nvvdd, ref rounded));
            if (useMsvdd) args.Add("--msvdd-offsets " + RailArg(tuning.Msvdd, ref rounded));
            if (args.Count == 0) { error = "没有需要下发的非零偏移"; return false; }

            string output = Run(exe, string.Join(" ", args), 30000, out error);
            if (output == null) return false;

            // Verify rather than trust the exit code.
            MVoltSnapshot check = QueryStatus(explicitPath);
            if (check.Ok)
            {
                if (useNvvdd && (check.Nvvdd.VminUv != tuning.Nvvdd.VminUv || check.Nvvdd.RelUv != tuning.Nvvdd.RelUv))
                {
                    error = "mVolt+ 未保留 NVDD 偏移（请求 rel=" + (tuning.Nvvdd.RelUv / 1000) +
                            " mV，回读 rel=" + (check.Nvvdd.RelUv / 1000) + " mV）";
                    return false;
                }
                if (useMsvdd && (check.Msvdd.VminUv != tuning.Msvdd.VminUv || check.Msvdd.RelUv != tuning.Msvdd.RelUv))
                {
                    error = "mVolt+ 未保留 MSVDD 偏移";
                    return false;
                }
                if (useDemand && check.DemandCoreMv != tuning.DemandCoreMv)
                {
                    error = "mVolt+ 未保留核心电压需求（请求 " + tuning.DemandCoreMv +
                            " mV，回读 " + check.DemandCoreMv + " mV）";
                    return false;
                }
            }
            return true;
        }

        private static string RailArg(RailOffsets r, ref bool rounded)
        {
            long vmin = UvToMv(r.VminUv, ref rounded);
            long rel = UvToMv(r.RelUv, ref rounded);
            long alt = UvToMv(r.AltUv, ref rounded);
            long ov = UvToMv(r.OvUv, ref rounded);
            return string.Format(CultureInfo.InvariantCulture, "{0},{1},{2},{3}", vmin, rel, alt, ov);
        }

        /// <summary>
        /// uV to mV, rounding to nearest and reporting that it happened: a stored
        /// value finer than 1 mV cannot be sent through this CLI, and silently
        /// applying a different number would make the readback check confusing.
        /// </summary>
        private static long UvToMv(long uv, ref bool rounded)
        {
            long sign = uv < 0 ? -1 : 1;
            long mag = uv < 0 ? -uv : uv;
            long mv = (mag + 500) / 1000;
            if (mv * 1000 != mag) rounded = true;
            return sign * mv;
        }

        /// <summary>
        /// Sets the SYS / host-interface clock offset through the companion tool.
        ///
        /// Separate from the other clock domains because it has no NVAPI path here: core and
        /// memory live in Pstates20 and XBAR goes through the ClockDomains transaction, but SYS is
        /// only reachable through mVolt+. Verified by readback rather than by exit code, for the
        /// same reason the voltage writes are.
        /// </summary>
        public static bool SetSysOffset(string explicitPath, long mhz, out string error)
        {
            error = null;
            string exe = Find(explicitPath);
            if (string.IsNullOrEmpty(exe)) { error = "未找到 mVolt+"; return false; }
            if (!File.Exists(exe)) { error = "配置的 mVolt+ 路径不存在: " + exe; return false; }

            if (Run(exe, "--sys-offset " + mhz.ToString(CultureInfo.InvariantCulture), 30000, out error) == null)
            {
                return false;
            }

            MVoltSnapshot check = QueryStatus(explicitPath);
            if (check.Ok && check.SysOffsetMhz != mhz)
            {
                error = "mVolt+ 未保留 SYS 偏移（请求 " + mhz + " MHz，回读 " + check.SysOffsetMhz + " MHz）";
                return false;
            }
            return true;
        }

        /// <summary>
        /// Toggles mVolt+'s Boost lock.
        ///
        /// BoostLock pins the boosted clock so the GPU stops ramping with load. It is a session
        /// setting on the companion tool's side — the CLI says "not saved in profiles" — so the
        /// state is read back rather than stored here: this program remembers nothing about it,
        /// and on the next start the button reflects whatever the tool reports.
        /// </summary>
        public static bool SetBoostLock(string explicitPath, bool on, out string error)
        {
            error = null;
            string exe = Find(explicitPath);
            if (string.IsNullOrEmpty(exe)) { error = "未找到 mVolt+"; return false; }
            if (!File.Exists(exe)) { error = "配置的 mVolt+ 路径不存在: " + exe; return false; }

            string output = Run(exe, "--boost-lock " + (on ? "on" : "off"), 30000, out error);
            if (output == null) return false;

            MVoltSnapshot check = QueryStatus(explicitPath);
            if (check.Ok && check.BoostLocked != on)
            {
                error = "mVolt+ 未保留 BoostLock 状态（请求" + (on ? "开启" : "关闭") +
                        "，回读" + (check.BoostLocked ? "开启" : "关闭") + "）";
                return false;
            }
            return true;
        }

        public static bool Reset(string explicitPath, out string error)
        {
            string exe = Find(explicitPath);
            if (string.IsNullOrEmpty(exe)) { error = "未找到 mVolt+"; return false; }
            if (!File.Exists(exe)) { error = "配置的 mVolt+ 路径不存在: " + exe; return false; }
            // Both controls are cleared: the demand is what the UI drives and the rail
            // limits are what callers may have set, and leaving either behind would show as
            // a voltage that will not return to stock.
            return Run(exe, "--core-vdemand 0 --nvvdd-offsets 0,0,0,0 --msvdd-offsets 0,0,0,0",
                       30000, out error) != null;
        }

        /// <summary>
        /// Proves the executable can be launched from this context. A background
        /// service and an interactive session are different environments, and
        /// finding out here gives a clearer message than a failed apply would.
        /// </summary>
        public static bool TestLaunch(string explicitPath, out string version, out string error)
        {
            version = null;
            string exe = Find(explicitPath);
            if (string.IsNullOrEmpty(exe)) { error = "未找到 mVolt+"; return false; }
            if (!File.Exists(exe)) { error = "配置的 mVolt+ 路径不存在: " + exe; return false; }

            string output = Run(exe, "--version", 15000, out error);
            if (output == null) return false;
            version = output.Trim();
            if (version.Length == 0) { error = "mVolt+ 未输出版本信息"; return false; }
            return true;
        }

        /// <summary>
        /// Opens the mVolt+ window.
        ///
        /// Voltage adjustment lives there rather than here, and the reason is
        /// recorded in IMPLEMENTATION_STATUS_1_9_0.md section 6.1: both candidate
        /// write interfaces are refused by this GPU, and percent_delta in
        /// particular returns NVAPI_NOT_SUPPORTED for every non-zero value. So the
        /// honest thing to offer is a way to get to the tool that can do it, not a
        /// set of fields that would silently fail.
        ///
        /// UseShellExecute with runas: mVolt+ is a tuning tool that needs
        /// administrator rights, and asking for them here means the user is not
        /// surprised by a second UAC prompt after the window appears.
        /// </summary>
        public static bool Launch(string explicitPath, out string error)
        {
            error = null;
            string exe = Find(explicitPath);
            if (string.IsNullOrEmpty(exe)) { error = "未找到 mVolt+。请在“设置”里指定它的路径。"; return false; }
            if (!File.Exists(exe)) { error = "配置的 mVolt+ 路径不存在: " + exe; return false; }

            try
            {
                ProcessStartInfo psi = new ProcessStartInfo
                {
                    FileName = exe,
                    WorkingDirectory = Path.GetDirectoryName(exe) ?? ".",
                    UseShellExecute = true,
                    Verb = "runas"      // the tool writes to the GPU; it needs elevation
                };
                Process.Start(psi);
                return true;
            }
            catch (Exception ex)
            {
                error = "启动 mVolt+ 失败: " + ex.Message;
                return false;
            }
        }
    }

    /// <summary>One prerequisite for loading the kernel helper, and its state.</summary>
    internal sealed class UnlockCheck
    {
        public string Name;
        public bool Ok;
        /// <summary>True when this item blocks the driver from loading.</summary>
        public bool Blocking;
        public string Detail;

        /// <summary>
        /// Identifier for the toggle this chip performs, or null when the setting cannot
        /// be changed from here (Secure Boot, which is firmware).
        /// </summary>
        public string ActionId;

        /// <summary>
        /// What clicking will do, phrased for the current state.
        ///
        /// Per-chip rather than a single "close/open" pair derived from Ok, because the chips do
        /// not all mean the same thing by Ok. For most of them Ok means the setting is already
        /// where the driver needs it, and clicking turns it off. For the CI policies Ok means the
        /// files have been cleared, and clicking puts them back.
        /// </summary>
        public string ClickAction;
        /// <summary>Why it cannot be toggled, shown when ActionId is null.</summary>
        public string ManualHint;
    }

    /// <summary>
    /// Reads the machine-level prerequisites for loading Nvpwr.sys.
    ///
    /// This exists because the single most confusing thing about this tool is the
    /// gap between "it installed" and "it works". On the reference machine the
    /// service is present, the certificate is trusted, the driver is signed Valid —
    /// and StartService still fails with 577, because Secure Boot is on and that
    /// locks the test-signing flag. Without a panel saying so, that looks like a bug
    /// in this program rather than a firmware setting.
    ///
    /// Every value is read from the registry; nothing is changed. A failed read is
    /// reported as unknown rather than as a problem, so a missing key on a different
    /// Windows build cannot produce a false alarm.
    /// </summary>
    internal static class UnlockDiagnostics
    {
        public static List<UnlockCheck> Gather()
        {
            List<UnlockCheck> list = new List<UnlockCheck>();

            // Secure Boot. Locking testsigning is the actual blocker on this machine:
            // bcdedit refuses to set it while this is on. Firmware, so not toggleable here.
            int? sb = ReadDword(@"SYSTEM\CurrentControlSet\Control\SecureBoot\State", "UEFISecureBootEnabled");
            list.Add(new UnlockCheck
            {
                Name = "Secure Boot",
                Ok = sb == 0,
                Blocking = sb == 1,
                Detail = sb == null ? "未知" : (sb == 1 ? "已开启" : "已关闭"),
                ActionId = null,
                ManualHint = "这个开关在主板固件里，程序无法修改。\n" +
                             "重启进入 BIOS → Security → Secure Boot → Disabled → 保存退出。\n" +
                             "关闭它之后“测试签名”才可能被打开，因为开着的 Secure Boot 会锁住该设置。"
            });

            /*
                EfiGuard, next to Secure Boot because it is the other item with nothing this
                program can do about it.

                It is deliberately not clickable. Everything about it was tried:

                  Copying the loader in is possible, and that part is automated by being absent —
                  the package ships the files and the notes say where they go.

                  Removing the loader does disable it: the firmware tries the entry, fails, and
                  boots Windows instead. But the firmware also drops the entry when that happens,
                  and putting the file back does not put the entry back. A control that only
                  works one way is worse than no control.

                  Reordering with bcdedit reports success and changes what bcdedit displays, and
                  the firmware ignores it, because the order it boots from is its own. Writing the
                  UEFI variables directly is refused with 1314 even with
                  SeSystemEnvironmentPrivilege enabled.

                So this is a status light with an explanation attached. The tooltip carries the
                detail and the step the user has to take.
            */
            EfiGuardSetup.Status efi = EfiGuardSetup.Detect();
            list.Add(new UnlockCheck
            {
                Name = "EfiGuard",
                Ok = efi.ActiveThisBoot,
                Blocking = !efi.ActiveThisBoot,
                Detail = EfiGuardSetup.Describe(efi),
                ActionId = null,
                ManualHint = EfiGuardSetup.Hint(efi)
            });

            /*
                Virtualisation-based security, which is the blocker people miss.

                The Windows Security page shows Memory Integrity, and that can read as off while
                VBS itself is still running — Memory Integrity is only one consumer of it. On the
                reference machine the page showed 内存完整性: 关 while
                EnableVirtualizationBasedSecurity was 1 and the hypervisor was live.

                It has to be off. The hypervisor protects kernel memory, and every route to
                loading this driver ends in a kernel-memory write: the companion tool patches the
                signature-enforcement variable, and a test-signed driver cannot be loaded at all
                while code integrity is being enforced.

                The page has no switch for this, so both values are cleared and the hypervisor is
                told not to start. The registry read cannot see a policy-driven VBS, so the detail
                line says what was read rather than asserting the machine is clean.
            */
            int? vbs = ReadDword(@"SYSTEM\CurrentControlSet\Control\DeviceGuard", "EnableVirtualizationBasedSecurity");
            int? hvci = ReadDword(@"SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity", "Enabled");
            bool vbsOn = vbs == 1 || hvci == 1;

            // Clickable only while VBS is running.
            //
            // With it running, EfiGuard cannot be in effect — the two are mutually exclusive,
            // measured over every boot on the reference machine — so switching it off is the one
            // action here that leads anywhere, and it is the step that lets the driver load at all.
            //
            // With it already off there is nothing to offer. Turning it back on would last until
            // the next boot, because a boot through EfiGuard is what makes Windows disable it:
            // observed on every EfiGuard boot, including three where the registry was set to 1
            // immediately beforehand and came back as 0. A control that writes a value its own
            // other component undoes is not a control.
            bool vbsLocked = FirmwareVbsLocked();

            list.Add(new UnlockCheck
            {
                Name = "虚拟化安全",
                Ok = !vbsOn,
                Blocking = vbsOn,
                Detail = vbsOn
                    ? "已开启"
                    : (vbs == null && hvci == null ? "未知" : "已关闭"),
                ActionId = vbsOn ? "vbs" : null,
                ClickAction = vbsOn ? "关闭虚拟化安全" : "",
                ManualHint = "这一项在 Windows 安全中心里没有开关（内存完整性只是它的一个消费者，关了它 VBS 仍可能运行）。\n" +
                             (vbsOn
                                 ? "点击会写入 EnableVirtualizationBasedSecurity=0 并执行 bcdedit /set hypervisorlaunchtype off，重启后生效。\n" +
                                   "注意：hypervisorlaunchtype off 会影响 WSL2、Windows 沙盒和部分虚拟化软件。\n" +
                                   "这也是 EfiGuard 能生效的前提 —— VBS 运行时它的内核修补会被安全内核拦下。"
                                 : "已关闭，无需操作。\n\n" +
                                   "为什么不能开回来：从 EfiGuard 引导时，它修补的启动链会让 Windows 判定\n" +
                                   "启动链验证失败，于是禁用 VBS 并把注册表重置为 0。\n" +
                                   "实测：每一次 EfiGuard 生效的启动，VBS 都被禁用；手动写的 =1 会在重启后变回 0。\n" +
                                   "要恢复 VBS，只能在不经过 EfiGuard 的情况下启动。") +
                             (vbsLocked && vbsOn
                                 ? "\n\n⚠ 上次启动时 Windows 报告 VBS 是被固件的退出标志关掉的。"
                                 : "")
            });

            // Vulnerable driver blocklist. Required off for this program because the driver is
            // loaded through a known-vulnerable signed driver rather than by its own signature;
            // the blocklist exists precisely to stop that class of driver from running.
            int? block = ReadDword(@"SYSTEM\CurrentControlSet\Control\CI\Config", "VulnerableDriverBlocklistEnable");
            list.Add(new UnlockCheck
            {
                Name = "驱动阻止列表",
                Ok = block == 0,
                Blocking = block == 1,
                Detail = block == null ? "未知" : (block == 1 ? "已开启" : "已关闭"),
                ActionId = "blocklist",
                ClickAction = block == 1 ? "关闭驱动阻止列表" : "开启驱动阻止列表",
                ManualHint = "位置：Windows 安全中心 → 设备安全性 → 内核隔离 → Microsoft 易受攻击的驱动程序阻止列表。"
            });

            /*
                WDAC code-integrity policies that survive the registry switch.

                Turning off VulnerableDriverBlocklistEnable is not sufficient on its own. The same
                blocklist is also installed as signed CI policy files, and a signed policy is
                enforced independently of that value. The reference tooling deletes these files as
                a separate step, from both the EFI system partition and the Windows directory,
                and warns that on recent Windows the driver will otherwise fail to load or take
                the machine down.

                {8F9CB695-...} is the driver blocklist policy; {784C4414-...} is the cross-certificate
                exceptions policy. Only the Windows-side copies are listed here — the EFI-side ones
                are checked at delete time, since reading the ESP needs it mounted.
            */
            string[] policyIds =
            {
                "{784C4414-79F4-4C32-A6A5-F0FB42A51D0D}",
                "{8F9CB695-5D48-48D6-A329-7202B44607E3}"
            };
            List<string> present = new List<string>();
            string policyDir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.Windows),
                "System32", "CodeIntegrity", "CiPolicies", "Active");
            foreach (string id in policyIds)
            {
                if (File.Exists(Path.Combine(policyDir, id + ".cip")))
                {
                    present.Add(id);
                }
            }
            list.Add(new UnlockCheck
            {
                Name = "CI 策略",
                Ok = present.Count == 0,
                Blocking = present.Count > 0,
                Detail = present.Count == 0 ? "已清除" : "未清除",
                ActionId = "cipolicy",
                ClickAction = present.Count == 0 ? "恢复 CI 策略文件" : "清除 CI 策略文件",
                ManualHint = "已签名的 WDAC 策略独立于注册表开关生效。清除会先备份到程序数据目录，可还原。\n" +
                             "这些文件受保护，操作需要管理员权限；Secure Boot 必须已关闭，否则策略会被重新应用。"
            });

            return list;
        }

        private static int? ReadDword(string subkey, string name)
        {
            try
            {
                using (Microsoft.Win32.RegistryKey k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(subkey, false))
                {
                    if (k == null) return null;
                    object v = k.GetValue(name);
                    if (v == null) return null;
                    return Convert.ToInt32(v);
                }
            }
            catch { return null; }
        }

        /// <summary>
        /// Applies one prerequisite change.
        ///
        /// Only the three items that live in Windows are touched; Secure Boot is firmware
        /// and is reported instead. Every one of these needs a reboot to take effect, so
        /// the caller is told so rather than left wondering why nothing changed.
        ///
        /// The registry values are written under HKLM and therefore require the elevated
        /// session the app already runs in.
        /// </summary>
        /// <summary>
        /// Where the pre-change values of the toggleable prerequisites are kept.
        ///
        /// These chips toggle, so switching one back has to restore what was actually there
        /// rather than a value guessed at the time. HVCI forces the issue: it is on for some
        /// machines and off for others, and once written nothing in the registry says which it
        /// was. Turning it back on for someone who never had it on is a change they did not ask
        /// for.
        /// </summary>
        private static string PrereqRecordPath
        {
            get { return System.IO.Path.Combine(Store.Dir, "prereq-original.ini"); }
        }

        /// <summary>
        /// Records a value the first time a setting is changed, and never again.
        ///
        /// Write-once is the point. A second toggle would otherwise record the value this
        /// program itself wrote, and the real original would be lost — the same way the factory
        /// power wall was lost before it was made to persist.
        /// </summary>
        private static void Remember(string key, string value)
        {
            try
            {
                string path = PrereqRecordPath;
                List<string> lines = File.Exists(path)
                    ? new List<string>(File.ReadAllLines(path, Encoding.UTF8))
                    : new List<string>();
                string prefix = key + "=";
                foreach (string line in lines)
                {
                    if (line.StartsWith(prefix, StringComparison.Ordinal)) return;
                }
                lines.Add(prefix + value);
                File.WriteAllLines(path, lines, new UTF8Encoding(false));
            }
            catch
            {
                // Best effort. A missing record only means the fallback default is used.
            }
        }

        /// <summary>Reads a recorded original, or null when nothing was recorded.</summary>
        private static string Recall(string key)
        {
            try
            {
                string path = PrereqRecordPath;
                if (!File.Exists(path)) return null;
                string prefix = key + "=";
                foreach (string line in File.ReadAllLines(path, Encoding.UTF8))
                {
                    if (line.StartsWith(prefix, StringComparison.Ordinal))
                    {
                        return line.Substring(prefix.Length);
                    }
                }
            }
            catch { }
            return null;
        }

        private static int RecallInt(string key, int fallback)
        {
            string v = Recall(key);
            int parsed;
            return (v != null && int.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out parsed))
                ? parsed : fallback;
        }

        /// <summary>
        /// Reads the current hypervisor launch type from the boot configuration.
        ///
        /// Needed for the same reason as the registry values: turning VBS back on has to restore
        /// the launch type that was in effect, and "auto" is only right if that is what it was.
        /// </summary>
        private static string ReadHypervisorLaunchType()
        {
            string output;
            if (RunBcd("/enum {current}", out output) != 0 || output == null) return null;
            foreach (string raw in output.Split('\n'))
            {
                string line = raw.Trim();
                if (!line.StartsWith("hypervisorlaunchtype", StringComparison.OrdinalIgnoreCase)) continue;
                string[] parts = line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length >= 2) return parts[1];
            }
            return null;
        }
        /// <summary>-1 unknown, 0 no, 1 yes. Resolved once; it cannot change while running.</summary>
        private static int _firmwareVbsLocked = -1;

        /// <summary>
        /// True when the firmware, rather than Windows, is what keeps VBS off.
        ///
        /// Windows records why VBS was or was not started in Kernel-Boot event 153 at every
        /// boot. Reason 2 is the firmware opt-out variable, and when that is the reason the
        /// registry value is forced back to zero on each boot no matter what is written.
        ///
        /// Measured on the reference machine: a click wrote EnableVirtualizationBasedSecurity = 1
        /// and reported success, the next boot reset it to 0, and the event for that boot carried
        /// reason 2. Without this check the 虚拟化安全 chip is a control that silently undoes
        /// itself — press it, reboot, and it is green again with nothing to explain why.
        ///
        /// The reason cannot be read any other way. The UEFI variable holding the opt-out is
        /// refused from user mode with 1314, reads included.
        /// </summary>
        private static bool FirmwareVbsLocked()
        {
            if (_firmwareVbsLocked >= 0) return _firmwareVbsLocked == 1;

            bool locked = false;
            try
            {
                const string selector =
                    "*[System[Provider[@Name='Microsoft-Windows-Kernel-Boot'] and (EventID=153)]]";
                EventLogQuery query = new EventLogQuery("System", PathType.LogName, selector);
                query.TolerateQueryErrors = true;
                using (EventLogReader reader = new EventLogReader(query))
                {
                    // ReadEvent walks forwards from wherever the reader sits, so seeking to the
                    // end first gives the most recent boot rather than the oldest on record.
                    reader.Seek(SeekOrigin.End, 0);
                    EventRecord record = reader.ReadEvent();
                    if (record != null)
                    {
                        Match m = Regex.Match(record.ToXml(), "EnableDisableReason[^>]*>(\\d+)<");
                        if (m.Success) locked = m.Groups[1].Value == "2";
                    }
                }
            }
            catch
            {
                // An unreadable log leaves the chip behaving normally, which is the safe
                // direction: it stays clickable and the user decides.
            }

            _firmwareVbsLocked = locked ? 1 : 0;
            Store.Log("VBS 固件锁定检测: " + (locked ? "是（固件禁用了 VBS，注册表改动重启后被重置）" : "否"));
            return locked;
        }


        public static bool Toggle(string actionId, out string message)
        {
            message = null;
            try
            {
                switch (actionId)
                {
                    case "vbs":
                    {
                        /*
                            A real toggle, in both directions.

                            It used to write zeros unconditionally, so the chip could turn VBS off
                            and then never turn it back on — pressing it again just wrote the same
                            zeros and reported success, which reads as "nothing happened".

                            Both values and the hypervisor start type move together, because
                            clearing only the registry leaves the hypervisor running and the driver
                            still blocked. Memory Integrity moves too: it cannot run without VBS,
                            so leaving it set would only turn VBS back on.
                        */
                        string output;
                        bool currentlyOff =
                            ReadDword(@"SYSTEM\CurrentControlSet\Control\DeviceGuard",
                                      "EnableVirtualizationBasedSecurity") == 0;

                        if (!currentlyOff)
                        {
                            // Record before changing, so a later toggle restores what was there.
                            Remember("vbs_enabled",
                                (ReadDword(@"SYSTEM\CurrentControlSet\Control\DeviceGuard",
                                           "EnableVirtualizationBasedSecurity") ?? 1)
                                .ToString(CultureInfo.InvariantCulture));
                            Remember("hvci_enabled",
                                (ReadDword(@"SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity",
                                           "Enabled") ?? 1)
                                .ToString(CultureInfo.InvariantCulture));
                            string launch = ReadHypervisorLaunchType();
                            if (launch != null) Remember("hypervisorlaunch", launch);
                        }

                        using (Microsoft.Win32.RegistryKey dg = Microsoft.Win32.Registry.LocalMachine.CreateSubKey(
                            @"SYSTEM\CurrentControlSet\Control\DeviceGuard"))
                        {
                            if (dg == null) { message = "无法打开注册表项 DeviceGuard。"; return false; }
                            dg.SetValue("EnableVirtualizationBasedSecurity", currentlyOff ? 1 : 0,
                                        Microsoft.Win32.RegistryValueKind.DWord);
                        }
                        using (Microsoft.Win32.RegistryKey hi = Microsoft.Win32.Registry.LocalMachine.CreateSubKey(
                            @"SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity"))
                        {
                            if (hi == null) { message = "无法打开注册表项 HypervisorEnforcedCodeIntegrity。"; return false; }
                            hi.SetValue("Enabled", currentlyOff ? RecallInt("hvci_enabled", 1) : 0,
                                        Microsoft.Win32.RegistryValueKind.DWord);
                        }

                        // Where nothing was recorded, "auto" is the Windows default and the value
                        // the reference instructions tell people to restore by hand.
                        string wantLaunch = currentlyOff ? (Recall("hypervisorlaunch") ?? "auto") : "off";
                        int rc = RunBcd("/set hypervisorlaunchtype " + wantLaunch, out output);
                        if (rc != 0)
                        {
                            message = "注册表值已写入，但 bcdedit /set hypervisorlaunchtype " + wantLaunch +
                                      " 失败（退出码 " + rc + "）。\n\n" + (output ?? "") +
                                      "\n\n只写注册表通常不足以让 hypervisor 停止启动。";
                            return false;
                        }

                        if (currentlyOff)
                        {
                            message = "虚拟化安全已恢复开启（注册表 + hypervisorlaunchtype " + wantLaunch +
                                      "）。需要重启才会生效。";
                        }
                        else
                        {
                            message = "虚拟化安全已关闭（注册表 + hypervisorlaunchtype off）。需要重启才会生效。\n\n" +
                                      "影响：WSL2、Windows 沙盒、部分虚拟化软件在重启后不可用。\n" +
                                      "再点一次这一项可以改回去。";
                        }
                        return true;
                    }

                    case "blocklist":
                    {
                        bool on = ReadDword(@"SYSTEM\CurrentControlSet\Control\CI\Config", "VulnerableDriverBlocklistEnable") == 1;
                        using (Microsoft.Win32.RegistryKey k = Microsoft.Win32.Registry.LocalMachine.CreateSubKey(
                            @"SYSTEM\CurrentControlSet\Control\CI\Config"))
                        {
                            if (k == null) { message = "无法打开注册表项。"; return false; }
                            k.SetValue("VulnerableDriverBlocklistEnable", on ? 0 : 1, Microsoft.Win32.RegistryValueKind.DWord);
                        }
                        message = on
                            ? "驱动黑名单已关闭。需要重启才会生效。"
                            : "驱动黑名单已开启。需要重启才会生效。";
                        return true;
                    }

                    case "cipolicy":
                        return ToggleCodeIntegrityPolicies(out message);

                    default:
                        message = "这一项无法由程序修改。";
                        return false;
                }
            }
            catch (Exception ex)
            {
                message = "修改失败：" + ex.Message;
                return false;
            }
        }

        /// <summary>
        /// Clears the policies when they are present, restores them when they are not.
        ///
        /// A toggle rather than a one-way action. Clearing used to be all this could do, so the
        /// chip read as a dead control once the files were gone: pressing it again found nothing
        /// to remove and reported that, which looks like a failure and leaves no way back short
        /// of copying files by hand.
        ///
        /// Which direction to go is decided from the files themselves, not from the chip's
        /// displayed state, because the two can disagree — the chip is refreshed on a timer, and
        /// the files can be put back by a Windows update in between.
        /// </summary>
        private static bool ToggleCodeIntegrityPolicies(out string message)
        {
            string[] ids =
            {
                "{784C4414-79F4-4C32-A6A5-F0FB42A51D0D}",
                "{8F9CB695-5D48-48D6-A329-7202B44607E3}"
            };
            string dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.Windows),
                "System32", "CodeIntegrity", "CiPolicies", "Active");
            string backup = Path.Combine(Store.Dir, "CiPolicyBackup");

            bool anyPresent = false;
            foreach (string id in ids)
            {
                if (File.Exists(Path.Combine(dir, id + ".cip"))) { anyPresent = true; break; }
            }

            if (anyPresent)
            {
                return ClearCodeIntegrityPolicies(out message);
            }
            return RestoreCodeIntegrityPolicies(ids, dir, backup, out message);
        }

        /// <summary>
        /// Puts the backed-up policies back.
        ///
        /// Restoring matters beyond convenience: these files are what the blocklist switch is
        /// backed up by, so a machine that has had them removed is running with one layer of
        /// protection gone. Someone who is done with this tool should be able to put the machine
        /// back the way they found it without hunting for the files.
        /// </summary>
        private static bool RestoreCodeIntegrityPolicies(string[] ids, string dir, string backup, out string message)
        {
            message = null;
            List<string> restored = new List<string>();
            List<string> missing = new List<string>();
            List<string> failed = new List<string>();

            foreach (string id in ids)
            {
                string src = Path.Combine(backup, id + ".cip");
                string dst = Path.Combine(dir, id + ".cip");
                if (!File.Exists(src)) { missing.Add(id); continue; }
                try
                {
                    File.Copy(src, dst, true);
                    restored.Add(id);
                }
                catch (Exception ex)
                {
                    failed.Add(id + "：" + ex.Message);
                }
            }

            if (restored.Count == 0 && missing.Count == ids.Length)
            {
                message = "没有找到可恢复的备份，也没有策略文件存在。\n\n备份目录：" + backup;
                return false;
            }
            if (failed.Count > 0)
            {
                message = "部分策略文件恢复失败：\n\n" + string.Join("\n", failed);
                return false;
            }
            message = "已恢复 " + restored.Count + " 个 CI 策略文件，来自：\n" + backup +
                      "\n\n需要重启才会生效。";
            if (missing.Count > 0)
            {
                message += "\n\n另有 " + missing.Count + " 个没有备份，无法恢复（本机原本就没有）。";
            }
            return true;
        }

        /// <summary>
        /// Removes the signed WDAC code-integrity policies that duplicate the blocklist switch.
        ///
        /// They are deleted rather than disabled because there is no supported way to disable
        /// them: the policy is applied by the boot manager, and the only lever is whether the
        /// file is present. That is also why Secure Boot has to be off — with it on the policy
        /// is re-applied from the firmware side and the deletion does not stick.
        ///
        /// Everything removed is copied to the program data directory first, so this is
        /// reversible even though the originals live in protected locations. The files are owned
        /// by TrustedInstaller, so ownership is taken before deleting — the same dance the
        /// reference tooling does.
        ///
        /// The EFI partition is not touched. Reading it requires mounting it, and the observed
        /// machines keep these policies in the Windows directory; the app reports what it finds
        /// rather than mounting system partitions behind the user's back.
        /// </summary>
        private static bool ClearCodeIntegrityPolicies(out string message)
        {
            message = null;
            string[] ids =
            {
                "{784C4414-79F4-4C32-A6A5-F0FB42A51D0D}",
                "{8F9CB695-5D48-48D6-A329-7202B44607E3}"
            };
            string dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.Windows),
                "System32", "CodeIntegrity", "CiPolicies", "Active");
            string backup = Path.Combine(Store.Dir, "CiPolicyBackup");

            List<string> removed = new List<string>();
            List<string> failed = new List<string>();
            try
            {
                Directory.CreateDirectory(backup);
            }
            catch (Exception ex)
            {
                message = "无法创建备份目录：" + backup + "\n\n" + ex.Message;
                return false;
            }

            foreach (string id in ids)
            {
                string file = Path.Combine(dir, id + ".cip");
                if (!File.Exists(file))
                {
                    continue;
                }
                try
                {
                    File.Copy(file, Path.Combine(backup, id + ".cip"), true);

                    // takeown then icacls, because the file is owned by TrustedInstaller and a
                    // plain delete fails with access denied.
                    RunTool("takeown.exe", "/f \"" + file + "\" /a");
                    RunTool("icacls.exe", "\"" + file + "\" /grant Administrators:F /c /q");

                    FileAttributes attrs = File.GetAttributes(file);
                    if ((attrs & FileAttributes.ReadOnly) != 0)
                    {
                        File.SetAttributes(file, attrs & ~FileAttributes.ReadOnly);
                    }
                    File.Delete(file);
                    removed.Add(id);
                }
                catch (Exception ex)
                {
                    failed.Add(id + "：" + ex.Message);
                }
            }

            if (removed.Count == 0 && failed.Count == 0)
            {
                message = "没有找到需要清除的 CI 策略文件。";
                return true;
            }
            if (failed.Count > 0)
            {
                message = "部分策略文件删除失败：\n\n" + string.Join("\n", failed) +
                          "\n\n备份在：" + backup;
                return false;
            }
            message = "已清除 " + removed.Count + " 个 CI 策略文件，备份在：\n" + backup +
                      "\n\n需要重启才会生效。\n" +
                      "还原方式：把备份目录里的文件复制回\n" + dir;
            return true;
        }

        /// <summary>Runs a console tool and ignores everything but the exit code.</summary>
        private static void RunTool(string exe, string arguments)
        {
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo
                {
                    FileName = exe,
                    Arguments = arguments,
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true
                };
                using (Process p = Process.Start(psi))
                {
                    if (p == null) return;
                    p.StandardOutput.ReadToEnd();
                    p.StandardError.ReadToEnd();
                    p.WaitForExit(20000);
                }
            }
            catch
            {
                // Best effort: if the tool is missing the delete below reports the real error.
            }
        }

        private static int RunBcd(string arguments, out string output)
        {
            output = null;
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo
                {
                    FileName = "bcdedit",
                    Arguments = arguments,
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true
                };
                using (Process p = Process.Start(psi))
                {
                    string so = p.StandardOutput.ReadToEnd();
                    string se = p.StandardError.ReadToEnd();
                    p.WaitForExit(8000);
                    output = (so + se).Trim();
                    return p.HasExited ? p.ExitCode : -1;
                }
            }
            catch (Exception ex)
            {
                output = ex.Message;
                return -1;
            }
        }
    }
}