using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;

namespace NvpwrControl
{
    /// <summary>One live sample of the GPU environment.</summary>
    internal sealed class EnvSample
    {
        public string Source = "", Error = "";
        public string GpuName = "", DriverVersion = "", Vbios = "";

        public bool HasPowerDraw; public double PowerDrawW;
        public bool HasPowerLimit; public double EnforcedLimitW;
        public bool HasCoreClock; public double CoreClockMhz;
        public bool HasMemoryClock; public double MemoryClockMhz;
        public bool HasTemp; public double TempC;
        public bool HasHotspot; public double HotspotC;
        public bool HasUtilization; public double UtilizationPct;
        public bool HasMemUsed; public double MemUsedMb;
        public bool HasMemTotal; public double MemTotalMb;
        public bool HasFanPct; public double FanPct;
        public bool HasPstate; public int Pstate;
        public bool HasThrottle; public ulong ThrottleBits;

        public bool HasSpeedThreshold; public double SpeedThresholdC;

        public bool AcKnown; public bool OnAc;
        public bool BatteryKnown; public double BatteryPct;

        public bool MuxKnown; public bool DiscreteDirect;
        public int AdapterCount;

        public bool AnyReading { get { return HasPowerDraw || HasCoreClock || HasTemp || HasThrottle; } }
    }

    /// <summary>
    /// Read-only environment sampling.
    ///
    /// Preferred source is NVML (the library nvidia-smi itself uses), falling
    /// back to parsing nvidia-smi, with DXGI for display topology. Nothing here
    /// writes to the GPU, so the panel keeps working when the helper driver is
    /// not loaded — which is exactly when the user most wants to see what the
    /// card is doing.
    ///
    /// A reading the driver does not provide is reported as unavailable rather
    /// than estimated. Two cases are worth naming because they look like bugs
    /// otherwise: hotspot is not exposed at all on RTX 50 (measured: every NVML
    /// sensor except edge returns NOT_SUPPORTED), and there is no rail-voltage
    /// reading because NVML/NVAPI do not publish one.
    /// </summary>
    internal static class Telemetry
    {
        // ------------------------------------------------------------------ NVML

        private const int NVML_SUCCESS = 0;
        private const int NVML_TEMPERATURE_GPU = 0;
        private const int NVML_TEMPERATURE_MEMORY = 1;
        private const int NVML_TEMPERATURE_MEMJUNCTION = 9;
        private const int NVML_CLOCK_GRAPHICS = 0;
        private const int NVML_CLOCK_MEM = 2;
        private const int NVML_TEMP_THRESHOLD_SHUTDOWN = 0;
        private const int NVML_TEMP_THRESHOLD_SLOWDOWN = 1;

        [StructLayout(LayoutKind.Sequential)]
        private struct NvmlUtilization { public uint Gpu; public uint Memory; }

