using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace NvpwrControl
{
    /// <summary>
    /// Asks Desktop Window Manager to draw a window's caption in dark colours.
    ///
    /// The companion tool's window has a dark title bar, and against this app's near-black
    /// content the default light one reads as a separate, brighter object sitting on top of it.
    ///
    /// A custom caption was not used. Doing that means taking over minimise, maximise, close,
    /// restore, snap layouts, the system menu, double-click-to-maximise and the resize borders,
    /// and every one of those is a chance to get window behaviour subtly wrong. This asks DWM to
    /// recolour the real caption instead, so all of it keeps working.
    ///
    /// Both attribute ids are tried: 20 is the current one and 19 is what shipped on Windows 10
    /// builds before 20H1. An older build rejects 20 with a non-zero result, which is the signal
    /// to retry with 19. Older still rejects both, and the caption simply stays light — not worth
    /// failing over.
    /// </summary>
    internal static class DarkTitleBar
    {
        private const int DwmwaUseImmersiveDarkMode = 20;
        private const int DwmwaUseImmersiveDarkModeBefore20H1 = 19;

        [DllImport("dwmapi.dll", PreserveSig = true)]
        private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);

        public static void Apply(Window window)
        {
            if (window == null)
            {
                return;
            }
            try
            {
                IntPtr handle = new WindowInteropHelper(window).Handle;
                if (handle == IntPtr.Zero)
                {
                    return;
                }
                int enabled = 1;
                if (DwmSetWindowAttribute(handle, DwmwaUseImmersiveDarkMode, ref enabled, sizeof(int)) != 0)
                {
                    DwmSetWindowAttribute(handle, DwmwaUseImmersiveDarkModeBefore20H1, ref enabled, sizeof(int));
                }
            }
            catch (DllNotFoundException)
            {
                // dwmapi is absent on anything older than Vista; nothing to do.
            }
            catch (EntryPointNotFoundException)
            {
            }
        }
    }
}
