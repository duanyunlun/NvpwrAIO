using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;

namespace NvpwrControl
{
    /// <summary>
    /// Driver Signature Enforcement, toggled through EfiGuard's EfiDSEFix.
    ///
    /// Why this exists at all: Nvpwr.sys is self-signed, and Windows will not load it with DSE
    /// enforced. The alternative, bcdedit /set testsigning on, is a boot configuration — it
    /// needs a reboot, puts a watermark on the desktop and leaves a flag that anti-cheat
    /// software reads directly. EfiGuard patches the kernel during boot instead, so this can
    /// turn DSE off and back on at run time and leave nothing behind.
    ///
    /// Both directions matter. DisableDse is only ever called immediately before loading the
    /// driver, and EnableDse immediately after — the window is about a second wide.
    /// </summary>
    internal static class DseControl
    {
        /// <summary>
        /// EfiDSEFix.exe, expected beside the application.
        ///
        /// It must be the build from EfiGuard commit 60a6a57 or later. Earlier builds cannot
        /// find g_CiOptions on current Windows kernels and answer every request with
        /// STATUS_NOT_FOUND — measured against the binary shipped with the reference release:
        /// -r returned 0xC0000225, while a build from that commit returned success.
        /// </summary>
        public static string ExePath
        {
            get { return Path.Combine(Service.ExePath, "EfiDSEFix.exe"); }
        }

        public static bool IsAvailable()
        {
            return File.Exists(ExePath);
        }

        /// <summary>
        /// True when the EfiGuard SetVariable hook answers, i.e. this boot went through
        /// EfiGuard. Everything else depends on it, so nothing is attempted when it is false.
        /// </summary>
        public static bool IsBooted()
        {
            return Run("-c", 15000, out _);
        }

        public static bool Disable(out string error)
        {
            bool ok = Run("-d", 20000, out error);
            if (!ok && string.IsNullOrEmpty(error))
            {
                error = "EfiDSEFix -d 失败。若提示找不到 g_CiOptions，说明 EfiDSEFix 版本过旧。";
            }
            return ok;
        }

        public static bool Enable(out string error)
        {
            return Run("-e", 20000, out error);
        }

