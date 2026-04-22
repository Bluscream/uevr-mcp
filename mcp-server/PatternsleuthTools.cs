using System.ComponentModel;
using System.Text.Json;
using System.Text.RegularExpressions;
using ModelContextProtocol.Server;

namespace UevrMcp;

/// <summary>
/// High-level wrappers over patternsleuth (https://github.com/trumank/patternsleuth) that
/// go beyond the thin `uevr_patternsleuth` shell-out:
///   - uevr_resolve_offsets:   run the named-resolver catalog, return structured JSON
///   - uevr_xref_scan:         find every code site referencing an address
///   - uevr_resolver_diff:     regenerate a report and diff vs. a saved baseline
///   - uevr_disassemble_fn:    disassemble a resolver target or raw range
///   - uevr_pdb_symbols:       regex-lookup symbols from an adjacent .pdb/.sym
/// All five shell out to `patternsleuth`. Locate via $PATTERNSLEUTH_EXE, PATH, or ~/.cargo/bin.
/// </summary>
[McpServerToolType]
public static class PatternsleuthTools
{
    // Curated list of the highest-value named resolvers. These are the offsets that
    // UEVR's built-in SDK doesn't already expose and that unlock useful workflows
    // (pak keys, allocator, BP VM, savegame, engine-loop hooks, lifecycle hooks).
    // Kept in a tiered order: lifecycle → memory → fname → engine → kismet → pak → savegame.
    internal static readonly string[] DefaultResolvers =
    {
        // UObject lifecycle / lookup
        "GUObjectArray",
        "FUObjectArrayAllocateUObjectIndex",
        "FUObjectArrayFreeUObjectIndex",
        "FUObjectHashTablesGet",
        "UObjectConditionalPostLoad",
        "UObjectBaseShutdown",
        "UObjectBaseUtilityGetPathName",
        "StaticFindObjectFast",
        "StaticConstructObjectInternal",
        // Allocator
        "GMalloc",
        "GMallocPatterns",
        // FName
        "FNamePool",
        "FNameCtorWchar",
        "FNameToString",
        "FNameToStringVoid",
        "FNameToStringFString",
        "StaticFNameConst",
        // FText
        "FTextFString",
        // Engine + build fingerprint
        "GEngine",
        "EngineVersion",
        "BuildConfiguration",
        "BuildChangeList",
        "InternalProjectName",
        "CustomVersionRegistry",
        "StaticCustomVersions",
        // Game loop
        "Main",
        "FEngineLoopInit",
        "FEngineLoopTick",
        "UGameEngineTick",
        // CVars
        "ConsoleManagerSingleton",
        // Kismet / BP VM
        "GNatives",
        "GNativesPatterns",
        "UObjectSkipFunction",
        "FFrameStep",
        "FFrameStepExplicitProperty",
        "UFunctionBind",
        // Pak + encryption
        "AESKeys",
        "FPakPlatformFileInitialize",
        // SaveGame
        "UGameplayStaticsSaveGameToMemory",
        "UGameplayStaticsSaveGameToSlot",
        "UGameplayStaticsLoadGameFromMemory",
        "UGameplayStaticsLoadGameFromSlot",
        "UGameplayStaticsDoesSaveGameExist",
    };

    static readonly JsonSerializerOptions Json = ExternalTools.Json;

    static string? ResolvePatternsleuth()
        => ExternalTools.ResolveExe("PATTERNSLEUTH_EXE", "patternsleuth.exe", "patternsleuth");

    // ─── uevr_resolve_offsets ───────────────────────────────────────────

