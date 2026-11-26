// SPDX-License-Identifier: GPL-2.0-only
using System.Collections.Specialized;
using System.Windows;
using System.Windows.Controls;

namespace NekoDrop;

/// <summary>Full persisted transfer history (received and sent) with filter, search and quick actions.</summary>
public partial class HistoryWindow : Window
{
    static App A => App.Instance;

    public HistoryWindow()
    {
        InitializeComponent();
        Native.Chrome(this);
        A.Model.History.CollectionChanged += OnHistoryChanged;
        Closed += (_, _) => A.Model.History.CollectionChanged -= OnHistoryChanged;
        Refresh();
    }

    void OnHistoryChanged(object? sender, NotifyCollectionChangedEventArgs e) => Refresh();

    void Filter_Changed(object sender, RoutedEventArgs e)
    {
        if (HistoryList is null) return;
        Refresh();
    }

    void Search_Changed(object sender, TextChangedEventArgs e)
    {
        if (SearchHint is not null)
            SearchHint.Visibility = string.IsNullOrEmpty(SearchBox.Text) ? Visibility.Visible : Visibility.Collapsed;
        Refresh();
    }

    void Refresh()
    {
        var query = (SearchBox?.Text ?? "").Trim();
        IEnumerable<TransferRecord> items = A.Model.History;
        if (FilterReceived?.IsChecked == true) items = items.Where(r => r.IsReceived);
        else if (FilterSent?.IsChecked == true) items = items.Where(r => r.IsSent);
        if (query.Length > 0)
            items = items.Where(r =>
                r.Name.Contains(query, StringComparison.OrdinalIgnoreCase) ||
                r.Peer.Contains(query, StringComparison.OrdinalIgnoreCase) ||
                r.Url.Contains(query, StringComparison.OrdinalIgnoreCase));
        var list = items.ToList();
        HistoryList.ItemsSource = list;
        EmptyText.Visibility = list.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        ClearButton.Visibility = A.Model.History.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        StatusText.Text = L.F("hist.count", list.Count);
    }

    async void Copy_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is not TransferRecord r) return;
        if (MediaHelper.CopyToClipboard(r.Paths, r.Url, out var label, out var error))
        {
            StatusText.Text = "✓ " + L.F("clip.copied", label);
            if (sender is Button btn && btn.Content is TextBlock tb)
            {
                var prev = tb.Text;
                tb.Text = "\uE73E";
                await Task.Delay(1200);
                tb.Text = prev;
            }
        }
        else
        {
            StatusText.Text = L.F("clip.failed", error);
        }
    }

    void Open_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is not TransferRecord r) return;
        if (!MediaHelper.OpenRecord(r)) StatusText.Text = L.T("clip.missing");
    }

    void Resend_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is not TransferRecord r) return;
        var existing = r.Paths.Where(p => System.IO.File.Exists(p) || System.IO.Directory.Exists(p)).ToList();
        if (existing.Count == 0) { StatusText.Text = L.T("clip.missing"); return; }
        A.Model.Files.Clear();
        foreach (var p in existing) A.Model.Files.Add(p);
        A.ShowSendWindow();
    }

    void Delete_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is not TransferRecord r) return;
        A.RemoveHistory(r);
    }

    void Clear_Click(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show(this, L.T("hist.clearAsk"), L.T("hist.heading"),
                MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
        A.ClearHistory();
    }

    void Close_Click(object sender, RoutedEventArgs e) => Close();
}
