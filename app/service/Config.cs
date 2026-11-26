// SPDX-License-Identifier: GPL-2.0-only
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace NekoDrop.Service;

/// <summary>How Wi-Fi and AirDrop share the card.</summary>
public enum NetMode
{
    /// <summary>Our driver joins the Wi-Fi network beside AWDL (WPA2-Personal or open).</summary>
    Concurrent,
    /// <summary>Intel's driver keeps Wi-Fi; the card is taken only while AirDrop is in use.</summary>
    OnDemand,
    /// <summary>The card always stays with Intel's driver.</summary>
    Off,
}

/// <summary>Machine-wide settings, in %ProgramData%\nekodrop\service.json.</summary>
public sealed class ServiceConfig
{
    [JsonConverter(typeof(JsonStringEnumConverter))]
    public NetMode Mode { get; set; } = NetMode.Concurrent;
    /// <summary>The AWDL channel to start on; the peer's channel is followed from there.</summary>
    public int StartChannel { get; set; } = 6;
    /// <summary>Last country learned from a peer or a network, "" if none.</summary>
    public string Country { get; set; } = "";
    /// <summary>Last 5 GHz channel a peer used (44 or 149), 0 if none.</summary>
    public int PeerChannel { get; set; }
    // "Aggregate" (the A-MPDU switch) is gone: always on since 0.1.35, after
    // AX201 (30.8 MB/s) and AX200 (9-25 MiB/s, #4) data. Old files' field is ignored.
    /// <summary>
    /// Experimental fast unlock of a passive 5 GHz AWDL channel (radio --fast-unlock, #11):
    /// earlier, repeated discovery scans so the firmware learns the country sooner. Off by default.
    /// </summary>
    public bool FastUnlock { get; set; }
    /// <summary>
    /// Experimental coexistence plan (radio --coex-plan, ForbiddenItlwm PR #30): with the
    /// station on another 5 GHz channel than AirDrop, the router's entries go into the AWDL
    /// schedule and the listener stays silent in them. Off by default.
    /// </summary>
    public bool CoexPlan { get; set; }
    /// <summary>
    /// Experimental high-throughput mode (awdld -H -M 1500, inspired by YiHoooong/owly #65):
    /// advertises Wi-Fi 6 HE80 in AWDL Data Path State TLV 12 on 5 GHz 80 MHz and raises
    /// awdl0 MTU from 1450 to 1500. Off by default.
    /// </summary>
    public bool HighThroughput { get; set; }
    /// <summary>
    /// The user's manual choice when the card is not recognized by its ID
    /// (Settings, "Wi-Fi card"): its PCI hardware ID ("PCI\VEN_xxxx&amp;DEV_yyyy")
    /// and a CardCatalog.ManualModels key; both "" for automatic.
    /// </summary>
    public string ManualCard { get; set; } = "";
    public string ManualModel { get; set; } = "";
    /// <summary>Seconds without AirDrop use before on-demand mode gives the card back.</summary>
    public int IdleSeconds { get; set; } = 90;
    /// <summary>Intel's own INF (oemNN.inf), remembered when the card is first taken.</summary>
    public string VendorInf { get; set; } = "";

    public static string Folder =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "nekodrop");
    static string FilePath => Path.Combine(Folder, "service.json");
    static readonly JsonSerializerOptions Options = new() { WriteIndented = true };

    public static void RejectReparsePoint(string path)
    {
        try
        {
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                throw new IOException($"Refusing reparse point: {path}");
        }
        catch (FileNotFoundException) { }
        catch (DirectoryNotFoundException) { }
    }

    public static void EnsureSecureDirectory(string? dir = null)
    {
        var target = dir ?? Folder;
        RejectReparsePoint(target);
        Directory.CreateDirectory(target);
        RejectReparsePoint(target);
        var acl = new DirectorySecurity();
        acl.SetAccessRuleProtection(true, false);
        foreach (var sid in new[] { WellKnownSidType.LocalSystemSid, WellKnownSidType.BuiltinAdministratorsSid })
            acl.AddAccessRule(new FileSystemAccessRule(
                new SecurityIdentifier(sid, null),
                FileSystemRights.FullControl,
                InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit,
                PropagationFlags.None,
                AccessControlType.Allow));
        new DirectoryInfo(target).SetAccessControl(acl);
    }

    static void LockAdminFile(string path)
    {
        var acl = new FileSecurity();
        acl.SetAccessRuleProtection(true, false);
        foreach (var sid in new[] { WellKnownSidType.LocalSystemSid, WellKnownSidType.BuiltinAdministratorsSid })
            acl.AddAccessRule(new FileSystemAccessRule(
                new SecurityIdentifier(sid, null),
                FileSystemRights.FullControl,
                AccessControlType.Allow));
        new FileInfo(path).SetAccessControl(acl);
    }

    public static ServiceConfig Load()
    {
        try
        {
            RejectReparsePoint(Folder);
            RejectReparsePoint(FilePath);
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<ServiceConfig>(File.ReadAllText(FilePath), Options) ?? new();
        }
        catch (Exception) { }
        return new();
    }

    public void Save()
    {
        EnsureSecureDirectory();
        var tmp = FilePath + ".tmp";
        RejectReparsePoint(tmp);
        RejectReparsePoint(FilePath);
        File.WriteAllText(tmp, JsonSerializer.Serialize(this, Options));
        LockAdminFile(tmp);
        File.Move(tmp, FilePath, true);
        LockAdminFile(FilePath);
    }
}
