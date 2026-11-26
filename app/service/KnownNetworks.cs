// SPDX-License-Identifier: GPL-2.0-only
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace NekoDrop.Service;

/// <summary>
/// The Wi-Fi networks nekodrop can join itself, like Windows' saved networks:
/// imported from Windows' profiles while Intel's driver has the card, and
/// added when the user connects with a password. Stored encrypted with DPAPI
/// (machine scope) in a file only SYSTEM and Administrators can open, since
/// once nekodrop owns the card Windows' own profiles can no longer be read.
/// </summary>
public sealed class KnownNetworks
{
    public sealed class Entry
    {
        public string Ssid { get; set; } = "";
        public string? Passphrase { get; set; }
        public string? PskHex { get; set; }
        public bool Open { get; set; }
        public bool AutoConnect { get; set; } = true;
        public bool Hidden { get; set; }
        public DateTime LastUsed { get; set; }
        [JsonIgnore] public bool HasKey => Open || Passphrase is not null || PskHex is not null;
    }

    static string FilePath => Path.Combine(ServiceConfig.Folder, "networks.dat");
    readonly object _lock = new();
    List<Entry> _entries = [];

    public static KnownNetworks Load()
    {
        var k = new KnownNetworks();
        try
        {
            ServiceConfig.RejectReparsePoint(ServiceConfig.Folder);
            ServiceConfig.RejectReparsePoint(FilePath);
            if (File.Exists(FilePath))
            {
                var plain = ProtectedData.Unprotect(File.ReadAllBytes(FilePath), null, DataProtectionScope.LocalMachine);
                k._entries = JsonSerializer.Deserialize<List<Entry>>(plain) ?? [];
            }
        }
        catch (Exception) { k._entries = []; }
        return k;
    }

    void Save()
    {
        ServiceConfig.EnsureSecureDirectory();
        ServiceConfig.RejectReparsePoint(FilePath);
        var data = ProtectedData.Protect(JsonSerializer.SerializeToUtf8Bytes(_entries), null, DataProtectionScope.LocalMachine);
        if (File.Exists(FilePath)) File.Delete(FilePath);
        File.WriteAllBytes(FilePath, data);
        var acl = new FileSecurity();
        acl.SetAccessRuleProtection(true, false);
        foreach (var sid in new[] { WellKnownSidType.LocalSystemSid, WellKnownSidType.BuiltinAdministratorsSid })
            acl.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(sid, null), FileSystemRights.FullControl,
                                                       AccessControlType.Allow));
        new FileInfo(FilePath).SetAccessControl(acl);
    }

    public Entry? Find(string ssid)
    {
        lock (_lock) return _entries.FirstOrDefault(e => e.Ssid == ssid);
    }

    public List<Entry> All()
    {
        lock (_lock) return [.. _entries.OrderByDescending(e => e.LastUsed)];
    }

    /// <summary>Adds or updates a network (a new password replaces the old one).</summary>
    public void Remember(string ssid, string? passphrase, string? pskHex, bool open,
                         bool? autoConnect = null, bool used = true, bool? hidden = null)
    {
        lock (_lock)
        {
            var e = _entries.FirstOrDefault(x => x.Ssid == ssid);
            if (e is null) _entries.Add(e = new Entry { Ssid = ssid });
            if (passphrase is not null || pskHex is not null || open)
            {
                e.Passphrase = passphrase;
                e.PskHex = pskHex;
                e.Open = open;
            }
            if (autoConnect is bool a) e.AutoConnect = a;
            if (hidden is bool h) e.Hidden = h;
            if (used) e.LastUsed = DateTime.UtcNow;
            Save();
        }
    }

    public bool Forget(string ssid)
    {
        lock (_lock)
        {
            var removed = _entries.RemoveAll(e => e.Ssid == ssid) > 0;
            if (removed) Save();
            return removed;
        }
    }

    public void SetAutoConnect(string ssid, bool on)
    {
        lock (_lock)
        {
            if (_entries.FirstOrDefault(e => e.Ssid == ssid) is { } e)
            {
                e.AutoConnect = on;
                Save();
            }
        }
    }

    /// <summary>Windows' saved profiles (only while Intel's driver has the card).</summary>
    public int ImportFromWindows()
    {
        var n = 0;
        foreach (var p in Wlan.SavedProfiles())
        {
            if (Find(p.Ssid) is { HasKey: true } existing)
            {
                if (p.Hidden && !existing.Hidden)
                    Remember(p.Ssid, null, null, false, used: false, hidden: true);
                continue;
            }
            Remember(p.Ssid, p.Passphrase, p.PskHex, p.Open, autoConnect: p.AutoConnect, used: false, hidden: p.Hidden);
            n++;
        }
        return n;
    }
}
