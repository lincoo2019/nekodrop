// SPDX-License-Identifier: GPL-2.0-only
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace NekoDrop;

/// <summary>
/// One geometry for everything on the radar, so the rings, the ripples, the
/// center and the devices can never drift apart: they all come from the size
/// of the stage. Devices sit with their icon centered on the middle ring.
/// </summary>
public readonly record struct RadarGeometry(Point Center, double Orbit, double Inner, double Outer)
{
    /// <summary>A device's icon (its circle), in pixels.</summary>
    public const double Node = 50;
    /// <summary>A device's whole box: 4 px padding, icon, name and status below it.</summary>
    public const double NodeWidth = 112, NodeHeight = 4 + Node + 4 + 18 + 17 + 6; // CJK lines need the extra pixels
    /// <summary>"This PC" in the middle.</summary>
    public const double Hub = 58;
    /// <summary>More devices than this go to the list.</summary>
    public const int MaxOnRadar = 8;

    public static RadarGeometry For(Size stage)
    {
        var center = new Point(stage.Width / 2, stage.Height / 2);
        // The orbit keeps every device box inside the stage. A box hangs its name
        // below the icon, so the lowest device needs more room than the top one;
        // no count up to MaxOnRadar puts a device lower than sin = 0.93 (eight
        // devices, the pair at 67.5 degrees), and only odd counts put one at the top.
        var byWidth = stage.Width / 2 - NodeWidth / 2 - 8;
        var byTop = stage.Height / 2 - Node / 2 - 8;
        var byBottom = (stage.Height / 2 - (NodeHeight - Node / 2) - 8) / 0.93;
        // Never so small that an upper device's status/speed line reaches the hub or its receiving ring.
        var orbit = Math.Max(Hub / 2 + NodeHeight - Node / 2 + 18, Math.Min(byWidth, Math.Min(byTop, byBottom)));
        // Two guide rings as in the design (154 and 248 px there): the orbit and one inside it.
        return new RadarGeometry(center, orbit, orbit * 0.62, orbit * 1.05);
    }

    /// <summary>
    /// Where device i of n goes, as an angle: evenly spread, the first at the
    /// top for odd counts and the pair at the sides for even ones, so the
    /// picture is always symmetric left to right.
    /// </summary>
    public static double Angle(int i, int n) =>
        -Math.PI / 2 + 2 * Math.PI * i / n + (n % 2 == 0 ? Math.PI / n : 0);

    public Point IconCenter(int i, int n)
    {
        if (n == 1) return new Point(Center.X, Center.Y - Orbit);
        var a = Angle(i, n);
        return new Point(Center.X + Orbit * Math.Cos(a), Center.Y + Orbit * Math.Sin(a));
    }
}

/// <summary>Puts its children (device boxes) on the radar's orbit.</summary>
public sealed class RadarPanel : Panel
{
    /// <summary>The request bubble's height, with its notch and the gap to the icon or the name.</summary>
    public const double Bubble = 70 + 6;

    /// <summary>
    /// Set on each child: its request bubble opens above the icon (true) or
    /// below the name. Outwards, away from this PC, where the stage has room:
    /// above for the upper half, below for the lower half, else the other way.
    /// </summary>
    public static readonly DependencyProperty BubbleAboveProperty = DependencyProperty.RegisterAttached(
        "BubbleAbove", typeof(bool), typeof(RadarPanel), new FrameworkPropertyMetadata(false));
    public static bool GetBubbleAbove(DependencyObject d) => (bool)d.GetValue(BubbleAboveProperty);
    public static void SetBubbleAbove(DependencyObject d, bool value) => d.SetValue(BubbleAboveProperty, value);

    /// <summary>
    /// Puts a device's request bubble above its icon (notch down) or below its
    /// name (notch up). Set here, on the template's named parts: a trigger in
    /// the item template cannot reach this panel's attached property on its container.
    /// </summary>
    static void PlaceBubble(UIElement child, bool above)
    {
        if (child is not ContentPresenter cp || cp.ContentTemplate is not { } template) return;
        if (template.FindName("bubble", cp) is UIElement bubble) Canvas.SetTop(bubble, above ? -74 : 98);
        if (template.FindName("notchUp", cp) is UIElement up) up.Visibility = above ? Visibility.Collapsed : Visibility.Visible;
        if (template.FindName("notchDown", cp) is UIElement down) down.Visibility = above ? Visibility.Visible : Visibility.Collapsed;
    }

    protected override Size MeasureOverride(Size available)
    {
        foreach (UIElement c in InternalChildren)
            c.Measure(new Size(RadarGeometry.NodeWidth, RadarGeometry.NodeHeight));
        return new Size(double.IsInfinity(available.Width) ? 0 : available.Width,
                        double.IsInfinity(available.Height) ? 0 : available.Height);
    }

