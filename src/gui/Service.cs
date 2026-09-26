using System;
using System.Diagnostics;
using System.IO;
using System.Security.Principal;
using System.ServiceProcess;
using System.Windows;
using Microsoft.Win32;

namespace NvpwrControl
{
    /// <summary>
    /// Service control and elevation.
    ///
    /// The background service owns "keep these settings applied across a reboot",
    /// because GPU power policy lives only in driver memory and is always lost on
    /// reload. The GUI works without it — it drives the device directly — so a
    /// missing service is a normal state, not an error.
    /// </summary>
    internal static class Service
    {
        public const string Name = "NvpwrSvc";
        public const string DriverName = "Nvpwr";
        public const string DisplayName = "NV显卡功耗软解 后台服务";

        /// <summary>
        /// Full path of this executable.
        ///
        /// WPF's Application has no ExecutablePath — that member belongs to
        /// WinForms — so it is read from the process. This is the path used both
        /// for relaunching elevated and for locating NvpwrSvc.exe alongside it.
        /// </summary>
        public static string ExePath
        {
            get
            {
                try { return Process.GetCurrentProcess().MainModule.FileName; }
                catch { return System.Reflection.Assembly.GetExecutingAssembly().Location; }
            }
        }

        public static bool IsElevated()
        {
            try
            {
                using (WindowsIdentity id = WindowsIdentity.GetCurrent())
                {
                    return new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator);
                }
            }
            catch { return false; }
        }

