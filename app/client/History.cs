// SPDX-License-Identifier: GPL-2.0-only
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

namespace NekoDrop;

/// <summary>One persisted AirDrop transfer (received or sent).</summary>
public sealed class TransferRecord
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N")[..10];
    /// <summary>"received" or "sent".</summary>
    public string Direction { get; set; } = "received";
    public string Peer { get; set; } = "";
    public string Name { get; set; } = "";
    public List<string> Paths { get; set; } = [];
    /// <summary>Web URL (http/https) when this record represents a shared link.</summary>
    public string Url { get; set; } = "";
    public long Bytes { get; set; }
    /// <summary>"done", "failed", or "declined".</summary>
    public string Status { get; set; } = "done";
    public string Error { get; set; } = "";
    public bool LivePhoto { get; set; }
    public DateTime When { get; set; } = DateTime.Now;

    [JsonIgnore] public string PrimaryPath => Paths.FirstOrDefault() ?? "";
    [JsonIgnore] public bool IsReceived => Direction == "received";
    [JsonIgnore] public bool IsSent => Direction == "sent";
    [JsonIgnore] public bool IsLink => !string.IsNullOrEmpty(Url);
    [JsonIgnore] public bool Failed => Status is "failed" or "declined";
    [JsonIgnore] public bool CanOpen => !Failed && (IsLink || Paths.Count > 0);
    [JsonIgnore] public bool CanCopy => !Failed && (IsLink || Paths.Count > 0);
    [JsonIgnore] public bool CanResend => IsSent && Paths.Count > 0;

    [JsonIgnore]
    public string Glyph => Failed ? "\uE783" : IsLink ? "\uE71B" : IsSent ? "\uE724" : "\uE896";

    [JsonIgnore]
    public string WhenText => When.Date == DateTime.Today ? When.ToString("HH:mm") : When.ToString("MM/dd HH:mm");

    [JsonIgnore]
    public string PeerLabel => IsSent ? "→ " + Peer : Peer;

    [JsonIgnore]
    public string SizeText => FormatBytes(Bytes);

    [JsonIgnore]
    public string StatusText => Status switch
    {
        "declined" => L.T("send.declined"),
        "failed" => IsSent ? L.T("hist.sendFailed") : L.T("main.incomplete"),
        _ => "",
    };

    [JsonIgnore]
    public string DetailLine
    {
        get
        {
            var parts = new List<string> { IsSent ? L.F("hist.to", Peer) : L.F("hist.from", Peer) };
            if (LivePhoto) parts.Add("Live Photo");
            if (SizeText.Length > 0) parts.Add(SizeText);
            parts.Add(WhenText);
            if (StatusText.Length > 0) parts.Add(string.IsNullOrEmpty(Error) ? StatusText : $"{StatusText}: {Error}");
            return string.Join(" · ", parts);
        }
    }

    [JsonIgnore]
    public string TooltipText => IsLink ? Url : PrimaryPath;

    public static string FormatBytes(long bytes)
    {
        if (bytes <= 0) return "";
        if (bytes < 1024) return $"{bytes} B";
        if (bytes < 1024L * 1024) return $"{bytes / 1024.0:0.#} KB";
        if (bytes < 1024L * 1024 * 1024) return $"{bytes / (1024.0 * 1024):0.#} MB";
        return $"{bytes / (1024.0 * 1024 * 1024):0.##} GB";
    }
}

/// <summary>
/// Persists recent AirDrop transfers in %LocalAppData%\nekodrop\history.json.
/// </summary>
public static partial class TransferHistory
{
    public const int MaxEntries = 200;

    static string Folder => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "nekodrop");
    static string FilePath => Path.Combine(Folder, "history.json");
    static readonly JsonSerializerOptions Options = new() { WriteIndented = true };

    public static List<TransferRecord> Load()
    {
        try
        {
            if (!File.Exists(FilePath)) return [];
            var items = JsonSerializer.Deserialize<List<TransferRecord>>(File.ReadAllText(FilePath), Options) ?? [];
            return items.Take(MaxEntries).ToList();
        }
        catch (Exception) { return []; }
    }

    public static void Save(IEnumerable<TransferRecord> records)
    {
        try
        {
            Directory.CreateDirectory(Folder);
            var trimmed = records.Take(MaxEntries).ToList();
            var tmp = FilePath + ".tmp";
            File.WriteAllText(tmp, JsonSerializer.Serialize(trimmed, Options));
            File.Move(tmp, FilePath, overwrite: true);
        }
        catch (Exception) { }
    }

    /// <summary>
    /// Extracts a safe http(s) URL from a stored .url shortcut or .webloc plist,
    /// returning "" for any other file or non-web scheme.
    /// </summary>
    public static string TryReadUrlFile(string path)
    {
        try
        {
            if (!File.Exists(path)) return "";
            var ext = Path.GetExtension(path).ToLowerInvariant();
            if (ext == ".url")
            {
                foreach (var line in File.ReadLines(path))
                {
                    if (!line.StartsWith("URL=", StringComparison.OrdinalIgnoreCase)) continue;
                    var url = line[4..].Trim();
                    return IsWebUrl(url) ? url : "";
                }
            }
            else if (ext == ".webloc")
            {
                var text = File.ReadAllText(path);
                var m = WeblocUrlRegex().Match(text);
                if (m.Success)
                {
                    var url = System.Net.WebUtility.HtmlDecode(m.Groups[1].Value.Trim());
                    return IsWebUrl(url) ? url : "";
                }
            }
            else if (ext == ".txt" && new FileInfo(path).Length <= 4096)
            {
                var text = File.ReadAllText(path);
                var m = PlainTextUrlRegex().Match(text);
                if (m.Success)
                {
                    var url = m.Value.TrimEnd('.', ',', ';', ':', '!', '?', ')', ']', '}', '）', '】', '》', '。', '，', '；', '：', '！', '？');
                    return IsWebUrl(url) ? url : "";
                }
            }
        }
        catch (Exception) { }
        return "";
    }

    public static bool IsWebUrl(string? url) =>
        !string.IsNullOrWhiteSpace(url) &&
        Uri.TryCreate(url, UriKind.Absolute, out var uri) &&
        (uri.Scheme == Uri.UriSchemeHttp || uri.Scheme == Uri.UriSchemeHttps);

    public static string ShortUrlLabel(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri)) return url;
        var path = uri.AbsolutePath.TrimEnd('/');
        if (path.Length > 28) path = path[..28] + "…";
        return string.IsNullOrEmpty(path) ? uri.Host : uri.Host + path;
    }

    [GeneratedRegex(@"<string>\s*(https?://[^<\s]+)\s*</string>", RegexOptions.IgnoreCase)]
    private static partial Regex WeblocUrlRegex();

    [GeneratedRegex(@"https?://[^\s<>""'\x00-\x1f\x7f]+", RegexOptions.IgnoreCase)]
    private static partial Regex PlainTextUrlRegex();
}
