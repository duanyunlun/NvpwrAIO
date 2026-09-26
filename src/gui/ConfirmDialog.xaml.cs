using System.Windows;
using System.Windows.Input;

namespace NvpwrControl
{
    /// <summary>
    /// A themed stand-in for MessageBox.
    ///
    /// Windows drew the message box in its own light chrome, which looked out of place beside
    /// every other window here, and at 200% scaling it measured itself from the unwrapped text —
    /// so a long warning stretched the dialog past the edge of the screen and its buttons were
    /// cut off. This version wraps, scrolls if the message is tall, and matches the app.
    /// </summary>
    public partial class ConfirmDialog : Window
    {
        private bool _accepted;

        public ConfirmDialog()
        {
            InitializeComponent();
        }

        /// <summary>
        /// Shows a modal prompt and returns true when the user confirms.
        ///
        /// A single-button call sets <paramref name="withCancel"/> false, which hides 取消 and
        /// turns the prompt into an acknowledgement.
        /// </summary>
        public static bool Ask(Window owner, string message, bool withCancel)
        {
            return Ask(owner, "NV显卡功耗软解", message, withCancel, warning: true);
        }

        public static bool Ask(Window owner, string title, string message, bool withCancel, bool warning)
        {
            ConfirmDialog dialog = new ConfirmDialog();
            if (owner != null && owner.IsLoaded)
            {
                dialog.Owner = owner;
            }
            dialog.TitleText.Text = title;
            dialog.MessageText.Text = message;
            if (!warning)
            {
                dialog.TitleAccent.Background = System.Windows.Media.Brushes.SeaGreen;
            }
            if (!withCancel)
            {
                dialog.CancelButton.Visibility = Visibility.Collapsed;
            }
            dialog.ShowDialog();
            return dialog._accepted;
        }

        /// <summary>
        /// Drags the window.
        ///
        /// The window has no system chrome, so nothing drags it unless this does. The button
        /// state is checked first: DragMove throws if the button is already released.
        /// </summary>
        private void OnTitleBarDrag(object sender, MouseButtonEventArgs e)
        {
            if (e.LeftButton == MouseButtonState.Pressed)
            {
                DragMove();
            }
        }

        private void OnOk(object sender, RoutedEventArgs e)
        {
            _accepted = true;
            Close();
        }

        private void OnCancel(object sender, RoutedEventArgs e)
        {
            _accepted = false;
            Close();
        }
    }
}
