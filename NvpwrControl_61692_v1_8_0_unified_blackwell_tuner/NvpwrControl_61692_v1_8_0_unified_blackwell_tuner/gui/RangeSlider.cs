using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Markup;
using System.Windows.Media;

namespace NvpwrControl
{
    /// <summary>
    /// A slider with two thumbs, for setting a lower and an upper bound on one track.
    ///
    /// WHY this exists: the upper voltage limit and the lower one are two ends of the same
    /// quantity, and the card showed them as two separate sliders stacked with a formula box
    /// between them explaining how they related. mVolt+ — which is what actually writes these
    /// values — presents the pair as one range, and reading "850–1025 mV" off a single control
    /// says what two independent sliders only imply.
    ///
    /// WPF ships Slider with one thumb and nothing with two, so this is written out. The thumbs
    /// are real Thumbs rather than hit-tested rectangles on a Canvas: dragging, keyboard
    /// handling and the cross-check against the other thumb are then the platform's problem.
    ///
    /// The two values are kept in order — the lower thumb cannot pass the upper one — because
    /// nothing downstream accepts an inverted range, and letting them cross only to reject it at
    /// apply time would put the error a whole interaction away from its cause.
    /// </summary>
    [ContentProperty("NoContent")]
    public sealed class RangeSlider : UserControl
    {
        private const double ThumbSize = 18.0;

        private readonly Canvas _canvas = new Canvas();
        private readonly Border _track = new Border();
        private readonly Border _fill = new Border();
        private readonly Thumb _lowerThumb = new Thumb();
        private readonly Thumb _upperThumb = new Thumb();

        /// <summary>Raised after either end moves, once per change.</summary>
        public event EventHandler RangeChanged;

        /// <summary>Set while the control is writing its own thumbs, to stop re-entry.</summary>
        private bool _syncing;

        public object NoContent { get { return null; } }

        public RangeSlider()
        {
            Height = 24;
            MinWidth = 80;

            _track.Height = 4;
            _track.CornerRadius = new CornerRadius(2);
            _track.Background = new SolidColorBrush(Color.FromRgb(0x3A, 0x42, 0x52));
            _track.VerticalAlignment = VerticalAlignment.Center;

            _fill.Height = 4;
            _fill.CornerRadius = new CornerRadius(2);
            _fill.Background = new SolidColorBrush(Color.FromRgb(0x94, 0xA3, 0xB8));
            _fill.VerticalAlignment = VerticalAlignment.Center;

            Style thumbStyle = BuildThumbStyle();
            _lowerThumb.Style = thumbStyle;
            _upperThumb.Style = thumbStyle;
            _lowerThumb.Width = _upperThumb.Width = ThumbSize;
            _lowerThumb.Height = _upperThumb.Height = ThumbSize;
            _lowerThumb.Cursor = _upperThumb.Cursor = System.Windows.Input.Cursors.Hand;

            _lowerThumb.DragDelta += (s, e) => Nudge(_lowerThumb, e.HorizontalChange, true);
            _upperThumb.DragDelta += (s, e) => Nudge(_upperThumb, e.HorizontalChange, false);

            _canvas.Children.Add(_track);
            _canvas.Children.Add(_fill);
            _canvas.Children.Add(_lowerThumb);
            _canvas.Children.Add(_upperThumb);
            Content = _canvas;

            SizeChanged += (s, e) => Layout();
            Loaded += (s, e) => Layout();
        }

        private static Style BuildThumbStyle()
        {
            // Written as markup because a Thumb's look has to come from a ControlTemplate, and a
            // template is far clearer as XAML than as a tree of FrameworkElementFactory calls.
            const string xaml =
                "<Style xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' " +
                       "xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' TargetType='Thumb'>" +
                  "<Setter Property='OverridesDefaultStyle' Value='True'/>" +
                  "<Setter Property='Template'>" +
                    "<Setter.Value>" +
                      "<ControlTemplate TargetType='Thumb'>" +
                        "<Grid>" +
                          "<Border x:Name='b' CornerRadius='4' Background='#FFFFFF' " +
                                  "BorderBrush='#8A93A5' BorderThickness='1'/>" +
                        "</Grid>" +
                        "<ControlTemplate.Triggers>" +
                          "<Trigger Property='IsMouseOver' Value='True'>" +
                            "<Setter TargetName='b' Property='Background' Value='#F1F5F9'/>" +
                          "</Trigger>" +
                          "<Trigger Property='IsDragging' Value='True'>" +
                            "<Setter TargetName='b' Property='Background' Value='#CBD5E1'/>" +
                          "</Trigger>" +
                        "</ControlTemplate.Triggers>" +
                      "</ControlTemplate>" +
                    "</Setter.Value>" +
                  "</Setter>" +
                "</Style>";
            return (Style)XamlReader.Parse(xaml);
        }

        // ---------------------------------------------------------------- properties

