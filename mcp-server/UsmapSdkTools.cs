using System.ComponentModel;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using ModelContextProtocol.Server;

namespace UevrMcp;

/// <summary>
/// "USMAP → full UHT project" converter. Takes any valid USMAP file — whether
/// produced by our own uevr_dump_usmap, by Dumper-7, or by jmap_dumper — and
/// drives the same UHT emitter + project scaffolder that uevr_dump_uht_sdk /
/// uevr_dump_ue_project use against live reflection.
///
/// Why: on fragile games (DX12 AAA titles where UEVR's stereo render hook
/// crashes the process), Dumper-7 reliably produces a USMAP without any D3D
/// hooking. We can take that USMAP and still generate a buildable UE4/UE5
/// project, just without the UEVR-only extras (property offsets, ClassFlags,
/// UFUNCTION flags, interfaces — USMAP doesn't carry those fields).
///
/// Shelling to jmap's usmap CLI for the parse keeps the format handling in
/// the single well-tested place (jmap/usmap crate).
/// </summary>
[McpServerToolType]
public static class UsmapSdkTools
{
    static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };

    // ─── Parse USMAP via jmap ──────────────────────────────────────────

    static string? FindUsmapCli()
    {
        var env = Environment.GetEnvironmentVariable("USMAP_CLI");
        if (!string.IsNullOrEmpty(env) && File.Exists(env)) return env;
        var path = (Environment.GetEnvironmentVariable("PATH") ?? "")
            .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries);
        foreach (var d in path)
        {
            var c = Path.Combine(d, "usmap.exe");
            if (File.Exists(c)) return c;
        }
        // Common local fallback — the jmap repo's built binary on this box.
        var local = @"E:\Github\jmap\target\release\usmap.exe";
        if (File.Exists(local)) return local;
        return null;
    }

    static JsonDocument ParseUsmap(string usmapPath)
    {
        var cli = FindUsmapCli()
            ?? throw new InvalidOperationException(
                "usmap CLI not found. Set $USMAP_CLI or build jmap's usmap crate: " +
                "`cargo build --release -p usmap --manifest-path E:/Github/jmap/Cargo.toml`.");

        var psi = new ProcessStartInfo(cli)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        psi.ArgumentList.Add("to-json");
        psi.ArgumentList.Add(usmapPath);

        using var p = Process.Start(psi)
            ?? throw new InvalidOperationException("Failed to start usmap CLI");
        var stdout = p.StandardOutput.ReadToEnd();
        var stderr = p.StandardError.ReadToEnd();
        p.WaitForExit();
        if (p.ExitCode != 0)
            throw new InvalidOperationException(
                $"usmap CLI exited {p.ExitCode}: {stderr}");

        return JsonDocument.Parse(stdout);
    }

    // ─── jmap USMAP JSON → our reflection schema ───────────────────────
    //
    // jmap emits:   {structs: {Name: {super_struct, properties: [{name, array_dim, index, inner}]}}, enums: {Name: {entries: {value: entryName}}}}
    // we consume:   {classes: [...], structs: [...], enums: [{name, entries: [{name, value}]}]}
    //
    // USMAP doesn't distinguish UClass from UScriptStruct — it's all one schema
    // array. We pick Class vs Struct by name prefix ('A' or 'U' for classes,
    // 'F' for structs, plus a falls-back-to-struct rule).

    static JsonDocument AdaptJmapToReflection(JsonElement jmapRoot)
    {
        var classes = new List<object>();
        var structs = new List<object>();
        var enums = new List<object>();

        bool LooksLikeClass(string name)
        {
            if (string.IsNullOrEmpty(name)) return false;
            char c0 = name[0];
            // UE convention: A* actors, U* uobjects, I* interfaces -> UClass
            // F* structs, E* enums -> non-class. Stretch heuristic for game names
            // that don't follow convention: names ending in "Component" / "Actor"
            // / "Controller" / "Manager" / etc. are almost always classes.
            if (c0 == 'F' || c0 == 'E') return false;
            if (c0 == 'A' || c0 == 'U' || c0 == 'I') return true;
            if (name.EndsWith("Component", StringComparison.Ordinal)) return true;
            if (name.EndsWith("Actor",     StringComparison.Ordinal)) return true;
            if (name.EndsWith("Controller",StringComparison.Ordinal)) return true;
            if (name.EndsWith("Manager",   StringComparison.Ordinal)) return true;
            if (name.EndsWith("Settings",  StringComparison.Ordinal)) return true;
            // Default to struct — minimizes bad-cast fallout in generated headers.
            return false;
        }

        if (jmapRoot.TryGetProperty("structs", out var jmapStructs)
            && jmapStructs.ValueKind == JsonValueKind.Object)
        {
            foreach (var p in jmapStructs.EnumerateObject())
            {
                var name = p.Name;
                var body = p.Value;
                var super = body.TryGetProperty("super_struct", out var s) && s.ValueKind != JsonValueKind.Null
                    ? s.GetString() : null;

                var fields = new List<object>();
                if (body.TryGetProperty("properties", out var props) && props.ValueKind == JsonValueKind.Array)
                {
                    foreach (var fp in props.EnumerateArray())
                    {
                        var fname = fp.TryGetProperty("name", out var fn) ? fn.GetString() : null;
                        if (string.IsNullOrEmpty(fname)) continue;
                        var innerRaw = fp.GetProperty("inner");
                        var tag = InnerToTag(innerRaw);
                        string typeName = tag.TryGetValue("type", out var t) && t is string ts ? ts : "Unknown";
                        fields.Add(new {
                            name  = fname,
                            type  = typeName,
                            offset = 0,               // USMAP doesn't carry memory offsets
                            owner = name,
                            tag   = tag,
                        });
                    }
                }

                var entry = new {
                    name,
                    fullName = $"Class /Script/Unknown.{name}", // USMAP lacks package origin
                    super,
                    fields,
                };
                if (LooksLikeClass(name)) classes.Add(entry);
                else structs.Add(entry);
            }
        }

        if (jmapRoot.TryGetProperty("enums", out var jmapEnums)
            && jmapEnums.ValueKind == JsonValueKind.Object)
        {
            foreach (var p in jmapEnums.EnumerateObject())
            {
                var name = p.Name;
                var body = p.Value;
                var entries = new List<object>();
                if (body.TryGetProperty("entries", out var entriesEl)
                    && entriesEl.ValueKind == JsonValueKind.Object)
                {
                    foreach (var e in entriesEl.EnumerateObject())
                    {
                        if (!long.TryParse(e.Name, out var value)) continue;
                        entries.Add(new { name = e.Value.GetString(), value });
                    }
                }
                enums.Add(new { name, fullName = $"Enum /Script/Unknown.{name}", entries });
            }
        }

        // Serialize with our expected schema.
        var shaped = new
        {
            classes,
            structs,
            enums,
            stats = new {
                totalScanned = classes.Count + structs.Count + enums.Count,
                totalMatched = classes.Count + structs.Count + enums.Count,
                classCount = classes.Count,
                structCount = structs.Count,
                enumCount = enums.Count,
                objectErrors = 0,
                source = "usmap",
            },
        };
        return JsonDocument.Parse(JsonSerializer.Serialize(shaped));
    }

    // Convert jmap's inner property tag (strings + nested objects) into the
    // object-shape our emitter already consumes. Cases mirror jmap's
    // PropertyInner enum.
    static Dictionary<string, object?> InnerToTag(JsonElement inner)
    {
        // Scalar string forms: "Int", "Bool", "Name", ...
        if (inner.ValueKind == JsonValueKind.String)
        {
            return new Dictionary<string, object?> {
                ["type"] = ScalarToPropertyType(inner.GetString()!)
            };
        }
        // Object forms: {"Array": {"inner": ...}}, {"Struct": {"name": ...}}, ...
        if (inner.ValueKind == JsonValueKind.Object)
        {
            foreach (var kv in inner.EnumerateObject())
            {
                var kind = kv.Name;
                var body = kv.Value;
                switch (kind)
                {
                    case "Array": return new Dictionary<string, object?> {
                        ["type"] = "ArrayProperty",
                        ["inner"] = InnerToTag(body.GetProperty("inner"))
                    };
                    case "Set": return new Dictionary<string, object?> {
                        ["type"] = "SetProperty",
                        ["inner"] = InnerToTag(body.GetProperty("key"))
                    };
                    case "Map": return new Dictionary<string, object?> {
                        ["type"] = "MapProperty",
                        ["key"]   = InnerToTag(body.GetProperty("key")),
                        ["value"] = InnerToTag(body.GetProperty("value"))
                    };
                    case "Struct": return new Dictionary<string, object?> {
                        ["type"] = "StructProperty",
                        ["structName"] = body.GetProperty("name").GetString()
                    };
                    case "Enum": return new Dictionary<string, object?> {
                        ["type"] = "EnumProperty",
                        ["enumName"] = body.GetProperty("name").GetString(),
                        ["inner"] = InnerToTag(body.GetProperty("inner"))
                    };
                    case "Optional": return new Dictionary<string, object?> {
                        ["type"] = "OptionalProperty",
                        ["inner"] = InnerToTag(body.GetProperty("inner"))
                    };
                }
            }
        }
        return new Dictionary<string, object?> { ["type"] = "Unknown" };
    }

    static string ScalarToPropertyType(string s) => s switch
    {
        "Byte"              => "ByteProperty",
        "Bool"              => "BoolProperty",
        "Int"               => "IntProperty",
        "Float"             => "FloatProperty",
        "Object"            => "ObjectProperty",
        "Name"              => "NameProperty",
        "Delegate"          => "DelegateProperty",
        "Double"            => "DoubleProperty",
        "Str"               => "StrProperty",
        "Text"              => "TextProperty",
        "Interface"         => "InterfaceProperty",
        "MulticastDelegate" => "MulticastDelegateProperty",
        "WeakObject"        => "WeakObjectProperty",
        "LazyObject"        => "LazyObjectProperty",
        "AssetObject"       => "AssetObjectProperty",
        "SoftObject"        => "SoftObjectProperty",
        "UInt64"            => "UInt64Property",
        "UInt32"            => "UInt32Property",
        "UInt16"            => "UInt16Property",
        "Int64"             => "Int64Property",
        "Int16"             => "Int16Property",
        "Int8"              => "Int8Property",
        "FieldPath"         => "FieldPathProperty",
        "Utf8Str"           => "Utf8StrProperty",
        "AnsiStr"           => "AnsiStrProperty",
        _                   => "Unknown",
    };

    // ─── Tool: USMAP → UHT project ─────────────────────────────────────

    [McpServerTool(Name = "uevr_dump_uht_from_usmap")]
    [Description("Convert any USMAP (Dumper-7, jmap, or our uevr_dump_usmap output) into a full UHT-style UE4/UE5 project scaffold. Shells to jmap's usmap CLI to parse, then runs the same UHT emitter + project generator used by uevr_dump_ue_project against live reflection. Use this when UEVR injection is unsafe (DX12 AAA games that crash UEVR's render hooks) and you've already dumped a USMAP via Dumper-7. Output matches uevr_dump_ue_project but without the UEVR-only fields (per-property offsets, UCLASS/UFUNCTION flags, interface lists, ClassConfigName) — USMAP doesn't carry those.")]
    public static string DumpUhtFromUsmap(
        [Description("Absolute path to the input .usmap file.")] string usmapPath,
        [Description("Absolute output directory for the generated project.")] string outDir,
        [Description("Project name for the .uproject / Target.cs. Defaults to the usmap file's base name.")] string? projectName = null,
        [Description("Module name to place all types in (USMAP doesn't carry package origin). Default 'SDK'.")] string moduleName = "SDK",
        [Description("Engine association written into .uproject (default '4.26').")] string engineAssociation = "4.26")
    {
        if (!File.Exists(usmapPath))
            return JsonSerializer.Serialize(new { ok = false, error = $"usmap not found: {usmapPath}" }, JsonOpts);

        JsonDocument reflection;
        try
        {
            using var jmapDoc = ParseUsmap(usmapPath);
            reflection = AdaptJmapToReflection(jmapDoc.RootElement);
        }
        catch (Exception ex)
        {
            return JsonSerializer.Serialize(new { ok = false, error = ex.Message }, JsonOpts);
        }

        using (reflection)
        {
            projectName ??= Path.GetFileNameWithoutExtension(usmapPath);
            // Drive the project scaffolder directly through the reflection-fetch
            // seam. We stub FetchReflectionPublic-style input by handing the
            // adapted doc to the UHT emitter via a shared internal path.
            return UhtSdkTools.EmitUhtProjectFromReflection(reflection.RootElement,
                outDir, projectName, moduleName, engineAssociation);
        }
    }
}
