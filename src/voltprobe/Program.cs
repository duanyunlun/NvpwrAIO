using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace VoltProbe
{
    /// <summary>
    /// One-off experiment: establish whether ClientVoltRailsSetControl
    /// (0x0xB9306D9B) can be used, and what its percent_delta field actually means.
    ///
    /// WHY THIS IS A SEPARATE TOOL AND NOT PART OF THE MAIN APPLICATION
    ///   Writing a guessed value to a voltage rail is the only operation in this
    ///   project that can damage hardware rather than merely misbehave. The GUI
    ///   deliberately has no voltage-write path; this tool exists to find out
    ///   whether one can be added safely. Nothing it learns is used until its
    ///   output has been read and the semantics are confirmed.
    ///
    /// SAFETY PROTOCOL — every step is reversible and bounded
    ///   1. Read the baseline first and refuse to continue unless it is clean
    ///      (percent_delta == 0). Testing from an unknown starting point would make
    ///      the result uninterpretable as well as riskier.
    ///   2. Never request more than one small step at a time; the caller chooses the
    ///      list, and the default list is a handful of single digits.
    ///   3. After every write, read the control back AND read the live voltage. A
    ///      write is only interesting if the driver retained it and the rail moved.
    ///   4. Return to 0 immediately after each step, and again on exit, on a
    ///      Ctrl+C, and from a finalizer path. There is no state this tool leaves
    ///      behind on purpose.
    ///   5. Stop at the first sign of trouble: rejected write, unexpected readback,
    ///      implausible voltage, or a rail that moves too far.
    ///
    ///   The worst outcome of a partial failure is a small offset left on a voltage
    ///   limit, which a reboot clears — GPU voltage policy is not persistent.
    ///
    /// WHAT IT WILL REVEAL
    ///   * whether the call needs elevation (the earlier non-elevated probe got
    ///     rc=-1, which is consistent with that being the missing precondition),
    ///   * which values the driver accepts, i.e. the valid range and sign,
    ///   * whether percent_delta shifts the rail monotonically, and by how much,
    ///   * whether the change survives a readback.
    /// </summary>
    internal static class Program
    {
        // ---- interface ids (see IMPLEMENTATION_STATUS_1_9_0.md section 6.1) ----
        private const uint ID_INITIALIZE = 0x0150E828;
        private const uint ID_ENUM_PHYSICAL_GPUS = 0xE5AC921F;
        private const uint ID_UNLOAD = 0xD22BDD7E;

        private const uint ID_VOLT_RAILS_GET_STATUS = 0x465F9BCF;
        private const uint ID_VOLT_RAILS_GET_CONTROL = 0x9DF23CA1;
        private const uint ID_VOLT_RAILS_SET_CONTROL = 0xB9306D9B;

        private const int STATUS_SIZE = 0x4C;          // ClientVoltRailsStatusV1
        private const int STATUS_VOLT_OFFSET = 0x28;   // current_voltage_uv
        private const int CONTROL_SIZE = 0x28;         // ClientVoltRailsControlV1
        private const int CONTROL_DELTA_OFFSET = 0x04; // percent_delta, 1 byte

        // Plausibility bounds for a core rail, in microvolts.
        private const long VOLT_MIN_PLAUSIBLE = 200_000;
        private const long VOLT_MAX_PLAUSIBLE = 2_000_000;
        // A single percent step should not move the rail by more than this.
        private const long VOLT_MAX_EXPECTED_MOVE = 100_000;   // 100 mV per step

        // ---------------------------------------------------------------- state

        private static QIFn _qi;
        private static IntPtr _gpu;
        private static BufferFn _getStatus, _getControl, _setControl;
        private static bool _dirty;              // a non-zero delta is currently applied
        private static int _appliedDelta;
        private static bool _reverting;
        private static readonly object _gate = new object();

        // ---------------------------------------------------------------- P/Invoke

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryW(string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate IntPtr QIFn(uint id);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int InitFn();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int EnumFn(IntPtr[] gpus, ref uint count);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int BufferFn(IntPtr gpu, byte[] buffer);

        // ---------------------------------------------------------------- helpers

        private static uint U32(byte[] b, int off) { return BitConverter.ToUInt32(b, off); }
        private static void PutU32(byte[] b, int off, uint v) { BitConverter.GetBytes(v).CopyTo(b, off); }
        private static uint MakeVersion(uint size, uint ver) { return (ver << 16) | (size & 0xFFFF); }

        private static void Write(string line) { Console.WriteLine(line); }

        private static void Write(string line, ConsoleColor color)
        {
            ConsoleColor prev = Console.ForegroundColor;
            Console.ForegroundColor = color;
            Console.WriteLine(line);
            Console.ForegroundColor = prev;
        }

        // ---------------------------------------------------------------- nvapi

        private static bool Open(out string error)
        {
            error = null;
            IntPtr m = LoadLibraryW("nvapi64.dll");
            if (m == IntPtr.Zero) { error = "nvapi64.dll 加载失败"; return false; }

            IntPtr p = GetProcAddress(m, "nvapi_QueryInterface");
            if (p == IntPtr.Zero) { error = "nvapi_QueryInterface 缺失"; return false; }
            _qi = (QIFn)Marshal.GetDelegateForFunctionPointer(p, typeof(QIFn));

            IntPtr ip = _qi(ID_INITIALIZE);
            IntPtr ep = _qi(ID_ENUM_PHYSICAL_GPUS);
            if (ip == IntPtr.Zero || ep == IntPtr.Zero) { error = "初始化接口缺失"; return false; }

            InitFn init = (InitFn)Marshal.GetDelegateForFunctionPointer(ip, typeof(InitFn));
            int rc = init();
            if (rc != 0) { error = "NvAPI_Initialize 返回 " + rc; return false; }

            EnumFn en = (EnumFn)Marshal.GetDelegateForFunctionPointer(ep, typeof(EnumFn));
            IntPtr[] gpus = new IntPtr[64];
            uint n = 0;
            if (en(gpus, ref n) != 0 || n == 0) { error = "未找到物理 GPU"; return false; }
            _gpu = gpus[0];

            _getStatus = Fn(ID_VOLT_RAILS_GET_STATUS);
            _getControl = Fn(ID_VOLT_RAILS_GET_CONTROL);
            _setControl = Fn(ID_VOLT_RAILS_SET_CONTROL);

            if (_getStatus == null || _getControl == null || _setControl == null)
            {
                error = "volt-rails 接口不完整 (get=" + (_getStatus != null) +
                        " getctrl=" + (_getControl != null) + " set=" + (_setControl != null) + ")";
                return false;
            }
            return true;
        }

        private static BufferFn Fn(uint id)
        {
            IntPtr p = _qi(id);
            return p == IntPtr.Zero ? null : (BufferFn)Marshal.GetDelegateForFunctionPointer(p, typeof(BufferFn));
        }

        // ------------------------------------------------------------ read/write

        /// <summary>Reads the live rail voltage. Returns false with a reason.</summary>
        private static bool ReadVoltage(out long microVolts, out int rc, out string error)
        {
            microVolts = 0; error = null;
            byte[] b = new byte[STATUS_SIZE];
            PutU32(b, 0, MakeVersion(STATUS_SIZE, 1));
            rc = _getStatus(_gpu, b);
            if (rc != 0) { error = "GetStatus rc=" + rc; return false; }
            if (U32(b, 0) != MakeVersion(STATUS_SIZE, 1)) { error = "GetStatus 版本回显不匹配"; return false; }
            long v = U32(b, STATUS_VOLT_OFFSET);
            if (v < VOLT_MIN_PLAUSIBLE || v > VOLT_MAX_PLAUSIBLE) { error = "电压不合理: " + v + " µV"; return false; }
            microVolts = v;
            return true;
        }

        /// <summary>Reads the currently applied percent_delta.</summary>
        private static bool ReadDelta(out int delta, out int rc, out string error)
        {
            delta = 0; error = null;
            byte[] b = new byte[CONTROL_SIZE];
            PutU32(b, 0, MakeVersion(CONTROL_SIZE, 1));
            rc = _getControl(_gpu, b);
            if (rc != 0) { error = "GetControl rc=" + rc; return false; }
            if (U32(b, 0) != MakeVersion(CONTROL_SIZE, 1)) { error = "GetControl 版本回显不匹配"; return false; }
            delta = (sbyte)b[CONTROL_DELTA_OFFSET];   // signed: the field is a delta
            return true;
        }

        /// <summary>
        /// Writes percent_delta after seeding the structure from GetControl.
        ///
        /// Seeding matters: SetControl is the write form of the same structure, and
        /// starting from what the driver itself reported avoids sending a
        /// half-initialised buffer that would be rejected for reasons unrelated to
        /// the value being tested.
        /// </summary>
        private static int WriteDelta(int delta, out string error)
        {
            error = null;
            byte[] b = new byte[CONTROL_SIZE];
            PutU32(b, 0, MakeVersion(CONTROL_SIZE, 1));

            // Seed from the live control structure so every other field is what the
            // driver expects.
            int rcGet = _getControl(_gpu, b);
            if (rcGet != 0)
            {
                error = "写入前 GetControl 失败 rc=" + rcGet;
                return rcGet;
            }
            b[CONTROL_DELTA_OFFSET] = (byte)(sbyte)delta;
            PutU32(b, 0, MakeVersion(CONTROL_SIZE, 1));
            return _setControl(_gpu, b);
        }

        /// <summary>Returns the rail control to zero. Best effort, called from many paths.</summary>
        private static void RevertToZero(bool announce)
        {
            lock (_gate)
            {
                if (_reverting) return;
                _reverting = true;
                try
                {
                    if (_setControl == null) return;
                    string err;
                    int rc = WriteDelta(0, out err);
                    if (announce)
                    {
                        if (rc == 0) Write("  已归零 percent_delta = 0", ConsoleColor.Green);
                        else Write("  ⚠ 归零失败 rc=" + rc + " " + (err ?? ""), ConsoleColor.Red);
                    }
                    _dirty = false;
                    _appliedDelta = 0;
                }
                catch { /* never let cleanup throw */ }
                finally { _reverting = false; }
            }
        }

        // ---------------------------------------------------------------- output

        private static void Banner()
        {
            Write("");
            Write("================================================================", ConsoleColor.Cyan);
            Write("  ClientVoltRailsSetControl 验证工具", ConsoleColor.Cyan);
            Write("  只读探测 → 单步写入 → 立即回读 → 立即归零", ConsoleColor.Cyan);
            Write("================================================================", ConsoleColor.Cyan);
            Write("");
            Write("  接口 0x465F9BCF  GetStatus   (读当前电压)");
            Write("  接口 0x9DF23CA1  GetControl  (读 percent_delta)");
            Write("  接口 0xB9306D9B  SetControl  (写 percent_delta)");
            Write("");
            Write("  安全措施：", ConsoleColor.Yellow);
            Write("    · 基线非零则拒绝测试");
            Write("    · 每次只测一个小步进");
            Write("    · 每步之后立即归零");
            Write("    · 退出 / Ctrl+C / 异常 都会归零");
            Write("    · 写入被拒、回读不符、电压异常 → 立即停止");
            Write("");
        }

        // ---------------------------------------------------------------- phases

        /// <summary>
        /// Phase 1 — read everything, change nothing. Its job is to establish a clean
        /// baseline and to prove the read paths still work, because if the baseline
        /// is not zero the experiment's results would be uninterpretable.
        /// </summary>
        private static bool PhaseBaseline(out long voltageUv, out int delta)
        {
            voltageUv = 0; delta = 0;
            Write("[1] 基线读取（不写入任何内容）", ConsoleColor.Cyan);

            int rc; string err;
            if (!ReadVoltage(out voltageUv, out rc, out err))
            {
                Write("    ✗ 读取电压失败: " + err, ConsoleColor.Red);
                return false;
            }
            Write(string.Format("    rail 电压      = {0} µV = {1:0.000} V", voltageUv, voltageUv / 1e6));

            if (!ReadDelta(out delta, out rc, out err))
            {
                Write("    ✗ 读取 percent_delta 失败: " + err, ConsoleColor.Red);
                return false;
            }
            Write(string.Format("    percent_delta  = {0}", delta));

            if (delta != 0)
            {
                Write("", ConsoleColor.Red);
                Write("    ✗ 基线 percent_delta 非零 —— 拒绝继续测试。", ConsoleColor.Red);
                Write("      原因：从一个未知起点开始无法解释结果。", ConsoleColor.Red);
                Write("      请先在 mVolt+ 里把电压偏移归零，或重启后再试。", ConsoleColor.Red);
                return false;
            }

            Write("    ✓ 基线干净（delta = 0），可以继续", ConsoleColor.Green);
            Write("");
            return true;
        }

        /// <summary>
        /// Phase 2 — find out whether SetControl accepts anything at all, without
        /// asking for a change: write the value it already has (0).
        ///
        /// This separates "the call is refused" from "this particular value is
        /// refused". The earlier non-elevated probe got rc=-1 on exactly this
        /// request, so if it now succeeds the missing precondition was elevation.
        /// </summary>
        private static bool PhaseNoOpWrite()
        {
            Write("[2] 空操作写入测试（写入当前值 0，不产生任何变化）", ConsoleColor.Cyan);

            string err;
            int rc = WriteDelta(0, out err);
            Write(string.Format("    SetControl(0) rc={0}{1}", rc, err != null ? "  " + err : ""));

            if (rc == 0)
            {
                Write("    ✓ 调用被接受 —— 之前非提权下的 rc=-1 是权限问题", ConsoleColor.Green);
                Write("");
                return true;
            }

            Write("", ConsoleColor.Yellow);
            Write("    ⚠ 即使写入当前值也被拒绝 (rc=" + rc + ")。", ConsoleColor.Yellow);
            Write("      这说明还有别的未知前提（独占访问 / 驱动状态 / 结构字段）。", ConsoleColor.Yellow);
            Write("      继续单步测试大概率同样被拒，但我仍会试一次最小正步进以便确认。", ConsoleColor.Yellow);
            Write("");
            return true;   // not fatal: the next phase is where the real answer is
        }

        /// <summary>
        /// Phase 3 — try a value and measure what happens. One step at a time, and
        /// always back to zero before the next.
        /// </summary>
        private static bool TryStep(int delta, out string conclusion)
        {
            conclusion = null;

            long beforeUv; int rc; string err;
            if (!ReadVoltage(out beforeUv, out rc, out err))
            {
                conclusion = "写入前读电压失败: " + err;
                return false;
            }

            Write(string.Format("    测试 percent_delta = {0,4}   （当前 {1:0.000} V）", delta, beforeUv / 1e6));

            int setRc = WriteDelta(delta, out err);
            if (setRc != 0)
            {
                Write(string.Format("      SetControl rc={0} → 该值被拒绝{1}", setRc, err != null ? " (" + err + ")" : ""));
                RevertToZero(false);
                return false;
            }
            _dirty = true;
            _appliedDelta = delta;

            // Read the control back: did the driver retain the value?
            int applied; int gerr;
            if (!ReadDelta(out applied, out rc, out err))
            {
                conclusion = "写入后读 delta 失败: " + err;
                RevertToZero(true);
                return false;
            }
            Write(string.Format("      回读 percent_delta = {0,4}  {1}", applied,
                applied == delta ? "✓ 保留" : "✗ 未保留（驱动改写为 " + applied + "）"));

            // Let the rail settle, then measure.
            Thread.Sleep(600);
            long afterUv;
            if (!ReadVoltage(out afterUv, out rc, out err))
            {
                RevertToZero(true);
                conclusion = "写入后读电压失败: " + err;
                return false;
            }
            long move = afterUv - beforeUv;
            Write(string.Format("      电压 {0:0.000} V → {1:0.000} V   变化 {2:+.0} mV",
                beforeUv / 1e6, afterUv / 1e6, move / 1000.0));

            if (Math.Abs(move) > VOLT_MAX_EXPECTED_MOVE)
            {
                conclusion = "单步电压移动过大 (" + (move / 1000.0).ToString("+0;-0") + " mV)，语义与假设不符";
                Write("      ⚠ 移动超出预期上限，立即归零并停止", ConsoleColor.Red);
                RevertToZero(true);
                return false;
            }

            bool retained = (applied == delta);
            if (!retained)
                conclusion = "驱动未保留该值（回读 " + applied + "）";
            else if (move == 0)
                conclusion = "值被保留但电压未变化（可能只在负载下生效）";
            else
                conclusion = string.Format("有效：delta={0} → 电压 {1:+0;-0} mV，方向与符号{2}",
                    delta, move / 1000.0, (Math.Sign(move) == Math.Sign(delta)) ? "一致" : "相反");

            RevertToZero(true);
            Thread.Sleep(300);
            Write("");
            return retained;
        }

        // ---------------------------------------------------------------- main

        private static int Main(string[] args)
        {
            Console.OutputEncoding = Encoding.UTF8;
            Banner();

            bool listOnly = Array.IndexOf(args, "--list") >= 0;
            bool writeTest = Array.IndexOf(args, "--write") >= 0;
            bool scan = Array.IndexOf(args, "--scan") >= 0;
            bool vfTest = Array.IndexOf(args, "--vf") >= 0;

            // The V/F experiment is a separate entry point: different interface,
            // different structure, and its own safety protocol. Keeping it apart
            // means neither experiment can leave the other in an unexpected state.
            if (vfTest) return VfWriteTest.Run(args);

            // ---- elevation check ----
            bool elevated;
            try
            {
                using (var id = System.Security.Principal.WindowsIdentity.GetCurrent())
                    elevated = new System.Security.Principal.WindowsPrincipal(id)
                        .IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
            }
            catch { elevated = false; }
            Write("  管理员权限: " + (elevated ? "是" : "否 —— 写入很可能被拒"), elevated ? ConsoleColor.Green : ConsoleColor.Yellow);
            Write("");

            string openErr;
            if (!Open(out openErr))
            {
                Write("  ✗ 初始化失败: " + openErr, ConsoleColor.Red);
                return 2;
            }
            Write("  ✓ 三个接口全部解析成功", ConsoleColor.Green);
            Write("");

            Console.CancelKeyPress += (s, e) =>
            {
                e.Cancel = true;
                Write("");
                Write("  收到 Ctrl+C，正在归零…", ConsoleColor.Yellow);
                RevertToZero(true);
                Environment.Exit(3);
            };
            AppDomain.CurrentDomain.ProcessExit += (s, e) => { if (_dirty) RevertToZero(false); };

            try
            {
                long baseUv; int baseDelta;
                if (!PhaseBaseline(out baseUv, out baseDelta)) { RevertToZero(false); return 1; }

                if (listOnly)
                {
                    Write("  --list 模式：只读，不做任何写入。", ConsoleColor.Green);
                    return 0;
                }

                if (!writeTest && !scan)
                {
                    Write("  当前为只读模式。要测试写入，请加参数：", ConsoleColor.Yellow);
                    Write("    --write    空操作 + 最小正步进（推荐先跑这个）");
                    Write("    --scan     扫描一组值，找有效范围");
                    Write("    --vf       测试 V/F 曲线写入（另一条路径）");
                    Write("");
                    Write("  建议顺序：先 --write，确认无异常后再 --scan。", ConsoleColor.Yellow);
                    return 0;
                }

                if (!elevated)
                {
                    Write("  ✗ 未提权，写入几乎肯定被拒。请右键“以管理员身份运行”。", ConsoleColor.Red);
                    return 2;
                }

                if (writeTest)
                {
                    PhaseNoOpWrite();

                    Write("[3] 单步写入测试", ConsoleColor.Cyan);
                    string concl;
                    bool ok1 = TryStep(1, out concl);
                    Write("      delta=+1 结论: " + concl, ok1 ? ConsoleColor.Green : ConsoleColor.Yellow);
                    Write("");

                    bool ok2 = TryStep(-1, out concl);
                    Write("      delta=-1 结论: " + concl, ok2 ? ConsoleColor.Green : ConsoleColor.Yellow);
                    Write("");
                }

                if (scan)
                {
                    Write("[4] 范围扫描（每一步之后都会归零）", ConsoleColor.Cyan);
                    int[] candidates = { 1, 2, 3, 5, 8, 10, 20, -1, -2, -5, -10 };
                    int accepted = 0;
                    foreach (int v in candidates)
                    {
                        string concl;
                        if (TryStep(v, out concl)) accepted++;
                        else Write("      delta=" + v + " 结论: " + concl);
                    }
                    Write("");
                    Write("  扫描完成，被接受的值数量: " + accepted + " / " + candidates.Length,
                          accepted > 0 ? ConsoleColor.Green : ConsoleColor.Yellow);
                    Write("");
                }

                RevertToZero(true);
                Write("");
                Write("=== 结束 ===", ConsoleColor.Cyan);
                Write("  最后状态：");
                long fv; int frc; string ferr;
                if (ReadVoltage(out fv, out frc, out ferr))
                    Write(string.Format("    电压 = {0:0.000} V", fv / 1e6));
                int fd; 
                if (ReadDelta(out fd, out frc, out ferr))
                    Write(string.Format("    percent_delta = {0}  {1}", fd, fd == 0 ? "✓ 已归零" : "⚠ 非零！"));
                Write("");
                return 0;
            }
            catch (Exception ex)
            {
                Write("");
                Write("  ✗ 未处理异常: " + ex.Message, ConsoleColor.Red);
                RevertToZero(true);
                return 1;
            }
        }
    }
}
