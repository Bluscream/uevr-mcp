using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.Versioning;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;
using ModelContextProtocol.Server;

namespace UevrMcp;

/// <summary>
/// Auto-compile-check tools. Given an emitted UHT project (from uevr_dump_ue_project /
/// uevr_dump_uht_from_usmap) and a host Unreal project to build inside of, merge the
/// generated headers into a temporary target module, invoke UnrealBuildTool, and
/// report which headers compiled cleanly vs which failed. This validates the emitter
/// output against a real UHT + MSVC pass — it's the ground-truth check the emitter's
/// pattern matching alone can't provide.
/// </summary>
[McpServerToolType]
[SupportedOSPlatform("windows")]
public static class CompileCheckTools
{
    static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    // Resolve a plausible Unreal install root given an uproject's EngineAssociation.
    // Returns null if the engine can't be found.
    static string? ResolveEngineRoot(string engineAssociation)
    {
        // Epic launcher installs live under LauncherInstalled.dat; we parse the
        // human path with a regex and fall back to common defaults.
        var launcherDat = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            "Epic", "UnrealEngineLauncher", "LauncherInstalled.dat");
        if (File.Exists(launcherDat))
        {
            try
            {
                var doc = JsonDocument.Parse(File.ReadAllText(launcherDat));
                if (doc.RootElement.TryGetProperty("InstallationList", out var list))
                {
                    foreach (var e in list.EnumerateArray())
                    {
                        var ver = e.TryGetProperty("AppVersion", out var v) ? v.GetString() : null;
                        var loc = e.TryGetProperty("InstallLocation", out var l) ? l.GetString() : null;
                        var appName = e.TryGetProperty("AppName", out var a) ? a.GetString() : null;
                        if (loc is null || ver is null) continue;
                        // AppName is e.g. "UE_5.5"
                        if (appName is not null && appName.StartsWith("UE_" + engineAssociation, StringComparison.Ordinal)
                            && Directory.Exists(loc))
                            return loc;
                        // Version prefix match ("5.5.1-xxxx" starts with "5.5")
                        if (ver.StartsWith(engineAssociation, StringComparison.Ordinal)
                            && Directory.Exists(loc))
                            return loc;
                    }
                }
            }
            catch { }
        }

