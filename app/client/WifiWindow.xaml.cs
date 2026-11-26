// SPDX-License-Identifier: GPL-2.0-only
/*
 * nekodrop's Wi-Fi settings, for while nekodrop owns the card and Windows'
 * own Wi-Fi list is gone: nearby networks (the radio's firmware scan, via the
 * service), connect with a saved or new password, hidden networks, the saved
 * networks with forget and auto-connect, and the current connection.
 */
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace NekoDrop;

public partial class WifiWindow : Window
{
    static App A => App.Instance;
    string? _expanded; /* the network whose password box is open */

    public WifiWindow()
    {
        InitializeComponent();
        Native.Chrome(this);
        A.Service.State += OnState;
        A.Service.Reply += OnReply;
        Closed += (_, _) =>
        {
            A.Service.State -= OnState;
            A.Service.Reply -= OnReply;
        };
        if (A.Service.Last is { } last) Render(last);
        Scan();
    }

    void OnState(JsonObject o) => Dispatcher.BeginInvoke(() => Render(o));

    void OnReply(JsonObject o) => Dispatcher.BeginInvoke(() =>
    {
        if ((string?)o["event"] != "wifi-connect") return;
        var error = (string?)o["error"] ?? "";
        if (error == "password needed")
        {
            _expanded = (string?)o["ssid"];
            if (A.Service.Last is { } last) Render(last);
            return;
        }
        ShowError(error == "needs concurrent mode" ? L.T("wifi.needConcurrent") : error);
    });

    void ShowError(string text)
    {
        ErrorText.Text = text;
        ErrorText.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
    }

    void Scan() => A.Service.Send(new JsonObject { ["do"] = "wifi-scan" });

    void Connect(string ssid, string? password, bool? auto, bool hidden = false)
    {
        ShowError("");
        var cmd = new JsonObject { ["do"] = "wifi-connect", ["ssid"] = ssid, ["hidden"] = hidden };
        if (!string.IsNullOrEmpty(password)) cmd["password"] = password;
        if (auto is bool a) cmd["auto"] = a;
        A.Service.Send(cmd);
    }

    static string Bars(int dbm) => dbm >= -55 ? "\uE701" : dbm >= -67 ? "\uE874" : dbm >= -78 ? "\uE873" : "\uE872";

    static string SecurityText(string s) => s switch
    {
        "open" => L.T("wifi.sec.open"),
        "wpa2-psk" => "WPA2",
        "wpa2/3-psk" => "WPA2/WPA3",
        "wpa3-sae" => "WPA3",
        "owe" => L.T("wifi.sec.owe"),
        "802.1x" => L.T("wifi.sec.enterprise"),
        "wep" => "WEP",
        _ => s,
    };

    static (string Ip, string Gateway) Address()
    {
        foreach (var n in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (n.Name != "nekodrop") continue;
            var p = n.GetIPProperties();
            var ip = p.UnicastAddresses.FirstOrDefault(a => a.Address.AddressFamily == AddressFamily.InterNetwork)?.Address.ToString() ?? "";
            var gw = p.GatewayAddresses.FirstOrDefault(a => a.Address.AddressFamily == AddressFamily.InterNetwork)?.Address.ToString() ?? "";
            return (ip, gw);
        }
        return ("", "");
    }

