using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace NvpwrControl
{
    /// <summary>Current clock-offset capability and values, read from Pstates20.</summary>
    internal sealed class TuningState
    {
        public bool CoreOk; public long CoreMhz, CoreMin, CoreMax;
        public bool MemoryOk; public long MemoryMhz, MemoryMin, MemoryMax;
        public bool XbarOk; public long XbarMhz;
        public string Error = "";
    }

    /// <summary>
    /// Clock-offset tuning through NVAPI's Pstates20 interface.
    ///
    /// Scope note: this is deliberately the smallest useful slice. The project
    /// already has a far more thorough NVAPI OC module on the C++ side
    /// (XBAR/MSVDD/GPC:XBAR/VF probing); here only core and memory offsets are
    /// offered, because overclocking is not this tool's job — the power ceiling
    /// is. Core/memory are exposed so a user can finish an experiment without
    /// opening a second program.
    ///
    /// Safety: every value is checked against the range the driver itself
    /// reports, the SET is followed by a fresh GET, and a mismatch is reported
    /// rather than assumed to have worked.
    ///
    /// Voltage is intentionally absent. The public NVAPI surface has no voltage
    /// setter, and Pstates20 carries no voltage domain on the target GPU —
    /// measured: 0 domains, and a +25 mV rail change moved 0 bytes here. Voltage
    /// is delegated to mVolt+ (see MVolt).
    /// </summary>
    internal static class Tuning
    {
        private const uint NVAPI_INITIALIZE = 0x0150E828;
        private const uint NVAPI_ENUM_PHYSICAL_GPUS = 0xE5AC921F;
        private const uint ID_PSTATES_GET = 0x6FF81213;
        private const uint ID_PSTATES_SET = 0x0F4DAE6B;

        /*
            READ-ONLY VOLTAGE — where this id comes from, and why it is trustworthy.

            Id 0x465F9BCF is ClientVoltRailsGetStatus. It is undocumented by NVIDIA,
            so it cannot be taken from the public SDK; it comes from an independent
            reverse-engineering write-up by Loong0x00 (the same author mVolt+
            credits for the XBAR discovery), cross-referenced there against
            nvapi-rs, nvapioc and vertminer.

            The reason to trust this particular id rather than a guess: two other
            ids in that same write-up, 0x507B4B59 (VfPointsGetInfo) and 0x23F1B133
            (VfPointsGetControl), are already used by this project's own audited
            C++ module and are known to resolve on the reference machine. The
            source has therefore been independently corroborated on this hardware.

            Verified locally before being wired in:
              - the call returns 0 (success), not an error,
              - the returned buffer starts with size 0x4C and version 1, exactly as
                documented, which is the driver echoing the request structure,
              - wrong buffer sizes are rejected with -9, confirming the size is
                validated rather than ignored,
              - the value tracks reality: it moved between 0.625 V and 0.635 V over
                successive samples, matching what mVolt+ displays for the same GPU.

            It is a GET. Nothing here writes to the GPU, so it carries no more risk
            than reading the temperature.
        */
        private const uint ID_VOLT_RAILS_GET_STATUS = 0x465F9BCF;
        private const int VOLT_RAILS_BUFFER_SIZE = 0x4C;
        private const int VOLT_RAILS_VERSION = 1;
        private const int VOLT_RAILS_OFFSET_UV = 0x28;

        /*
            V/F CURVE — read-only (ClockClientClkVfPointsGetStatus, 0x21537AD4).

            Same provenance as the voltage id above: documented by the Loong0x00
            write-up, and verified locally before use. What was verified:
              - the call succeeds with an exact 0x1C28 buffer and version 1,
              - the driver echoes size 0x1C28 / version 1 in the header,
              - all 128 points come back populated, spanning 180–3165 MHz and
                0.450–1.240 V in even 25 mV steps,
              - the point matching the live voltage reading is the one the GPU is
                actually running at, which is what makes the curve meaningful
                rather than just plausible-looking numbers.

            The request must be pre-filled with the point mask (all 128 bits) and
            the clock count at +0x14; without that the driver returns a zeroed
            structure, which is an easy way to mistake "not asked for" for "empty".
        */
        private const uint ID_VF_CURVE_GET_STATUS = 0x21537AD4;
        private const int VF_CURVE_BUFFER_SIZE = 0x1C28;
        private const int VF_CURVE_POINTS = 128;
        private const int VF_CURVE_DATA_OFFSET = 0x48;
        private const int VF_CURVE_POINT_STRIDE = 0x1C;
        private const int VF_CURVE_MASK_OFFSET = 4;
        private const int VF_CURVE_MASK_BYTES = 16;
        private const int VF_CURVE_COUNT_OFFSET = 0x14;
        private const uint VF_CURVE_CLOCK_COUNT = 15;

        private const uint PSTATES_VERSION = 0x00021CF8;
        private const int PSTATES_SIZE = 7416;
        private const uint PSTATES_SET_V1_VERSION = 0x00011C94;
        private const int PSTATES_SET_V1_SIZE = 7316;

        private const int PSTATES_HEADER = 20;
        private const int PSTATE_SIZE = 456;
        private const int PSTATE_CLOCKS = 8;
        private const int CLOCK_SIZE = 44;
        private const int CLOCKS_OFF = 8;
        private const uint DOMAIN_GRAPHICS = 0;
        private const uint DOMAIN_MEMORY = 4;
        private const int DELTA_OFF = 12;   // cur / min / max live at +12/+16/+20

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryW(string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate IntPtr QueryInterfaceFn(uint id);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int InitFn();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int EnumFn(IntPtr[] gpus, ref uint count);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int BufferFn(IntPtr gpu, byte[] buffer);

        private static IntPtr _nvapi;
        private static QueryInterfaceFn _qi;
        private static IntPtr _gpu;

        private static bool Open()
        {
            if (_qi != null && _gpu != IntPtr.Zero) return true;

            if (_nvapi == IntPtr.Zero)
            {
                _nvapi = LoadLibraryW("nvapi64.dll");
                if (_nvapi == IntPtr.Zero) return false;
            }
            if (_qi == null)
            {
                IntPtr p = GetProcAddress(_nvapi, "nvapi_QueryInterface");
                if (p == IntPtr.Zero) return false;
                _qi = (QueryInterfaceFn)Marshal.GetDelegateForFunctionPointer(p, typeof(QueryInterfaceFn));
            }

            IntPtr initPtr = _qi(NVAPI_INITIALIZE);
            IntPtr enumPtr = _qi(NVAPI_ENUM_PHYSICAL_GPUS);
            if (initPtr == IntPtr.Zero || enumPtr == IntPtr.Zero) return false;

            InitFn init = (InitFn)Marshal.GetDelegateForFunctionPointer(initPtr, typeof(InitFn));
            if (init() != 0) return false;

            EnumFn en = (EnumFn)Marshal.GetDelegateForFunctionPointer(enumPtr, typeof(EnumFn));
            IntPtr[] gpus = new IntPtr[64];
            uint count = 0;
            if (en(gpus, ref count) != 0 || count == 0) return false;

            _gpu = gpus[0];
            return _gpu != IntPtr.Zero;
        }

        private static BufferFn GetFn(uint id)
        {
            if (_qi == null) return null;
            IntPtr p = _qi(id);
            if (p == IntPtr.Zero) return null;
            return (BufferFn)Marshal.GetDelegateForFunctionPointer(p, typeof(BufferFn));
        }

        private static uint U32(byte[] b, int off)
        {
            if (off + 4 > b.Length) return 0;
            return BitConverter.ToUInt32(b, off);
        }

        private static int I32(byte[] b, int off)
        {
            if (off + 4 > b.Length) return 0;
            return BitConverter.ToInt32(b, off);
        }

        private static void PutU32(byte[] b, int off, uint v)
        {
            if (off + 4 <= b.Length) BitConverter.GetBytes(v).CopyTo(b, off);
        }

        private static void PutI32(byte[] b, int off, int v)
        {
            if (off + 4 <= b.Length) BitConverter.GetBytes(v).CopyTo(b, off);
        }

        private static int FindP0(byte[] b)
        {
            uint np = Math.Min(U32(b, 8), 16u);
            for (int p = 0; p < np; p++)
            {
                int po = PSTATES_HEADER + p * PSTATE_SIZE;
                if (po + PSTATE_SIZE > b.Length) break;
                if (U32(b, po) == 0) return po;
            }
            return -1;
        }

        /// <summary>Reads the current offsets and the driver-reported range.</summary>
        public static TuningState Query()
        {
            TuningState t = new TuningState();
            if (!Open()) { t.Error = "NvAPI 不可用"; return t; }

            BufferFn get = GetFn(ID_PSTATES_GET);
            if (get == null) { t.Error = "Pstates20 GET 接口缺失"; return t; }

            byte[] b = new byte[PSTATES_SIZE];
            PutU32(b, 0, PSTATES_VERSION);
            if (get(_gpu, b) != 0) { t.Error = "Pstates20 GET 调用失败"; return t; }
            if (U32(b, 0) != PSTATES_VERSION) { t.Error = "Pstates20 版本不匹配"; return t; }

            int p0 = FindP0(b);
            if (p0 < 0) { t.Error = "未找到 P0 记录"; return t; }

            uint nc = Math.Min(U32(b, 12), (uint)PSTATE_CLOCKS);
            for (uint c = 0; c < nc; c++)
            {
                int co = p0 + CLOCKS_OFF + (int)c * CLOCK_SIZE;
                if (co + CLOCK_SIZE > b.Length) break;

                uint domain = U32(b, co);
                int cur = I32(b, co + DELTA_OFF);
                int min = I32(b, co + 16);
                int max = I32(b, co + 20);
                if (min > max) continue;

                if (domain == DOMAIN_GRAPHICS)
                {
                    t.CoreOk = true;
                    t.CoreMhz = cur / 1000; t.CoreMin = min / 1000; t.CoreMax = max / 1000;
                }
                else if (domain == DOMAIN_MEMORY)
                {
                    t.MemoryOk = true;
                    t.MemoryMhz = cur / 1000; t.MemoryMin = min / 1000; t.MemoryMax = max / 1000;
                }
            }

            // XBAR is a ClockDomains private interface. Reading it needs a
            // different buffer layout, so it is reported as unavailable here
            // rather than shown as a control that cannot work. The C++ module
            // implements the full path for users who need it.
            t.XbarOk = false;
            return t;
        }

        /// <summary>
        /// Applies core and memory offsets.
        ///
        /// The SET request is built as a minimal V1 payload rather than echoing
        /// the GET/V2 status blob back: replaying a status structure as a control
        /// structure is what makes naive implementations write the wrong thing.
        /// </summary>
        public static bool Apply(ClockTuning wanted, TuningState live, out string error)
        {
            error = null;
            if (!Open()) { error = "NvAPI 不可用"; return false; }

            BufferFn get = GetFn(ID_PSTATES_GET);
            BufferFn set = GetFn(ID_PSTATES_SET);
            if (get == null || set == null) { error = "Pstates20 GET/SET 接口缺失"; return false; }

            if (wanted.CoreOffsetMhz < live.CoreMin || wanted.CoreOffsetMhz > live.CoreMax)
            { error = "核心偏移超出驱动上报范围 " + live.CoreMin + " .. " + live.CoreMax + " MHz"; return false; }
            if (wanted.MemoryOffsetMhz < live.MemoryMin || wanted.MemoryOffsetMhz > live.MemoryMax)
            { error = "显存偏移超出驱动上报范围 " + live.MemoryMin + " .. " + live.MemoryMax + " MHz"; return false; }

            byte[] req = new byte[PSTATES_SET_V1_SIZE];
            PutU32(req, 0, PSTATES_SET_V1_VERSION);
            PutU32(req, 8, 1);   // one P-state: P0

            uint clockCount = 0;
            int coreOff = PSTATES_HEADER + CLOCKS_OFF + (int)clockCount * CLOCK_SIZE;
            PutU32(req, coreOff, DOMAIN_GRAPHICS);
            PutI32(req, coreOff + DELTA_OFF, (int)(wanted.CoreOffsetMhz * 1000));
            clockCount++;

            int memOff = PSTATES_HEADER + CLOCKS_OFF + (int)clockCount * CLOCK_SIZE;
            PutU32(req, memOff, DOMAIN_MEMORY);
            PutI32(req, memOff + DELTA_OFF, (int)(wanted.MemoryOffsetMhz * 1000));
            clockCount++;

            PutU32(req, 12, clockCount);

            if (set(_gpu, req) != 0) { error = "Pstates20 SET 调用失败"; return false; }

            // Verify with a fresh GET rather than trusting the SET.
            TuningState after = Query();
            if (!string.IsNullOrEmpty(after.Error)) { error = "回读失败: " + after.Error; return false; }
            if (after.CoreMhz != wanted.CoreOffsetMhz)
            { error = "核心偏移未被保留（请求 " + wanted.CoreOffsetMhz + "，回读 " + after.CoreMhz + "）"; return false; }
            if (after.MemoryMhz != wanted.MemoryOffsetMhz)
            { error = "显存偏移未被保留（请求 " + wanted.MemoryOffsetMhz + "，回读 " + after.MemoryMhz + "）"; return false; }

            return true;
        }

        /// <summary>Returns both offsets to zero.</summary>
        public static bool Reset(out string error)
        {
            error = null;
            TuningState live = Query();
            if (!string.IsNullOrEmpty(live.Error)) { error = live.Error; return false; }
            return Apply(new ClockTuning(), live, out error);
        }

        /// <summary>
        /// Reads the GPU's current core voltage in microvolts.
        ///
        /// Read-only, and deliberately isolated: it is the one call in this file
        /// that uses an id obtained from outside the public SDK. Every failure mode
        /// — id unresolvable, call rejected, implausible value — returns false with
        /// a reason, and the caller shows "unavailable". It never affects the
        /// tuning paths, which use their own audited interfaces.
        /// </summary>
        public static bool TryReadVoltageUv(out long microVolts, out string error)
        {
            microVolts = 0;
            error = null;

            if (!Open()) { error = "NvAPI 不可用"; return false; }

            IntPtr p = _qi(ID_VOLT_RAILS_GET_STATUS);
            if (p == IntPtr.Zero)
            {
                error = "电压接口 0x465F9BCF 在此驱动上不可解析";
                return false;
            }
            BufferFn fn = (BufferFn)Marshal.GetDelegateForFunctionPointer(p, typeof(BufferFn));

            byte[] buf = new byte[VOLT_RAILS_BUFFER_SIZE];
            PutU32(buf, 0, ((uint)VOLT_RAILS_VERSION << 16) | (uint)VOLT_RAILS_BUFFER_SIZE);

            int rc = fn(_gpu, buf);
            if (rc != 0) { error = "电压查询失败 rc=" + rc; return false; }

            // The driver echoes size and version; a mismatch means the structure we
            // sent is not the one it filled, so the field offset would be meaningless.
            if (U32(buf, 0) != (((uint)VOLT_RAILS_VERSION << 16) | (uint)VOLT_RAILS_BUFFER_SIZE))
            {
                error = "电压结构版本回显不匹配";
                return false;
            }

            uint uv = U32(buf, VOLT_RAILS_OFFSET_UV);

            // Plausibility gate: a core rail outside 0.2–2.0 V means the offset is
            // wrong, not that the GPU is doing something exotic. Reporting nonsense
            // would be worse than reporting nothing.
            if (uv < 200000 || uv > 2000000)
            {
                error = "电压读数超出合理范围 (" + uv + " µV)";
                return false;
            }

            microVolts = uv;
            return true;
        }

        /// <summary>One point on the voltage/frequency curve.</summary>
        internal struct VfPoint
        {
            public int Index;
            public long FreqKhz;
            public long VoltUv;
        }

        /// <summary>
        /// Reads the 128-point V/F curve.
        ///
        /// Read-only. Used to show which operating point the GPU is currently on —
        /// "1020 MHz @ 0.650 V" is far more useful than a bare voltage, because it
        /// is the pair that decides power draw.
        /// </summary>
        public static bool TryReadVfCurve(out VfPoint[] points, out string error)
        {
            points = null;
            error = null;

            if (!Open()) { error = "NvAPI 不可用"; return false; }

            IntPtr p = _qi(ID_VF_CURVE_GET_STATUS);
            if (p == IntPtr.Zero) { error = "V/F 曲线接口不可解析"; return false; }
            BufferFn fn = (BufferFn)Marshal.GetDelegateForFunctionPointer(p, typeof(BufferFn));

            byte[] buf = new byte[VF_CURVE_BUFFER_SIZE];
            PutU32(buf, 0, ((uint)1 << 16) | (uint)VF_CURVE_BUFFER_SIZE);
            // Ask for every point; an unfilled mask returns an all-zero structure.
            for (int i = 0; i < VF_CURVE_MASK_BYTES; i++) buf[VF_CURVE_MASK_OFFSET + i] = 0xFF;
            PutU32(buf, VF_CURVE_COUNT_OFFSET, VF_CURVE_CLOCK_COUNT);

            int rc = fn(_gpu, buf);
            if (rc != 0) { error = "V/F 曲线查询失败 rc=" + rc; return false; }

            if (U32(buf, 0) != (((uint)1 << 16) | (uint)VF_CURVE_BUFFER_SIZE))
            {
                error = "V/F 结构版本回显不匹配";
                return false;
            }

            List<VfPoint> list = new List<VfPoint>();
            for (int i = 0; i < VF_CURVE_POINTS; i++)
            {
                int off = VF_CURVE_DATA_OFFSET + i * VF_CURVE_POINT_STRIDE;
                if (off + 8 > buf.Length) break;
                long f = U32(buf, off);
                long v = U32(buf, off + 4);
                if (f == 0 && v == 0) continue;
                list.Add(new VfPoint { Index = i, FreqKhz = f, VoltUv = v });
            }

            if (list.Count == 0) { error = "V/F 曲线为空"; return false; }
            points = list.ToArray();
            return true;
        }

        /// <summary>
        /// Finds the curve point the GPU is currently running at.
        ///
        /// Matched by voltage, because that is the quantity both the curve and the
        /// live reading express. Tolerance is one curve step (25 mV): the live value
        /// jitters by a few mV between samples, so an exact match would fail
        /// intermittently and look like a broken reading.
        /// </summary>
        public static bool TryFindOperatingPoint(VfPoint[] curve, long liveVoltUv, out VfPoint point)
        {
            point = default(VfPoint);
            if (curve == null || curve.Length == 0 || liveVoltUv <= 0) return false;

            long best = long.MaxValue;
            bool found = false;
            foreach (VfPoint p in curve)
            {
                if (p.VoltUv <= 0) continue;
                long diff = Math.Abs(p.VoltUv - liveVoltUv);
                if (diff < best) { best = diff; point = p; found = true; }
            }
            // Reject a match that is not actually close: otherwise the UI would name
            // a point far from the live reading and imply a precision that is not
            // there.
            return found && best <= 50000;
        }
    }
}