        /// <summary>Relaunches elevated when needed, since the driver needs admin.</summary>
        public static bool EnsureElevated()
        {
            if (IsElevated()) return true;

            try
            {
                ProcessStartInfo psi = new ProcessStartInfo
                {
                    FileName = ExePath,
                    UseShellExecute = true,
                    Verb = "runas"
                };
                Process.Start(psi);
            }
            catch
            {
                MessageBox.Show("需要管理员权限才能控制驱动。请右键“以管理员身份运行”。",
                                "NV显卡功耗软解", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            return false;
        }

        public static string Query()
        {
            try
            {
                ServiceController sc = new ServiceController(Name);
                switch (sc.Status)
                {
                    case ServiceControllerStatus.Running: return "运行中";
                    case ServiceControllerStatus.Stopped: return "已停止";
                    case ServiceControllerStatus.StartPending: return "启动中";
                    case ServiceControllerStatus.StopPending: return "停止中";
                    default: return sc.Status.ToString();
                }
            }
            catch (InvalidOperationException)
            {
                return "未安装";
            }
            catch (Exception ex)
            {
                return "未知 (" + ex.Message + ")";
            }
        }

        private static string ServiceExePath()
        {
            string dir = Path.GetDirectoryName(ExePath);
            return Path.Combine(dir ?? ".", "NvpwrSvc.exe");
        }

        public static bool Install(out string error)
        {
            error = null;
            string exe = ServiceExePath();
            if (!File.Exists(exe))
            {
                error = "未找到 NvpwrSvc.exe（应与本程序同目录）：\r\n" + exe;
                return false;
            }

            bool installed = Query() != "未安装";

            /*
                Stop it first, whether or not it is running.

                Not "uninstall then install" — the service is not deleted — but a restart has to
                happen, and the reason is specific.

                sc config changes binPath, and that takes effect at the NEXT start. sc start on a
                service that is already running returns 1056, which this method tolerates because
                it is normally harmless. Put together, those two produce a state that is not
                harmless at all: the configuration can point at the new binary while the running
                process is still the old one — most easily when the program is run from a new
                folder, so binPath is a different path than the running image. Nothing in the
                service list or in this window would show it, and this service is the component
                that rewrites driver memory at boot.

                Stopping first makes the restarted process the configured binary by construction.
                It costs about a second.
            */
            if (installed && Query() == "运行中")
            {
                string stopOut;
                RunSc("stop " + Name, out stopOut);
                for (int i = 0; i < 40; i++)
                {
                    if (Query() != "运行中" && Query() != "停止中") break;
                    System.Threading.Thread.Sleep(200);
                }
                if (Query() == "运行中" || Query() == "停止中")
                {
                    error = "旧的服务进程没有在 8 秒内停下，无法确保新版本真正生效。\r\n" +
                            "继续安装会留下一个跑着旧代码的服务进程，所以这里停手了。\r\n\r\n" +
                            "请手动结束 NvpwrSvc.exe 后重试。";
                    return false;
                }
                Store.Log("安装服务: 已先停止旧进程，确保重启后运行的是当前版本");
            }

            // sc.exe is used rather than the SCM API because a single call is
            // clearer here and the failure text is directly actionable.
            //
            // delayed-auto, not auto. An auto-start service runs about twelve seconds after the
            // kernel hands over, while nvlddmkm is still building the display stack — and the
            // replay raises the power ceiling by re-entering NVIDIA's own power-policy
            // generator, which hung this machine outright in that window. The service also waits
            // for a logon on its own, so this is a second line of defence rather than the
            // mechanism: it means the process is not even running during the risky part of boot.
            string output;
            if (!RunSc("create " + Name + " binPath= \"" + exe + "\" start= delayed-auto", out output))
            {
                // Already exists: fall through to a config update instead.
                if (output.IndexOf("1073", StringComparison.Ordinal) < 0 &&
                    output.IndexOf("already exists", StringComparison.OrdinalIgnoreCase) < 0 &&
                    output.IndexOf("已存在", StringComparison.Ordinal) < 0)
                {
                    error = "注册服务失败：\r\n" + output;
                    return false;
                }
            }
            RunSc("config " + Name + " binPath= \"" + exe + "\" start= delayed-auto", out output);

            if (!RunSc("start " + Name, out output))
            {
                if (output.IndexOf("1056", StringComparison.Ordinal) < 0)   // already running
                {
                    error = "服务已注册但启动失败：\r\n" + output;
                    return false;
                }
            }

            // Report what is actually running now, not what was asked for. The check above
            // makes a mismatch impossible in the normal path, but printing the configured path
            // turns "should be right" into something the log can be read against.
            Store.Log("后台服务已安装并启动: " + exe);
            return true;
        }

        public static bool Remove(out string error)
        {
            error = null;
            string output;
            RunSc("stop " + Name, out output);
            for (int i = 0; i < 30; i++)
            {
                if (Query() == "已停止" || Query() == "未安装") break;
                System.Threading.Thread.Sleep(200);
            }
            if (!RunSc("delete " + Name, out output))
            {
                if (output.IndexOf("1060", StringComparison.Ordinal) >= 0) return true;   // not installed
                error = "移除服务失败：\r\n" + output;
                return false;
            }
            return true;
        }

        public static bool Start(out string error)
        {
            error = null;
            string output;
            if (RunSc("start " + Name, out output)) return true;
            if (output.IndexOf("1056", StringComparison.Ordinal) >= 0) return true;
            error = output;
            return false;
        }

        private static bool RunSc(string arguments, out string output)
        {
            output = "";
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo(
                    Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "sc.exe"), arguments)
                {
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true
                };
                using (Process p = Process.Start(psi))
                {
                    if (p == null) { output = "无法启动 sc.exe"; return false; }
                    output = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
                    p.WaitForExit(15000);
                    return p.ExitCode == 0;
                }
            }
            catch (Exception ex)
            {
                output = ex.Message;
                return false;
            }
        }

        /// <summary>
        /// Registers the GUI itself for auto-start. Also accepts the "--tray"
        /// argument so a minimized start is possible.
        /// </summary>
        public static bool SetAutoStart(bool enable, out string error)
        {
            error = null;
            try
            {
                using (RegistryKey key = Registry.CurrentUser.CreateSubKey(
                    @"Software\Microsoft\Windows\CurrentVersion\Run"))
                {
                    if (key == null) { error = "无法打开启动项注册表键"; return false; }
                    if (enable)
                        key.SetValue("NvpwrControl", "\"" + ExePath + "\"");
                    else
                        key.DeleteValue("NvpwrControl", false);
                }
                return true;
            }
            catch (Exception ex)
            {
                error = "设置开机自启失败: " + ex.Message;
                return false;
            }
        }
    }
}