    internal void Render(JsonObject o)
    {
        var wifi = (string?)o["wifi"] ?? "";
        var ssid = (string?)o["ssid"] ?? "";
        var radio = (string?)o["radio"] ?? "";
        var mode = (string?)o["mode"] ?? "";
        var scanning = (bool?)o["scanning"] ?? false;
        var connected = wifi == "connected";

        var concurrent = mode == "concurrent";
        NotConcurrent.Visibility = concurrent ? Visibility.Collapsed : Visibility.Visible;
        Concurrent.Visibility = concurrent ? Visibility.Visible : Visibility.Collapsed;

        var portal = A.Model.Portal;
        PortalNotice.Visibility = portal != "" && connected && portal == ssid ? Visibility.Visible : Visibility.Collapsed;
        PortalTitle.Text = L.F("wifi.portal.title", portal);

        // The current connection: the design's card with an accent stripe when connected.
        var stateBrush = "Text2";
        if (connected)
        {
            var (ip, gw) = Address();
            var sec = SecurityText((string?)(o["networks"] as JsonArray)?.OfType<JsonObject>()
                .FirstOrDefault(n => (string?)n["ssid"] == ssid)?["security"] ?? "");
            StateTitle.Text = ssid;
            StateDetail.Text = L.T("wifi.connected") + (sec == "" ? "" : " · " + sec);
            stateBrush = "Success";
            IpText.Text = ip == "" ? L.T("wifi.gettingAddress") : "IP: " + ip;
            GatewayText.Text = gw == "" ? "" : L.T("wifi.gateway") + ": " + gw;
        }
        else if (ssid != "" && wifi != "")
        {
            StateTitle.Text = L.F("wifi.connectingTo", ssid);
            StateDetail.Text = wifi;
        }
        else
        {
            StateTitle.Text = L.T("wifi.notConnected");
            StateDetail.Text = radio == "on" ? L.T("wifi.pick") : L.T("wifi.radioOff");
        }
        StateDetail.SetResourceReference(TextBlock.ForegroundProperty, stateBrush);
        StateGlyph.SetResourceReference(TextBlock.ForegroundProperty, connected ? "Accent" : "Text3");
        // Connected: the whole card is highlighted (accent tint and outline).
        CurrentCard.SetResourceReference(Border.BackgroundProperty, connected ? "AccentSubtle" : "Card");
        CurrentCard.SetResourceReference(Border.BorderBrushProperty, connected ? "Accent" : "Border");
        AddressRow.Visibility = connected ? Visibility.Visible : Visibility.Collapsed;
        DisconnectButton.Visibility = ssid != "" ? Visibility.Visible : Visibility.Collapsed;
        RefreshButton.IsEnabled = radio == "on" && !scanning;
        ScanText.Text = scanning ? L.T("wifi.scanning") : "";
        var serviceError = (string?)o["wifiError"] ?? "";
        if (serviceError != "") ShowError(serviceError);

        NetworkList.Children.Clear();
        var networks = (o["networks"] as JsonArray)?.OfType<JsonObject>().ToList() ?? [];
        EmptyCard.Visibility = networks.Count == 0 && !scanning ? Visibility.Visible : Visibility.Collapsed;
        NetworkCard.Visibility = networks.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        for (var i = 0; i < networks.Count; i++) NetworkList.Children.Add(Row(networks[i], i > 0));

        SavedList.Children.Clear();
        var known = (o["known"] as JsonArray)?.OfType<JsonObject>().ToList() ?? [];
        SavedTitle.Text = L.F("wifi.savedCount", known.Count);
        for (var i = 0; i < known.Count; i++)
        {
            var k = known[i];
            var kSsid = (string?)k["ssid"] ?? "";
            SavedList.Children.Add(SavedRow(kSsid, (bool?)k["auto"] ?? true, (bool?)k["hidden"] ?? false,
                                            kSsid == ssid && connected, i > 0));
        }
    }

    /// <summary>A rule above a row of a card list, after the first.</summary>
    Border RowBox(bool ruled, Thickness padding) => new()
    {
        BorderThickness = new Thickness(0, ruled ? 1 : 0, 0, 0),
        BorderBrush = (Brush)FindResource("Border"),
        Padding = padding,
    };