    protected override Size ArrangeOverride(Size final)
    {
        var g = RadarGeometry.For(final);
        var n = InternalChildren.Count;
        for (var i = 0; i < n; i++)
        {
            var p = g.IconCenter(i, n);
            var top = p.Y - 4 - RadarGeometry.Node / 2; // the box's top edge
            var roomAbove = top - Bubble >= 0;
            var roomBelow = top + RadarGeometry.NodeHeight + Bubble <= final.Height;
            var upper = p.Y < g.Center.Y - 1;
            var above = upper ? roomAbove || !roomBelow : !roomBelow && roomAbove;
            SetBubbleAbove(InternalChildren[i], above);
            PlaceBubble(InternalChildren[i], above);
            // The icon is at the top of the box: its center is padding and half an icon down.
            var child = InternalChildren[i];
            var at = new Point(p.X - RadarGeometry.NodeWidth / 2, p.Y - 4 - RadarGeometry.Node / 2);
            child.Arrange(new Rect(at, new Size(RadarGeometry.NodeWidth, RadarGeometry.NodeHeight)));
            Glide(child, at);
        }
        return final;
    }

    readonly System.Runtime.CompilerServices.ConditionalWeakTable<UIElement, object> _was = new();

    /// <summary>A device whose place changed (another came or went) slides there from where it was.</summary>
    void Glide(UIElement child, Point at)
    {
        if (_was.TryGetValue(child, out var old) && old is Point from && (from - at).Length > 0.5 && !Motion.Reduced)
        {
            var move = child.RenderTransform as TranslateTransform ?? new TranslateTransform();
            child.RenderTransform = move;
            var ease = new FluentEase();
            var t = TimeSpan.FromMilliseconds(420);
            move.BeginAnimation(TranslateTransform.XProperty, new DoubleAnimation(from.X - at.X + move.X, 0, t) { EasingFunction = ease });
            move.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(from.Y - at.Y + move.Y, 0, t) { EasingFunction = ease });
        }
        _was.AddOrUpdate(child, at);
    }
}

