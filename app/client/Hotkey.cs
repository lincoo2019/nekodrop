// SPDX-License-Identifier: GPL-2.0-only
using System.Runtime.InteropServices;
using System.Windows.Input;
using System.Windows.Interop;

namespace NekoDrop;

/// <summary>
/// The global shortcut, Win+Shift+D unless the user records another
/// (RegisterHotKey on a message-only window). Registering fails quietly when
/// another program holds the combination.
/// </summary>
public sealed class Hotkey : IDisposable
{
    const int WM_HOTKEY = 0x0312, Id = 0x6E6B; // "nk"
    const uint MOD_ALT = 0x1, MOD_CONTROL = 0x2, MOD_SHIFT = 0x4, MOD_WIN = 0x8, MOD_NOREPEAT = 0x4000;
    public const string Default = "Win+Shift+D";

    [DllImport("user32.dll", SetLastError = true)]
    static extern bool RegisterHotKey(IntPtr hWnd, int id, uint modifiers, uint vk);
    [DllImport("user32.dll")]
    static extern bool UnregisterHotKey(IntPtr hWnd, int id);

    readonly HwndSource _window;
    readonly Action _pressed;

    Hotkey(HwndSource window, Action pressed)
    {
        _window = window;
        _pressed = pressed;
        _window.AddHook(Hook);
    }

    public static Hotkey? Register(string combo, Action pressed)
    {
        if (!TryParse(combo, out var mods, out var key)) return null;
        var p = new HwndSourceParameters("nekodrop-hotkey") { ParentWindow = new IntPtr(-3) /* HWND_MESSAGE */ };
        var window = new HwndSource(p);
        var vk = (uint)KeyInterop.VirtualKeyFromKey(key);
        if (RegisterHotKey(window.Handle, Id, mods | MOD_NOREPEAT, vk)) return new Hotkey(window, pressed);
        window.Dispose();
        return null;
    }

    /// <summary>"Ctrl+Alt+K" into RegisterHotKey's modifiers and a WPF key: at least one modifier and one other key.</summary>
    public static bool TryParse(string combo, out uint mods, out Key key)
    {
        mods = 0;
        key = Key.None;
        foreach (var part in combo.Split('+', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
        {
            switch (part.ToLowerInvariant())
            {
                case "ctrl": mods |= MOD_CONTROL; break;
                case "alt": mods |= MOD_ALT; break;
                case "shift": mods |= MOD_SHIFT; break;
                case "win": mods |= MOD_WIN; break;
                default:
                    if (key != Key.None || !Enum.TryParse(part, true, out key)) return false;
                    break;
            }
        }
        return mods != 0 && key != Key.None;
    }

    /// <summary>A key press as a combination ("Ctrl+Shift+K"), or null while only modifiers are down or none is.</summary>
    public static string? FromKeys(ModifierKeys m, Key key)
    {
        if (key is Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt or Key.LeftShift or Key.RightShift
            or Key.LWin or Key.RWin or Key.None) return null;
        var parts = new List<string>();
        if (m.HasFlag(ModifierKeys.Control)) parts.Add("Ctrl");
        if (m.HasFlag(ModifierKeys.Alt)) parts.Add("Alt");
        if (m.HasFlag(ModifierKeys.Shift)) parts.Add("Shift");
        if (m.HasFlag(ModifierKeys.Windows)) parts.Add("Win");
        if (parts.Count == 0) return null;
        parts.Add(key.ToString());
        return string.Join("+", parts);
    }

    /// <summary>For display: "Win + Shift + D" (digit keys without WPF's D prefix).</summary>
    public static string Show(string combo) =>
        string.Join(" + ", combo.Split('+', StringSplitOptions.TrimEntries)
            .Select(p => p.Length == 2 && p[0] == 'D' && char.IsDigit(p[1]) ? p[1..] : p));

    IntPtr Hook(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WM_HOTKEY && wParam.ToInt32() == Id) { handled = true; _pressed(); }
        return IntPtr.Zero;
    }

    public void Dispose()
    {
        UnregisterHotKey(_window.Handle, Id);
        _window.RemoveHook(Hook);
        _window.Dispose();
    }
}