    UIElement Row(JsonObject n, bool ruled)
    {
        var ssid = (string?)n["ssid"] ?? "";
        var dbm = (int?)n["dbm"] ?? -90;
        var sec = (string?)n["security"] ?? "";
        var saved = (bool?)n["saved"] ?? false;
        var isConnected = (bool?)n["connected"] ?? false;
        var open = sec is "open" or "owe";
        var box = RowBox(ruled, new Thickness(12));
        // The network in use: the row highlighted, "connected" in the accent instead of a button.
        if (isConnected) box.SetResourceReference(Border.BackgroundProperty, "AccentSubtle");
        var stack = new StackPanel();
        var line = new DockPanel();
        var action = new Button
        {
            Content = L.T("wifi.connect"),
            Style = (Style)FindResource("WinButton"),
            VerticalAlignment = VerticalAlignment.Center,
        };
        FrameworkElement right = action;
        if (isConnected)
        {
            var done = new TextBlock { Text = L.T("wifi.connected"), FontSize = 12, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center };
            done.SetResourceReference(TextBlock.ForegroundProperty, "Accent");
            right = done;
        }
        DockPanel.SetDock(right, Dock.Right);
        line.Children.Add(right);
        var glyph = new TextBlock { Style = (Style)FindResource("Icon"), Text = Bars(dbm), FontSize = 15, Margin = new Thickness(0, 0, 10, 0) };
        glyph.SetResourceReference(TextBlock.ForegroundProperty, "Text2");
        DockPanel.SetDock(glyph, Dock.Left);
        line.Children.Add(glyph);
        var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        var title = new StackPanel { Orientation = Orientation.Horizontal };
        title.Children.Add(new TextBlock { Text = ssid, FontSize = 13, FontWeight = FontWeights.Medium, TextTrimming = TextTrimming.CharacterEllipsis, MaxWidth = 190 });
        if (!open)
        {
            var lockGlyph = new TextBlock { Style = (Style)FindResource("Icon"), Text = "\uE72E", FontSize = 10, Margin = new Thickness(6, 1, 0, 0), ToolTip = SecurityText(sec) };
            lockGlyph.SetResourceReference(TextBlock.ForegroundProperty, "Text3");
            title.Children.Add(lockGlyph);
        }
        if (saved)
        {
            var tag = new Border { CornerRadius = new CornerRadius(8), Padding = new Thickness(6, 0, 6, 1), Margin = new Thickness(6, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            tag.SetResourceReference(Border.BackgroundProperty, "AccentSubtle");
            var tagText = new TextBlock { Text = L.T("wifi.savedTag"), FontSize = 10 };
            tagText.SetResourceReference(TextBlock.ForegroundProperty, "Accent");
            tag.Child = tagText;
            title.Children.Add(tag);
        }
        text.Children.Add(title);
        var sub = new TextBlock { Text = SecurityText(sec) + $" · {dbm} dBm", FontSize = 11 };
        sub.SetResourceReference(TextBlock.ForegroundProperty, "Text3");
        text.Children.Add(sub);
        line.Children.Add(text);
        stack.Children.Add(line);

        if (_expanded == ssid && !isConnected)
        {
            var pass = new PasswordBox { Style = (Style)FindResource("WinPasswordBox"), ToolTip = L.T("wifi.password") };
            var auto = new CheckBox { Style = (Style)FindResource("WinToggle"), IsChecked = true, Margin = new Thickness(0, 0, 6, 0) };
            var go = new Button { Content = L.T("wifi.connect"), Style = (Style)FindResource("WinPrimary"), Margin = new Thickness(8, 0, 0, 0) };
            go.Click += (_, _) => { _expanded = null; Connect(ssid, pass.Password, auto.IsChecked == true); };
            pass.KeyDown += (_, e) => { if (e.Key == System.Windows.Input.Key.Enter) { _expanded = null; Connect(ssid, pass.Password, auto.IsChecked == true); } };
            var entry = new DockPanel { Margin = new Thickness(0, 10, 0, 0) };
            DockPanel.SetDock(go, Dock.Right);
            entry.Children.Add(go);
            entry.Children.Add(pass);
            stack.Children.Add(entry);
            var autoRow = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 8, 0, 0) };
            autoRow.Children.Add(auto);
            var autoText = new TextBlock { Text = L.T("wifi.autoConnect"), FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
            autoText.SetResourceReference(TextBlock.ForegroundProperty, "Text2");
            autoRow.Children.Add(autoText);
            stack.Children.Add(autoRow);
            Dispatcher.BeginInvoke(() => pass.Focus());
        }
        action.Click += (_, _) =>
        {
            if (saved || open) Connect(ssid, null, null);
            else
            {
                _expanded = _expanded == ssid ? null : ssid;
                if (A.Service.Last is { } last) Render(last);
            }
        };
        box.Child = stack;
        return box;
    }

    UIElement SavedRow(string ssid, bool auto, bool hidden, bool isConnected, bool ruled)
    {
        var box = RowBox(ruled, new Thickness(12, 10, 12, 10));
        if (isConnected) box.SetResourceReference(Border.BackgroundProperty, "AccentSubtle");
        var row = new DockPanel();
        var forget = new Button { Content = L.T("wifi.forget"), Style = (Style)FindResource("LinkButton"), Margin = new Thickness(10, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
        forget.SetResourceReference(ForegroundProperty, "Error");
        forget.Click += (_, _) => A.Service.Send(new JsonObject { ["do"] = "wifi-forget", ["ssid"] = ssid });
        DockPanel.SetDock(forget, Dock.Right);
        row.Children.Add(forget);

        FrameworkElement connectSlot;
        if (isConnected)
        {
            var done = new TextBlock { Text = L.T("wifi.connected"), FontSize = 11, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(10, 0, 0, 0) };
            done.SetResourceReference(TextBlock.ForegroundProperty, "Accent");
            connectSlot = done;
        }
        else
        {
            var connect = new Button { Content = L.T("wifi.connect"), Style = (Style)FindResource("Small"), Margin = new Thickness(10, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            connect.Click += (_, _) => Connect(ssid, null, null, hidden: true);
            connectSlot = connect;
        }
        DockPanel.SetDock(connectSlot, Dock.Right);
        row.Children.Add(connectSlot);

        var check = new CheckBox { Style = (Style)FindResource("WinToggle"), IsChecked = auto, ToolTip = L.T("wifi.autoConnect") };
        check.Click += (_, _) => A.Service.Send(new JsonObject { ["do"] = "wifi-auto", ["ssid"] = ssid, ["on"] = check.IsChecked == true });
        DockPanel.SetDock(check, Dock.Right);
        row.Children.Add(check);
        var autoText = new TextBlock { Text = L.T("wifi.auto"), FontSize = 11, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 6, 0), ToolTip = L.T("wifi.autoConnect") };
        autoText.SetResourceReference(TextBlock.ForegroundProperty, "Text2");
        DockPanel.SetDock(autoText, Dock.Right);
        row.Children.Add(autoText);
        var label = new TextBlock { Text = ssid, FontSize = 12, FontWeight = FontWeights.Medium, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
        if (hidden) label.ToolTip = L.T("wifi.hiddenShort");
        row.Children.Add(label);
        box.Child = row;
        return box;
    }

    void Refresh_Click(object sender, RoutedEventArgs e) => Scan();

    void Disconnect_Click(object sender, RoutedEventArgs e) => A.Service.Send(new JsonObject { ["do"] = "wifi-disconnect" });

    void ShowHidden_Click(object sender, RoutedEventArgs e) =>
        HiddenForm.Visibility = HiddenForm.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible;

    void Hidden_Click(object sender, RoutedEventArgs e)
    {
        var ssid = HiddenSsid.Text.Trim();
        if (ssid.Length is 0 or > 32) return;
        Connect(ssid, HiddenPassword.Password, true, hidden: true);
        HiddenForm.Visibility = Visibility.Collapsed;
    }

    void SwitchMode_Click(object sender, RoutedEventArgs e) => A.Service.SetMode("concurrent");

    void Portal_Click(object sender, RoutedEventArgs e)
    {
        try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("http://www.msftconnecttest.com/redirect") { UseShellExecute = true }); }
        catch (Exception) { }
    }

    void Close_Click(object sender, RoutedEventArgs e) => Close();
}
