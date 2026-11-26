// SPDX-License-Identifier: GPL-2.0-only
using System.Windows;
using System.Windows.Media;

namespace NekoDrop;

/// <summary>
/// The colours of the design (docs: the nekodrop UI prototype): a light and a
/// dark set after Windows 11 Mica, and one accent at a time, either Windows'
/// own or one of four the user can switch to in the settings.
/// </summary>
public static class Theme
{
    /// <summary>An accent: its colour on light and on dark, and the text drawn on it in dark mode.</summary>
    public sealed record AccentSet(string Key, Color Light, Color Dark, Color DarkOn);

    static Color C(uint argb) => Color.FromArgb((byte)(argb >> 24), (byte)(argb >> 16), (byte)(argb >> 8), (byte)argb);

    /// <summary>The accents offered besides Windows' own ("").</summary>
    public static readonly AccentSet[] Accents =
    [
        new("blue", C(0xFF0067C0), C(0xFF4CC2FF), C(0xFF001B2E)),
        new("indigo", C(0xFF4F46E5), C(0xFF818CF8), C(0xFF0B0A2E)),
        new("teal", C(0xFF0F766E), C(0xFF2DD4BF), C(0xFF042422)),
        new("rose", C(0xFFBE123C), C(0xFFFB7185), C(0xFF2E050E)),
    ];

    public static bool Dark { get; private set; }

    /// <summary>The accent setting for colourful: Windows' accent, each state in its own colour.</summary>
    public const string Colourful = "colourful";

    /// <summary>
    /// The colours. In the default one-colour look, only the accent (and green
    /// for success, red for real errors) is used; colourful shows a declined
    /// send in amber and a failed one in red, as the design drew them.
    /// </summary>
    public static ResourceDictionary Build(string chosen = "", bool? forceDark = null, bool colourful = false)
    {
        var dark = Dark = forceDark ?? Native.AppsUseDarkTheme();
        colourful |= chosen == Colourful;
        Color accent, onAccent;
        if (Accents.FirstOrDefault(a => a.Key == chosen) is { } set)
        {
            accent = dark ? set.Dark : set.Light;
            onAccent = dark ? set.DarkOn : Colors.White;
        }
        else
        {
            var system = Native.Accent();
            accent = dark ? Lighten(system, 0.45) : system;
            onAccent = dark ? Darken(system, 0.75) : Colors.White;
        }
        SolidColorBrush B(Color c) { var b = new SolidColorBrush(c); b.Freeze(); return b; }
        Color A(Color c, double alpha) => Color.FromArgb((byte)(alpha * 255), c.R, c.G, c.B);

        var text = dark ? C(0xFFFFFFFF) : C(0xFF1A1A1A);
        var text2 = dark ? C(0xFFC8C8C8) : C(0xFF595959);
        var text3 = dark ? C(0xFF9E9E9E) : C(0xFF6E6E6E);
        var border = dark ? A(Colors.White, 0.09) : A(Colors.Black, 0.08);
        var borderStrong = dark ? A(Colors.White, 0.18) : A(Colors.Black, 0.16);
        var card = dark ? A(Colors.White, 0.065) : A(Colors.White, 0.80);
        var cardHover = dark ? A(Colors.White, 0.11) : A(Colors.White, 0.96);
        var success = dark ? C(0xFF6CCB5F) : C(0xFF0F7B0F);
        var amber = dark ? C(0xFFFCE100) : C(0xFF9D5D00);
        var error = dark ? C(0xFFFF99A4) : C(0xFFC42B1C);

        return new ResourceDictionary
        {
            // The design's names
            ["Accent"] = B(accent),
            ["AccentSubtle"] = B(A(accent, dark ? 0.16 : 0.11)),
            ["OnAccent"] = B(onAccent),
            ["Mica"] = B(dark ? C(0xF0202226) : C(0xF0F3F5F9)),
            ["MicaSolid"] = B(dark ? C(0xFF202226) : C(0xFFF3F5F9)),
            ["MicaAlt"] = B(dark ? C(0xF7282A30) : C(0xF7F8F9FC)),
            ["Card"] = B(card),
            ["CardHover"] = B(cardHover),
            ["RadarBg"] = B(dark ? A(Colors.White, 0.032) : A(Colors.White, 0.50)),
            ["Border"] = B(border),
            ["BorderStrong"] = B(borderStrong),
            ["Text"] = B(text),
            ["Text2"] = B(text2),
            ["Text3"] = B(text3),
            // One colour, the accent; success stays green, the design's amber is a
            // neutral grey, and only real errors are red.
            ["Success"] = B(success),
            ["SuccessBg"] = B(dark ? A(success, 0.15) : C(0xFFDFF6DD)),
            ["Warn"] = B(colourful ? amber : text2),
            ["WarnBg"] = B(colourful ? (dark ? A(amber, 0.14) : C(0xFFFFF4CE)) : card),
            // A device's send that was declined or failed (the radar and the list).
            ["Declined"] = B(colourful ? amber : text3),
            ["Failed"] = B(colourful ? error : text3),
            ["FailedText"] = B(colourful ? error : text2),
            ["Error"] = B(error),
            ["ErrorBg"] = B(dark ? A(error, 0.16) : C(0xFFFDE7E9)),
            // Older names still used by some windows
            ["AccentText"] = B(accent),
            ["TextSecondary"] = B(text2),
            ["Window"] = B(dark ? C(0xFF202226) : C(0xFFF3F5F9)),
            ["CardStroke"] = B(border),
            ["Divider"] = B(border),
            ["DropWell"] = B(card),
            ["Caution"] = B(colourful ? amber : text2),
            ["Critical"] = B(error),
            ["UiFont"] = new FontFamily("Segoe UI Variable Text, Segoe UI, Microsoft YaHei UI"),
            ["IconFont"] = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"),
        };
    }

    static Color Lighten(Color c, double t) =>
        Color.FromRgb((byte)(c.R + (255 - c.R) * t), (byte)(c.G + (255 - c.G) * t), (byte)(c.B + (255 - c.B) * t));
    static Color Darken(Color c, double t) =>
        Color.FromRgb((byte)(c.R * (1 - t)), (byte)(c.G * (1 - t)), (byte)(c.B * (1 - t)));
}