        foreach (var probe in new[] {
            @"C:\Program Files\Epic Games\UE_" + engineAssociation,
            @"D:\Program Files\Epic Games\UE_" + engineAssociation,
            @"E:\Program Files\Epic Games\UE_" + engineAssociation,
        })
        {
            if (Directory.Exists(probe)) return probe;
        }
        return null;
    }

    static string? UbtPath(string engineRoot)
    {
        // UE5 path (Build.bat drives UBT)
        var ubt = Path.Combine(engineRoot, "Engine", "Build", "BatchFiles", "Build.bat");
        if (File.Exists(ubt)) return ubt;
        return null;
    }

    // Lazy-cached per-engine set of header basenames (with .h extension). Used
    // to filter out emitted headers whose name would trigger UHT's "Two headers
    // with the same name is not allowed" manifest error. Scans Engine/Source
    // and Engine/Plugins recursively — tens of thousands of files but bounded
    // at ~100ms on SSD and cheap once cached.
    static readonly Dictionary<string, HashSet<string>> _engineHeaderCache = new(StringComparer.OrdinalIgnoreCase);
    static readonly Dictionary<string, HashSet<string>> _engineTypeNameCache = new(StringComparer.OrdinalIgnoreCase);
    static readonly object _engineHeaderLock = new();

    static HashSet<string> GetEngineHeaderNames(string engineRoot)
    {
        lock (_engineHeaderLock)
        {
            if (_engineHeaderCache.TryGetValue(engineRoot, out var cached)) return cached;
            var set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var sub in new[] {
                Path.Combine(engineRoot, "Engine", "Source"),
                Path.Combine(engineRoot, "Engine", "Plugins"),
            })
            {
                if (!Directory.Exists(sub)) continue;
                try
                {
                    foreach (var h in Directory.EnumerateFiles(sub, "*.h", SearchOption.AllDirectories))
                        set.Add(Path.GetFileName(h));
                }
                catch { }
            }
            _engineHeaderCache[engineRoot] = set;
            return set;
        }
    }

    // Scan engine headers for UCLASS/USTRUCT/UENUM/UINTERFACE declared names.
    // This catches struct-inside-bigger-header cases the filename scan misses —
    // UHT's "shares engine name 'ActorTickFunction' with struct 'FActorTickFunction'"
    // errors happen when a struct is defined inside EngineBaseTypes.h but our
    // emit produces its own ActorTickFunction.h. File scan doesn't match because
    // the engine struct isn't in its own file; macro scan does.
    //
    // Cache file on disk so repeat runs skip the ~2s parse.
    // Two-pass strategy. First find macro-annotation sites, then look forward
    // from each for the next `class|struct|enum|namespace` declaration,
    // skipping comments and blank lines. A single regex doesn't cover every
    // UE idiom: UENUM is often followed by a comment then `enum X : int`
    // (no "class"); USTRUCT can have meta=(...) nested parens; UCLASS may have
    // [Deprecated] attributes before the declaration.
    static readonly Regex ReMacroSite = new(
        @"(UCLASS|USTRUCT|UINTERFACE|UENUM)\s*\(",
        RegexOptions.Compiled);
    static readonly Regex ReNextDecl = new(
        @"\G(?:\s|/\*[\s\S]*?\*/|//[^\n]*\n|\[[^\]]*\])*" +
        @"(?<kind>class|struct|enum(?:\s+class)?|namespace)" +
        // Zero or more UPPERCASE attribute macros between kind and name:
        // ENGINE_API, UE_DEPRECATED(...), DEPRECATED(X, "..."), etc.
        @"(?:\s+[A-Z_][A-Z_0-9]*(?:\s*\([^)]*\))?)*" +
        @"\s+(?<name>\w+)",
        RegexOptions.Compiled);

    static HashSet<string> GetEngineTypeNames(string engineRoot)
    {
        lock (_engineHeaderLock)
        {
            if (_engineTypeNameCache.TryGetValue(engineRoot, out var cached)) return cached;
            var cacheFile = Path.Combine(Path.GetTempPath(), "uevr-mcp-engine-types-v5-" +
                BitConverter.ToString(System.Security.Cryptography.SHA1.HashData(
                    System.Text.Encoding.UTF8.GetBytes(engineRoot))).Replace("-", "")[..16] + ".txt");
            // Case-insensitive because UHT's "shares engine name" check lowercases
            // both sides before comparing.
            var set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            if (File.Exists(cacheFile))
            {
                try
                {
                    foreach (var line in File.ReadAllLines(cacheFile)) if (line.Length > 0) set.Add(line);
                    _engineTypeNameCache[engineRoot] = set;
                    return set;
                }
                catch { set.Clear(); }
            }

            foreach (var sub in new[] {
                Path.Combine(engineRoot, "Engine", "Source"),
                Path.Combine(engineRoot, "Engine", "Plugins"),
            })
            {
                if (!Directory.Exists(sub)) continue;
                try
                {
                    foreach (var h in Directory.EnumerateFiles(sub, "*.h", SearchOption.AllDirectories))
                    {
                        string text;
                        try { text = File.ReadAllText(h); } catch { continue; }
                        foreach (Match site in ReMacroSite.Matches(text))
                        {
                            // Skip past the macro's paren group (balanced) so we
                            // land right after it.
                            int i = site.Index + site.Length;
                            int depth = 1;
                            while (i < text.Length && depth > 0)
                            {
                                char c = text[i++];
                                if (c == '(') depth++;
                                else if (c == ')') depth--;
                            }
                            if (depth != 0) continue;
                            var next = ReNextDecl.Match(text, i);
                            if (!next.Success) continue;
                            var name = next.Groups["name"].Value;
                            if (name == "class") continue; // `struct class X` unreachable
                            // Strip A/U/F/E prefix.
                            if (name.Length >= 2 && (name[0] == 'A' || name[0] == 'U' || name[0] == 'F' || name[0] == 'E' || name[0] == 'I')
                                && (char.IsUpper(name[1]) || char.IsDigit(name[1])))
                                name = name.Substring(1);
                            // Also strip DEPRECATED_ prefix — UE renames old types
                            // as UDEPRECATED_Foo but UHT still compares against "Foo".
                            if (name.StartsWith("DEPRECATED_", StringComparison.Ordinal))
                                name = name.Substring("DEPRECATED_".Length);
                            set.Add(name);
                        }
                    }
                }
                catch { }
            }
            try { File.WriteAllLines(cacheFile, set); } catch { }
            _engineTypeNameCache[engineRoot] = set;
            return set;
        }
    }

    // UHT compares "engine names" after prefix-stripping. Mimic that.
    static string StripUePrefixForCompare(string name)
    {
        if (name.Length >= 2 && (name[0] == 'A' || name[0] == 'U' || name[0] == 'F' || name[0] == 'E' || name[0] == 'I')
            && (char.IsUpper(name[1]) || char.IsDigit(name[1])))
            name = name.Substring(1);
        if (name.StartsWith("DEPRECATED_", StringComparison.Ordinal))
            name = name.Substring("DEPRECATED_".Length);
        return name;
    }

    // Parse UBT log output for per-file errors/warnings. We key by filename so we
    // can report "header.h → 3 errors, 2 warnings" per emitted header.
    // Two formats to handle:
    //   - MSVC: path(line,col): error C1234: message
    //   - UHT:  path(line): Error: message  (no code, different casing)
    static readonly Regex ReMsvcDiag = new(
        @"^(?<path>[^\(\r\n]+?)\((?<line>\d+)(?:,\d+)?\)\s*:\s*(?<kind>[Ee]rror|[Ww]arning)(?:\s+(?<code>[A-Z]\d+))?\s*:?\s*(?<msg>.+)$",
        RegexOptions.Compiled | RegexOptions.Multiline);

    record FileDiag(string File, int Errors, int Warnings, List<string> Samples);

    static Dictionary<string, FileDiag> ParseDiagnostics(string log, string moduleDir)
    {
        var map = new Dictionary<string, FileDiag>(StringComparer.OrdinalIgnoreCase);
        foreach (Match m in ReMsvcDiag.Matches(log))
        {
            var path = m.Groups["path"].Value.Trim();
            var kind = m.Groups["kind"].Value.ToLowerInvariant();
            var msg = m.Groups["msg"].Value.Trim();
            // Only attribute diagnostics inside the emitted module — UBT spews
            // warnings from engine headers too, they're not our bugs.
            string? norm;
            try { norm = Path.GetFullPath(path); }
            catch { norm = path; }
            if (!norm.StartsWith(moduleDir, StringComparison.OrdinalIgnoreCase)) continue;
            var key = Path.GetFileName(norm);
            if (!map.TryGetValue(key, out var fd)) fd = new FileDiag(norm, 0, 0, new List<string>());
            if (kind == "error") fd = fd with { Errors = fd.Errors + 1 };
            else fd = fd with { Warnings = fd.Warnings + 1 };
            if (fd.Samples.Count < 3) fd.Samples.Add($"{kind} {m.Groups["code"].Value}: {msg}");
            map[key] = fd;
        }
        return map;
    }

    [McpServerTool(Name = "uevr_compile_check")]
    [Description("Run UnrealBuildTool on an emitted UHT project merged into a host .uproject. Copies emitted Source/<Module>/Public/*.h into a throwaway compile module inside the host project, invokes UBT, and reports per-header pass/fail with error samples. Validates the emitter output against a real UHT+MSVC pass.")]
    public static async Task<string> CompileCheck(
        [Description("Path to emitted UHT project dir (contains Source/<Module>/...). From uevr_dump_ue_project or uevr_dump_uht_from_usmap.")] string emittedDir,
        [Description("Path to a host .uproject to build inside. Its engine association is used to locate UBT.")] string hostUproject,
        [Description("Module name to create inside the host project. Defaults to the emitted module name.")] string? moduleName = null,
        [Description("Skip modifying the host .uproject file (assume module entry is already there).")] bool skipUprojectPatch = false,
        [Description("Timeout for UBT in seconds. Defaults to 600 (10 min) — large emitted modules can take a while.")] int timeoutSec = 600,
        [Description("Maximum number of headers to copy from emitted Public/ (sampled alphabetically). 0 = all. Useful for smoke-testing the emitter before committing to a full compile.")] int maxHeaders = 0,
        [Description("Only copy headers whose filename matches this substring (case-insensitive). Useful for narrowing a compile-check to a known-problematic family of types.")] string? headerFilter = null)
    {
        try
        {
            if (!Directory.Exists(emittedDir)) return Err($"emittedDir does not exist: {emittedDir}");
            if (!File.Exists(hostUproject)) return Err($"hostUproject does not exist: {hostUproject}");

            var emittedSource = Path.Combine(emittedDir, "Source");
            if (!Directory.Exists(emittedSource)) return Err($"No Source/ under {emittedDir}");
            var sourceModules = Directory.GetDirectories(emittedSource);
            if (sourceModules.Length == 0) return Err($"No modules under {emittedSource}");
            // Prefer the first module dir as the source. For multi-module emits we
            // pick the largest (most headers).
            var srcModuleDir = sourceModules.OrderByDescending(d =>
                Directory.EnumerateFiles(Path.Combine(d, "Public"), "*.h", SearchOption.TopDirectoryOnly).Count()).First();
            var srcModuleName = Path.GetFileName(srcModuleDir);
            moduleName ??= srcModuleName;
            // Sanitize module name — UBT is strict.
            moduleName = new string(moduleName.Where(c => char.IsLetterOrDigit(c) || c == '_').ToArray());
            if (string.IsNullOrEmpty(moduleName)) moduleName = "GeneratedSdk";

            var hostDir = Path.GetDirectoryName(Path.GetFullPath(hostUproject))!;
            var hostSourceDir = Path.Combine(hostDir, "Source");
            if (!Directory.Exists(hostSourceDir)) return Err($"Host project has no Source/: {hostSourceDir}");

            // Resolve engine root early so the header-copy step can filter out
            // basename collisions with the installed engine's own headers.
            string earlyEngineAssociation;
            using (var doc = JsonDocument.Parse(File.ReadAllText(hostUproject)))
                earlyEngineAssociation = doc.RootElement.TryGetProperty("EngineAssociation", out var ea)
                    ? ea.GetString() ?? "5.5" : "5.5";
            var engineRoot = ResolveEngineRoot(earlyEngineAssociation);

            // Build destination module dir. We always blow this away first so re-runs
            // are clean.
            var destModuleDir = Path.Combine(hostSourceDir, moduleName);
            if (Directory.Exists(destModuleDir)) Directory.Delete(destModuleDir, recursive: true);
            var destPublic = Path.Combine(destModuleDir, "Public");
            var destPrivate = Path.Combine(destModuleDir, "Private");
            Directory.CreateDirectory(destPublic);
            Directory.CreateDirectory(destPrivate);

            // Copy emitted headers + private sources.
            var srcPublic = Path.Combine(srcModuleDir, "Public");
            var srcPrivate = Path.Combine(srcModuleDir, "Private");
            int hdrCount = 0;
            int skippedEngineConflicts = 0;
            if (Directory.Exists(srcPublic))
            {
                IEnumerable<string> headers = Directory.EnumerateFiles(srcPublic, "*.h", SearchOption.TopDirectoryOnly).OrderBy(x => x, StringComparer.Ordinal);
                if (!string.IsNullOrEmpty(headerFilter))
                    headers = headers.Where(h => Path.GetFileName(h).Contains(headerFilter, StringComparison.OrdinalIgnoreCase));
                if (maxHeaders > 0)
                    headers = headers.Take(maxHeaders);
                foreach (var h in headers)
                {
                    // Skip headers whose basename OR declared type name collides
                    // with any UE engine header. Two checks:
                    //  - filename match → UHT's manifest duplicate-name rejection
                    //  - stripped-name match → UHT's "shares engine name" check
                    //    (catches struct-inside-bigger-header cases: our emit of
                    //    ActorTickFunction.h collides with engine's FActorTickFunction
                    //    which lives inside EngineBaseTypes.h)
                    if (engineRoot is not null)
                    {
                        var hname = Path.GetFileName(h);
                        if (GetEngineHeaderNames(engineRoot).Contains(hname))
                        {
                            skippedEngineConflicts++;
                            continue;
                        }
                        var stripped = StripUePrefixForCompare(Path.GetFileNameWithoutExtension(h));
                        if (GetEngineTypeNames(engineRoot).Contains(stripped))
                        {
                            skippedEngineConflicts++;
                            continue;
                        }
                    }
                    File.Copy(h, Path.Combine(destPublic, Path.GetFileName(h)));
                    hdrCount++;
                }
            }
            // Module stub — has to use the destination module name, not the emitted one.
            var modCpp = $$"""
                #include "Modules/ModuleManager.h"

                IMPLEMENT_MODULE(FDefaultGameModuleImpl, {{moduleName}});
                """;
            File.WriteAllText(Path.Combine(destPrivate, moduleName + ".cpp"), modCpp);

            // Build.cs with the same dependency set the emitter picks.
            var buildCs = $$"""
                using UnrealBuildTool;

                public class {{moduleName}} : ModuleRules {
                    public {{moduleName}}(ReadOnlyTargetRules Target) : base(Target) {
                        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
                        PublicDependencyModuleNames.AddRange(new string[] {
                            "Core", "CoreUObject", "Engine", "InputCore", "UMG",
                            "SlateCore", "Slate", "AIModule", "GameplayTags"
                        });
                    }
                }
                """;
            File.WriteAllText(Path.Combine(destModuleDir, moduleName + ".Build.cs"), buildCs);

            // Patch host .uproject to include the module. UBT will skip modules
            // that aren't listed.
            var engineAssociation = earlyEngineAssociation;

            if (!skipUprojectPatch)
            {
                var raw = File.ReadAllText(hostUproject);
                using var doc2 = JsonDocument.Parse(raw);
                var root = doc2.RootElement;
                var outDoc = new Dictionary<string, object?>();
                foreach (var prop in root.EnumerateObject()) outDoc[prop.Name] = JsonElementToObject(prop.Value);
                var modules = outDoc.TryGetValue("Modules", out var mv) && mv is List<object?> lv ? lv : new List<object?>();
                bool present = false;
                foreach (var mo in modules.OfType<Dictionary<string, object?>>())
                    if (mo.TryGetValue("Name", out var nv) && nv is string sn && sn == moduleName) { present = true; break; }
                if (!present)
                {
                    modules.Add(new Dictionary<string, object?>
                    {
                        ["Name"] = moduleName,
                        ["Type"] = "Runtime",
                        ["LoadingPhase"] = "Default",
                    });
                    outDoc["Modules"] = modules;
                    File.WriteAllText(hostUproject, JsonSerializer.Serialize(outDoc, JsonOpts));
                }
            }

            // Engine + UBT (engineRoot was resolved earlier for the copy filter).
            if (engineRoot is null)
                return Err($"Could not locate UE {engineAssociation} install. Checked LauncherInstalled.dat and common paths.");
            var ubt = UbtPath(engineRoot);
            if (ubt is null) return Err($"UBT not found under {engineRoot}");

            // Pick the editor target — most hosts have <Name>Editor.Target.cs.
            var hostStem = Path.GetFileNameWithoutExtension(hostUproject);
            var editorTarget = hostStem + "Editor";

            var psi = new ProcessStartInfo
            {
                FileName = ubt,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            };
            // Quoting: UBT parses the module + target + config as positional args
            // and -Project=... for the uproject. This is the standard recipe.
            psi.ArgumentList.Add(editorTarget);
            psi.ArgumentList.Add("Win64");
            psi.ArgumentList.Add("Development");
            psi.ArgumentList.Add("-Project=" + hostUproject);
            psi.ArgumentList.Add("-TargetType=Editor");
            psi.ArgumentList.Add("-Progress");
            psi.ArgumentList.Add("-NoHotReloadFromIDE");

            var sw = Stopwatch.StartNew();
            var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
            var stdout = new StringBuilder();
            var stderr = new StringBuilder();
            p.OutputDataReceived += (_, e) => { if (e.Data != null) stdout.AppendLine(e.Data); };
            p.ErrorDataReceived += (_, e) => { if (e.Data != null) stderr.AppendLine(e.Data); };
            p.Start();
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            if (!await WaitForExitAsync(p, TimeSpan.FromSeconds(timeoutSec)))
            {
                try { p.Kill(entireProcessTree: true); } catch { }
                return Err($"UBT timeout after {timeoutSec}s. Partial log len: {stdout.Length + stderr.Length}");
            }
            sw.Stop();

            var combined = stdout.ToString() + "\n" + stderr.ToString();
            var diags = ParseDiagnostics(combined, destModuleDir);
            int totalErrors = diags.Values.Sum(d => d.Errors);
            int totalWarnings = diags.Values.Sum(d => d.Warnings);

            // Rank: failing files first, most errors at top.
            var perFile = diags.Values
                .OrderByDescending(d => d.Errors)
                .ThenByDescending(d => d.Warnings)
                .Take(30)
                .Select(d => new {
                    file = Path.GetFileName(d.File),
                    errors = d.Errors,
                    warnings = d.Warnings,
                    samples = d.Samples,
                });

            // Tail of the log is most informative — UBT prints summary + link errors at the bottom.
            var tail = combined.Length > 8000 ? combined.Substring(combined.Length - 8000) : combined;

            return JsonSerializer.Serialize(new {
                ok = p.ExitCode == 0,
                data = new {
                    exitCode = p.ExitCode,
                    elapsedSec = Math.Round(sw.Elapsed.TotalSeconds, 1),
                    moduleName,
                    srcHeaders = hdrCount,
                    skippedEngineConflicts,
                    totalErrors,
                    totalWarnings,
                    perFile,
                    engineRoot,
                    logTail = tail,
                },
            }, JsonOpts);
        }
        catch (Exception ex)
        {
            return Err(ex.ToString());
        }
    }

    static async Task<bool> WaitForExitAsync(Process p, TimeSpan timeout)
    {
        using var cts = new CancellationTokenSource(timeout);
        try { await p.WaitForExitAsync(cts.Token); return true; }
        catch (OperationCanceledException) { return false; }
    }

    static object? JsonElementToObject(JsonElement e) => e.ValueKind switch
    {
        JsonValueKind.String => e.GetString(),
        JsonValueKind.Number => e.TryGetInt64(out var i) ? i : e.GetDouble(),
        JsonValueKind.True => true,
        JsonValueKind.False => false,
        JsonValueKind.Null => null,
        JsonValueKind.Array => e.EnumerateArray().Select(JsonElementToObject).ToList(),
        JsonValueKind.Object => e.EnumerateObject().ToDictionary(p => p.Name, p => JsonElementToObject(p.Value)),
        _ => null,
    };

    static string Err(string msg) => JsonSerializer.Serialize(new { ok = false, error = msg }, JsonOpts);
}
