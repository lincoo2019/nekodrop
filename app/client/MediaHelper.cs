// SPDX-License-Identifier: GPL-2.0-only
// Clipboard dual-format image+file copy and HEIC/Live Photo JPEG copy UX
// inspired by DropBridge (https://github.com/ndpyzwy-0w0/DropBridge).
using System.Collections.Specialized;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Media.Imaging;

namespace NekoDrop;

/// <summary>
/// File, URL, clipboard, and HEIC/Live Photo helpers shared by notifications,
/// the main flyout, and the transfer history window.
/// </summary>
public static partial class MediaHelper
{
    static readonly HashSet<string> ImageExts = new(StringComparer.OrdinalIgnoreCase)
    {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tif", ".tiff", ".heic", ".heif", ".webp",
    };

    static readonly HashSet<string> ToastHeroExts = new(StringComparer.OrdinalIgnoreCase)
    {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp",
    };

    static readonly HashSet<string> HeicExts = new(StringComparer.OrdinalIgnoreCase)
    {
        ".heic", ".heif",
    };

    static readonly HashSet<string> MotionExts = new(StringComparer.OrdinalIgnoreCase)
    {
        ".mov", ".mp4",
    };

    public static bool IsImage(string path) => ImageExts.Contains(Path.GetExtension(path));
    public static bool IsToastPreviewable(string path) => ToastHeroExts.Contains(Path.GetExtension(path));
    public static bool IsHeic(string path) => HeicExts.Contains(Path.GetExtension(path));
    public static bool IsMotion(string path) => MotionExts.Contains(Path.GetExtension(path));