        public static readonly DependencyProperty MinimumProperty =
            DependencyProperty.Register("Minimum", typeof(double), typeof(RangeSlider),
                new PropertyMetadata(0.0, OnRangePropertyChanged));
        public static readonly DependencyProperty MaximumProperty =
            DependencyProperty.Register("Maximum", typeof(double), typeof(RangeSlider),
                new PropertyMetadata(100.0, OnRangePropertyChanged));
        public static readonly DependencyProperty LowerValueProperty =
            DependencyProperty.Register("LowerValue", typeof(double), typeof(RangeSlider),
                new PropertyMetadata(0.0, OnValuePropertyChanged));
        public static readonly DependencyProperty UpperValueProperty =
            DependencyProperty.Register("UpperValue", typeof(double), typeof(RangeSlider),
                new PropertyMetadata(100.0, OnValuePropertyChanged));
        public static readonly DependencyProperty TickFrequencyProperty =
            DependencyProperty.Register("TickFrequency", typeof(double), typeof(RangeSlider),
                new PropertyMetadata(1.0));

        public double Minimum { get { return (double)GetValue(MinimumProperty); } set { SetValue(MinimumProperty, value); } }
        public double Maximum { get { return (double)GetValue(MaximumProperty); } set { SetValue(MaximumProperty, value); } }
        public double LowerValue { get { return (double)GetValue(LowerValueProperty); } set { SetValue(LowerValueProperty, value); } }
        public double UpperValue { get { return (double)GetValue(UpperValueProperty); } set { SetValue(UpperValueProperty, value); } }
        public double TickFrequency { get { return (double)GetValue(TickFrequencyProperty); } set { SetValue(TickFrequencyProperty, value); } }

        private static void OnRangePropertyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            RangeSlider r = (RangeSlider)d;
            r.CoerceValues();
            r.Layout();
        }

        private static void OnValuePropertyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            RangeSlider r = (RangeSlider)d;
            if (r._syncing) return;
            r.CoerceValues();
            r.Layout();
            if (r.RangeChanged != null) r.RangeChanged(r, EventArgs.Empty);
        }

        /// <summary>
        /// Keeps the pair inside the range and in order.
        ///
        /// Whichever end moved is the one that gets pushed back, which is why this looks at both
        /// properties rather than trusting the one that changed: a caller setting LowerValue past
        /// UpperValue means "this is the new floor", so the ceiling has to give, and the other
        /// way round.
        /// </summary>
        private void CoerceValues()
        {
            _syncing = true;
            try
            {
                double lo = Minimum, hi = Maximum;
                if (hi < lo) hi = lo;

                double a = LowerValue, b = UpperValue;
                if (a < lo) a = lo;
                if (a > hi) a = hi;
                if (b < lo) b = lo;
                if (b > hi) b = hi;

                if (a > b)
                {
                    // The one that just changed wins; the other follows.
                    if (LowerValue != a) b = a;
                    else a = b;
                }

                if (LowerValue != a) SetValue(LowerValueProperty, a);
                if (UpperValue != b) SetValue(UpperValueProperty, b);
            }
            finally { _syncing = false; }
        }

        private double Snap(double v)
        {
            double step = TickFrequency;
            if (step <= 0) return v;
            double snapped = Math.Round((v - Minimum) / step) * step + Minimum;
            if (snapped < Minimum) snapped = Minimum;
            if (snapped > Maximum) snapped = Maximum;
            return snapped;
        }

        // ---------------------------------------------------------------- interaction

        private void Nudge(Thumb thumb, double horizontalChange, bool isLower)
        {
            double span = Math.Max(1.0, ActualWidth - ThumbSize);
            double perPixel = (Maximum - Minimum) / span;
            double delta = horizontalChange * perPixel;

            if (isLower)
            {
                double v = Snap(LowerValue + delta);
                if (v > UpperValue) v = UpperValue;
                if (v < Minimum) v = Minimum;
                LowerValue = v;
            }
            else
            {
                double v = Snap(UpperValue + delta);
                if (v < LowerValue) v = LowerValue;
                if (v > Maximum) v = Maximum;
                UpperValue = v;
            }
        }

        private void Layout()
        {
            double w = ActualWidth;
            if (w <= 0) return;

            double span = Math.Max(1.0, w - ThumbSize);
            double range = Math.Max(0.0001, Maximum - Minimum);

            double loFrac = (LowerValue - Minimum) / range;
            double hiFrac = (UpperValue - Minimum) / range;
            loFrac = Math.Max(0, Math.Min(1, loFrac));
            hiFrac = Math.Max(0, Math.Min(1, hiFrac));

            double loX = loFrac * span;
            double hiX = hiFrac * span;

            _track.Width = w;
            Canvas.SetLeft(_track, 0);
            Canvas.SetTop(_track, (Height - _track.Height) / 2);

            _fill.Width = Math.Max(0, hiX - loX);
            Canvas.SetLeft(_fill, loX + ThumbSize / 2);
            Canvas.SetTop(_fill, (Height - _fill.Height) / 2);

            Canvas.SetLeft(_lowerThumb, loX);
            Canvas.SetTop(_lowerThumb, (Height - ThumbSize) / 2);
            Canvas.SetLeft(_upperThumb, hiX);
            Canvas.SetTop(_upperThumb, (Height - ThumbSize) / 2);

            // The lower thumb is drawn on top when they meet, so it stays grabbable.
            Panel.SetZIndex(_lowerThumb, 2);
            Panel.SetZIndex(_upperThumb, 1);
        }
    }
}