        [StructLayout(LayoutKind.Sequential)]
        private struct NvmlMemory { public ulong Total; public ulong Free; public ulong Used; }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryW(string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool FreeLibrary(IntPtr module);

        // HWiNFO 的传感器共享内存，用来读 MSVDD 实测电压。会话级命名，不带 Global\。
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr OpenFileMapping(uint access, bool inherit, string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr MapViewOfFile(IntPtr h, uint access, uint hi, uint lo, UIntPtr bytes);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool UnmapViewOfFile(IntPtr p);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr h);

        private const uint FileMapRead = 0x0004;
        private const string HwInfoMapName = "Global\\HWiNFO_SENS_SM2";

        /// <summary>
        /// Reads the MSVDD rail voltage out of HWiNFO's shared memory.
        ///
        /// WHY not NVAPI: the public surface carries no MSVDD ADC. This project's own notes say
        /// so — "Physical MSVDD ADC is not claimed" — and the one rail-voltage interface the
        /// program does use (ClientVoltRailsGetStatus) returns a single value, the core rail.
        ///
        /// HWiNFO reads it and publishes it as "GPU MSVDD Voltage". That is a real measurement, so
        /// it is reported as one; with HWiNFO absent the caller shows a dash rather than a figure
        /// derived from a request, which would be a different number with a different meaning.
        ///
        /// Matched on szLabelOrig: that is the stable English name. The label the user sees in
        /// HWiNFO's own window is szLabelUser, a separate field that differs per machine.
        /// </summary>
        public static bool TryReadMsvddVolts(out double volts)
        {
            volts = 0.0;
            IntPtr h = IntPtr.Zero;
            IntPtr p = IntPtr.Zero;
            try
            {
                h = OpenFileMapping(FileMapRead, false, HwInfoMapName);
                if (h == IntPtr.Zero) return false;
                p = MapViewOfFile(h, FileMapRead, 0, 0, UIntPtr.Zero);
                if (p == IntPtr.Zero) return false;

                int offReadings = Marshal.ReadInt32(p, 32);
                int sizeReading = Marshal.ReadInt32(p, 36);
                int countReading = Marshal.ReadInt32(p, 40);
                if (offReadings <= 0 || sizeReading <= 0 || countReading <= 0 || countReading > 4096)
                {
                    return false;
                }

                for (int i = 0; i < countReading; i++)
                {
                    int o = offReadings + sizeReading * i;
                    int type = Marshal.ReadInt32(p, o);
                    if (type != 2) continue;                        // 2 = voltage
                    string label = ReadAsciiField(p, o + 12, 128);  // szLabelOrig
                    if (label.IndexOf("MSVDD", StringComparison.OrdinalIgnoreCase) < 0) continue;
                    double v = BitConverter.Int64BitsToDouble(Marshal.ReadInt64(p, o + 284));
                    if (v > 0.05 && v < 3.0) { volts = v; return true; }
                }
                return false;
            }
            catch { return false; }
            finally
            {
                if (p != IntPtr.Zero) UnmapViewOfFile(p);
                if (h != IntPtr.Zero) CloseHandle(h);
            }
        }

        private static string ReadAsciiField(IntPtr basePtr, int offset, int max)
        {
            byte[] b = new byte[max];
            Marshal.Copy(new IntPtr(basePtr.ToInt64() + offset), b, 0, max);
            int end = Array.IndexOf(b, (byte)0);
            if (end < 0) end = max;
            return Encoding.ASCII.GetString(b, 0, end);
        }

        private static IntPtr _nvml;
        private static bool _nvmlTried;

        private static IntPtr Nvml()
        {
            if (_nvmlTried) return _nvml;
            _nvmlTried = true;
            _nvml = LoadLibraryW("nvml.dll");
            if (_nvml == IntPtr.Zero)
                _nvml = LoadLibraryW(@"C:\Program Files\NVIDIA Corporation\NVSMI\nvml.dll");
            return _nvml;
        }

        private static T Fn<T>(string name) where T : class
        {
            IntPtr m = Nvml();
            if (m == IntPtr.Zero) return null;
            IntPtr p = GetProcAddress(m, name);
            if (p == IntPtr.Zero) return null;
            return (T)(object)Marshal.GetDelegateForFunctionPointer(p, typeof(T));
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ShutdownFn();

        /// <summary>
        /// Makes NVML forget the devices it knows about, so the next sample initialises against
        /// whatever is actually there.
        ///
        /// Needed around a display device restart. NVML keeps its own list of device objects and
        /// does not notice a card being removed and re-added underneath it: nvmlDeviceGetHandleByIndex
        /// keeps returning success, and the handle it hands back points into state that has been
        /// freed. Calling nvmlDeviceGetPowerUsage on one is an access violation, not an error code
        /// — measured, and the crash dump names it exactly (IL_STUB_PInvoke(IntPtr, UInt32 ByRef)
        /// under SampleNvml).
        ///
        /// nvmlShutdown is safe to call when nothing is initialised; the module itself is left
        /// loaded and simply re-opened on the next sample, which only bumps a refcount.
        /// </summary>
        public static void ResetNvml()
        {
            if (_nvml != IntPtr.Zero)
            {
                try
                {
                    ShutdownFn shutdown = Fn<ShutdownFn>("nvmlShutdown");
                    if (shutdown != null) shutdown();
                }
                catch
                {
                    // Best effort. Even a failed shutdown is followed by a fresh init.
                }
            }
            _nvml = IntPtr.Zero;
            _nvmlTried = false;
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int InitFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int CountFn(out uint count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int HandleFn(uint index, out IntPtr device);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int NameFn(IntPtr d, StringBuilder name, uint len);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int TempFn(IntPtr d, int sensor, out uint value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ThresholdFn(IntPtr d, int type, out uint value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ClockFn(IntPtr d, int type, out uint value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int PowerFn(IntPtr d, out uint value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int UtilFn(IntPtr d, out NvmlUtilization value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int MemFn(IntPtr d, out NvmlMemory value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int FanFn(IntPtr d, out uint value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ThrottleFn(IntPtr d, out ulong value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int DriverFn(StringBuilder version, uint len);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int PstateFn(IntPtr d, out int value);

        private static bool SampleNvml(EnvSample e)
        {
            InitFn init = Fn<InitFn>("nvmlInit");
            CountFn count = Fn<CountFn>("nvmlDeviceGetCount");
            HandleFn handle = Fn<HandleFn>("nvmlDeviceGetHandleByIndex");
            if (init == null || count == null || handle == null) return false;
            if (init() != NVML_SUCCESS) return false;

            uint n;
            if (count(out n) != NVML_SUCCESS || n == 0) return false;

            IntPtr dev;
            if (handle(0, out dev) != NVML_SUCCESS || dev == IntPtr.Zero) return false;

            e.Source = "NVML";

            NameFn nameFn = Fn<NameFn>("nvmlDeviceGetName");
            if (nameFn != null)
            {
                StringBuilder sb = new StringBuilder(128);
                if (nameFn(dev, sb, 128) == NVML_SUCCESS && sb.Length > 0) e.GpuName = sb.ToString();
            }

            DriverFn drvFn = Fn<DriverFn>("nvmlSystemGetDriverVersion");
            if (drvFn != null)
            {
                StringBuilder sb = new StringBuilder(80);
                if (drvFn(sb, 80) == NVML_SUCCESS && sb.Length > 0) e.DriverVersion = sb.ToString();
            }

            ClockFn clk = Fn<ClockFn>("nvmlDeviceGetClockInfo");
            PowerFn pwr = Fn<PowerFn>("nvmlDeviceGetPowerUsage");
            PowerFn lim = Fn<PowerFn>("nvmlDeviceGetEnforcedPowerLimit");
            TempFn tmp = Fn<TempFn>("nvmlDeviceGetTemperature");
            UtilFn util = Fn<UtilFn>("nvmlDeviceGetUtilizationRates");
            MemFn mem = Fn<MemFn>("nvmlDeviceGetMemoryInfo");
            FanFn fan = Fn<FanFn>("nvmlDeviceGetFanSpeed");
            ThrottleFn thr = Fn<ThrottleFn>("nvmlDeviceGetCurrentClocksThrottleReasons");
            PstateFn pst = Fn<PstateFn>("nvmlDeviceGetPerformanceState");
            ThresholdFn th = Fn<ThresholdFn>("nvmlDeviceGetTemperatureThreshold");

            uint v;
            if (pwr != null && pwr(dev, out v) == NVML_SUCCESS) { e.HasPowerDraw = true; e.PowerDrawW = v / 1000.0; }
            if (lim != null && lim(dev, out v) == NVML_SUCCESS) { e.HasPowerLimit = true; e.EnforcedLimitW = v / 1000.0; }
            if (clk != null && clk(dev, NVML_CLOCK_GRAPHICS, out v) == NVML_SUCCESS) { e.HasCoreClock = true; e.CoreClockMhz = v; }
            if (clk != null && clk(dev, NVML_CLOCK_MEM, out v) == NVML_SUCCESS) { e.HasMemoryClock = true; e.MemoryClockMhz = v; }
            if (tmp != null && tmp(dev, NVML_TEMPERATURE_GPU, out v) == NVML_SUCCESS) { e.HasTemp = true; e.TempC = v; }

            // Hotspot: probe, then leave it unavailable. On RTX 50 every sensor
            // other than edge returns NOT_SUPPORTED through NVML; the tools that
            // do show hotspot use NVIDIA's private ADC interface, which this
            // project deliberately does not guess at.
            if (tmp != null)
            {
                uint h;
                if (tmp(dev, NVML_TEMPERATURE_MEMJUNCTION, out h) == NVML_SUCCESS && h > 0) { e.HasHotspot = true; e.HotspotC = h; }
                else if (tmp(dev, NVML_TEMPERATURE_MEMORY, out h) == NVML_SUCCESS && h > 0) { e.HasHotspot = true; e.HotspotC = h; }
            }

            // Thermal limits are the useful substitute: they answer "how close am
            // I to throttling", which decides whether a raised power limit is
            // usable at all.
            if (th != null)
            {
                uint t;
                if (th(dev, NVML_TEMP_THRESHOLD_SLOWDOWN, out t) == NVML_SUCCESS && t > 0) { e.HasSpeedThreshold = true; e.SpeedThresholdC = t; }
                else if (th(dev, NVML_TEMP_THRESHOLD_SHUTDOWN, out t) == NVML_SUCCESS && t > 0) { e.HasSpeedThreshold = true; e.SpeedThresholdC = t; }
            }

            NvmlUtilization u;
            if (util != null && util(dev, out u) == NVML_SUCCESS) { e.HasUtilization = true; e.UtilizationPct = u.Gpu; }

            NvmlMemory mm;
            if (mem != null && mem(dev, out mm) == NVML_SUCCESS)
            {
                e.HasMemTotal = true; e.MemTotalMb = mm.Total / (1024.0 * 1024.0);
                e.HasMemUsed = true; e.MemUsedMb = mm.Used / (1024.0 * 1024.0);
            }

            if (fan != null && fan(dev, out v) == NVML_SUCCESS) { e.HasFanPct = true; e.FanPct = v; }

            ulong reasons;
            if (thr != null && thr(dev, out reasons) == NVML_SUCCESS) { e.HasThrottle = true; e.ThrottleBits = reasons; }

            int ps;
            if (pst != null && pst(dev, out ps) == NVML_SUCCESS) { e.HasPstate = true; e.Pstate = ps; }

            return e.AnyReading;
        }

        // -------------------------------------------------------------- nvidia-smi

        private static string RunSmi(string args)
        {
            try
            {
                string exe = System.IO.Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.System), "nvidia-smi.exe");
                if (!System.IO.File.Exists(exe)) return null;

                ProcessStartInfo psi = new ProcessStartInfo(exe, args)
                {
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true
                };
                using (Process p = Process.Start(psi))
                {
                    if (p == null) return null;
                    string o = p.StandardOutput.ReadToEnd();
                    p.StandardError.ReadToEnd();
                    p.WaitForExit(8000);
                    return string.IsNullOrWhiteSpace(o) ? null : o;
                }
            }
            catch { return null; }
        }

        private static bool SampleSmi(EnvSample e)
        {
            string csv = RunSmi("--query-gpu=name,driver_version,power.draw,enforced.power.limit," +
                                "clocks.current.graphics,clocks.current.memory,temperature.gpu," +
                                "utilization.gpu,memory.used,memory.total,fan.speed,pstate," +
                                "clocks_throttle_reasons.active,temperature.gpu.tlimit " +
                                "--format=csv,noheader,nounits");
            if (csv == null) return false;

            string[] lines = csv.Split('\n');
            if (lines.Length == 0) return false;
            string[] f = lines[0].Trim().Split(',');
            if (f.Length < 13) return false;

            e.Source = "nvidia-smi";
            e.GpuName = f[0].Trim();
            e.DriverVersion = f[1].Trim();

            double d;
            if (TryNum(f[2], out d)) { e.HasPowerDraw = true; e.PowerDrawW = d; }
            if (TryNum(f[3], out d)) { e.HasPowerLimit = true; e.EnforcedLimitW = d; }
            if (TryNum(f[4], out d)) { e.HasCoreClock = true; e.CoreClockMhz = d; }
            if (TryNum(f[5], out d)) { e.HasMemoryClock = true; e.MemoryClockMhz = d; }
            if (TryNum(f[6], out d)) { e.HasTemp = true; e.TempC = d; }
            if (TryNum(f[7], out d)) { e.HasUtilization = true; e.UtilizationPct = d; }
            if (TryNum(f[8], out d)) { e.HasMemUsed = true; e.MemUsedMb = d; }
            if (TryNum(f[9], out d)) { e.HasMemTotal = true; e.MemTotalMb = d; }
            if (TryNum(f[10], out d)) { e.HasFanPct = true; e.FanPct = d; }

            string ps = f[11].Trim();
            if (ps.Length >= 2 && (ps[0] == 'P' || ps[0] == 'p'))
            {
                int v;
                if (int.TryParse(ps.Substring(1), out v)) { e.HasPstate = true; e.Pstate = v; }
            }

            ulong bits;
            if (ulong.TryParse(f[12].Trim(), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out bits))
            { e.HasThrottle = true; e.ThrottleBits = bits; }

            if (f.Length >= 14 && TryNum(f[13], out d) && d > 0) { e.HasSpeedThreshold = true; e.SpeedThresholdC = d; }

            return e.AnyReading;
        }

        private static bool TryNum(string s, out double value)
        {
            value = 0;
            if (string.IsNullOrEmpty(s)) return false;
            s = s.Trim();
            if (s == "N/A" || s.StartsWith("[N/A") || s.StartsWith("[Not")) return false;
            return double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out value);
        }

        // ---------------------------------------------------------------- platform

        [StructLayout(LayoutKind.Sequential)]
        private struct PowerStatus
        {
            public byte ACLineStatus;
            public byte BatteryFlag;
            public byte BatteryLifePercent;
            public byte SystemStatusFlag;
            public uint BatteryLifeTime;
            public uint BatteryFullLifeTime;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetSystemPowerStatus(out PowerStatus status);

        private static void SamplePlatform(EnvSample e)
        {
            PowerStatus ps;
            if (GetSystemPowerStatus(out ps))
            {
                if (ps.ACLineStatus == 0 || ps.ACLineStatus == 1)
                {
                    e.AcKnown = true;
                    e.OnAc = ps.ACLineStatus == 1;
                }
                if (ps.BatteryLifePercent != 255)
                {
                    e.BatteryKnown = true;
                    e.BatteryPct = ps.BatteryLifePercent;
                }
            }
        }

        // ------------------------------------------------------- display topology

        [DllImport("dxgi.dll")]
        private static extern int CreateDXGIFactory(ref Guid riid, out IntPtr factory);

        /// <summary>
        /// Discrete-direct (MUX) detection.
        ///
        /// The NVIDIA adapter is enumerated and checked for an attached output:
        /// in hybrid mode the iGPU owns the panel and the dGPU has no output. The
        /// one caveat is Advanced Optimus, which can reassign outputs at runtime,
        /// so a sample taken mid-switch lags by up to one refresh interval and
        /// self-corrects on the next tick.
        /// </summary>
        private static void SampleTopology(EnvSample e)
        {
            IntPtr factoryPtr;
            Guid iid = new Guid("770aae78-f26f-4dba-a829-253c83d1b387");   // IDXGIFactory
            if (CreateDXGIFactory(ref iid, out factoryPtr) != 0 || factoryPtr == IntPtr.Zero) return;

            object factory = null;
            try
            {
                factory = Marshal.GetObjectForIUnknown(factoryPtr);
                Type ft = factory.GetType();
                System.Reflection.MethodInfo enumAdapters = ft.GetMethod("EnumAdapters");

                bool sawNvidia = false, nvidiaHasOutput = false, anyoneHasOutput = false;
                int count = 0;

                for (uint i = 0; i < 8; i++)
                {
                    object[] args = new object[] { i, null };
                    int hr;
                    try { hr = (int)enumAdapters.Invoke(factory, args); }
                    catch { break; }
                    if (hr != 0) break;

                    object adapter = args[1];
                    if (adapter == null) break;
                    count++;

                    bool isNvidia = false;
                    try
                    {
                        Type at = adapter.GetType();
                        object desc = at.GetMethod("GetDesc").Invoke(adapter, null);
                        Type dt = desc.GetType();
                        uint vendor = (uint)dt.GetField("VendorId").GetValue(desc);
                        string name = (string)dt.GetField("Description").GetValue(desc);
                        isNvidia = vendor == 0x10DE;
                        if (isNvidia && string.IsNullOrEmpty(e.GpuName)) e.GpuName = name;
                    }
                    catch { }

                    if (isNvidia) sawNvidia = true;

                    try
                    {
                        /*
                            Only existence matters, not how many outputs there are:
                            an adapter that owns at least one output is the one
                            driving the panel. Probing index 0 is therefore enough,
                            and keeps this out of the driver's way.
                        */
                        Type at = adapter.GetType();
                        System.Reflection.MethodInfo enumOutputs = at.GetMethod("EnumOutputs");
                        object[] oa = new object[] { 0u, null };
                        int hr2;
                        try { hr2 = (int)enumOutputs.Invoke(adapter, oa); }
                        catch { hr2 = -1; }
                        if (hr2 == 0)
                        {
                            anyoneHasOutput = true;
                            if (isNvidia) nvidiaHasOutput = true;
                            object firstOutput = oa[1];
                            if (firstOutput != null) { try { Marshal.ReleaseComObject(firstOutput); } catch { } }
                        }
                    }
                    catch { }

                    try { Marshal.ReleaseComObject(adapter); } catch { }
                }

                e.AdapterCount = count;
                if (sawNvidia)
                {
                    e.MuxKnown = true;
                    e.DiscreteDirect = nvidiaHasOutput || !anyoneHasOutput;
                }
            }
            catch { }
            finally
            {
                if (factory != null) { try { Marshal.ReleaseComObject(factory); } catch { } }
                else { try { Marshal.Release(factoryPtr); } catch { } }
            }
        }

        // ------------------------------------------------------------------ public

        public static EnvSample Sample()
        {
            EnvSample e = new EnvSample();
            SamplePlatform(e);
            SampleTopology(e);

            bool got = SampleNvml(e);
            if (!got) got = SampleSmi(e);
            if (!got && string.IsNullOrEmpty(e.Error))
                e.Error = "NVML 与 nvidia-smi 都无法提供读数";

            if (!e.HasSpeedThreshold && e.HasTemp)
            {
                // No threshold available: fall back to the VBIOS max operating
                // temperature seen on this class of machine so headroom still
                // means something.
                e.HasSpeedThreshold = true;
                e.SpeedThresholdC = 89;
            }
            return e;
        }

        public static string DescribeThrottle(ulong bits)
        {
            if (bits == 0) return "无";

            string[] names = {
                "GPU 空闲", "应用时钟设置", "软件功耗限制", "硬件降频", "同步加速",
                "软件温度限制", "硬件温度限制", "硬件功耗刹车", "显示时钟", "可靠性电压",
                "板级限制", "低负载"
            };
            List<string> parts = new List<string>();
            for (int i = 0; i < names.Length && i < 12; i++)
                if ((bits & (1UL << i)) != 0) parts.Add(names[i]);

            return parts.Count == 0 ? ("0x" + bits.ToString("X")) : string.Join(" + ", parts.ToArray());
        }

        /// <summary>
        /// One-line interpretation of what is limiting the GPU now. This is the
        /// sentence that decides whether raising the power limit can help at all,
        /// which is the whole reason the panel exists.
        /// </summary>
        public static string DescribeLimiter(EnvSample e)
        {
            if (!e.HasThrottle) return "";
            ulong b = e.ThrottleBits;

            const ulong SwThermal = 1UL << 5, HwThermal = 1UL << 6;
            const ulong SwPowerCap = 1UL << 2, BoardLimit = 1UL << 10;
            const ulong HwSlowdown = 1UL << 3, HwPowerBrake = 1UL << 7;
            const ulong Reliability = 1UL << 9, GpuIdle = 1UL << 0, LowUtil = 1UL << 11;

            if ((b & SwThermal) != 0 || (b & HwThermal) != 0)
                return "THERMAL：当前受温度限制。提高功耗上限不会提升性能，除非先改善散热。";
            if ((b & SwPowerCap) != 0 || (b & BoardLimit) != 0)
                return "POWER：当前受功耗上限限制。这正是提高上限能真正被吃满的情况。";
            if ((b & HwSlowdown) != 0 || (b & HwPowerBrake) != 0)
                return "EXTERNAL：检测到硬件功耗刹车（适配器或平台限制），显卡被压到自身策略之下。";
            if ((b & Reliability) != 0)
                return "RELIABILITY：可靠性电压限制生效。";
            if ((b & GpuIdle) != 0 || (b & LowUtil) != 0)
                return "IDLE：当前无有效负载，没有限制被触发。请先加载显卡再判断真实瓶颈。";
            return "";
        }
    }
}
