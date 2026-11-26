// SPDX-License-Identifier: GPL-2.0-only
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json.Nodes;

namespace NekoDrop;

/// <summary>
/// winawdl's AirDrop engine (airdrop\engine.py, MIT), run in the user's session
/// with the bundled Python: it receives into the user's own folder and its
/// Bluetooth beacon runs where WinRT Bluetooth is allowed. One JSON command a
/// line in, one event a line out (see engine.py's docstring).
/// </summary>
public sealed class Engine : IDisposable
{
    Process? _process;
    readonly object _lock = new();

    public event Action<JsonObject>? Event;
    public event Action<string>? Log;
    public bool Running => _process is { HasExited: false };

    static string Root => AppContext.BaseDirectory;

    static string? Python()
    {
        var bundled = Path.Combine(Root, "python", "python.exe");
        if (File.Exists(bundled)) return bundled;
        // A development checkout: the first Python 3.10+ on PATH.
        foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';'))
        {
            var p = Path.Combine(dir.Trim(), "python.exe");
            if (File.Exists(p) && !p.Contains("WindowsApps", StringComparison.OrdinalIgnoreCase)) return p;
        }
        return null;
    }

    static string? Script()
    {
        foreach (var candidate in new[]
                 {
                     Path.Combine(Root, "airdrop", "engine.py"),
                     Path.GetFullPath(Path.Combine(Root, @"..\..\..\..\..\third_party\winawdl\airdrop\engine.py")),
                 })
            if (File.Exists(candidate)) return candidate;
        return null;
    }

    public bool Start(Settings s, out string error)
    {
        error = "";
        var python = Python();
        var script = Script();
        if (python is null || script is null)
        {
            error = python is null ? "Python not found" : "airdrop\\engine.py not found";
            return false;
        }
        Directory.CreateDirectory(s.SaveFolder);
        if (s.ServiceId.Length != 12)
        {
            s.ServiceId = Convert.ToHexString(System.Security.Cryptography.RandomNumberGenerator.GetBytes(6)).ToLowerInvariant();
            s.Save();
        }
        var psi = new ProcessStartInfo(python)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
            WorkingDirectory = Path.GetDirectoryName(script)!,
        };
        psi.ArgumentList.Add("-u");
        psi.ArgumentList.Add(script);
        psi.ArgumentList.Add("--name");
        psi.ArgumentList.Add(s.DeviceName);
        psi.ArgumentList.Add("--outdir");
        psi.ArgumentList.Add(s.SaveFolder);
        if (!s.Ask) psi.ArgumentList.Add("--take-all");
        // The receiver's identity needs OpenSSL: the bundled one, else Git's.
        var openssl = Path.Combine(Root, "openssl");
        if (Directory.Exists(openssl)) psi.Environment["PATH"] = openssl + ";" + Environment.GetEnvironmentVariable("PATH");
        psi.Environment["PYTHONIOENCODING"] = "utf-8";
        psi.Environment["NEKODROP_SERVICE_ID"] = s.ServiceId; /* one device on the iPhone across restarts */
        if (s.MediaFormat == "original") psi.Environment["NEKODROP_MEDIA_MODE"] = "original";
        if (s.BleStandby) psi.Environment["NEKODROP_BLE_STANDBY"] = "1";
        // Contacts Only: the Apple ID identity for installer\nekodrop_identity.py.
        if (s.AppleIdOn && AppleId.Present)
        {
            psi.Environment["NEKODROP_IDENTITY"] = AppleId.Dir;
            psi.Environment["NEKODROP_RECEIVE"] = s.Receive == "contacts" ? "contacts" : "everyone";
        }
        try { _process = Process.Start(psi); }
        catch (Exception e) { error = e.Message; return false; }
        if (_process is null) { error = "the engine did not start"; return false; }
        _process.OutputDataReceived += (_, e) =>
        {
            if (string.IsNullOrWhiteSpace(e.Data)) return;
            try { if (JsonNode.Parse(e.Data) is JsonObject o) Event?.Invoke(o); }
            catch (System.Text.Json.JsonException) { Log?.Invoke(e.Data); }
        };
        _process.ErrorDataReceived += (_, e) => { if (e.Data is not null) Log?.Invoke(e.Data); };
        _process.BeginOutputReadLine();
        _process.BeginErrorReadLine();
        return true;
    }

    public void Send(JsonObject command)
    {
        lock (_lock)
        {
            if (!Running) return;
            try
            {
                _process!.StandardInput.WriteLine(command.ToJsonString());
                _process.StandardInput.Flush();
            }
            catch (IOException) { }
        }
    }

    public void Look(bool on) => Send(new JsonObject { ["do"] = "look", ["on"] = on });
    public void Answer(string id, bool accept) => Send(new JsonObject { ["do"] = "answer", ["id"] = id, ["accept"] = accept });
    public void Cancel(string id) => Send(new JsonObject { ["do"] = "cancel", ["id"] = id });

    public void SendFiles(string id, string peer, IEnumerable<string> files)
    {
        var list = new JsonArray();
        foreach (var f in files) list.Add(f);
        Send(new JsonObject { ["do"] = "send", ["id"] = id, ["peer"] = peer, ["files"] = list });
    }

    public void Apply(Settings s) =>
        Send(new JsonObject { ["do"] = "set", ["name"] = s.DeviceName, ["folder"] = s.SaveFolder, ["ask"] = s.Ask });

    public void Dispose()
    {
        Send(new JsonObject { ["do"] = "quit" });
        try
        {
            if (_process is { HasExited: false } && !_process.WaitForExit(3000)) _process.Kill();
        }
        catch (InvalidOperationException) { }
    }
}
