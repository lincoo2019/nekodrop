// SPDX-License-Identifier: GPL-2.0-only
using System.IO.Pipes;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using System.Text.Json.Nodes;

namespace NekoDrop.Service;

/// <summary>
/// \\.\pipe\nekodrop: one JSON object a line in each direction. Commands:
/// {"do":"status"}, {"do":"mode","mode":"concurrent|ondemand|off"},
/// {"do":"wake"}, {"do":"touch"}, {"do":"stop"}. Events: "state", "log".
/// Interactive users and SYSTEM may connect; nothing from the network can.
/// </summary>
public sealed class ControlPipe(Supervisor sup)
{
    const string Name = "nekodrop";
    readonly List<StreamWriter> _clients = [];

    public async Task Run(CancellationToken ct)
    {
        sup.Changed += Broadcast;
        sup.Log += line => Broadcast(new JsonObject { ["event"] = "log", ["line"] = line });
        while (!ct.IsCancellationRequested)
        {
            var server = Create();
            try { await server.WaitForConnectionAsync(ct); }
            catch (OperationCanceledException) { server.Dispose(); break; }
            _ = Task.Run(() => Serve(server, ct), ct);
        }
    }

    static NamedPipeServerStream Create()
    {
        var security = new PipeSecurity();
        security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.InteractiveSid, null),
            PipeAccessRights.ReadWrite, AccessControlType.Allow));
        security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
            PipeAccessRights.FullControl, AccessControlType.Allow));
        security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.NetworkSid, null),
            PipeAccessRights.FullControl, AccessControlType.Deny));
        return NamedPipeServerStreamAcl.Create(Name, PipeDirection.InOut, NamedPipeServerStream.MaxAllowedServerInstances,
            PipeTransmissionMode.Byte, PipeOptions.Asynchronous, 4096, 4096, security);
    }

    async Task Serve(NamedPipeServerStream pipe, CancellationToken ct)
    {
        using var _ = pipe;
        var reader = new StreamReader(pipe, Encoding.UTF8);
        var writer = new StreamWriter(pipe, new UTF8Encoding(false)) { AutoFlush = true };
        lock (_clients) _clients.Add(writer);
        try
        {
            await Send(writer, sup.Snapshot());
            while (!ct.IsCancellationRequested)
            {
                var line = await reader.ReadLineAsync(ct);
                if (line is null) break;
                JsonObject? cmd;
                try { cmd = JsonNode.Parse(line) as JsonObject; }
                catch (System.Text.Json.JsonException) { continue; }
                switch ((string?)cmd?["do"])
                {
                    case "status": await Send(writer, sup.Snapshot()); break;
                    case "wake": sup.Wake(retry: (bool?)cmd?["retry"] == true); break;
                    case "touch": sup.Touch(); break;
                    case "stop": sup.Stop("asked by the client", giveBack: true); break;
                    case "mode":
                        if (Enum.TryParse<NetMode>((string?)cmd?["mode"], true, out var m)) sup.SetMode(m);
                        break;
                    case "fast-unlock": sup.SetFastUnlock((bool?)cmd?["on"] ?? false); break;
                    case "coex-plan": sup.SetCoexPlan((bool?)cmd?["on"] ?? false); break;
                    case "high-throughput": sup.SetHighThroughput((bool?)cmd?["on"] ?? false); break;
                    case "cards":
                    {
                        // For the manual choice: the PCI network controllers and the models a card can be run as.
                        var candidates = new JsonArray();
                        foreach (var c in Card.Candidates())
                            candidates.Add(new JsonObject { ["hw"] = c.HardwareId, ["name"] = c.Name, ["recognized"] = c.Recognized });
                        var models = new JsonArray();
                        foreach (var model in CardCatalog.ManualModels)
                            models.Add(new JsonObject { ["key"] = model.Key, ["name"] = model.Name, ["vendor"] = model.VendorId });
                        await Send(writer, new JsonObject { ["event"] = "cards", ["candidates"] = candidates, ["models"] = models });
                        break;
                    }
                    case "card-manual":
                    {
                        var why = sup.SetManualCard((string?)cmd?["hw"] ?? "", (string?)cmd?["model"] ?? "");
                        await Send(writer, new JsonObject { ["event"] = "card-manual", ["error"] = why });
                        break;
                    }
                    case "wifi-scan": sup.WifiScan((string?)cmd?["ssid"]); break;
                    case "wifi-connect":
                    {
                        var why = sup.WifiConnect((string?)cmd?["ssid"] ?? "", (string?)cmd?["password"],
                                                  (bool?)cmd?["auto"], (bool?)cmd?["hidden"] ?? false);
                        await Send(writer, new JsonObject { ["event"] = "wifi-connect", ["ssid"] = (string?)cmd?["ssid"], ["error"] = why });
                        break;
                    }
                    case "wifi-disconnect": sup.WifiDisconnect(); break;
                    case "wifi-forget": sup.WifiForget((string?)cmd?["ssid"] ?? ""); break;
                    case "wifi-auto": sup.WifiAuto((string?)cmd?["ssid"] ?? "", (bool?)cmd?["on"] ?? true); break;
                }
            }
        }
        catch (IOException) { }
        catch (OperationCanceledException) { }
        finally { lock (_clients) _clients.Remove(writer); }
    }

    /// <summary>One write at a time per client: a broadcast and a reply used to
    /// overlap on the same StreamWriter, which throws (Codex review #34).</summary>
    static readonly System.Runtime.CompilerServices.ConditionalWeakTable<StreamWriter, SemaphoreSlim> WriteLocks = new();

    static async Task Send(StreamWriter w, JsonObject o)
    {
        var gate = WriteLocks.GetValue(w, _ => new SemaphoreSlim(1, 1));
        // A client that stops reading is not waited for forever.
        if (!await gate.WaitAsync(TimeSpan.FromSeconds(5))) return;
        // The deadline covers the write too (round 2 #16): a client that stops
        // reading has its pipe closed instead of holding the lock.
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        try { await w.WriteLineAsync(o.ToJsonString().AsMemory(), cts.Token); }
        catch (OperationCanceledException) { try { w.BaseStream.Dispose(); } catch (Exception) { } }
        catch (IOException) { }
        catch (ObjectDisposedException) { }
        catch (InvalidOperationException) { }
        finally { gate.Release(); }
    }

    void Broadcast(JsonObject o)
    {
        StreamWriter[] all;
        lock (_clients) all = [.. _clients];
        foreach (var w in all) _ = Send(w, (JsonObject)o.DeepClone());
    }
}
