// SPDX-License-Identifier: GPL-2.0-only
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Threading;
using System.Xml.Linq;
using Microsoft.Win32;

namespace NekoDrop;

/// <summary>
/// Software update from the project's GitHub releases: finds a newer
/// nekodrop-X.Y.Z-setup.exe, downloads it, checks its SHA-256 against the
/// digest GitHub publishes for the asset, and runs it (Windows asks for
/// administrator rights). Nothing installs without the user choosing to.
/// </summary>
public static class Updater
{
    const string ReleasesApi = "https://api.github.com/repos/zheznanohana/nekodrop/releases?per_page=15";
    const string ReleasesAtom = "https://github.com/zheznanohana/nekodrop/releases.atom";
    const string ReleasesLatest = "https://github.com/zheznanohana/nekodrop/releases/latest";
    const string TagUrlPrefix = "https://github.com/zheznanohana/nekodrop/releases/tag/";
    const string DownloadUrlPrefix = "https://github.com/zheznanohana/nekodrop/releases/download/";
    public const string ReleasesPage = "https://github.com/zheznanohana/nekodrop/releases";

    public enum CheckStatus { UpToDate, Available, RateLimited, Failed }

    public sealed record Release(Version Version, string Tag, string Url, string Sha256, bool Preview, string Page);

    public readonly record struct DownloadProgress(int Percent, long BytesPerSecond);

    public sealed record CheckResult(
        CheckStatus Status,
        Release? Release = null,
        string? NewerTag = null,
        string? PageUrl = null,
        string? Detail = null);

