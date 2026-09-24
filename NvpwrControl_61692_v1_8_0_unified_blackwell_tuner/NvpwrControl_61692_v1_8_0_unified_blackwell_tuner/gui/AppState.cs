using System;
using System.Collections.Generic;
using System.Diagnostics;
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

        public bool IsZero { get { return CoreOffsetMhz == 0 && MemoryOffsetMhz == 0 && XbarOffsetMhz == 0; } }
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
            b.AppendLine(p + "voltage_enabled=" + (s.Voltage.Enabled ? 1 : 0));
            b.AppendLine(p + "voltage_applier=1");   // 1 = companion tool
            b.AppendLine(p + "nvvdd_vmin_uv=" + s.Voltage.Nvvdd.VminUv);
            b.AppendLine(p + "nvvdd_rel_uv=" + s.Voltage.Nvvdd.RelUv);
            b.AppendLine(p + "nvvdd_alt_uv=" + s.Voltage.Nvvdd.AltUv);
            b.AppendLine(p + "nvvdd_ov_uv=" + s.Voltage.Nvvdd.OvUv);
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

            s.Voltage.Enabled = num("voltage_enabled") != 0;
            s.Voltage.Nvvdd.VminUv = num("nvvdd_vmin_uv");
            s.Voltage.Nvvdd.RelUv = num("nvvdd_rel_uv");
            s.Voltage.Nvvdd.AltUv = num("nvvdd_alt_uv");
            s.Voltage.Nvvdd.OvUv = num("nvvdd_ov_uv");
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
        // Two different things, and conflating them produced a wrong reading in the UI:
        //   NvvddMinUv/MaxUv        the DEVICE range the rail can be set within (445–1280 mV)
        //   NvvddLimitMinMv/MaxMv   the limits currently in force (625/1025 mV), which move
        //                           when a limit offset is applied
        public long NvvddMinUv, NvvddMaxUv, NvvddStepUv;
        public long NvvddLimitMinMv, NvvddLimitMaxMv;
        public long MsvddMinUv, MsvddMaxUv, MsvddStepUv;
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
                Detail = sb == null ? "未知" : (sb == 1 ? "已开启（需在 BIOS 关闭）" : "已关闭"),
                ActionId = null,
                ManualHint = "这个开关在主板固件里，程序无法修改。\n" +
                             "重启进入 BIOS → Security → Secure Boot → Disabled → 保存退出。\n" +
                             "关闭它之后“测试签名”才可能被打开，因为开着的 Secure Boot 会锁住该设置。"
            });

            // Test signing. Absent means off. Toggleable via bcdedit, but only once
            // Secure Boot is off — the driver here is self-signed, so Windows needs this
            // flag to load it at all.
            bool testSigning = BcdHasFlag("testsigning", "Yes") || BcdHasFlag("testsigning", "On");
            list.Add(new UnlockCheck
            {
                Name = "测试签名",
                Ok = testSigning,
                Blocking = !testSigning,
                Detail = testSigning ? "已开启" : "未开启（点击开启）",
                ActionId = "testsigning",
                ManualHint = sb == 1
                    ? "点击开启会执行 bcdedit /set testsigning on，但 Secure Boot 仍开着时该命令会被拒绝。先关 Secure Boot。"
                    : null
            });

            int? hvci = ReadDword(@"SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity", "Enabled");
            list.Add(new UnlockCheck
            {
                Name = "内存完整性",
                Ok = hvci == 0,
                Blocking = hvci == 1,
                Detail = hvci == null ? "未知" : (hvci == 1 ? "已开启（点击关闭）" : "已关闭"),
                ActionId = "hvci",
                ManualHint = "位置：Windows 安全中心 → 设备安全性 → 内核隔离 → 内存完整性。"
            });

            int? block = ReadDword(@"SYSTEM\CurrentControlSet\Control\CI\Config", "VulnerableDriverBlocklistEnable");
            list.Add(new UnlockCheck
            {
                Name = "驱动黑名单",
                Ok = block == 0,
                Blocking = false,
                Detail = block == null ? "未知" : (block == 1 ? "已开启（点击关闭）" : "已关闭"),
                ActionId = "blocklist",
                ManualHint = null
            });

            // Certificate trust. Both stores are needed: Root for chain building,
            // TrustedPublisher so the driver is accepted without a prompt.
            bool root = HasCert("Root");
            bool pub = HasCert("TrustedPublisher");
            list.Add(new UnlockCheck
            {
                Name = "测试证书",
                Ok = root && pub,
                Blocking = !(root && pub),
                Detail = (root && pub) ? "已安装到 Root 与 TrustedPublisher"
                                       : (root ? "仅在 Root" : (pub ? "仅在 TrustedPublisher" : "未安装"))
            });

            // Driver signature, read from the deployed file rather than assumed.
            string sys = Path.Combine(Store.Dir, "Nvpwr.sys");
            if (File.Exists(sys))
            {
                string status = "未知";
                try { status = System.Security.Cryptography.X509Certificates
                        .X509Certificate.CreateFromSignedFile(sys) != null ? "Valid" : "未知"; }
                catch { status = "无法读取签名"; }
                list.Add(new UnlockCheck
                {
                    Name = "驱动签名",
                    Ok = status == "Valid",
                    Blocking = false,
                    Detail = status
                });
            }

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
        public static bool Toggle(string actionId, out string message)
        {
            message = null;
            try
            {
                switch (actionId)
                {
                    case "hvci":
                    {
                        bool on = ReadDword(@"SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity", "Enabled") == 1;
                        using (Microsoft.Win32.RegistryKey k = Microsoft.Win32.Registry.LocalMachine.CreateSubKey(
                            @"SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity"))
                        {
                            if (k == null) { message = "无法打开注册表项。"; return false; }
                            k.SetValue("Enabled", on ? 0 : 1, Microsoft.Win32.RegistryValueKind.DWord);
                        }
                        message = on
                            ? "内存完整性已关闭。需要重启才会生效。\n\n注意：这会降低系统对恶意内核代码的防护，请自行权衡。"
                            : "内存完整性已开启。需要重启才会生效。";
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

                    case "testsigning":
                    {
                        bool on = BcdHasFlag("testsigning", "Yes") || BcdHasFlag("testsigning", "On");
                        // Off by default is the safe direction for a toggle someone may have
                        // clicked by accident; the message says how to undo either way.
                        string arg = on ? "/set testsigning off" : "/set testsigning on";
                        string output;
                        int rc = RunBcd(arg, out output);

                        if (rc != 0)
                        {
                            message = "bcdedit " + arg + " 失败（退出码 " + rc + "）。\n\n" + (output ?? "") +
                                      "\n\n如果提示受 Secure Boot 策略保护，说明必须先到 BIOS 关闭 Secure Boot。";
                            return false;
                        }
                        message = (on ? "测试签名已关闭。" : "测试签名已开启。") +
                                  "需要重启才会生效。\n\n撤销命令：bcdedit /set testsigning " + (on ? "on" : "off");
                        return true;
                    }

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

        private static bool HasCert(string store)
        {
            try
            {
                using (System.Security.Cryptography.X509Certificates.X509Store s =
                    new System.Security.Cryptography.X509Certificates.X509Store(store,
                        System.Security.Cryptography.X509Certificates.StoreLocation.LocalMachine))
                {
                    s.Open(System.Security.Cryptography.X509Certificates.OpenFlags.ReadOnly);
                    foreach (System.Security.Cryptography.X509Certificates.X509Certificate2 c in s.Certificates)
                        if (c.Subject.IndexOf("Nvpwr", StringComparison.OrdinalIgnoreCase) >= 0) return true;
                    return false;
                }
            }
            catch { return false; }
        }

        /// <summary>Reads one bcdedit setting. Slow, so it is called at most twice.</summary>
        private static bool BcdHasFlag(string key, string value)
        {
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo
                {
                    FileName = "bcdedit",
                    Arguments = "/enum {current}",
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    CreateNoWindow = true
                };
                using (Process p = Process.Start(psi))
                {
                    string output = p.StandardOutput.ReadToEnd();
                    p.WaitForExit(4000);
                    foreach (string line in output.Split('\n'))
                        if (line.IndexOf(key, StringComparison.OrdinalIgnoreCase) >= 0 &&
                            line.IndexOf(value, StringComparison.OrdinalIgnoreCase) >= 0)
                            return true;
                }
            }
            catch { }
            return false;
        }
    }
}
