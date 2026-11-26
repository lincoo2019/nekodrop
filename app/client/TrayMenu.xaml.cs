// SPDX-License-Identifier: GPL-2.0-only
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Shapes;

namespace NekoDrop;

/// <summary>
/// The tray icon's menu as the design draws it: open, allow receiving,
/// network mode (a panel beside the menu), Wi-Fi, settings, diagnostic
/// report, quit. It opens at the pointer and closes when it loses focus.
/// </summary>
public partial class TrayMenu : Window
{
    static App A => App.Instance;
    bool _closing;

    public TrayMenu()
    {
        InitializeComponent();
        AllowItem.Tag = A.Model.Allow ? "" : "";
        AutoAcceptItem.Tag = A.Settings.AutoAcceptMode != "off" ? "" : "";
        Deactivated += (_, _) => Shut();
    }

    /// <summary>Shows the menu with its lower right corner at the pointer, inside the work area.</summary>
    public void ShowAt(Point pointer)
    {
        Show();
        UpdateLayout();
        var area = SystemParameters.WorkArea;
        Left = Math.Max(area.Left, Math.Min(pointer.X - ActualWidth + 10, area.Right - ActualWidth));
        Top = Math.Max(area.Top, Math.Min(pointer.Y - ActualHeight + 10, area.Bottom - ActualHeight));
        Activate();
    }

    void Shut()
    {
        if (_closing) return;
        _closing = true;
        Close();
    }

    /// <summary>Opens the network mode panel (also for --snapshot).</summary>
    internal void OpenModes() { if (ModePanel.Visibility != Visibility.Visible) Mode_Click(this, new RoutedEventArgs()); }

    void Mode_Click(object sender, RoutedEventArgs e)
    {
        var open = ModePanel.Visibility != Visibility.Visible;
        var right = Left + ActualWidth;
        ModePanel.Visibility = open ? Visibility.Visible : Visibility.Collapsed;
        ModeItem.SetResourceReference(BackgroundProperty, open ? "AccentSubtle" : "Transparent");
        if (open) BuildModes();
        // The panel opens to the left: keep the menu where it was.
        UpdateLayout();
        Left = right - ActualWidth;
    }

    void BuildModes()
    {
        ModeList.Children.Clear();
        foreach (var (key, text) in new[] { ("concurrent", L.T("mode.concurrent.short")), ("ondemand", L.T("mode.ondemand.short")), ("off", L.T("mode.off.short")) })
        {
            var chosen = A.Model.Mode == key;
            var dot = new Grid { Width = 12, Height = 12, Margin = new Thickness(0, 2, 8, 0), VerticalAlignment = VerticalAlignment.Top };
            var ring = new Ellipse { StrokeThickness = 1.5 };
            ring.SetResourceReference(Shape.StrokeProperty, chosen ? "Accent" : "Text3");
            dot.Children.Add(ring);
            if (chosen)
            {
                var fill = new Ellipse { Width = 6, Height = 6 };
                fill.SetResourceReference(Shape.FillProperty, "Accent");
                dot.Children.Add(fill);
            }
            var label = new TextBlock { Text = text, FontSize = 12, TextWrapping = TextWrapping.Wrap };
            if (chosen) { label.FontWeight = FontWeights.SemiBold; label.SetResourceReference(TextBlock.ForegroundProperty, "Accent"); }
            var row = new DockPanel();
            DockPanel.SetDock(dot, Dock.Left);
            row.Children.Add(dot);
            row.Children.Add(label);
            var b = new Button { Style = (Style)FindResource("Item"), Content = row, Margin = new Thickness(0, 0, 0, 2) };
            b.SetResourceReference(BackgroundProperty, chosen ? "AccentSubtle" : "Transparent");
            b.Tag = null;
            b.Click += (_, _) => { A.Service.SetMode(key); Shut(); };
            ModeList.Children.Add(b);
        }
    }

    void Open_Click(object sender, RoutedEventArgs e) { Shut(); A.ShowWindow(); }
    void Allow_Click(object sender, RoutedEventArgs e) { Shut(); A.SetDiscoverable(!A.Model.Allow); }
    void AutoAccept_Click(object sender, RoutedEventArgs e) { Shut(); A.SetAutoAccept(A.Settings.AutoAcceptMode == "off" ? "everyone" : "off"); }
    void Wifi_Click(object sender, RoutedEventArgs e) { Shut(); A.ShowWifi(); }
    void Settings_Click(object sender, RoutedEventArgs e) { Shut(); A.ShowSettings("general"); }
    void Report_Click(object sender, RoutedEventArgs e) { Shut(); App.RunReport(); }
    void Quit_Click(object sender, RoutedEventArgs e) { Shut(); A.QuitApp(); }
}
