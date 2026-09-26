using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace VoltProbe
{
    /// <summary>
    /// Second experiment: can the V/F curve be written?
    ///
    /// RATIONALE
    ///   ClientVoltRailsSetControl is not available on this GPU — every non-zero
    ///   percent_delta was refused with NVAPI_NOT_SUPPORTED (see the --write/--scan
    ///   output). So the rail-percentage route to changing core voltage is closed.
    ///
    ///   The remaining candidate is the V/F curve itself, via
    ///   ClockClientClkVfPointsSetControl (0x0733E009). That is what the open-source
    ///   nvcurve tool uses, and it is a strictly more expressive control: editing the
    ///   frequency at a given voltage is how undervolting is actually done. The
    ///   voltage a GPU runs at is a consequence of the curve, not an independent knob.
    ///
    /// STRUCTURE (ClockClientClkVfPointsControlV1, verified against LACT's declaration
    /// and against the driver's own read-back in the first experiment)
    ///   version         u32          +0x00
    ///   vf_points_mask  [u32; 8]     +0x04   (32 bytes)
    ///   reserved        [u8; 32]     +0x24
    ///   vf_points       [point; 255] +0x44
    ///     point: type u32, reserved [u8;16], data { i32 freq_offset_khz }
    ///     -> stride 0x24, frequency offset at point+0x14
    ///   total 0x2420
    ///
    /// SAFETY
    ///   Identical protocol to the first tool, and deliberately more conservative:
    ///     * phase 1 only writes back the value the driver already reported (all
    ///       offsets zero), so it establishes "is the call accepted on this GPU"
    ///       without requesting any change at all;
    ///     * phase 2 asks for the smallest non-zero clock offset it can, at one
    ///       point, and reverts immediately;
    ///     * every path zeros the whole point mask and offsets on exit.
    ///
    ///   This is a clock offset, not a voltage. The worst case is a transient clock
    ///   change that is reverted within a second, and a reboot clears it regardless.
    /// </summary>
    internal static class VfWriteTest
    {
        private const uint ID_INITIALIZE = 0x0150E828;
        private const uint ID_ENUM_PHYSICAL_GPUS = 0xE5AC921F;

        private const uint ID_VF_GET_STATUS = 0x21537AD4;
        private const uint ID_VF_GET_CONTROL = 0x23F1B133;
        private const uint ID_VF_SET_CONTROL = 0x0733E009;
        private const uint ID_VOLT_GET_STATUS = 0x465F9BCF;

        private const int VF_SIZE = 0x2420;
        private const int VF_MASK_OFFSET = 0x04;
        private const int VF_MASK_EXPECTED = 0x20;     // 8 * u32
        private const int VF_POINTS_OFFSET = 0x44;
        private const int VF_POINT_STRIDE = 0x24;
        private const int VF_POINT_COUNT = 255;
        private const int VF_POINT_TYPE_OFFSET = 0x00;
        private const int VF_POINT_FREQ_OFFSET = 0x14;

        private const int STATUS_SIZE = 0x4C;
        private const int STATUS_VOLT_OFFSET = 0x28;

        private static QIFn _qi;
        private static IntPtr _gpu;
        private static BufferFn _vfGetControl, _vfSetControl, _vfGetStatus, _voltGet;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr QIFn(uint id);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int InitFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int EnumFn(IntPtr[] g, ref uint n);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int BufferFn(IntPtr gpu, byte[] buf);

        private static void W(string s) { Console.WriteLine(s); }
        private static void W(string s, ConsoleColor c)
        {
            ConsoleColor p = Console.ForegroundColor; Console.ForegroundColor = c;
            Console.WriteLine(s); Console.ForegroundColor = p;
        }
        private static uint Ver(int size) { return (1u << 16) | ((uint)size & 0xFFFF); }

        public static int Run(string[] args)
        {
            W("");
            W("================================================================", ConsoleColor.Cyan);
            W("  V/F 曲线写入测试  ClkVfPointsSetControl 0x0733E009", ConsoleColor.Cyan);
            W("================================================================", ConsoleColor.Cyan);
            W("");

            IntPtr m = LoadLibraryW("nvapi64.dll");
            if (m == IntPtr.Zero) { W("  nvapi64.dll 加载失败", ConsoleColor.Red); return 2; }
            _qi = (QIFn)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "nvapi_QueryInterface"), typeof(QIFn));
            ((InitFn)Marshal.GetDelegateForFunctionPointer(_qi(ID_INITIALIZE), typeof(InitFn)))();
            IntPtr[] g = new IntPtr[64]; uint n = 0;
            ((EnumFn)Marshal.GetDelegateForFunctionPointer(_qi(ID_ENUM_PHYSICAL_GPUS), typeof(EnumFn)))(g, ref n);
            if (n == 0) { W("  未找到 GPU", ConsoleColor.Red); return 2; }
            _gpu = g[0];

            _vfGetControl = Fn(ID_VF_GET_CONTROL);
            _vfSetControl = Fn(ID_VF_SET_CONTROL);
            _vfGetStatus = Fn(ID_VF_GET_STATUS);
            _voltGet = Fn(ID_VOLT_GET_STATUS);
            if (_vfGetControl == null || _vfSetControl == null)
            {
                W("  V/F 接口缺失", ConsoleColor.Red); return 2;
            }
            W("  ✓ 接口解析成功 (GetControl / SetControl / GetStatus)");
            W("");

            AppDomain.CurrentDomain.ProcessExit += (s, e) => { try { ZeroAll(false); } catch { } };
            Console.CancelKeyPress += (s, e) =>
            {
                e.Cancel = true;
                W("");
                W("  Ctrl+C，清零中…", ConsoleColor.Yellow);
                ZeroAll(true);
                Environment.Exit(3);
            };

            try
            {
                // ---------------- phase 1: read baseline ----------------
                W("[1] 基线读取（只读）", ConsoleColor.Cyan);
                int ctrlRc; string maskUsed;
                byte[] ctrl = ReadControl(out ctrlRc, out maskUsed);
                if (ctrl == null)
                {
                    W(string.Format("    ✗ 三种掩码全部失败，最后 rc={0} ({1})", ctrlRc, Describe(ctrlRc)), ConsoleColor.Red);
                    return 1;
                }
                W(string.Format("    GetControl 成功，掩码 = {0}", maskUsed));
                W(string.Format("    前 8 字节 = {0}", BitConverter.ToString(ctrl, 0, 8).Replace("-", " ")));

                int dirty = 0;
                for (int i = 0; i < VF_POINT_COUNT; i++)
                {
                    int off = VF_POINTS_OFFSET + i * VF_POINT_STRIDE;
                    if (off + VF_POINT_FREQ_OFFSET + 4 > ctrl.Length) break;
                    if (BitConverter.ToInt32(ctrl, off + VF_POINT_FREQ_OFFSET) != 0) dirty++;
                }
                W(string.Format("    非零频率偏移点数 = {0}", dirty));
                if (dirty != 0)
                {
                    W("    ✗ 已有非零偏移，拒绝测试（结果无法解释）", ConsoleColor.Red);
                    return 1;
                }

                // which points actually carry data in the curve?
                byte[] st = ReadCurveStatus();
                int live = 0;
                if (st != null)
                {
                    for (int i = 0; i < VF_POINT_COUNT; i++)
                    {
                        int off = 0x48 + i * 0x1C;
                        if (off + 8 > st.Length) break;
                        if (BitConverter.ToUInt32(st, off) != 0 || BitConverter.ToUInt32(st, off + 4) != 0) live++;
                    }
                }
                W(string.Format("    曲线有效点数     = {0}", live));
                W("    ✓ 基线干净");
                W("");

                bool write = Array.IndexOf(args, "--write") >= 0;

                // ---------------- phase 2: no-op write ----------------
                W("[2] 空操作写入（把驱动已报告的值写回，不请求任何改变）", ConsoleColor.Cyan);
                byte[] echo = (byte[])ctrl.Clone();
                SetMask(echo);
                BitConverter.GetBytes(Ver(VF_SIZE)).CopyTo(echo, 0);
                int rc0 = _vfSetControl(_gpu, echo);
                W(string.Format("    SetControl(原值) rc={0}  ({1})", rc0, Describe(rc0)));
                if (rc0 == 0)
                    W("    ✓ 调用被接受 —— 可以继续测试真实偏移", ConsoleColor.Green);
                else
                {
                    W("    ⚠ 连回写原值都被拒。", ConsoleColor.Yellow);
                    W("      可能原因：点掩码语义不符 / 需要特定 type 字段 / 该 GPU 不开放此控制。", ConsoleColor.Yellow);
                    if (!write) { ZeroAll(true); return 0; }
                }
                W("");

                if (!write)
                {
                    W("  只读模式结束。加 --write 继续测试真实偏移。", ConsoleColor.Yellow);
                    ZeroAll(true);
                    return 0;
                }

                // ---------------- phase 3: smallest real offset ----------------
                W("[3] 最小步进写入（对 1 个点写 +15 MHz，随后立即清零）", ConsoleColor.Cyan);
                int targetIndex = -1;
                if (st != null)
                {
                    for (int i = 0; i < VF_POINT_COUNT; i++)
                    {
                        int off = 0x48 + i * 0x1C;
                        if (off + 8 > st.Length) break;
                        if (BitConverter.ToUInt32(st, off) != 0 && BitConverter.ToUInt32(st, off + 4) != 0)
                        { targetIndex = i; break; }
                    }
                }
                if (targetIndex < 0) { W("    找不到有效点，放弃", ConsoleColor.Yellow); ZeroAll(true); return 0; }
                W(string.Format("    目标点 #{0}", targetIndex));

                long vBefore = ReadVoltageUv(out int vr);
                W(string.Format("    写入前电压 = {0:0.000} V", vBefore / 1e6));

                byte[] req = (byte[])ctrl.Clone();
                BitConverter.GetBytes(Ver(VF_SIZE)).CopyTo(req, 0);
                // Ask only for the single target point.
                for (int i = 0; i < VF_MASK_EXPECTED; i++) req[VF_MASK_OFFSET + i] = 0;
                int word = targetIndex / 32;
                int bit = targetIndex % 32;
                if (word < 8)
                    BitConverter.GetBytes(1u << bit).CopyTo(req, VF_MASK_OFFSET + word * 4);

                int po = VF_POINTS_OFFSET + targetIndex * VF_POINT_STRIDE;
                if (po + VF_POINT_FREQ_OFFSET + 4 <= req.Length)
                {
                    BitConverter.GetBytes(0u).CopyTo(req, po + VF_POINT_TYPE_OFFSET);   // PROG
                    BitConverter.GetBytes(15000).CopyTo(req, po + VF_POINT_FREQ_OFFSET); // +15 MHz
                }

                int rc1 = _vfSetControl(_gpu, req);
                W(string.Format("    SetControl(#%d, +15 MHz) rc=%d  (%s)", targetIndex, rc1, Describe(rc1)));

                if (rc1 != 0)
                {
                    W("    ✗ 该写入被拒绝 —— V/F 写入在此 GPU 上同样不可用", ConsoleColor.Red);
                    ZeroAll(true);
                    return 0;
                }

                Thread.Sleep(700);
                byte[] after = ReadControl();
                int applied = -1;
                if (after != null && po + VF_POINT_FREQ_OFFSET + 4 <= after.Length)
                    applied = BitConverter.ToInt32(after, po + VF_POINT_FREQ_OFFSET);
                long vAfter = ReadVoltageUv(out int vr2);
                W(string.Format("    回读该点偏移 = {0} kHz  ({1})", applied, applied == 15000 ? "✓ 保留" : "✗ 未保留"));
                W(string.Format("    电压 {0:0.000} V → {1:0.000} V", vBefore / 1e6, vAfter / 1e6));

                W("");
                W("    清零中…");
                ZeroAll(true);

                byte[] final = ReadControl();
                int residue = 0;
                if (final != null)
                    for (int i = 0; i < VF_POINT_COUNT; i++)
                    {
                        int o = VF_POINTS_OFFSET + i * VF_POINT_STRIDE;
                        if (o + VF_POINT_FREQ_OFFSET + 4 > final.Length) break;
                        if (BitConverter.ToInt32(final, o + VF_POINT_FREQ_OFFSET) != 0) residue++;
                    }
                W(string.Format("  最终残留非零偏移点 = {0}  {1}", residue, residue == 0 ? "✓ 干净" : "⚠ 需重启"),
                  residue == 0 ? ConsoleColor.Green : ConsoleColor.Red);
                return 0;
            }
            catch (Exception ex)
            {
                W("");
                W("  ✗ 未处理异常: " + ex.Message, ConsoleColor.Red);
                try { ZeroAll(true); } catch { }
                return 1;
            }
        }

        private static BufferFn Fn(uint id)
        {
            IntPtr p = _qi(id);
            return p == IntPtr.Zero ? null : (BufferFn)Marshal.GetDelegateForFunctionPointer(p, typeof(BufferFn));
        }

        private static void SetMask(byte[] buf)
        {
            for (int i = 0; i < VF_MASK_EXPECTED; i++) buf[VF_MASK_OFFSET + i] = 0xFF;
        }

        /// <summary>
        /// Mask covering only the first 128 points, which is the pattern that
        /// returned rc=0 in the exploratory probe. The full 255-point mask is
        /// tried first and falls back to this, because the driver treats the two
        /// differently and only one of them was observed to work.
        /// </summary>
        private static void SetMaskLow(byte[] buf)
        {
            for (int i = 0; i < VF_MASK_EXPECTED; i++) buf[VF_MASK_OFFSET + i] = 0x0F;
        }

        /// <summary>
        /// Reads the V/F control, trying both mask widths.
        ///
        /// The mask is not cosmetic: an over-broad mask is rejected by this driver
        /// with a generic NVAPI_ERROR, which is indistinguishable from a permission
        /// problem unless the alternatives are tried explicitly.
        /// </summary>
        private static byte[] ReadControl(out int rc, out string maskUsed)
        {
            maskUsed = "";
            rc = -1;   // assigned before the loop so every exit path has a value
            foreach (string mode in new[] { "0xFF(255点)", "0x0F(128点)", "0x00(空掩码)" })
            {
                byte[] b = new byte[VF_SIZE];
                BitConverter.GetBytes(Ver(VF_SIZE)).CopyTo(b, 0);
                if (mode.StartsWith("0xFF")) SetMask(b);
                else if (mode.StartsWith("0x0F")) SetMaskLow(b);

                int r = _vfGetControl(_gpu, b);
                if (r == 0) { rc = r; maskUsed = mode; return b; }
                rc = r;
            }
            return null;
        }

        private static byte[] ReadControl() { int rc; string m; return ReadControl(out rc, out m); }

        private static byte[] ReadCurveStatus()
        {
            byte[] b = new byte[0x1C28];
            BitConverter.GetBytes(Ver(0x1C28)).CopyTo(b, 0);
            for (int i = 4; i < 20; i++) b[i] = 0xFF;
            BitConverter.GetBytes(15u).CopyTo(b, 0x14);
            return _vfGetStatus(_gpu, b) == 0 ? b : null;
        }

        private static long ReadVoltageUv(out int rc)
        {
            byte[] b = new byte[STATUS_SIZE];
            BitConverter.GetBytes(Ver(STATUS_SIZE)).CopyTo(b, 0);
            rc = _voltGet(_gpu, b);
            return rc == 0 ? BitConverter.ToUInt32(b, STATUS_VOLT_OFFSET) : 0;
        }

        /// <summary>Zeros the whole point mask and every offset. Best effort.</summary>
        private static void ZeroAll(bool announce)
        {
            try
            {
                byte[] b = new byte[VF_SIZE];
                BitConverter.GetBytes(Ver(VF_SIZE)).CopyTo(b, 0);
                SetMask(b);   // mask everything, then ask for zero on all of it
                int rc = _vfSetControl(_gpu, b);
                if (announce)
                {
                    if (rc == 0) W("    已清零（全掩码 + 全零偏移）", ConsoleColor.Green);
                    else W("    ⚠ 清零返回 rc=" + rc + "  " + Describe(rc), ConsoleColor.Yellow);
                }
            }
            catch { }
        }

        private static string Describe(int rc)
        {
            switch (rc)
            {
                case 0: return "OK";
                case -1: return "NVAPI_ERROR（通用错误，常见于未提权）";
                case -5: return "NVAPI_INVALID_ARGUMENT";
                case -9: return "NVAPI_INCOMPATIBLE_STRUCT_VERSION";
                case -104: return "NVAPI_NOT_SUPPORTED（该 GPU 不支持）";
                default: return "rc=" + rc;
            }
        }
    }
}