/// <summary>
/// A ring around a device's icon: the share sent so far as an arc from the
/// top, clockwise; while nothing is sent yet (waiting for the other side to
/// accept) a short arc turns round instead.
/// </summary>
public sealed class ProgressRing : FrameworkElement
{
    public static readonly DependencyProperty ProgressProperty = DependencyProperty.Register(
        nameof(Progress), typeof(double), typeof(ProgressRing),
        new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender, (d, e) => ((ProgressRing)d).Moved((double)e.OldValue, (double)e.NewValue)));
    public static readonly DependencyProperty StrokeProperty = DependencyProperty.Register(
        nameof(Stroke), typeof(Brush), typeof(ProgressRing),
        new FrameworkPropertyMetadata(Brushes.DodgerBlue, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty TrackProperty = DependencyProperty.Register(
        nameof(Track), typeof(Brush), typeof(ProgressRing),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty ThicknessProperty = DependencyProperty.Register(
        nameof(Thickness), typeof(double), typeof(ProgressRing),
        new FrameworkPropertyMetadata(3.0, FrameworkPropertyMetadataOptions.AffectsRender));

    public double Progress { get => (double)GetValue(ProgressProperty); set => SetValue(ProgressProperty, value); }

    /// <summary>The share drawn: follows Progress linearly over a quarter second, so the arc never jumps.</summary>
    static readonly DependencyProperty ShownProperty = DependencyProperty.Register(
        "Shown", typeof(double), typeof(ProgressRing), new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender));
    double Shown => (double)GetValue(ShownProperty);

    public Brush Stroke { get => (Brush)GetValue(StrokeProperty); set => SetValue(StrokeProperty, value); }
    public Brush? Track { get => (Brush?)GetValue(TrackProperty); set => SetValue(TrackProperty, value); }
    public double Thickness { get => (double)GetValue(ThicknessProperty); set => SetValue(ThicknessProperty, value); }

    readonly RotateTransform _turn = new();

    public ProgressRing()
    {
        RenderTransform = _turn;
        RenderTransformOrigin = new Point(0.5, 0.5);
        IsVisibleChanged += (_, _) => Spin();
        Loaded += (_, _) => Spin();
    }

    bool Waiting => Progress <= 0;

    void Moved(double from, double to)
    {
        Spin();
        if (Motion.Reduced || to <= 0 || to < from) { BeginAnimation(ShownProperty, null); SetValue(ShownProperty, to); return; }
        BeginAnimation(ShownProperty, new DoubleAnimation(to, TimeSpan.FromMilliseconds(250)), HandoffBehavior.SnapshotAndReplace);
    }

    void Spin()
    {
        if (Waiting && IsVisible && !Motion.Reduced)
        {
            if (_turn.HasAnimatedProperties) return;
            _turn.BeginAnimation(RotateTransform.AngleProperty,
                new DoubleAnimation(0, 360, TimeSpan.FromSeconds(1.2)) { RepeatBehavior = RepeatBehavior.Forever });
        }
        else
        {
            _turn.BeginAnimation(RotateTransform.AngleProperty, null);
            _turn.Angle = 0;
        }
    }

    protected override void OnRender(DrawingContext dc)
    {
        var size = Math.Min(ActualWidth, ActualHeight);
        if (size <= Thickness) return;
        var r = (size - Thickness) / 2;
        var c = new Point(ActualWidth / 2, ActualHeight / 2);
        if (Track is not null) dc.DrawEllipse(null, new Pen(Track, Thickness), c, r, r);
        var share = Waiting ? 0.22 : Math.Min(1, Math.Max(0.001, Shown));
        var pen = new Pen(Stroke, Thickness) { StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round };
        if (share >= 0.999) { dc.DrawEllipse(null, pen, c, r, r); return; }
        var end = -Math.PI / 2 + 2 * Math.PI * share;
        var geo = new StreamGeometry();
        using (var g = geo.Open())
        {
            g.BeginFigure(new Point(c.X, c.Y - r), false, false);
            g.ArcTo(new Point(c.X + r * Math.Cos(end), c.Y + r * Math.Sin(end)), new Size(r, r), 0, share > 0.5,
                    SweepDirection.Clockwise, true, false);
        }
        geo.Freeze();
        dc.DrawGeometry(null, pen, geo);
    }
}

/// <summary>A thin progress bar: the share done, filled from the left with rounded ends.</summary>
public sealed class Bar : FrameworkElement
{
    public static readonly DependencyProperty ProgressProperty = DependencyProperty.Register(
        nameof(Progress), typeof(double), typeof(Bar), new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty FillProperty = DependencyProperty.Register(
        nameof(Fill), typeof(Brush), typeof(Bar), new FrameworkPropertyMetadata(Brushes.DodgerBlue, FrameworkPropertyMetadataOptions.AffectsRender));
    public double Progress { get => (double)GetValue(ProgressProperty); set => SetValue(ProgressProperty, value); }
    public Brush Fill { get => (Brush)GetValue(FillProperty); set => SetValue(FillProperty, value); }

    protected override void OnRender(DrawingContext dc)
    {
        var w = ActualWidth * Math.Clamp(Progress, 0, 1);
        if (w <= 0) return;
        var r = ActualHeight / 2;
        dc.DrawRoundedRectangle(Fill, null, new Rect(0, 0, Math.Max(w, ActualHeight), ActualHeight), r, r);
    }
}

/// <summary>
/// The AirDrop mark: three concentric arcs open at the bottom around a dot,
/// drawn to any size in one brush (the title bars and the tray menu).
/// </summary>
public sealed class AirDropMark : FrameworkElement
{
    public static readonly DependencyProperty BrushProperty = DependencyProperty.Register(
        nameof(Brush), typeof(Brush), typeof(AirDropMark), new FrameworkPropertyMetadata(Brushes.DodgerBlue, FrameworkPropertyMetadataOptions.AffectsRender));
    public Brush Brush { get => (Brush)GetValue(BrushProperty); set => SetValue(BrushProperty, value); }

    protected override void OnRender(DrawingContext dc)
    {
        var size = Math.Min(ActualWidth, ActualHeight);
        if (size <= 0) return;
        var k = size / 20; // drawn on a 20 x 20 grid
        var c = new Point(ActualWidth / 2, ActualHeight / 2);
        var pen = new Pen(Brush, 1.6 * k) { StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round };
        foreach (var r in new[] { 8.2, 5.4 })
        {
            // From 120 degrees round the top to 60 degrees: a gap of 60 degrees at the bottom.
            var radius = r * k;
            var from = new Point(c.X + radius * Math.Cos(Math.PI * 2 / 3), c.Y + radius * Math.Sin(Math.PI * 2 / 3));
            var to = new Point(c.X + radius * Math.Cos(Math.PI / 3), c.Y + radius * Math.Sin(Math.PI / 3));
            var arc = new StreamGeometry();
            using (var g = arc.Open())
            {
                g.BeginFigure(from, false, false);
                g.ArcTo(to, new Size(radius, radius), 0, true, SweepDirection.Clockwise, true, false);
            }
            arc.Freeze();
            dc.DrawGeometry(null, pen, arc);
        }
        dc.DrawEllipse(Brush, null, c, 2.4 * k, 2.4 * k);
    }
}

/// <summary>True when the value is not null (a request is open).</summary>
public sealed class IsSet : System.Windows.Data.IValueConverter
{
    public static readonly IsSet Instance = new();
    public object? Convert(object? value, Type targetType, object? parameter, System.Globalization.CultureInfo culture) => value is not null;
    public object? ConvertBack(object? value, Type targetType, object? parameter, System.Globalization.CultureInfo culture) =>
        throw new NotSupportedException();
}

/// <summary>Windows' "Animation effects" setting: off means no looping motion.</summary>
/// <summary>
/// The cubic Bézier curves of Windows' Fluent motion: by default "decelerate"
/// (0.1, 0.9, 0.2, 1), quick out of the gate and settling softly; "standard"
/// is (0.8, 0, 0.2, 1). Usable from XAML as an easing function.
/// </summary>
public sealed class FluentEase : EasingFunctionBase
{
    public double X1 { get; set; } = 0.1;
    public double Y1 { get; set; } = 0.9;
    public double X2 { get; set; } = 0.2;
    public double Y2 { get; set; } = 1;

    public FluentEase() => EasingMode = EasingMode.EaseIn; // the curve as given, not mirrored

    static double Bezier(double t, double a, double b) => 3 * a * (1 - t) * (1 - t) * t + 3 * b * (1 - t) * t * t + t * t * t;

    protected override double EaseInCore(double x)
    {
        // Find t with Bx(t) = x by bisection (monotonic for these curves), then return By(t).
        double lo = 0, hi = 1, t = x;
        for (var i = 0; i < 24; i++)
        {
            t = (lo + hi) / 2;
            if (Bezier(t, X1, X2) < x) lo = t; else hi = t;
        }
        return Bezier(t, Y1, Y2);
    }

    protected override Freezable CreateInstanceCore() => new FluentEase { X1 = X1, Y1 = Y1, X2 = X2, Y2 = Y2 };
}

public static class Motion
{
    /// <summary>Set by --snapshot: the build machine has animations off, and the pictures should show the ripple.</summary>
    public static bool ForceOn { get; set; }
    public static bool Reduced => !ForceOn && !SystemParameters.ClientAreaAnimation;

    static readonly IEasingFunction Out = new FluentEase();
    static readonly IEasingFunction In = new FluentEase { X1 = 0.7, Y1 = 0, X2 = 1, Y2 = 0.5 }; // Fluent "accelerate"

    /// <summary>Fades in while sliding up by <paramref name="rise"/> pixels (a panel opening, a view switching).</summary>
    public static void FadeIn(UIElement e, double rise, double ms = 220)
    {
        if (Reduced) return;
        var t = TimeSpan.FromMilliseconds(ms);
        var move = e.RenderTransform as TranslateTransform ?? new TranslateTransform();
        e.RenderTransform = move;
        e.BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(0, 1, t) { EasingFunction = Out });
        move.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(rise, 0, t) { EasingFunction = Out });
    }

    /// <summary>Fades out while sliding down, then runs <paramref name="done"/>.</summary>
    public static void FadeOut(UIElement e, double fall, Action done, double ms = 140)
    {
        if (Reduced) { done(); return; }
        var t = TimeSpan.FromMilliseconds(ms);
        var move = e.RenderTransform as TranslateTransform ?? new TranslateTransform();
        e.RenderTransform = move;
        var fade = new DoubleAnimation(1, 0, t) { EasingFunction = In };
        fade.Completed += (_, _) =>
        {
            done();
            e.BeginAnimation(UIElement.OpacityProperty, null);
            move.BeginAnimation(TranslateTransform.YProperty, null);
        };
        move.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(0, fall, t) { EasingFunction = In });
        e.BeginAnimation(UIElement.OpacityProperty, fade);
    }

    /// <summary>A short swell and settle (1 → 1.15 → 1), for a moment worth noticing.</summary>
    public static void Pop(FrameworkElement e)
    {
        if (Reduced) return;
        var s = e.RenderTransform as ScaleTransform ?? new ScaleTransform();
        e.RenderTransform = s;
        e.RenderTransformOrigin = new Point(0.5, 0.5);
        var a = new DoubleAnimationUsingKeyFrames { Duration = TimeSpan.FromMilliseconds(420) };
        a.KeyFrames.Add(new EasingDoubleKeyFrame(1.15, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(160)), Out));
        a.KeyFrames.Add(new EasingDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(420)), Out));
        s.BeginAnimation(ScaleTransform.ScaleXProperty, a);
        s.BeginAnimation(ScaleTransform.ScaleYProperty, a);
    }
}