    /// <summary>
    /// Reads the current Windows user proxy (Clash/v2rayN/VPN system proxy enabled
    /// after boot, #62), falling back to HttpClient.DefaultProxy (env vars, PAC/WPAD,
    /// TUN-mode VPNs). Built fresh per check/download so toggling a VPN takes effect
    /// immediately without restarting nekodrop.
    /// </summary>
    static IWebProxy ResolveSystemProxy()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Internet Settings");
            if (key is not null && Convert.ToInt32(key.GetValue("ProxyEnable", 0)) != 0)
            {
                var raw = (key.GetValue("ProxyServer") as string)?.Trim();
                if (!string.IsNullOrEmpty(raw))
                {
                    var picked = PickProxyEndpoint(raw);
                    if (picked is not null && Uri.TryCreate(picked, UriKind.Absolute, out var uri))
                    {
                        var overrideRaw = (key.GetValue("ProxyOverride") as string) ?? "";
                        var bypassLocal = overrideRaw.Contains("<local>", StringComparison.OrdinalIgnoreCase);
                        return new WebProxy(uri, bypassLocal);
                    }
                }
            }
        }
        catch (Exception) { }
        return HttpClient.DefaultProxy;
    }

    static string? PickProxyEndpoint(string raw)
    {
        if (!raw.Contains('='))
            return raw.Contains("://", StringComparison.Ordinal) ? raw : "http://" + raw;

        string? https = null, http = null, socks = null;
        foreach (var part in raw.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var eq = part.IndexOf('=');
            if (eq <= 0 || eq >= part.Length - 1) continue;
            var scheme = part[..eq].Trim().ToLowerInvariant();
            var host = part[(eq + 1)..].Trim();
            if (string.IsNullOrEmpty(host)) continue;
            if (scheme == "https") https ??= host.Contains("://", StringComparison.Ordinal) ? host : "http://" + host;
            else if (scheme == "http") http ??= host.Contains("://", StringComparison.Ordinal) ? host : "http://" + host;
            else if (scheme is "socks" or "socks5") socks ??= host.Contains("://", StringComparison.Ordinal) ? host : "socks5://" + host;
        }
        return https ?? http ?? socks;
    }

    static HttpClient MakeClient(TimeSpan timeout)
    {
        var handler = new SocketsHttpHandler
        {
            UseProxy = true,
            Proxy = ResolveSystemProxy(),
            AllowAutoRedirect = true,
            AutomaticDecompression = DecompressionMethods.All,
            ConnectTimeout = TimeSpan.FromSeconds(15),
        };
        var c = new HttpClient(handler, disposeHandler: true) { Timeout = timeout };
        c.DefaultRequestHeaders.UserAgent.ParseAdd("nekodrop-updater");
        return c;
    }

    static string CachePath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "nekodrop", "update-cache.json");

    static (string? Etag, string? Json) ReadCache()
    {
        try
        {
            if (!File.Exists(CachePath)) return (null, null);
            var node = JsonNode.Parse(File.ReadAllText(CachePath));
            return ((string?)node?["etag"], (string?)node?["json"]);
        }
        catch (Exception) { return (null, null); }
    }

    static void WriteCache(string? etag, string json)
    {
        try
        {
            var dir = Path.GetDirectoryName(CachePath);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            var obj = new JsonObject { ["etag"] = etag ?? "", ["json"] = json };
            File.WriteAllText(CachePath, obj.ToJsonString());
        }
        catch (Exception) { }
    }

    static Release? ParseReleases(string json, bool includePreview)
    {
        Release? best = null;
        foreach (var r in JsonNode.Parse(json)?.AsArray() ?? [])
        {
            if (r is null || (bool?)r["draft"] == true) continue;
            var preview = (bool?)r["prerelease"] == true;
            if (preview && !includePreview) continue;
            var tag = (string?)r["tag_name"] ?? "";
            if (!Version.TryParse(tag.TrimStart('v'), out var v) || v <= Current) continue;
            foreach (var a in r["assets"]?.AsArray() ?? [])
            {
                var name = (string?)a?["name"] ?? "";
                var digest = (string?)a?["digest"] ?? "";
                if (!name.StartsWith("nekodrop-", StringComparison.Ordinal) || !name.EndsWith("-setup.exe", StringComparison.Ordinal)) continue;
                // An installer without a published digest is never run.
                if (!digest.StartsWith("sha256:", StringComparison.Ordinal)) continue;
                var url = (string?)a?["browser_download_url"] ?? "";
                if (!url.StartsWith("https://github.com/zheznanohana/nekodrop/releases/download/", StringComparison.Ordinal)) continue;
                if (best is null || v > best.Version)
                    best = new Release(v, tag, url, digest["sha256:".Length..].ToLowerInvariant(), preview, (string?)r["html_url"] ?? "");
            }
        }
        return best;
    }

    /// <summary>
    /// Browser-style HTTPS fallback on github.com when api.github.com is rate-limited
    /// or unreachable (#4, #62). Resolves the latest release via /releases/latest (Stable)
    /// or releases.atom (including previews) and parses the published 64-hex SHA-256 from
    /// the release page; never auto-installs without a valid published SHA-256.
    /// </summary>
    static async Task<(bool Reached, Release? Release, string? Tag, string? Url)> FindNewerFromWeb(
        HttpClient http, bool includePreview, CancellationToken ct)
    {
        bool reached = false;
        string? tag = null;
        string? pageUrl = null;
        string? html = null;

        if (!includePreview)
        {
            try
            {
                using var req = new HttpRequestMessage(HttpMethod.Get, ReleasesLatest);
                req.Headers.Accept.ParseAdd("text/html");
                using var resp = await http.SendAsync(req, ct);
                var finalUrl = resp.RequestMessage?.RequestUri?.AbsoluteUri ?? "";
                if (resp.IsSuccessStatusCode && finalUrl.StartsWith(TagUrlPrefix, StringComparison.Ordinal))
                {
                    reached = true;
                    var candidate = finalUrl[TagUrlPrefix.Length..].Split('?', '#')[0];
                    if (Version.TryParse(candidate.TrimStart('v'), out var v) && v > Current)
                    {
                        tag = candidate;
                        pageUrl = TagUrlPrefix + candidate;
                        html = await resp.Content.ReadAsStringAsync(ct);
                    }
                    else
                    {
                        return (true, null, null, null);
                    }
                }
            }
            catch (Exception) { }
        }

        if (tag is null)
        {
            var (atomOk, atomTag, atomUrl) = await FindNewerFromAtom(http, ct);
            reached |= atomOk;
            tag = atomTag;
            pageUrl = atomUrl;
        }

        if (tag is null || pageUrl is null || !Version.TryParse(tag.TrimStart('v'), out var ver) || ver <= Current)
            return (reached, null, null, null);

        try
        {
            if (html is null)
            {
                using var req = new HttpRequestMessage(HttpMethod.Get, pageUrl);
                req.Headers.Accept.ParseAdd("text/html");
                using var resp = await http.SendAsync(req, ct);
                if (resp.IsSuccessStatusCode) html = await resp.Content.ReadAsStringAsync(ct);
            }
            if (!string.IsNullOrEmpty(html))
            {
                var isPreview = html.Contains(">Pre-release<", StringComparison.OrdinalIgnoreCase);
                if (isPreview && !includePreview) return (true, null, null, null);
                var m = Regex.Match(html, @"SHA-256:\s*(?:<code[^>]*>)?([0-9a-fA-F]{64})", RegexOptions.IgnoreCase);
                if (m.Success)
                {
                    var sha = m.Groups[1].Value.ToLowerInvariant();
                    var dlUrl = $"{DownloadUrlPrefix}{tag}/nekodrop-{ver}-setup.exe";
                    return (true, new Release(ver, tag, dlUrl, sha, isPreview, pageUrl), tag, pageUrl);
                }
            }
        }
        catch (Exception) { }

        return (reached, null, tag, pageUrl);
    }

    static async Task<(bool Reached, string? Tag, string? Url)> FindNewerFromAtom(HttpClient http, CancellationToken ct)
    {
        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, ReleasesAtom);
            req.Headers.Accept.ParseAdd("application/atom+xml");
            using var resp = await http.SendAsync(req, ct);
            if (!resp.IsSuccessStatusCode) return (false, null, null);
            var xml = await resp.Content.ReadAsStringAsync(ct);
            var doc = XDocument.Parse(xml);
            XNamespace atom = "http://www.w3.org/2005/Atom";
            Version? bestVer = null;
            string? bestTag = null, bestUrl = null;
            foreach (var entry in doc.Descendants(atom + "entry"))
            {
                var href = entry.Element(atom + "link")?.Attribute("href")?.Value ?? "";
                if (!href.StartsWith(TagUrlPrefix, StringComparison.Ordinal)) continue;
                var tag = href[TagUrlPrefix.Length..].Split('?', '#')[0];
                if (!Version.TryParse(tag.TrimStart('v'), out var v) || v <= Current) continue;
                if (bestVer is null || v > bestVer)
                {
                    bestVer = v;
                    bestTag = tag;
                    bestUrl = href;
                }
            }
            return (true, bestTag, bestUrl);
        }
        catch (Exception) { return (false, null, null); }
    }

    /// <summary>This build's version (0.1.16 of "0.1.16+commit").</summary>
    public static Version Current
    {
        get
        {
            var info = Assembly.GetEntryAssembly()?.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
            var text = (info ?? "0.0.0").Split('+')[0];
            return Version.TryParse(text, out var v) ? v : new Version(0, 0, 0);
        }
    }

    /// <summary>
    /// Checks GitHub releases with ETag caching (304 responses do not consume the
    /// 60/hr unauthenticated quota), dynamic Windows proxy resolution, and a
    /// browser-style github.com web fallback when api.github.com fails (#4, #62).
    /// </summary>
    public static async Task<CheckResult> Check(bool includePreview)
    {
        var (cachedEtag, cachedJson) = ReadCache();
        using var http = MakeClient(TimeSpan.FromSeconds(20));
        bool rateLimited = false;
        string? failureDetail = null;

        try
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            using var req = new HttpRequestMessage(HttpMethod.Get, ReleasesApi);
            req.Headers.Accept.ParseAdd("application/vnd.github+json");
            if (!string.IsNullOrWhiteSpace(cachedEtag)) req.Headers.IfNoneMatch.TryParseAdd(cachedEtag);
            using var resp = await http.SendAsync(req, cts.Token);

            if (resp.StatusCode == HttpStatusCode.NotModified && !string.IsNullOrEmpty(cachedJson))
            {
                var best = ParseReleases(cachedJson, includePreview);
                return best is not null ? new CheckResult(CheckStatus.Available, best) : new CheckResult(CheckStatus.UpToDate);
            }

            if (resp.IsSuccessStatusCode)
            {
                var json = await resp.Content.ReadAsStringAsync(cts.Token);
                WriteCache(resp.Headers.ETag?.ToString(), json);
                var best = ParseReleases(json, includePreview);
                return best is not null ? new CheckResult(CheckStatus.Available, best) : new CheckResult(CheckStatus.UpToDate);
            }

            if (resp.StatusCode is HttpStatusCode.Forbidden or HttpStatusCode.TooManyRequests)
            {
                rateLimited = resp.StatusCode == HttpStatusCode.TooManyRequests ||
                    (resp.Headers.TryGetValues("X-RateLimit-Remaining", out var rem) && rem.FirstOrDefault() == "0");
            }
            failureDetail = $"HTTP {(int)resp.StatusCode} ({resp.ReasonPhrase})";
        }
        catch (OperationCanceledException)
        {
            failureDetail = "timeout";
        }
        catch (Exception e)
        {
            failureDetail = e.Message;
        }

        if (rateLimited && !string.IsNullOrEmpty(cachedJson) && ParseReleases(cachedJson, includePreview) is { } cached)
            return new CheckResult(CheckStatus.Available, cached);

        try
        {
            using var webCts = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var (webOk, webRelease, webTag, webUrl) = await FindNewerFromWeb(http, includePreview, webCts.Token);
            if (webRelease is not null)
                return new CheckResult(CheckStatus.Available, webRelease);
            if (webOk && webTag is null)
                return new CheckResult(CheckStatus.UpToDate);
            if (rateLimited)
                return new CheckResult(CheckStatus.RateLimited, NewerTag: webTag, PageUrl: webUrl ?? ReleasesPage);
            return new CheckResult(CheckStatus.Failed, NewerTag: webTag, PageUrl: webUrl ?? ReleasesPage, Detail: failureDetail);
        }
        catch (Exception)
        {
            return rateLimited
                ? new CheckResult(CheckStatus.RateLimited, PageUrl: ReleasesPage)
                : new CheckResult(CheckStatus.Failed, PageUrl: ReleasesPage, Detail: failureDetail);
        }
    }

    /// <summary>The newest release above this build with an installer and a digest; null if none.</summary>
    public static async Task<Release?> FindNewer(bool includePreview) => (await Check(includePreview)).Release;

    /// <summary>Downloads, verifies and starts the installer. Returns "" or why not.</summary>
    public static async Task<string> Install(Release r, IProgress<DownloadProgress>? progress = null, bool deleteAfterInstall = true, bool showWindow = true)
    {
        var dir = Path.Combine(Path.GetTempPath(), "nekodrop-update");
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, $"nekodrop-{r.Version}-setup.exe");
        using var http = MakeClient(TimeSpan.FromMinutes(10));
        using (var resp = await http.GetAsync(r.Url, HttpCompletionOption.ResponseHeadersRead))
        {
            resp.EnsureSuccessStatusCode();
            var total = resp.Content.Headers.ContentLength ?? 0;
            await using var src = await resp.Content.ReadAsStreamAsync();
            await using var dst = File.Create(path);
            var buf = new byte[1 << 16];
            long done = 0;
            int finished = 0;
            int n;
            var clock = Stopwatch.StartNew();
            long lastDone = 0;
            long lastMs = 0;
            void Report()
            {
                if (Volatile.Read(ref finished) != 0) return;
                var now = clock.ElapsedMilliseconds;
                var elapsed = now - lastMs;
                if (elapsed < 1) return;
                var curDone = Volatile.Read(ref done);
                var bps = Math.Max(0, (curDone - lastDone) * 1000 / elapsed);
                if (total > 0) progress?.Report(new((int)Math.Min(100, curDone * 100 / total), bps));
                lastDone = curDone;
                lastMs = now;
            }
            System.Threading.Timer? timer = null;
            if (progress is not null)
                timer = new System.Threading.Timer(_ => { try { Report(); } catch (Exception) { } }, null, 500, 500);
            try
            {
                while ((n = await src.ReadAsync(buf)) > 0)
                {
                    await dst.WriteAsync(buf.AsMemory(0, n));
                    Interlocked.Add(ref done, n);
                }
            }
            finally
            {
                Interlocked.Exchange(ref finished, 1);
                timer?.Dispose();
            }
            if (progress is not null && total > 0)
            {
                var finalDone = Volatile.Read(ref done);
                var avg = clock.ElapsedMilliseconds > 0 ? finalDone * 1000 / clock.ElapsedMilliseconds : 0;
                progress.Report(new(100, avg));
            }
        }
        string hash;
        await using (var f = File.OpenRead(path))
            hash = Convert.ToHexString(await SHA256.HashDataAsync(f)).ToLowerInvariant();
        if (hash != r.Sha256)
        {
            File.Delete(path);
            return "the download does not match the published SHA-256";
        }
        // The installer closes this app itself (CloseApplications) and starts it again afterwards.
        StartInstaller(path, deleteAfterInstall, showWindow);
        return "";
    }

    /// <summary>
    /// Starts the installer. With deleteAfterInstall, a detached PowerShell helper
    /// waits for the installer and removes the setup file only after a successful
    /// exit. The app is closed by the installer itself, so the helper must live
    /// outside this process. showWindow asks the installer to open the main panel
    /// instead of starting the new version in the background.
    /// </summary>
    static void StartInstaller(string path, bool deleteAfterInstall, bool showWindow)
    {
        if (!deleteAfterInstall)
        {
            Process.Start(InstallerStart(path, showWindow));
            return;
        }

        string? helper = null;
        try
        {
            helper = Path.Combine(Path.GetDirectoryName(path)!, $"{Path.GetFileNameWithoutExtension(path)}-cleanup.ps1");
            File.WriteAllText(helper, CleanupScript);
            var powershell = Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe");
            var psi = new ProcessStartInfo(powershell)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
            };
            psi.ArgumentList.Add("-NoProfile");
            psi.ArgumentList.Add("-NonInteractive");
            psi.ArgumentList.Add("-ExecutionPolicy");
            psi.ArgumentList.Add("Bypass");
            psi.ArgumentList.Add("-File");
            psi.ArgumentList.Add(helper);
            psi.ArgumentList.Add("-Installer");
            psi.ArgumentList.Add(path);
            if (showWindow) psi.ArgumentList.Add("-ShowApp");
            Process.Start(psi);
        }
        catch (Exception)
        {
            // If the helper cannot start, still run the update; leaving the file is
            // better than blocking the installation.
            if (helper is not null)
            {
                try { File.Delete(helper); } catch (Exception) { }
            }
            Process.Start(InstallerStart(path, showWindow));
        }
    }

    static ProcessStartInfo InstallerStart(string path, bool showWindow)
    {
        var args = "/SILENT /NORESTART" + (showWindow ? " /SHOWAPP" : "");
        return new ProcessStartInfo(path, args) { UseShellExecute = true };
    }

    const string CleanupScript = """
param([Parameter(Mandatory = $true)][string]$Installer, [switch]$ShowApp)
$ErrorActionPreference = 'SilentlyContinue'
try {
    $setupArgs = @('/SILENT', '/NORESTART')
    if ($ShowApp) { $setupArgs += '/SHOWAPP' }
    $p = Start-Process -FilePath $Installer -ArgumentList $setupArgs -PassThru -ErrorAction Stop
    if ($p.WaitForExit(1800000) -and $p.ExitCode -eq 0) {
        Remove-Item -LiteralPath $Installer -Force
    }
} catch {
}
Remove-Item -LiteralPath $PSCommandPath -Force
""";
}