    [McpServerTool(Name = "uevr_resolve_offsets")]
    [Description(
        "Resolve UE engine offsets by name against a game EXE using patternsleuth's resolver catalog. " +
        "Returns a structured JSON map { resolverName: result } where result is either the resolver's " +
        "serde output (typically an address or struct of addresses) or an error string. " +
        "Unlocks offsets UEVR's built-in SDK doesn't expose — GMalloc, GNatives (BP VM), AESKeys (pak), " +
        "StaticConstructObjectInternal, FUObjectArrayAllocate/FreeUObjectIndex (spawn/destroy hook points), " +
        "SaveGame primitives, engine-loop hooks, build fingerprint, etc. " +
        "Under the hood: stages the exe into a temp games/ dir, runs `patternsleuth report`, reads the JSON. " +
        "If 'resolvers' is omitted, runs a curated 40-resolver catalog covering the common UEVR-adjacent needs. " +
        "Pair with uevr_setup_game to discover the exe path."
    )]
    public static async Task<string> ResolveOffsets(
        [Description("Absolute path to the game .exe to scan.")] string exePath,
        [Description("Optional: comma/space-separated list of resolver names. Pass 'all' to run the full built-in catalog. Omit for the curated default set.")] string? resolvers = null,
        [Description("Timeout in ms (default 180000 = 3 min — some resolvers disassemble heavily).")] int timeoutMs = 180_000)
    {
        var psExe = ResolvePatternsleuth();
        if (psExe is null)
            return ExternalTools.Err("patternsleuth not found. Install with `cargo install --git https://github.com/trumank/patternsleuth patternsleuth_cli` (no crates.io / GitHub releases exist — master HEAD is the only source), or point $PATTERNSLEUTH_EXE at an existing binary.");
        if (!File.Exists(exePath))
            return ExternalTools.Err($"exe not found: {exePath}");

        var resolverList = ParseResolverList(resolvers);
        using var staged = StagedGameDir.Create(exePath);

        var args = new List<string> { "report", "--game", staged.GameName };
        if (resolverList is not null)
            foreach (var name in resolverList) { args.Add("--resolver"); args.Add(name); }

        var run = await ExternalTools.Run(psExe, args, cwd: staged.Root, timeoutMs: timeoutMs);
        if (run.ExitCode != 0)
            return JsonSerializer.Serialize(new { ok = false, step = "report", exitCode = run.ExitCode, command = run.Command, stdout = run.Stdout, stderr = run.Stderr }, Json);

        var reportFile = Directory
            .EnumerateFiles(Path.Combine(staged.Root, "reports"), "*.json", SearchOption.TopDirectoryOnly)
            .OrderByDescending(File.GetLastWriteTimeUtc)
            .FirstOrDefault();
        if (reportFile is null)
            return JsonSerializer.Serialize(new { ok = false, error = "patternsleuth produced no report JSON", stdout = run.Stdout, stderr = run.Stderr }, Json);

        using var doc = JsonDocument.Parse(File.ReadAllText(reportFile));
        // Report shape: { gameName: { resolverName: {"Ok": <...>} | {"Err": "..."} } }
        // We always staged under a single game name; unwrap that layer.
        var inner = doc.RootElement.EnumerateObject().FirstOrDefault().Value;

        var ok = new Dictionary<string, JsonElement>();
        var errors = new Dictionary<string, JsonElement>();
        if (inner.ValueKind == JsonValueKind.Object)
        {
            foreach (var prop in inner.EnumerateObject())
            {
                if (prop.Value.ValueKind == JsonValueKind.Object
                    && prop.Value.TryGetProperty("Ok", out var okEl))
                {
                    ok[prop.Name] = okEl.Clone();
                }
                else if (prop.Value.ValueKind == JsonValueKind.Object
                    && prop.Value.TryGetProperty("Err", out var errEl))
                {
                    errors[prop.Name] = errEl.Clone();
                }
                else
                {
                    ok[prop.Name] = prop.Value.Clone();
                }
            }
        }

        return JsonSerializer.Serialize(new
        {
            ok = true,
            exe = Path.GetFullPath(exePath),
            resolvers_requested = resolverList?.Count ?? 0,
            resolved = ok.Count,
            failed = errors.Count,
            offsets = ok,
            errors = errors.Count > 0 ? errors : null,
        }, Json);
    }

    // ─── uevr_xref_scan ─────────────────────────────────────────────────

