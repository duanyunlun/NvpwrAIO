using System;
using System.Runtime.InteropServices;
using System.Text;

namespace NvpwrControl
{
    /// <summary>
    /// Kernel-helper interop.
    ///
    /// This is the only part of the application that talks to Nvpwr.sys. The
    /// structures mirror shared/nvpwr_ioctl.h exactly — field order and the
    /// packing matter, because the kernel writes the same layout back. If they
    /// ever diverge the driver rejects the request rather than corrupting state,
    /// but the symptom would be a confusing "status query failed".
    /// </summary>
    internal static class Driver
    {
        public const string DevicePath = @"\\.\Nvpwr";

        // CONTROL codes: CTL_CODE(FILE_DEVICE_UNKNOWN, fn, METHOD_BUFFERED, access)
        private const uint FILE_DEVICE_UNKNOWN = 0x00000022;
        private const uint METHOD_BUFFERED = 0;
        private const uint FILE_READ_ACCESS = 0x0001;
        private const uint FILE_WRITE_ACCESS = 0x0002;

        public static readonly uint IOCTL_STATUS = Ctl(0x800, FILE_READ_ACCESS);
        public static readonly uint IOCTL_SET_POWER = Ctl(0x801, FILE_READ_ACCESS | FILE_WRITE_ACCESS);
        public static readonly uint IOCTL_RESTORE = Ctl(0x802, FILE_READ_ACCESS | FILE_WRITE_ACCESS);

        private static uint Ctl(uint function, uint access)
        {
            return (FILE_DEVICE_UNKNOWN << 16) | (access << 14) | (function << 2) | METHOD_BUFFERED;
        }

        public const uint SET_VERSION = 3;
        public const uint STATUS_VERSION = 8;

        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        public struct SetPowerRequest
        {
            public uint Version;
            public uint TargetMilliwatts;
            public uint Profile;
            public uint MaxMilliwatts;
            public uint Reserved;
        }

        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        public struct Status
        {
            public uint Version;
            public uint State;
            public int LastNtStatus;
            public uint Detail;

            public ulong ModuleBase;
            public uint TimeDateStamp;
            public uint SizeOfImage;

            public uint RegistryCount;
            public uint SelectedIndex;
            public uint GpuId;
            public uint Reserved0;

            public ulong DriverGlobal;
            public ulong GpuTable;
            public ulong MajorObject;
            public ulong PowerRoot;
            public ulong LookupFunction;
            public ulong BoardObject;
            public ulong BoardSetFunction;

            public byte RootInitialized;
            public byte Eligibility;
            public byte AmountActive;
            public byte PolicyKey;
            public uint CtgpTarget;
            public uint PpabAmount;
            public uint LowerBoundary;
            public uint UpperBoundary;
            public uint Aux28;
            public uint Aux2C;

            public byte MaxMode;
            public byte MaxCount;
            public byte MaxSource0;
            public byte CurrentMode;
            public byte CurrentCount;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)]
            public byte[] Reserved1;
            public uint MaxEffective;
            public uint MaxSecondary;
            public uint MaxSource0Value;
            public uint CurrentEffective;
            public uint CurrentF7Value;
            public uint PredictedF7;

            public uint AppliedTarget;
            public uint LastNvStatus;

            public uint OemBaseline;
            public uint ActiveProfile;
            public uint SupportedMin;
            public uint SupportedMax;

