using System;
using System.IO;
using System.Windows;

namespace NvpwrControl
{
    /// <summary>
    /// Settings dialog.
    ///
    /// Holds no state of its own: it edits the MainWindow's DesiredState
    /// directly and writes it back on close. Keeping one owner for the record
    /// avoids the two copies drifting apart — which is exactly how a saved slot
    /// ends up describing something the UI no longer shows.
    /// </summary>
    public partial class SettingsDialog : Window
    {
        private readonly MainWindow _owner;
        private readonly DesiredState _state;

        /// <summary>
        /// Internal rather than public: DesiredState is internal, and a public
        /// constructor cannot expose a less accessible type. The XAML-generated
        /// half of the partial class is public, so the visibility lives here.
        /// </summary>
        internal SettingsDialog(MainWindow owner, DesiredState state)
        {
            InitializeComponent();
            _owner = owner;
            _state = state;

            ChkStartWin.IsChecked = AutoStartEnabled();
            ChkStartMin.IsChecked = _state.StartMinimized;
            TxtMvoltPath.Text = _state.MvoltPath;

            RefreshServiceState();
        }

        public void RefreshPath(string path)
        {
            TxtMvoltPath.Text = path;
        }

        private void RefreshServiceState()
        {
            string s = Service.Query();
            TxtService.Text = "当前状态：" + s;
            TxtService.SetResourceReference(ForegroundProperty, s == "运行中" ? "Accent" : "TextMuted");
        }

        // ------------------------------------------------------------- startup

        private void OnStartWinChanged(object sender, RoutedEventArgs e)
        {
            bool want = ChkStartWin.IsChecked == true;
            string error;
            if (!Service.SetAutoStart(want, out error))
            {
                MessageBox.Show(this, error, "Nvpwr 控制台", MessageBoxButton.OK, MessageBoxImage.Warning);
                ChkStartWin.IsChecked = !want;
                return;
            }
            _state.StartWithWindows = want;
        }

        private void OnStartMinChanged(object sender, RoutedEventArgs e)
        {
            _state.StartMinimized = ChkStartMin.IsChecked == true;
        }

        private static bool AutoStartEnabled()
        {
            try
            {
                using (Microsoft.Win32.RegistryKey key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(
                    @"Software\Microsoft\Windows\CurrentVersion\Run", false))
                {
                    return key != null && key.GetValue("NvpwrControl") != null;
                }
            }
            catch { return false; }
        }

        // ------------------------------------------------------------- service

        private void OnInstallService(object sender, RoutedEventArgs e)
        {
            string error;
            if (!Service.Install(out error))
                MessageBox.Show(this, error, "Nvpwr 控制台", MessageBoxButton.OK, MessageBoxImage.Warning);
            else
            {
                Store.Log("后台服务已安装并启动");
                MessageBox.Show(this, "后台服务已安装。开机后会自动重新下发功耗与电压。",
                                "Nvpwr 控制台", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            RefreshServiceState();
        }

        private void OnRemoveService(object sender, RoutedEventArgs e)
        {
            if (MessageBox.Show(this, "停止并移除后台服务？移除后开机不会自动重放设置。",
                                "Nvpwr 控制台", MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK)
                return;

            string error;
            if (!Service.Remove(out error))
                MessageBox.Show(this, error, "Nvpwr 控制台", MessageBoxButton.OK, MessageBoxImage.Warning);
            else Store.Log("后台服务已移除");
            RefreshServiceState();
        }

        // -------------------------------------------------------------- mVolt+

        private void OnBrowseMvolt(object sender, RoutedEventArgs e)
        {
            Microsoft.Win32.OpenFileDialog dlg = new Microsoft.Win32.OpenFileDialog
            {
                Title = "选择 mVolt+.exe",
                Filter = "mVolt+ (mVolt+.exe)|mVolt+.exe|可执行文件 (*.exe)|*.exe"
            };
            if (dlg.ShowDialog(this) != true) return;
            _state.MvoltPath = dlg.FileName;
            TxtMvoltPath.Text = dlg.FileName;
        }

        private void OnAutoFindMvolt(object sender, RoutedEventArgs e)
        {
            _state.MvoltPath = "";
            TxtMvoltPath.Text = MVolt.Find("");
        }

        // ------------------------------------------------------ compatibility

        private void OnStandardMode(object sender, RoutedEventArgs e)
        {
            if (MessageBox.Show(this,
                "切换到标准模式？\r\n\r\n会恢复出厂设置、停止服务并移除内核驱动，以便运行要求开启 Secure Boot 的反作弊游戏。你的设置会保留，切回解锁模式后会重新应用。",
                "Nvpwr 控制台", MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK)
                return;

            string error;
            Driver.Restore(out error);
            if (MVolt.Available(_state.MvoltPath)) { string e2; MVolt.Reset(_state.MvoltPath, out e2); }
            Service.Remove(out error);

            Store.Log("已切换到标准模式");
            MessageBox.Show(this, "已切换到标准模式。要重新启用解锁，请重启后再打开本程序并安装服务。",
                            "Nvpwr 控制台", MessageBoxButton.OK, MessageBoxImage.Information);
            RefreshServiceState();
        }

        private void OnClose(object sender, RoutedEventArgs e)
        {
            Close();
        }
    }
}