    [McpServerTool(Name = "uevr_xref_scan")]
    [Description(
        "Find every instruction in a game EXE that references a given address — immediate loads, RIP-relative " +
        "LEA/MOV, direct CALL/JMP. Answers 'where is GMalloc called from' or 'who reads GEngine'. " +
        "Complements uevr_pattern_scan (byte-patterns only, same-function matches) and UEVR's in-process " +
        "scanner (no xref support). Offline: runs against the on-disk exe, so the game doesn't need to be running."
    )]
    public static async Task<string> XrefScan(
        [Description("Absolute path to the game .exe to scan.")] string exePath,
        [Description("Target address, decimal or 0x-prefixed hex (e.g. '0x14b5c0090').")] string address,
        [Description("Max matches to return (default 200).")] int maxMatches = 200,
        [Description("Timeout in ms (default 120000).")] int timeoutMs = 120_000)
    {
        var psExe = ResolvePatternsleuth();
        if (psExe is null)
            return ExternalTools.Err("patternsleuth not found. Install with `cargo install --git https://github.com/trumank/patternsleuth patternsleuth_cli` (no crates.io / GitHub releases exist — master HEAD is the only source), or point $PATTERNSLEUTH_EXE at an existing binary.");
        if (!File.Exists(exePath))
            return ExternalTools.Err($"exe not found: {exePath}");

        // Normalise to 0x-prefixed hex so patternsleuth's parser accepts it consistently.
        if (!TryParseAddress(address, out var addr))
            return ExternalTools.Err($"could not parse address: {address}");

        var r = await ExternalTools.Run(psExe,
            new[] { "scan", "--path", Path.GetFullPath(exePath), "--xref", $"0x{addr:x}" },
            timeoutMs: timeoutMs);

        var hits = ParseScanAddresses(r.Stdout).Take(maxMatches).ToList();

        return JsonSerializer.Serialize(new
        {
            ok = r.ExitCode == 0,
            exe = Path.GetFullPath(exePath),
            target = $"0x{addr:x}",
            command = r.Command,
            count = hits.Count,
            xrefs = hits.Select(a => $"0x{a:x}").ToArray(),
            stdout = r.Stdout.Length > 32_000 ? r.Stdout[..32_000] + "\n[truncated]" : r.Stdout,
            stderr = r.Stderr,
        }, Json);
    }

    // ─── uevr_resolver_diff ─────────────────────────────────────────────

