// SPDX-License-Identifier: GPL-2.0-only
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace NekoDrop.Service;

/// <summary>
/// Keeps Windows' saved Wi-Fi networks across the card changing hands.
/// Windows files them per WLAN interface; while nekodrop holds the card there
/// is none, and when Intel's driver comes back the interface can get a new
/// GUID, which leaves every saved network behind under the old one.
///
/// Before the card is taken, the all-user profiles are copied to
/// %ProgramData%\nekodrop\wlan-backup (SYSTEM and Administrators only). Keys
/// stay as Windows keeps them (protected=true, encrypted for this machine):
/// the backup is no easier to read than Windows' own store. Once Intel's
/// interface is back, every backed-up profile it does not have is added
/// again, and so is any profile left in another interface's folder of
/// Windows' store (networks lost before this backup existed). A profile that
/// is already there is never overwritten.
/// </summary>
public static class WlanBackup
{
    /// <summary>The boot restore, a give-back and a take can overlap.</summary>
    static readonly object Gate = new();

    static string Dir => Path.Combine(ServiceConfig.Folder, "wlan-backup");
    static string IndexPath => Path.Combine(Dir, "index.json");

    /// <summary>Windows' own profile store, one folder per interface GUID.</summary>
    static string WindowsStore => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
        "Microsoft", "Wlansvc", "Profiles", "Interfaces");

    sealed record Index(string Interface, List<string> Profiles);

    /// <summary>
    /// Copies the all-user profiles of the WLAN interface (call while Intel's
    /// driver has the card). Profiles that disappeared from the same interface
    /// were deleted by the user and leave the backup too; with a different
    /// interface GUID nothing is dropped, since that is the loss being guarded
    /// against. Returns the number of profiles in the backup.
    /// </summary>
    public static int Save(Action<string> say)
    {
        lock (Gate) return SaveLocked(say);
    }

    static int SaveLocked(Action<string> say)
    {
        if (!Open(out var h)) return -1;
        try
        {
            var guid = FirstInterface(h);
            if (guid is null) return -1;
            var g = guid.Value;
            var current = new Dictionary<string, string>();
            foreach (var (name, flags) in ProfileNames(h, g))
            {
                // Group policy profiles come back by themselves; per-user ones
                // can only be read and written in that user's session.
                if ((flags & (WLAN_PROFILE_GROUP_POLICY | WLAN_PROFILE_USER)) != 0) continue;
                uint f = 0; /* keys stay encrypted (protected=true) */
                if (WlanGetProfile(h, ref g, name, IntPtr.Zero, out var xmlPtr, ref f, out _) != 0) continue;
                try { current[name] = Marshal.PtrToStringUni(xmlPtr) ?? ""; }
                finally { WlanFreeMemory(xmlPtr); }
            }

            ServiceConfig.EnsureSecureDirectory();
            ServiceConfig.EnsureSecureDirectory(Dir);
            var old = LoadIndex();
            var keep = new List<string>();
            if (old is not null && !string.Equals(old.Interface, g.ToString(), StringComparison.OrdinalIgnoreCase))
                keep.AddRange(old.Profiles.Where(p => !current.ContainsKey(p) && File.Exists(FileFor(p))));
            foreach (var (name, xml) in current)
            {
                var path = FileFor(name);
                ServiceConfig.RejectReparsePoint(path);
                File.WriteAllText(path, xml, Encoding.UTF8);
            }
            var names = current.Keys.Concat(keep).ToList();
            // Files of profiles the user deleted go too.
            foreach (var f in Directory.GetFiles(Dir, "*.xml"))
                if (!names.Any(n => string.Equals(FileFor(n), f, StringComparison.OrdinalIgnoreCase)))
                    File.Delete(f);
            File.WriteAllText(IndexPath, JsonSerializer.Serialize(new Index(g.ToString(), names)));
            if (current.Count > 0 || keep.Count > 0)
                say($"Wi-Fi: backed up {current.Count} saved network(s) of Windows" +
                    (keep.Count > 0 ? $" (and kept {keep.Count} from an earlier interface)" : ""));
            return names.Count;
        }
        catch (Exception e)
        {
            say("Wi-Fi: could not back up Windows' saved networks: " + e.Message);
            return -1;
        }
        finally { WlanCloseHandle(h, IntPtr.Zero); }
    }

    /// <summary>
    /// Adds back every backed-up profile (and every profile stranded in another
    /// interface's folder) that the current WLAN interface lacks. Waits up to
    /// `wait` for Intel's interface to appear. Returns the number added.
    /// </summary>
    public static int Restore(Action<string> say, TimeSpan wait)
    {
        try
        {
            var end = DateTime.UtcNow + wait;
            while (true)
            {
                if (Open(out var h))
                {
                    try
                    {
                        if (FirstInterface(h) is { } g) lock (Gate) return RestoreTo(h, g, say);
                    }
                    finally { WlanCloseHandle(h, IntPtr.Zero); }
                }
                if (DateTime.UtcNow >= end) return 0;
                Thread.Sleep(1000);
            }
        }
        catch (Exception e)
        {
            say("Wi-Fi: could not restore Windows' saved networks: " + e.Message);
            return 0;
        }
    }

    static int RestoreTo(IntPtr h, Guid guid, Action<string> say)
    {
        var have = ProfileNames(h, guid).Select(p => p.Name).ToHashSet(StringComparer.Ordinal);
        var candidates = new List<(string Name, string Xml, string From)>();
        // The backup only speaks for another interface: on the one it was taken
        // from, a missing profile is one the user deleted.
        var index = LoadIndex();
        var fromBackup = index is not null && !string.Equals(index.Interface, guid.ToString(), StringComparison.OrdinalIgnoreCase);
        if (fromBackup)
            foreach (var name in index!.Profiles)
            {
                var path = FileFor(name);
                if (File.Exists(path) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0)
                    candidates.Add((name, File.ReadAllText(path, Encoding.UTF8), "backup"));
            }
        // Profiles stranded under an interface that is no longer there (lost
        // before this backup existed, or by a card re-detected outside
        // nekodrop). Each such folder is taken in once, so a network the user
        // deletes afterwards stays deleted.
        var present = Interfaces(h);
        var done = LoadDone();
        var newlyDone = new List<string>();
        try
        {
            if (Directory.Exists(WindowsStore))
                foreach (var dir in Directory.GetDirectories(WindowsStore))
                {
                    if (!Guid.TryParse(Path.GetFileName(dir), out var other) || present.Contains(other)) continue;
                    if (done.Contains(other.ToString())) continue;
                    newlyDone.Add(other.ToString());
                    foreach (var f in Directory.GetFiles(dir, "*.xml"))
                    {
                        var xml = File.ReadAllText(f);
                        if (ProfileName(xml) is { } n) candidates.Add((n, xml, "old interface"));
                    }
                }
        }
        catch (Exception) { /* the store is SYSTEM's; nothing to recover from it then */ }

        int added = 0, failed = 0;
        var pending = candidates.Where(c => !have.Contains(c.Name)).ToList();
        // Right after the card goes back to Intel's driver its interface exists
        // but is not ready: every profile fails with ERROR_INVALID_STATE (#18).
        // Those are tried again for a while instead of being given up.
        var until = DateTime.UtcNow + TimeSpan.FromSeconds(30);
        while (true)
        {
            var notReady = new List<(string Name, string Xml, string From)>();
            failed = 0;
            foreach (var c in pending)
            {
                if (have.Contains(c.Name)) continue;
                var g = guid;
                var rc = WlanSetProfile(h, ref g, 0, c.Xml, null, false, IntPtr.Zero, out var reason);
                if (rc == 0) { have.Add(c.Name); added++; }
                else if (rc == ERROR_INVALID_STATE) notReady.Add(c);
                else if (rc != ERROR_ALREADY_EXISTS)
                {
                    failed++;
                    say($"Wi-Fi: could not restore a saved network from the {c.From} (error {rc}, reason {reason})");
                }
            }
            if (notReady.Count == 0 || DateTime.UtcNow >= until)
            {
                failed += notReady.Count;
                if (notReady.Count > 0)
                    say($"Wi-Fi: {notReady.Count} saved network(s) not restored yet: the Wi-Fi interface is not ready (kept for the next try)");
                break;
            }
            pending = notReady;
            Thread.Sleep(2000);
        }
        if (added > 0) say($"Wi-Fi: restored {added} saved network(s) to Windows");
        // Nothing is marked as done while a profile is still missing: the
        // next start tries the backup and the old folders again.
        if (failed > 0) return added;

        ServiceConfig.EnsureSecureDirectory();
        ServiceConfig.EnsureSecureDirectory(Dir);
        if (fromBackup)
            File.WriteAllText(IndexPath, JsonSerializer.Serialize(index! with { Interface = guid.ToString() }));
        if (newlyDone.Count > 0)
            File.AppendAllLines(DonePath, newlyDone);
        return added;
    }

    static string DonePath => Path.Combine(Dir, "imported-interfaces.txt");

    static HashSet<string> LoadDone()
    {
        try
        {
            if (!File.Exists(DonePath)) return [];
            ServiceConfig.RejectReparsePoint(DonePath);
            return File.ReadAllLines(DonePath).Select(l => l.Trim()).Where(l => l != "")
                .ToHashSet(StringComparer.OrdinalIgnoreCase);
        }
        catch (Exception) { return []; }
    }

    static HashSet<Guid> Interfaces(IntPtr h)
    {
        var result = new HashSet<Guid>();
        if (WlanEnumInterfaces(h, IntPtr.Zero, out var list) != 0) return result;
        try
        {
            // WLAN_INTERFACE_INFO_LIST: count, index, then 532-byte entries starting with the GUID.
            int n = Marshal.ReadInt32(list);
            for (int i = 0; i < n; i++) result.Add(Marshal.PtrToStructure<Guid>(list + 8 + i * 532));
        }
        finally { WlanFreeMemory(list); }
        return result;
    }

    static string? ProfileName(string xml)
    {
        try
        {
            var doc = System.Xml.Linq.XDocument.Parse(xml);
            var ns = doc.Root?.Name.Namespace ?? System.Xml.Linq.XNamespace.None;
            return doc.Root?.Element(ns + "name")?.Value;
        }
        catch (Exception) { return null; }
    }

    static Index? LoadIndex()
    {
        try
        {
            if (!File.Exists(IndexPath)) return null;
            ServiceConfig.RejectReparsePoint(IndexPath);
            return JsonSerializer.Deserialize<Index>(File.ReadAllText(IndexPath));
        }
        catch (Exception) { return null; }
    }

    /// <summary>A profile name can hold any character: the file is named by its hex.</summary>
    static string FileFor(string name) => Path.Combine(Dir, Convert.ToHexString(Encoding.UTF8.GetBytes(name)) + ".xml");

    static bool Open(out IntPtr h) => WlanOpenHandle(2, IntPtr.Zero, out _, out h) == 0;

    static Guid? FirstInterface(IntPtr h)
    {
        if (WlanEnumInterfaces(h, IntPtr.Zero, out var list) != 0) return null;
        try { return Marshal.ReadInt32(list) == 0 ? null : Marshal.PtrToStructure<Guid>(list + 8); }
        finally { WlanFreeMemory(list); }
    }

    static List<(string Name, uint Flags)> ProfileNames(IntPtr h, Guid guid)
    {
        var result = new List<(string, uint)>();
        if (WlanGetProfileList(h, ref guid, IntPtr.Zero, out var list) != 0) return result;
        try
        {
            // WLAN_PROFILE_INFO_LIST: count, index, then WCHAR[256] + flags (516 bytes) each.
            int n = Marshal.ReadInt32(list);
            for (int i = 0; i < n; i++)
            {
                var item = list + 8 + i * 516;
                result.Add((Marshal.PtrToStringUni(item, 256).TrimEnd('\0'), (uint)Marshal.ReadInt32(item + 512)));
            }
        }
        finally { WlanFreeMemory(list); }
        return result;
    }

    const uint WLAN_PROFILE_GROUP_POLICY = 1, WLAN_PROFILE_USER = 2;
    const int ERROR_ALREADY_EXISTS = 183;
    const int ERROR_INVALID_STATE = 5023;

    [DllImport("wlanapi.dll")] static extern int WlanOpenHandle(uint version, IntPtr reserved, out uint negotiated, out IntPtr handle);
    [DllImport("wlanapi.dll")] static extern int WlanCloseHandle(IntPtr handle, IntPtr reserved);
    [DllImport("wlanapi.dll")] static extern int WlanEnumInterfaces(IntPtr handle, IntPtr reserved, out IntPtr list);
    [DllImport("wlanapi.dll")] static extern int WlanGetProfileList(IntPtr handle, ref Guid iface, IntPtr reserved, out IntPtr list);
    [DllImport("wlanapi.dll", CharSet = CharSet.Unicode)] static extern int WlanGetProfile(IntPtr handle, ref Guid iface,
        string profile, IntPtr reserved, out IntPtr xml, ref uint flags, out uint access);
    [DllImport("wlanapi.dll", CharSet = CharSet.Unicode)] static extern int WlanSetProfile(IntPtr handle, ref Guid iface,
        uint flags, string xml, string? security, bool overwrite, IntPtr reserved, out uint reason);
    [DllImport("wlanapi.dll")] static extern void WlanFreeMemory(IntPtr memory);
}