            public uint CeilingMax;
            public uint SessionMax;
        }

        // Driver-reported state, mirroring NVPWR_STATE.
        public const uint StateUnknown = 0;
        public const uint StateArmed = 1;
        public const uint StateApplied = 2;
        public const uint StateMixed = 3;
        public const uint StateWrongBuild = 4;
        public const uint StateModuleNotFound = 5;
        public const uint StateGpuNotFound = 6;
        public const uint StateContextInvalid = 7;
        public const uint StatePreconditionNotReady = 8;
        public const uint StateStockBaseline = 9;

        // GPU profiles, mirroring NVPWR_GPU_PROFILE.
        public const uint ProfileUnknown = 0;
        public const uint ProfileRtx5070Ti = 1;
        public const uint ProfileRtx5080 = 2;
        public const uint ProfileRtx5090 = 3;
        public const uint ProfileRtx5050 = 4;
        public const uint ProfileRtx5060 = 5;
        public const uint ProfileRtx5070 = 6;
        public const uint ProfileRtx4090 = 7;
        public const uint ProfileRtx4080 = 8;
        public const uint ProfileRtx4070 = 9;
        public const uint ProfileRtx4060 = 10;
        public const uint ProfileRtx4050 = 11;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateFileW(string name, uint access, uint share,
                                                 IntPtr sa, uint disposition, uint flags, IntPtr template);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool DeviceIoControl(IntPtr device, uint code,
                                                   IntPtr inBuf, uint inSize,
                                                   IntPtr outBuf, uint outSize,
                                                   out uint returned, IntPtr overlapped);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);

        private static IntPtr Open()
        {
            return CreateFileW(DevicePath, 0xC0000000u /* GENERIC_READ|WRITE */, 0,
                               IntPtr.Zero, 3 /* OPEN_EXISTING */, 0, IntPtr.Zero);
        }

        public static bool IsOpenable()
        {
            IntPtr h = Open();
            if (h == new IntPtr(-1)) return false;
            CloseHandle(h);
            return true;
        }

        /// <summary>Reads the live driver status. Returns false with a reason.</summary>
        public static bool QueryStatus(out Status status, out string error)
        {
            status = new Status();
            error = null;

            IntPtr h = Open();
            if (h == new IntPtr(-1))
            {
                error = "无法打开 " + DevicePath + "（驱动未加载）";
                return false;
            }

            int size = Marshal.SizeOf(typeof(Status));
            IntPtr buf = Marshal.AllocHGlobal(size);
            try
            {
                uint returned;
                if (!DeviceIoControl(h, IOCTL_STATUS, IntPtr.Zero, 0, buf, (uint)size, out returned, IntPtr.Zero))
                {
                    error = "状态查询失败 (Win32 " + Marshal.GetLastWin32Error() + ")";
                    return false;
                }
                status = (Status)Marshal.PtrToStructure(buf, typeof(Status));
                return true;
            }
            finally
            {
                Marshal.FreeHGlobal(buf);
                CloseHandle(h);
            }
        }

        /// <summary>
        /// Requests a power target. The driver validates the profile range, clamps
        /// the ceiling to its own compiled limit, and rolls back if NVIDIA's
        /// generator does not converge on the value — so a true return means the
        /// limit is live, not merely accepted.
        /// </summary>
        public static bool SetPower(uint milliwatts, uint ceilingMw, uint profile, out string error)
        {
            error = null;

            IntPtr h = Open();
            if (h == new IntPtr(-1)) { error = "驱动未加载"; return false; }

            SetPowerRequest req = new SetPowerRequest
            {
                Version = SET_VERSION,
                TargetMilliwatts = milliwatts,
                Profile = profile,
                MaxMilliwatts = ceilingMw,
                Reserved = 0
            };

            int size = Marshal.SizeOf(typeof(SetPowerRequest));
            IntPtr buf = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.StructureToPtr(req, buf, false);
                uint returned;
                if (!DeviceIoControl(h, IOCTL_SET_POWER, buf, (uint)size, IntPtr.Zero, 0, out returned, IntPtr.Zero))
                {
                    int e = Marshal.GetLastWin32Error();
                    error = DescribeSetFailure(e);
                    return false;
                }
                return true;
            }
            finally
            {
                Marshal.FreeHGlobal(buf);
                CloseHandle(h);
            }
        }

        public static bool Restore(out string error)
        {
            error = null;

            IntPtr h = Open();
            if (h == new IntPtr(-1)) { error = "驱动未加载"; return false; }

            try
            {
                uint returned;
                if (!DeviceIoControl(h, IOCTL_RESTORE, IntPtr.Zero, 0, IntPtr.Zero, 0, out returned, IntPtr.Zero))
                {
                    error = "恢复出厂失败 (Win32 " + Marshal.GetLastWin32Error() + ")";
                    return false;
                }
                return true;
            }
            finally
            {
                CloseHandle(h);
            }
        }

        /// <summary>
        /// Turns the driver's NTSTATUS into something actionable. The two cases
        /// that matter are "you asked for something the hardware will not take"
        /// and "the process is not elevated", because both are user-fixable and
        /// neither is obvious from the raw code.
        /// </summary>
        private static string DescribeSetFailure(int win32)
        {
            const int ERROR_ACCESS_DENIED = 5;
            const int ERROR_INVALID_PARAMETER = 87;
            const int ERROR_GEN_FAILURE = 31;

            switch (win32)
            {
                case ERROR_ACCESS_DENIED:
                    return "访问被拒绝 —— 请以管理员身份运行。";
                case ERROR_INVALID_PARAMETER:
                    return "驱动拒绝了该目标值：可能超出本机型允许范围，或 OEM 基线不匹配。";
                case ERROR_GEN_FAILURE:
                    return "驱动回滚了本次修改：NVIDIA 生成器未能收敛到该目标值。这通常意味着" +
                           "硬件不接受这个功耗（供电或 EC 上限）。";
                default:
                    return "设置失败 (Win32 " + win32 + ")";
            }
        }

        public static string StateText(uint state)
        {
            switch (state)
            {
                case StateStockBaseline: return "出厂状态";
                case StateApplied: return "已生效";
                case StateArmed: return "过渡中";
                case StateMixed: return "状态混合 —— 建议恢复出厂";
                case StateWrongBuild: return "不支持的 NVIDIA 驱动版本";
                case StateModuleNotFound: return "未加载 nvlddmkm.sys";
                case StateGpuNotFound: return "未找到 NVIDIA 显卡";
                case StateContextInvalid: return "驱动上下文无效";
                case StatePreconditionNotReady: return "前提条件未满足";
                default: return "未知";
            }
        }

        public static string ProfileName(uint profile)
        {
            switch (profile)
            {
                case ProfileRtx5070Ti: return "RTX 5070 Ti Laptop";
                case ProfileRtx5080: return "RTX 5080 Laptop";
                case ProfileRtx5090: return "RTX 5090 Laptop";
                case ProfileRtx5050: return "RTX 5050 Laptop";
                case ProfileRtx5060: return "RTX 5060 Laptop";
                case ProfileRtx5070: return "RTX 5070 Laptop";
                case ProfileRtx4090: return "RTX 4090 Laptop";
                case ProfileRtx4080: return "RTX 4080 Laptop";
                case ProfileRtx4070: return "RTX 4070 Laptop";
                case ProfileRtx4060: return "RTX 4060 Laptop";
                case ProfileRtx4050: return "RTX 4050 Laptop";
                default: return "未识别";
            }
        }

        /// <summary>
        /// Detects the profile from the adapter name, mirroring the driver's own
        /// matching order. The kernel re-validates independently, so a wrong guess
        /// here only produces a clearer error, never an unsafe write.
        /// </summary>
        public static uint DetectProfile(string name)
        {
            if (string.IsNullOrEmpty(name)) return ProfileUnknown;
            // 5070 Ti must be tested before the generic 5070.
            if (name.Contains("RTX 5070 Ti")) return ProfileRtx5070Ti;
            if (name.Contains("RTX 5050")) return ProfileRtx5050;
            if (name.Contains("RTX 5060")) return ProfileRtx5060;
            if (name.Contains("RTX 5070")) return ProfileRtx5070;
            if (name.Contains("RTX 5080")) return ProfileRtx5080;
            if (name.Contains("RTX 5090")) return ProfileRtx5090;
            if (name.Contains("RTX 4090")) return ProfileRtx4090;
            if (name.Contains("RTX 4080")) return ProfileRtx4080;
            if (name.Contains("RTX 4070")) return ProfileRtx4070;
            if (name.Contains("RTX 4060")) return ProfileRtx4060;
            if (name.Contains("RTX 4050")) return ProfileRtx4050;
            return ProfileUnknown;
        }

        /// <summary>
        /// Acceptable target window for a profile, mirroring the driver's
        /// IsSupportedTarget so the UI can refuse impossible input before it
        /// reaches the kernel and can show the bound while typing. The driver
        /// remains the authority.
        /// </summary>
        public static void ProfileRange(uint profile, uint ceilingMw, out uint loMw, out uint hiMw)
        {
            uint ceiling = (ceilingMw == 0) ? 350000u : ceilingMw;
            switch (profile)
            {
                case ProfileRtx5050:
                case ProfileRtx5060:
                case ProfileRtx5070: loMw = 120000; hiMw = 140000; break;
                case ProfileRtx5070Ti: loMw = 145000; hiMw = ceiling; break;
                case ProfileRtx5080:
                case ProfileRtx5090: loMw = 175000; hiMw = ceiling; break;
                case ProfileRtx4090: loMw = 150000; hiMw = ceiling; break;
                case ProfileRtx4080: loMw = 150000; hiMw = ceiling; break;
                case ProfileRtx4070:
                case ProfileRtx4060: loMw = 120000; hiMw = 150000; break;
                case ProfileRtx4050: loMw = 115000; hiMw = 140000; break;
                default: loMw = 0; hiMw = 0; break;
            }
        }

        public const uint DefaultCeilingMw = 350000;
    }
}