    [McpServerTool(Name = "uevr_resolver_diff")]
    [Description(
        "Regenerate a patternsleuth resolver report for a game EXE and diff it against a saved baseline. " +
        "Use this to catch offset drift after a game patch before UEVR's runtime scan silently mis-binds. " +
        "If 'baselineJsonPath' is omitted, just writes a fresh report and returns the path (that becomes the " +
        "next-run baseline). When both runs exist: returns three lists — moved (address changed), lost " +
        "(resolved before, now errors or missing), and gained (new resolvers that now resolve). " +
        "Accepts a report file written by a prior call, or any JSON in patternsleuth's report shape."
    )]
    public static async Task<string> ResolverDiff(
        [Description("Absolute path to the game .exe to scan now.")] string exePath,
        [Description("Optional: path to a baseline JSON report produced by an earlier call or by `patternsleuth report`. If null, no diff is performed.")] string? baselineJsonPath = null,
        [Description("Optional: destination path for the newly generated report JSON. If null, keeps it inside a temp dir (lost on process exit).")] string? outputJsonPath = null,
        [Description("Optional: comma/space-separated resolver names (default: curated catalog).")] string? resolvers = null,
        [Description("Timeout in ms (default 180000).")] int timeoutMs = 180_000)
    {
        var psExe = ResolvePatternsleuth();
        if (psExe is null)
            return ExternalTools.Err("patternsleuth not found. Install with `cargo install --git https://github.com/trumank/patternsleuth patternsleuth_cli` (no crates.io / GitHub releases exist — master HEAD is the only source), or point $PATTERNSLEUTH_EXE at an existing binary.");
        if (!File.Exists(exePath))
            return ExternalTools.Err($"exe not found: {exePath}");
        if (baselineJsonPath is not null && !File.Exists(baselineJsonPath))
            return ExternalTools.Err($"baseline not found: {baselineJsonPath}");

        var resolverList = ParseResolverList(resolvers);
        using var staged = StagedGameDir.Create(exePath);

        var args = new List<string> { "report", "--game", staged.GameName };
        if (resolverList is not null)
            foreach (var r in resolverList) { args.Add("--resolver"); args.Add(r); }

        var run = await ExternalTools.Run(psExe, args, cwd: staged.Root, timeoutMs: timeoutMs);
        if (run.ExitCode != 0)
            return JsonSerializer.Serialize(new { ok = false, step = "report", exitCode = run.ExitCode, stdout = run.Stdout, stderr = run.Stderr }, Json);

        var fresh = Directory
            .EnumerateFiles(Path.Combine(staged.Root, "reports"), "*.json", SearchOption.TopDirectoryOnly)
            .OrderByDescending(File.GetLastWriteTimeUtc)
            .FirstOrDefault();
        if (fresh is null)
            return JsonSerializer.Serialize(new { ok = false, error = "no report emitted", stderr = run.Stderr }, Json);

        string persistedPath = fresh;
        if (!string.IsNullOrEmpty(outputJsonPath))
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outputJsonPath))!);
            File.Copy(fresh, outputJsonPath, overwrite: true);
            persistedPath = Path.GetFullPath(outputJsonPath);
        }

        if (baselineJsonPath is null)
        {
            return JsonSerializer.Serialize(new
            {
                ok = true,
                mode = "baseline-only",
                report = persistedPath,
                hint = "re-run with baselineJsonPath set to this file after a game patch to detect drift",
            }, Json);
        }

        var diff = DiffReports(File.ReadAllText(baselineJsonPath), File.ReadAllText(fresh));
        return JsonSerializer.Serialize(new
        {
            ok = true,
            mode = "diff",
            baseline = Path.GetFullPath(baselineJsonPath),
            current = persistedPath,
            moved = diff.Moved,
            lost = diff.Lost,
            gained = diff.Gained,
            unchanged = diff.Unchanged,
        }, Json);
    }

    // ─── uevr_disassemble_function ──────────────────────────────────────

    [McpServerTool(Name = "uevr_disassemble_fn")]
    [Description(
        "Disassemble a function in a game EXE, either by resolver name (patternsleuth resolves the address " +
        "then walks its exception table to find bounds) or by explicit start/end addresses. Uses iced-x86 " +
        "so RIP-relative operands are rendered correctly. Complements UEVR's uevr_read_memory by handling " +
        "x86-64 decoding and function boundary detection. Works offline against the on-disk exe."
    )]
    public static async Task<string> DisassembleFunction(
        [Description("Absolute path to the game .exe.")] string exePath,
        [Description("Either a resolver name (e.g. 'GMalloc', 'FNameToString') OR a 'start:end' hex range (e.g. '0x14b5c0090:0x14b5c01f0'). If a resolver is given, its target function is walked.")] string resolverOrRange,
        [Description("Timeout in ms (default 120000).")] int timeoutMs = 120_000)
    {
        var psExe = ResolvePatternsleuth();
        if (psExe is null)
            return ExternalTools.Err("patternsleuth not found. Install with `cargo install --git https://github.com/trumank/patternsleuth patternsleuth_cli` (no crates.io / GitHub releases exist — master HEAD is the only source), or point $PATTERNSLEUTH_EXE at an existing binary.");
        if (!File.Exists(exePath))
            return ExternalTools.Err($"exe not found: {exePath}");

        var fullExe = Path.GetFullPath(exePath);
        var isRange = resolverOrRange.Contains(':');

        if (isRange)
        {
            // view-symbol --function <path>:<start>:<end>. Its parser splits on ':',
            // so a Windows drive letter ("C:\...") produces 4 fields, not 3, and the
            // spec fails to parse. Stage the exe into a temp dir and pass a relative
            // path that contains no ':' so the two address colons are the only ones.
            var parts = resolverOrRange.Split(':');
            if (parts.Length != 2
                || !TryParseAddress(parts[0], out var start)
                || !TryParseAddress(parts[1], out var end))
                return ExternalTools.Err($"expected 'start:end' hex range, got '{resolverOrRange}'");

            using var stagedRange = StagedGameDir.Create(exePath);
            var relExe = Path.Combine("games", stagedRange.GameName, Path.GetFileName(exePath))
                .Replace('\\', '/');
            var r = await ExternalTools.Run(psExe,
                new[] { "view-symbol", "--function", $"{relExe}:0x{start:x}:0x{end:x}" },
                cwd: stagedRange.Root,
                timeoutMs: timeoutMs);

            return JsonSerializer.Serialize(new
            {
                ok = r.ExitCode == 0,
                mode = "range",
                range = $"0x{start:x}..0x{end:x}",
                command = r.Command,
                disasm = r.Stdout,
                stderr = r.Stderr,
            }, Json);
        }

        // Resolver mode: `scan --resolver --disassemble-merged` only disassembles
        // pattern matches, not resolver results. Use `view-symbol --resolver` which
        // resolves the address, walks its exception-table bounds, and disassembles.
        // view-symbol iterates every game in the games/ dir — stage one so we only
        // get disasm for the target.
        using var staged = StagedGameDir.Create(exePath);
        var vrun = await ExternalTools.Run(psExe,
            new[] { "view-symbol", "--resolver", resolverOrRange },
            cwd: staged.Root,
            timeoutMs: timeoutMs);

        return JsonSerializer.Serialize(new
        {
            ok = vrun.ExitCode == 0,
            mode = "resolver",
            resolver = resolverOrRange,
            command = vrun.Command,
            disasm = vrun.Stdout,
            stderr = vrun.Stderr,
        }, Json);
    }

    // ─── uevr_pdb_symbols ───────────────────────────────────────────────

    [McpServerTool(Name = "uevr_pdb_symbols")]
    [Description(
        "Regex-lookup public symbols from a .pdb or .sym file adjacent to a game EXE. Shipping titles rarely " +
        "include a PDB, but engine-source builds, early-access titles, and some modding targets do. Returns " +
        "matched symbol names with their addresses and disassembly. Errors clearly if no PDB/.sym is present."
    )]
    public static async Task<string> PdbSymbols(
        [Description("Absolute path to the game .exe. Must have a .pdb or .sym next to it.")] string exePath,
        [Description("Regex to match symbol names (passed to patternsleuth verbatim, Rust regex syntax).")] string regex,
        [Description("Timeout in ms (default 180000).")] int timeoutMs = 180_000)
    {
        var psExe = ResolvePatternsleuth();
        if (psExe is null)
            return ExternalTools.Err("patternsleuth not found. Install with `cargo install --git https://github.com/trumank/patternsleuth patternsleuth_cli` (no crates.io / GitHub releases exist — master HEAD is the only source), or point $PATTERNSLEUTH_EXE at an existing binary.");
        if (!File.Exists(exePath))
            return ExternalTools.Err($"exe not found: {exePath}");

        var dir = Path.GetDirectoryName(exePath)!;
        var stem = Path.GetFileNameWithoutExtension(exePath);
        var pdb = Path.Combine(dir, stem + ".pdb");
        var sym = Path.Combine(dir, stem + ".sym");
        var hasSymbols = File.Exists(pdb) || File.Exists(sym);
        if (!hasSymbols)
            return ExternalTools.Err($"no .pdb or .sym found next to {exePath}. patternsleuth symbols requires one.");

        using var staged = StagedGameDir.Create(exePath);
        // Also stage the .pdb/.sym next to the exe in the temp dir.
        var stagedExe = Path.Combine(staged.Root, "games", staged.GameName, Path.GetFileName(exePath));
        var stagedDir = Path.GetDirectoryName(stagedExe)!;
        if (File.Exists(pdb)) File.Copy(pdb, Path.Combine(stagedDir, Path.GetFileName(pdb)), overwrite: true);
        if (File.Exists(sym)) File.Copy(sym, Path.Combine(stagedDir, Path.GetFileName(sym)), overwrite: true);

        var r = await ExternalTools.Run(psExe,
            new[] { "symbols", "--game", staged.GameName, "--symbol", regex },
            cwd: staged.Root,
            timeoutMs: timeoutMs);

        return JsonSerializer.Serialize(new
        {
            ok = r.ExitCode == 0,
            exe = Path.GetFullPath(exePath),
            regex,
            command = r.Command,
            stdout = r.Stdout,
            stderr = r.Stderr,
        }, Json);
    }

    // ─── helpers ────────────────────────────────────────────────────────

    static List<string>? ParseResolverList(string? s)
    {
        if (string.IsNullOrWhiteSpace(s)) return DefaultResolvers.ToList();
        if (s.Trim().Equals("all", StringComparison.OrdinalIgnoreCase)) return null; // null => no --resolver flags => patternsleuth runs its full default set
        return s.Split(new[] { ',', ' ', '\t', '\n' }, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .ToList();
    }

    static bool TryParseAddress(string s, out ulong addr)
    {
        s = s.Trim();
        if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            return ulong.TryParse(s.AsSpan(2), System.Globalization.NumberStyles.HexNumber, null, out addr);
        return ulong.TryParse(s, out addr)
            || ulong.TryParse(s, System.Globalization.NumberStyles.HexNumber, null, out addr);
    }

    // Prettytable rows from `scan` look like: | sig | "000000014b5c0090 "arg[xref 0]"" |
    // We just grab any 16-hex-digit word that appears in a row. Robust across color/no-color.
    static readonly Regex ScanAddrRegex = new(@"\b([0-9a-fA-F]{16})\b", RegexOptions.Compiled);
    static IEnumerable<ulong> ParseScanAddresses(string stdout)
    {
        var seen = new HashSet<ulong>();
        foreach (Match m in ScanAddrRegex.Matches(stdout))
        {
            if (ulong.TryParse(m.Groups[1].Value, System.Globalization.NumberStyles.HexNumber, null, out var a)
                && seen.Add(a))
                yield return a;
        }
    }

    internal record DiffResult(
        Dictionary<string, object?> Moved,
        Dictionary<string, object?> Lost,
        Dictionary<string, object?> Gained,
        int Unchanged);

    static DiffResult DiffReports(string baselineJson, string currentJson)
    {
        var baseline = Flatten(baselineJson);
        var current = Flatten(currentJson);

        var moved = new Dictionary<string, object?>();
        var lost = new Dictionary<string, object?>();
        var gained = new Dictionary<string, object?>();
        int unchanged = 0;

        foreach (var kv in baseline)
        {
            if (!current.TryGetValue(kv.Key, out var cur))
            {
                lost[kv.Key] = new { was = kv.Value };
                continue;
            }
            if (!JsonEqual(kv.Value, cur))
                moved[kv.Key] = new { from = kv.Value, to = cur };
            else
                unchanged++;
        }
        foreach (var kv in current)
            if (!baseline.ContainsKey(kv.Key)) gained[kv.Key] = new { now = kv.Value };

        return new DiffResult(moved, lost, gained, unchanged);
    }

    // Flatten a patternsleuth report: { game: { resolver: <result> } } → { resolver: <result> }
    // (we stage exactly one game, so the outer layer is single-entry; if not, keys are prefixed).
    static Dictionary<string, JsonElement> Flatten(string json)
    {
        var outp = new Dictionary<string, JsonElement>();
        using var doc = JsonDocument.Parse(json);
        if (doc.RootElement.ValueKind != JsonValueKind.Object) return outp;

        var games = doc.RootElement.EnumerateObject().ToList();
        bool prefix = games.Count != 1;
        foreach (var g in games)
        {
            if (g.Value.ValueKind != JsonValueKind.Object) continue;
            foreach (var r in g.Value.EnumerateObject())
            {
                var key = prefix ? $"{g.Name}::{r.Name}" : r.Name;
                outp[key] = r.Value.Clone();
            }
        }
        return outp;
    }

    static bool JsonEqual(JsonElement a, JsonElement b)
        => JsonSerializer.Serialize(a) == JsonSerializer.Serialize(b);

    // Build a temp `games/<name>/<exe>` layout that patternsleuth's report/symbols
    // subcommands require, using a File.Copy (fast enough for 100–300 MB exes).
    // Cleaned up automatically on Dispose.
    internal sealed class StagedGameDir : IDisposable
    {
        public string Root { get; }
        public string GameName { get; }

        StagedGameDir(string root, string gameName)
        {
            Root = root;
            GameName = gameName;
        }

        public static StagedGameDir Create(string exePath)
        {
            var root = Directory.CreateTempSubdirectory("uevr-ps-").FullName;
            var gameName = SafeName(Path.GetFileNameWithoutExtension(exePath));
            var gameDir = Path.Combine(root, "games", gameName);
            Directory.CreateDirectory(gameDir);
            var dest = Path.Combine(gameDir, Path.GetFileName(exePath));
            File.Copy(exePath, dest, overwrite: true);
            return new StagedGameDir(root, gameName);
        }

        static string SafeName(string s)
        {
            var safe = new string(s.Select(c => char.IsLetterOrDigit(c) || c == '-' || c == '_' ? c : '_').ToArray());
            return string.IsNullOrEmpty(safe) ? "target" : safe;
        }

        public void Dispose()
        {
            try { Directory.Delete(Root, recursive: true); } catch { /* temp cleanup is best-effort */ }
        }
    }
}