        private static bool Run(string arguments, int timeoutMs, out string output)
        {
            output = null;
            if (!IsAvailable())
            {
                output = "找不到 EfiDSEFix.exe：" + ExePath;
                return false;
            }
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo
                {
                    FileName = ExePath,
                    Arguments = arguments,
                    UseShellExecute = false,
                    CreateNoWindow = true,

                    // EfiDSEFix writes to the console with WriteConsoleW, which bypasses stdout
                    // redirection entirely. Capturing yields nothing, so the exit code is the
                    // only signal available — which is why every call is judged on it.
                    RedirectStandardOutput = false,
                    RedirectStandardError = false
                };
                using (Process p = Process.Start(psi))
                {
                    if (p == null)
                    {
                        output = "无法启动 EfiDSEFix.exe";
                        return false;
                    }
                    if (!p.WaitForExit(timeoutMs))
                    {
                        try { p.Kill(); } catch { }
                        output = "EfiDSEFix " + arguments + " 超时（" + timeoutMs + " ms）";
                        return false;
                    }
                    if (p.ExitCode != 0)
                    {
                        output = "EfiDSEFix " + arguments + " 退出码 0x" +
                                 ((uint)p.ExitCode).ToString("X8", CultureInfo.InvariantCulture);
                        return false;
                    }
                    return true;
                }
            }
            catch (Exception ex)
            {
                output = "EfiDSEFix 调用失败：" + ex.Message;
                return false;
            }
        }
    }

    /// <summary>
    /// Restarts the NVIDIA display device through PnP.
    ///
    /// This is the only way to get the factory power wall back without rebooting. The wall is
    /// held in nvlddmkm's runtime memory, and disabling then re-enabling the device reloads that
    /// driver from scratch — measured: a rail sitting at 210 W returned to 175 W after the
    /// cycle.
    ///
    /// It matters because the kernel helper identifies the factory baseline by recognising a
    /// stock-looking limit. Once the limit has been changed, a freshly loaded helper can no
    /// longer tell the changed value from a factory one and refuses to work at all
    /// (OemBaseline reads zero). Restarting the device puts the value back where the helper can
    /// recognise it.
    ///
    /// Costs a few seconds of black screen, so it is only done when the wall is not already at
    /// the factory value — which is the case at boot, and not the case after a change.
    /// </summary>
    internal static class GpuDevice
    {
        /// <summary>Watts the GPU currently allows, straight from NVML. Negative if unknown.</summary>
        public static int CurrentWallW()
        {
            try
            {
                EnvSample e = Telemetry.Sample();
                if (e.HasPowerLimit && e.EnforcedLimitW > 0.0)
                {
                    return (int)Math.Round(e.EnforcedLimitW);
                }
            }
            catch { }
            return -1;
        }

        public static bool Restart(out string error)
        {
            error = null;

            // PowerShell is used rather than the SetupDi APIs: Disable-PnpDevice/Enable-PnpDevice
            // already do the device-install dance correctly (removing and re-adding the stack),
            // and reimplementing it with DIF_PROPERTYCHANGE is a lot of surface for a routine
            // that runs rarely and only when something has already gone sideways.
            string script =
                "$ErrorActionPreference='Stop';" +
                "$d=@(Get-PnpDevice -Class Display -PresentOnly | " +
                "Where-Object { $_.InstanceId -like 'PCI\\VEN_10DE*' -and $_.Status -ne 'Unknown' });" +
                "if($d.Count -ne 1){ throw \"检测到 $($d.Count) 块 NVIDIA 显卡，预期 1 块\" };" +
                "Disable-PnpDevice -InstanceId $d[0].InstanceId -Confirm:$false;" +
                "Start-Sleep -Seconds 2;" +
                "Enable-PnpDevice -InstanceId $d[0].InstanceId -Confirm:$false;" +
                "Start-Sleep -Seconds 3;";

            try
            {
                ProcessStartInfo psi = new ProcessStartInfo
                {
                    FileName = "powershell.exe",
                    Arguments = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" +
                                script.Replace("\"", "\\\"") + "\"",
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true
                };
                using (Process p = Process.Start(psi))
                {
                    if (p == null)
                    {
                        error = "无法启动 PowerShell";
                        return false;
                    }
                    string stdout = p.StandardOutput.ReadToEnd();
                    string stderr = p.StandardError.ReadToEnd();
                    if (!p.WaitForExit(60000))
                    {
                        try { p.Kill(); } catch { }
                        error = "重启显卡设备超时";
                        return false;
                    }
                    if (p.ExitCode != 0)
                    {
                        error = "重启显卡设备失败：" + (string.IsNullOrWhiteSpace(stderr) ? stdout : stderr);
                        return false;
                    }
                }

                // The device comes back asynchronously; give NVML a moment to see the new stack.
                for (int i = 0; i < 20; i++)
                {
                    if (CurrentWallW() > 0)
                    {
                        return true;
                    }
                    System.Threading.Thread.Sleep(500);
                }
                error = "显卡设备已重启，但功耗墙读数仍未恢复";
                return false;
            }
            catch (Exception ex)
            {
                error = "重启显卡设备异常：" + ex.Message;
                return false;
            }
        }
    }

    /// <summary>
    /// Loading the kernel helper, applying a power target, and putting everything back.
    ///
    /// The shape of the whole thing is dictated by two facts established by measurement:
    ///
    ///   DSE is only consulted when a driver is loaded, so it can be re-enabled the moment the
    ///   service reports RUNNING. It is never left off.
    ///
    ///   The helper identifies the factory baseline by recognising a stock-looking limit, and
    ///   refuses to do anything if it cannot. So it must be loaded while the rail is still at
    ///   the factory value. Once a target has been applied, the only way back to a loadable
    ///   state is restarting the display device.
    ///
    /// Everything that turns something off turns it back on in a finally block, so a failure at
    /// any step cannot leave the machine with signature enforcement disabled or a stray driver.
    /// </summary>
    internal static class UnlockChain
    {
        public const string ServiceName = "Nvpwr";

        /// <summary>How long to wait for the device object after the service reports RUNNING.</summary>
        private const int ReadyTimeoutMs = 15000;

        /// <summary>
        /// Brings the helper up, restarting the display device first if the rail is no longer at
        /// the factory value.
        ///
        /// Returns false with a reason rather than throwing, because every caller here is a UI
        /// action that has to report something to the user.
        /// </summary>
        public static bool EnsureLoaded(int factoryFloorW, out string error)
        {
            error = null;

            if (!DseControl.IsAvailable())
            {
                error = "找不到 EfiDSEFix.exe。它需要与本程序放在同一目录。";
                return false;
            }
            if (!DseControl.IsBooted())
            {
                error = "本次启动没有经过 EfiGuard，无法临时关闭驱动签名强制。\n\n" +
                        "请从 EfiGuard 的启动项重新启动电脑。";
                return false;
            }

            // Only when it can help. At boot the rail is already factory and this would be a
            // pointless few seconds of black screen.
            int wall = GpuDevice.CurrentWallW();
            if (factoryFloorW > 0 && wall > 0 && wall != factoryFloorW)
            {
                if (!GpuDevice.Restart(out error))
                {
                    error = "功耗墙当前为 " + wall + " W，不是出厂值 " + factoryFloorW +
                            " W，需要先重启显卡设备才能重新调整，但重启失败。\n\n" + error;
                    return false;
                }
            }

            bool dseOff = false;
            try
            {
                if (!DseControl.Disable(out error))
                {
                    error = "关闭驱动签名强制失败。\n\n" + error;
                    return false;
                }
                dseOff = true;

                if (!LoadService(out error))
                {
                    return false;
                }

                if (!Driver.WaitUntilReady(ReadyTimeoutMs))
                {
                    error = "驱动服务已启动，但设备 \\.\\Nvpwr 在 " + (ReadyTimeoutMs / 1000) +
                            " 秒内没有就绪。";
                    UnloadService();
                    return false;
                }
                return true;
            }
            finally
            {
                if (dseOff)
                {
                    // Restored here rather than in the caller so that it cannot be skipped by an
                    // early return. A failure is logged but not propagated: the driver state is
                    // the more useful thing to report, and DSE being left off is recoverable by
                    // rebooting.
                    string e2;
                    if (!DseControl.Enable(out e2))
                    {
                        Store.Log("⚠ 恢复驱动签名强制失败: " + e2);
                    }
                }
            }
        }

        /// <summary>
        /// Applies a power target through the loaded helper.
        ///
        /// Reads the result back rather than trusting the call: the helper reports what it asked
        /// the driver for, and the driver can still refuse or settle somewhere else.
        /// </summary>
        public static bool Apply(uint watts, uint ceilingMw, uint profile, out string error)
        {
            error = null;
            if (!Driver.IsOpenable())
            {
                error = "驱动未加载，无法下发功耗。";
                return false;
            }

            uint mw = watts * 1000u;
            if (!Driver.SetPower(mw, ceilingMw, profile, out error))
            {
                return false;
            }

            Driver.Status status;
            string serr;
            if (!Driver.QueryStatus(out status, out serr))
            {
                error = "下发后无法读取驱动状态：" + serr;
                return false;
            }
            if (status.CurrentEffective != mw)
            {
                error = "申请 " + watts + " W，驱动实际生效 " +
                        (status.CurrentEffective / 1000) + " W。\n\n" +
                        "常见原因：功耗墙已被改过，需要重启显卡设备后重新调整。";
                return false;
            }
            return true;
        }

        public static void Unload()
        {
            UnloadService();
        }

        // ---------------------------------------------------------------- service plumbing

        /// <summary>
        /// Creates and starts the kernel service.
        ///
        /// Delete-then-create rather than reusing whatever is registered: the ImagePath may point
        /// at an older copy of the driver, and a stale service that half-starts is harder to
        /// diagnose than one that is rebuilt every time.
        /// </summary>
        private static bool LoadService(out string error)
        {
            error = null;
            string sys = Path.Combine(Service.ExePath, "Nvpwr.sys");
            if (!File.Exists(sys))
            {
                error = "找不到驱动文件：" + sys;
                return false;
            }

            UnloadService();
            Sc("delete " + ServiceName, 15000);

            string output;
            if (!Sc("create " + ServiceName + " type= kernel binPath= " + sys, 20000, out output))
            {
                error = "创建 Nvpwr 服务失败。\n\n" + output;
                return false;
            }
            if (!Sc("start " + ServiceName, 30000, out output))
            {
                error = "启动 Nvpwr 服务失败。这通常意味着驱动被拒绝加载。\n\n" + output;
                return false;
            }
            return true;
        }

        private static void UnloadService()
        {
            Sc("stop " + ServiceName, 20000);
        }

        private static bool Sc(string arguments, int timeoutMs)
        {
            return Sc(arguments, timeoutMs, out _);
        }

        private static bool Sc(string arguments, int timeoutMs, out string output)
        {
            output = null;
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo
                {
                    FileName = "sc.exe",
                    Arguments = arguments,
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true
                };
                using (Process p = Process.Start(psi))
                {
                    if (p == null)
                    {
                        output = "无法启动 sc.exe";
                        return false;
                    }
                    string so = p.StandardOutput.ReadToEnd();
                    string se = p.StandardError.ReadToEnd();
                    if (!p.WaitForExit(timeoutMs))
                    {
                        try { p.Kill(); } catch { }
                        output = "sc " + arguments + " 超时";
                        return false;
                    }
                    output = (so + "\n" + se).Trim();
                    return p.ExitCode == 0;
                }
            }
            catch (Exception ex)
            {
                output = "sc " + arguments + " 异常：" + ex.Message;
                return false;
            }
        }
    }
}
