// SPDX-License-Identifier: GPL-2.0-only
using System.Windows;
using System.Windows.Media;

namespace NekoDrop;

/// <summary>
/// Signing in to an Apple ID for Contacts Only: e-mail and password, then the
/// verification code if the account asks for one. appleid.exe does the work
/// (AppleId.SignIn); the password goes to it once and is never stored.
/// </summary>
public partial class AppleIdWindow : Window
{
    TaskCompletionSource<string?>? _code;
    bool _busy;

    public AppleIdWindow()
    {
        InitializeComponent();
        Native.Chrome(this);
        ExportLogButton.Visibility = AppleId.HasLog ? Visibility.Visible : Visibility.Collapsed;
        Loaded += (_, _) => EmailBox.Focus();
    }

    /// <summary>Apple's and Windows' terse answers, said so that the reader knows what to do (#42, #45).</summary>
    static string Explain(string failure)
    {
        // Apple refuses the iCloud for Windows 7.21 component for this account.
        if (failure.Contains("-27952")) return L.F("aid.tooOld", failure);
        // ERROR_BAD_EXE_FORMAT: AOSKit.dll was replaced by a 64-bit one (a newer iCloud's).
        if (failure.Contains("AOSKit") && failure.Contains("193")) return L.F("aid.wrongAoskit", failure);
        return failure;
    }

    void Say(string text, bool error = false)
    {
        StatusText.Text = text;
        StatusText.SetResourceReference(ForegroundProperty, error ? "Error" : "Text2");
    }

    async void Go_Click(object sender, RoutedEventArgs e)
    {
        if (_code is not null)
        {
            var typed = CodeBox.Text.Trim();
            if (typed.Length < 6) { Say(L.T("aid.codeShort"), true); return; }
            GoButton.IsEnabled = false;
            _code.TrySetResult(typed);
            _code = null;
            return;
        }
        if (_busy) return;
        var email = EmailBox.Text.Trim();
        var password = PasswordBox.Password;
        if (email.Length == 0 || password.Length == 0) { Say(L.T("aid.fill"), true); return; }
        _busy = true;
        GoButton.IsEnabled = false;
        Credentials.IsEnabled = false;
        Say(L.T("aid.working"));
        var failure = await AppleId.SignIn(email, password,
            ev => Dispatcher.Invoke(() =>
            {
                if ((string?)ev["ev"] == "status" && (string?)ev["msg"] is { } msg) { DownloadBar.Visibility = Visibility.Collapsed; Say(msg); }
                // The first sign-in downloads iCloud's component (about 160 MB): show how far (#42).
                if ((string?)ev["ev"] == "progress")
                {
                    var got = (double?)ev["done"] ?? 0;
                    var total = (double?)ev["total"] ?? 0;
                    DownloadBar.Visibility = Visibility.Visible;
                    DownloadBar.IsIndeterminate = total <= 0;
                    if (total > 0) DownloadBar.Value = got * 100 / total;
                    Say(total > 0
                        ? L.F("aid.downloading", (int)(got * 100 / total), (int)(got / 1048576), (int)(total / 1048576))
                        : L.F("aid.downloadingMb", (int)(got / 1048576)));
                }
            }),
            sent => Dispatcher.Invoke(() =>
            {
                _code = new TaskCompletionSource<string?>();
                Credentials.Visibility = Visibility.Collapsed;
                CodePanel.Visibility = Visibility.Visible;
                CodeHint.Text = L.T(sent ? "aid.codeSent" : "aid.codeSms");
                GoButton.Content = L.T("aid.verify");
                GoButton.IsEnabled = true;
                CodeBox.Focus();
                Say("");
                return _code.Task;
            }));
        _busy = false;
        if (failure is null)
        {
            DialogResult = true;
            return;
        }
        Say(Explain(failure), true);
        ExportLogButton.Visibility = AppleId.HasLog ? Visibility.Visible : Visibility.Collapsed;
        Credentials.IsEnabled = true;
        Credentials.Visibility = Visibility.Visible;
        CodePanel.Visibility = Visibility.Collapsed;
        GoButton.Content = L.T("aid.signIn");
        GoButton.IsEnabled = true;
    }

    void ExportLog_Click(object sender, RoutedEventArgs e)
    {
        if (!AppleId.HasLog) { Say(L.T("aid.noLog"), true); return; }
        var stamp = DateTime.Now.ToString("yyyyMMdd-HHmm");
        var dlg = new Microsoft.Win32.SaveFileDialog
        {
            Title = L.T("aid.exportLog"),
            Filter = "Log (*.txt)|*.txt",
            FileName = $"nekodrop-appleid-log-{stamp}.txt",
            InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.Desktop),
        };
        if (Native.ShowDialog(this, dlg, dlg.Title) != true) return;
        try
        {
            AppleId.ExportLog(dlg.FileName);
            Say(L.F("aid.logExported", System.IO.Path.GetFileName(dlg.FileName)));
            try { System.Diagnostics.Process.Start("explorer.exe", $"/select,\"{dlg.FileName}\""); } catch (Exception) { }
        }
        catch (Exception x) { Say(x.Message, true); }
    }

    void Cancel_Click(object sender, RoutedEventArgs e)
    {
        _code?.TrySetResult(null);
        if (!_busy) Close();
    }
}