    /// <summary>
    /// Security boundary for nekodrop: protocol actions: ensures a path lies inside
    /// the configured SaveFolder so untrusted external URIs cannot read arbitrary files.
    /// </summary>
    public static bool IsInsideFolder(string file, string folder)
    {
        try
        {
            var root = Path.GetFullPath(folder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var full = Path.GetFullPath(file);
            return full.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception) { return false; }
    }

    /// <summary>
    /// Copies a web URL, a picture (both Bitmap for chat apps and FileDrop for Explorer),
    /// or one/more files to the Windows clipboard. Must be called on the UI (STA) thread.
    /// </summary>
    public static bool CopyToClipboard(IEnumerable<string> paths, string? url, out string label, out string error)
    {
        label = "";
        error = "";
        try
        {
            if (TransferHistory.IsWebUrl(url))
            {
                Clipboard.SetText(url!);
                label = TransferHistory.ShortUrlLabel(url!);
                return true;
            }
            var existing = paths.Where(p => File.Exists(p) || Directory.Exists(p)).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
            if (existing.Count == 0)
            {
                error = L.T("clip.missing");
                return false;
            }

            // If a single .url / .webloc shortcut is copied, copy its URL text directly.
            if (existing.Count == 1)
            {
                var linkUrl = TransferHistory.TryReadUrlFile(existing[0]);
                if (TransferHistory.IsWebUrl(linkUrl))
                {
                    Clipboard.SetText(linkUrl);
                    label = TransferHistory.ShortUrlLabel(linkUrl);
                    return true;
                }
            }

            var drop = new StringCollection();
            foreach (var p in existing) drop.Add(p);
            var data = new DataObject();
            data.SetFileDropList(drop);

            // For a single image (or a Live Photo whose primary file is an image),
            // also put the decoded Bitmap on the clipboard so pasting in WeChat/QQ/etc.
            // sends the picture directly while Explorer still pastes the file.
            var imageFile = existing.FirstOrDefault(p => File.Exists(p) && IsImage(p));
            if (imageFile is not null && (existing.Count == 1 || existing.All(p => IsImage(p) || IsMotion(p))))
            {
                var bmp = TryLoadBitmap(imageFile);
                if (bmp is not null) data.SetImage(bmp);
            }

            Clipboard.SetDataObject(data, true);
            label = existing.Count == 1 ? Path.GetFileName(existing[0]) : L.F("done.items", existing.Count);
            return true;
        }
        catch (Exception e)
        {
            error = e.Message;
            return false;
        }
    }

    /// <summary>Opens a web URL in the default browser, or reveals a file/folder in Explorer.</summary>
    public static bool OpenRecord(TransferRecord record)
    {
        if (record.IsLink && TransferHistory.IsWebUrl(record.Url))
            return OpenUrl(record.Url);
        var path = record.Paths.FirstOrDefault(p => File.Exists(p) || Directory.Exists(p));
        return path is not null && RevealInExplorer(path);
    }

    public static bool OpenUrl(string url)
    {
        if (!TransferHistory.IsWebUrl(url)) return false;
        try
        {
            Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
            return true;
        }
        catch (Exception) { return false; }
    }

    public static bool RevealInExplorer(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                Process.Start("explorer.exe", $"/select,\"{path}\"");
                return true;
            }
            if (Directory.Exists(path))
            {
                Process.Start("explorer.exe", $"\"{path}\"");
                return true;
            }
        }
        catch (Exception) { }
        return false;
    }

    static BitmapSource? TryLoadBitmap(string path)
    {
        try
        {
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            var decoder = BitmapDecoder.Create(fs, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
            var frame = decoder.Frames.FirstOrDefault();
            if (frame is null) return null;
            var oriented = ApplyExifOrientation(frame);
            oriented.Freeze();
            return oriented;
        }
        catch (Exception) { return null; }
    }

    /// <summary>
    /// Converts a .heic / .heif photo into a sibling .jpg copy using Windows WIC
    /// (requires Microsoft's HEIF Image Extensions on Windows 10/11; fails gracefully if absent).
    /// </summary>
    public static bool TryConvertHeicToJpg(string sourcePath, out string jpgPath, out string error)
    {
        jpgPath = "";
        error = "";
        if (!File.Exists(sourcePath) || !IsHeic(sourcePath)) return false;
        try
        {
            var dir = Path.GetDirectoryName(sourcePath)!;
            var stem = Path.GetFileNameWithoutExtension(sourcePath);
            var target = UniquePath(dir, stem, ".jpg");

            using var fs = new FileStream(sourcePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            var decoder = BitmapDecoder.Create(fs, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
            var frame = decoder.Frames.FirstOrDefault();
            if (frame is null) { error = "no image frame"; return false; }
            var oriented = ApplyExifOrientation(frame);

            var encoder = new JpegBitmapEncoder { QualityLevel = 92 };
            encoder.Frames.Add(BitmapFrame.Create(oriented));
            var tmp = target + ".tmp";
            using (var outStream = new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None))
                encoder.Save(outStream);
            File.Move(tmp, target, overwrite: true);
            jpgPath = target;
            return true;
        }
        catch (Exception e)
        {
            error = e.Message;
            return false;
        }
    }

    static BitmapSource ApplyExifOrientation(BitmapFrame frame)
    {
        try
        {
            if (frame.Metadata is BitmapMetadata meta && meta.ContainsQuery("System.Photo.Orientation"))
            {
                var raw = meta.GetQuery("System.Photo.Orientation");
                var orientation = raw is ushort u ? u : raw is int i ? (ushort)i : (ushort)1;
                System.Windows.Media.Transform? transform = orientation switch
                {
                    2 => new System.Windows.Media.ScaleTransform(-1, 1),
                    3 => new System.Windows.Media.RotateTransform(180),
                    4 => new System.Windows.Media.ScaleTransform(1, -1),
                    6 => new System.Windows.Media.RotateTransform(90),
                    8 => new System.Windows.Media.RotateTransform(270),
                    _ => null,
                };
                if (transform is not null)
                    return new TransformedBitmap(frame, transform);
            }
        }
        catch (Exception) { /* metadata missing or codec doesn't expose EXIF orientation */ }
        return frame;
    }

    static string UniquePath(string dir, string stem, string ext)
    {
        var candidate = Path.Combine(dir, stem + ext);
        if (!File.Exists(candidate)) return candidate;
        for (var i = 2; i < 1000; i++)
        {
            var next = Path.Combine(dir, $"{stem} ({i}){ext}");
            if (!File.Exists(next)) return next;
        }
        return Path.Combine(dir, $"{stem}-{Guid.NewGuid().ToString("N")[..6]}{ext}");
    }

    public static long SumBytes(IEnumerable<string> paths)
    {
        long total = 0;
        foreach (var p in paths)
        {
            try
            {
                if (File.Exists(p)) total += new FileInfo(p).Length;
            }
            catch (Exception) { }
        }
        return total;
    }

    static readonly HttpClient ThumbHttp = CreateThumbHttp();

    static HttpClient CreateThumbHttp()
    {
        var handler = new HttpClientHandler
        {
            AllowAutoRedirect = true,
            AutomaticDecompression = System.Net.DecompressionMethods.All,
        };
        var c = new HttpClient(handler) { Timeout = TimeSpan.FromMilliseconds(3500) };
        c.DefaultRequestHeaders.UserAgent.ParseAdd("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
        c.DefaultRequestHeaders.AcceptLanguage.ParseAdd("zh-CN,zh;q=0.9,en;q=0.8");
        return c;
    }

    static bool _j2kReady;

    /// <summary>
    /// Ensures a preview icon saved from AirDrop Ask's FileIcon is toast-ready (.jpg/.png).
    /// Decodes Apple's JPEG 2000 (.jp2) icons via CoreJ2K.Windows; returns null if unreadable.
    /// </summary>
    public static string? PrepareToastPreview(string? path)
    {
        if (string.IsNullOrEmpty(path) || !File.Exists(path)) return null;
        if (IsToastPreviewable(path)) return path;
        try
        {
            var target = Path.ChangeExtension(path, ".jpg");
            if (Path.GetExtension(path).Equals(".jp2", StringComparison.OrdinalIgnoreCase))
            {
                if (!_j2kReady)
                {
                    CoreJ2K.Util.ImageFactory.Register(new CoreJ2K.Util.WindowsBitmapImageCreator());
                    _j2kReady = true;
                }
                using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                var img = CoreJ2K.J2kImage.FromStream(fs);
                using var bmp = img.As<System.Drawing.Bitmap>();
                if (bmp is null) return null;
                bmp.Save(target, System.Drawing.Imaging.ImageFormat.Jpeg);
                return target;
            }
            var wic = TryLoadBitmap(path);
            if (wic is null) return null;
            var enc = new JpegBitmapEncoder { QualityLevel = 88 };
            enc.Frames.Add(BitmapFrame.Create(wic));
            using (var outFs = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None))
                enc.Save(outFs);
            return target;
        }
        catch (Exception) { return null; }
    }

    public static string? TryExtractYoutubeId(string? url)
    {
        if (!TransferHistory.IsWebUrl(url) || !Uri.TryCreate(url, UriKind.Absolute, out var uri)) return null;
        var host = uri.Host.ToLowerInvariant();
        string? candidate = null;
        if (host is "youtu.be" or "www.youtu.be")
        {
            candidate = uri.AbsolutePath.Trim('/').Split('/')[0];
        }
        else if (host is "youtube.com" or "www.youtube.com" or "m.youtube.com" or "music.youtube.com")
        {
            var segs = uri.AbsolutePath.Trim('/').Split('/');
            if (segs.Length >= 1 && segs[0].Equals("watch", StringComparison.OrdinalIgnoreCase))
            {
                var q = System.Web.HttpUtility.ParseQueryString(uri.Query);
                candidate = q["v"];
            }
            else if (segs.Length >= 2 && (segs[0].Equals("shorts", StringComparison.OrdinalIgnoreCase) ||
                                          segs[0].Equals("embed", StringComparison.OrdinalIgnoreCase) ||
                                          segs[0].Equals("live", StringComparison.OrdinalIgnoreCase)))
            {
                candidate = segs[1];
            }
        }
        return candidate is not null && YoutubeIdRegex().IsMatch(candidate) ? candidate : null;
    }

    /// <summary>
    /// Fetches clean 16:9 cover thumbnail and video title for YouTube and Bilibili links
    /// (3 s timeout, silent fallback to local FileIcon / raw URL if offline).
    /// </summary>
    public static async Task<(string? Title, string? ThumbPath)> TryFetchLinkPreviewAsync(string? url, string? existingThumb)
    {
        if (!TransferHistory.IsWebUrl(url) || !Uri.TryCreate(url, UriKind.Absolute, out var uri))
            return (null, existingThumb);

        var ytId = TryExtractYoutubeId(url);
        if (ytId is not null)
        {
            var titleTask = FetchYoutubeTitleAsync(ytId);
            var thumbTask = FetchYoutubeThumb16x9Async(ytId);
            await Task.WhenAll(titleTask, thumbTask);
            return (titleTask.Result, thumbTask.Result ?? existingThumb);
        }

        var host = uri.Host.ToLowerInvariant();
        if (host is "b23.tv" or "www.b23.tv" or "bilibili.com" or "www.bilibili.com" or "m.bilibili.com")
            return await FetchBilibiliPreviewAsync(url!, existingThumb);

        return (null, existingThumb);
    }

    static async Task<string?> FetchYoutubeTitleAsync(string videoId)
    {
        try
        {
            var oembed = $"https://www.youtube.com/oembed?url=https://www.youtube.com/watch?v={videoId}&format=json";
            using var res = await ThumbHttp.GetAsync(oembed);
            if (!res.IsSuccessStatusCode) return null;
            var node = System.Text.Json.Nodes.JsonNode.Parse(await res.Content.ReadAsStringAsync());
            var title = ((string?)node?["title"])?.Trim();
            return string.IsNullOrEmpty(title) ? null : title;
        }
        catch (Exception) { return null; }
    }

    static async Task<string?> FetchYoutubeThumb16x9Async(string videoId)
    {
        try
        {
            var dir = Path.Combine(Path.GetTempPath(), "nekodrop-previews");
            Directory.CreateDirectory(dir);
            var target = Path.Combine(dir, $"yt16x9-{videoId}.jpg");
            if (File.Exists(target) && new FileInfo(target).Length > 1000) return target;
            // Pure 16:9 endpoints (no 4:3 letterbox black bars like hqdefault.jpg).
            foreach (var variant in new[] { "maxresdefault.jpg", "mqdefault.jpg" })
            {
                try
                {
                    using var res = await ThumbHttp.GetAsync($"https://i.ytimg.com/vi/{videoId}/{variant}");
                    if (!res.IsSuccessStatusCode) continue;
                    var bytes = await res.Content.ReadAsByteArrayAsync();
                    if (bytes.Length < 1000 || bytes.Length > 2 * 1024 * 1024 || bytes[0] != 0xFF || bytes[1] != 0xD8) continue;
                    var tmp = target + ".tmp";
                    await File.WriteAllBytesAsync(tmp, bytes);
                    File.Move(tmp, target, overwrite: true);
                    return target;
                }
                catch (Exception) { }
            }
        }
        catch (Exception) { }
        return null;
    }

    static async Task<(string? Title, string? ThumbPath)> FetchBilibiliPreviewAsync(string url, string? existingThumb)
    {
        try
        {
            using var res = await ThumbHttp.GetAsync(url, HttpCompletionOption.ResponseHeadersRead);
            if (!res.IsSuccessStatusCode) return (null, existingThumb);
            var finalUri = res.RequestMessage?.RequestUri;
            if (finalUri is null) return (null, existingThumb);
            var finalHost = finalUri.Host.ToLowerInvariant();
            if (!finalHost.EndsWith("bilibili.com", StringComparison.Ordinal) && !finalHost.EndsWith("b23.tv", StringComparison.Ordinal))
                return (null, existingThumb);

            // Read only the first 96 KB (more than enough for <head> OpenGraph meta tags).
            using var stream = await res.Content.ReadAsStreamAsync();
            var buf = new byte[98304];
            var read = 0;
            while (read < buf.Length)
            {
                var n = await stream.ReadAsync(buf.AsMemory(read, buf.Length - read));
                if (n <= 0) break;
                read += n;
            }
            var html = System.Text.Encoding.UTF8.GetString(buf, 0, read);

            var title = ExtractMetaContent(html, "og:title");
            if (string.IsNullOrEmpty(title))
            {
                var tm = HtmlTitleRegex().Match(html);
                if (tm.Success) title = System.Net.WebUtility.HtmlDecode(tm.Groups[1].Value).Trim();
            }
            if (!string.IsNullOrEmpty(title))
            {
                foreach (var suffix in new[] { "_哔哩哔哩_bilibili", "-哔哩哔哩", " - 哔哩哔哩", "_bilibili" })
                {
                    if (title.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
                    {
                        title = title[..^suffix.Length].Trim();
                        break;
                    }
                }
                if (title.Length == 0 || title.Contains("视频去哪了呢", StringComparison.Ordinal) || title.StartsWith("哔哩哔哩", StringComparison.Ordinal))
                    title = null;
            }

            var pic = ExtractMetaContent(html, "og:image");
            string? thumb = existingThumb;
            if (!string.IsNullOrEmpty(pic))
            {
                if (pic.StartsWith("//", StringComparison.Ordinal)) pic = "https:" + pic;
                else if (pic.StartsWith("http://", StringComparison.OrdinalIgnoreCase)) pic = "https://" + pic[7..];
                if (Uri.TryCreate(pic, UriKind.Absolute, out var picUri) &&
                    picUri.Host.EndsWith(".hdslb.com", StringComparison.OrdinalIgnoreCase))
                {
                    var key = BvidRegex().Match(finalUri.AbsoluteUri) is { Success: true } bm
                        ? bm.Value
                        : Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(pic)))[..12];
                    var downloaded = await DownloadPreviewImageAsync(pic, $"bili-{key}.jpg", referer: "https://www.bilibili.com/");
                    if (downloaded is not null) thumb = downloaded;
                }
            }
            return (title, thumb);
        }
        catch (Exception) { return (null, existingThumb); }
    }

    static string? ExtractMetaContent(string html, string property)
    {
        var dq = $"\"{property}\"";
        var sq = $"'{property}'";
        foreach (Match m in MetaTagRegex().Matches(html))
        {
            var tag = m.Value;
            if (!tag.Contains(dq, StringComparison.OrdinalIgnoreCase) && !tag.Contains(sq, StringComparison.OrdinalIgnoreCase)) continue;
            var cm = MetaContentRegex().Match(tag);
            if (!cm.Success) continue;
            var val = System.Net.WebUtility.HtmlDecode(cm.Groups[1].Value).Trim();
            if (!string.IsNullOrEmpty(val)) return val;
        }
        return null;
    }

    static async Task<string?> DownloadPreviewImageAsync(string imgUrl, string fileName, string? referer = null)
    {
        try
        {
            var dir = Path.Combine(Path.GetTempPath(), "nekodrop-previews");
            Directory.CreateDirectory(dir);
            var target = Path.Combine(dir, fileName);
            if (File.Exists(target) && new FileInfo(target).Length > 1000) return target;
            using var req = new HttpRequestMessage(HttpMethod.Get, imgUrl);
            if (!string.IsNullOrEmpty(referer)) req.Headers.Referrer = new Uri(referer);
            using var res = await ThumbHttp.SendAsync(req);
            if (!res.IsSuccessStatusCode) return null;
            var bytes = await res.Content.ReadAsByteArrayAsync();
            if (bytes.Length < 1000 || bytes.Length > 2 * 1024 * 1024) return null;
            var isJpg = bytes[0] == 0xFF && bytes[1] == 0xD8;
            var isPng = bytes[0] == 0x89 && bytes[1] == 0x50;
            if (!isJpg && !isPng) return null;
            var tmp = target + ".tmp";
            await File.WriteAllBytesAsync(tmp, bytes);
            File.Move(tmp, target, overwrite: true);
            return target;
        }
        catch (Exception) { return null; }
    }

    [GeneratedRegex(@"^[A-Za-z0-9_-]{11}$")]
    private static partial Regex YoutubeIdRegex();

    [GeneratedRegex(@"BV[0-9A-Za-z]{10}")]
    private static partial Regex BvidRegex();

    [GeneratedRegex(@"<meta\b[^>]*>", RegexOptions.IgnoreCase)]
    private static partial Regex MetaTagRegex();

    [GeneratedRegex(@"\bcontent\s*=\s*[""']([^""']+)[""']", RegexOptions.IgnoreCase)]
    private static partial Regex MetaContentRegex();

    [GeneratedRegex(@"<title[^>]*>(.*?)</title>", RegexOptions.IgnoreCase | RegexOptions.Singleline)]
    private static partial Regex HtmlTitleRegex();
}
