using System;
using System.Windows;

namespace NvpwrControl
{
    public partial class App : Application
    {
        protected override void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);

            // The kernel helper needs administrator rights to load and drive, so
            // the whole UI runs elevated. Relaunching rather than failing is the
            // difference between "works" and "looks broken" for a user who
            // double-clicked the exe from Explorer.
            if (!Service.IsElevated())
            {
                Service.EnsureElevated();
                Shutdown();
                return;
            }

            DispatcherUnhandledException += (s, args) =>
            {
                Store.Log("未处理异常: " + args.Exception);
                MessageBox.Show("程序发生未处理异常：\r\n\r\n" + args.Exception.Message,
                                "Nvpwr 控制台", MessageBoxButton.OK, MessageBoxImage.Error);
                args.Handled = true;
            };
        }
    }
}
