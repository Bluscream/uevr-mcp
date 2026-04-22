# UevrMcpServer.exe CLI cheat sheet

Every MCP tool that matters has a positional-arg CLI verb. No MCP client
required. Drive it from `cmd`, PowerShell, bash, or any CI pipeline.

Absolute path to the binary after running `tools\setup.ps1`:
```
<repo>\mcp-server\bin\Release\net9.0\UevrMcpServer.exe
```

Every verb prints the same JSON the MCP tool would return, then exits.
If the first arg doesn't match any verb, the server falls through to
normal MCP stdio mode (the behaviour MCP clients rely on).

## Game lifecycle

| Verb | Args | What it does |
|---|---|---|
| `setup-game` | `<gameExe>` | Install uevr_mcp.dll → launch game → inject UEVRBackend → verify both modules loaded |
| `attach` | `<gameExe>` | Same flow but requires the game to already be running (skip launch). Needed for launcher-stub games: Hogwarts Legacy, Stellar Blade, any Steam/Epic launcher that spawns a child exe and exits |
| `stop-game` | `<gameExe>` | Graceful WM_CLOSE then Kill fallback |
| `wait-plugin` | `[timeoutMs]` | Block until the plugin HTTP on :8899 responds |
| `plugin-info` | `<gameExe>` | Report built-vs-installed DLL mtimes + pending-DLL presence + HTTP reachability + engine version hint |
| `plugin-rebuild` | — | `cmake --build plugin/build --config Release --target uevr_mcp` |

Example:
```powershell
UevrMcpServer.exe attach "D:\SteamLibrary\steamapps\common\Hogwarts Legacy\Phoenix\Binaries\Win64\HogwartsLegacy.exe"
```

## Dumping

| Verb | Args | What it does |
|---|---|---|
| `dump-usmap` | `<outPath> [filter] [compression]` | Emit a .usmap binary (jmap v4 / FModel-compatible). `compression` is `none` (default, most compatible) or `brotli` |
| `dump-ue-project` | `<outDir> [project] [modules] [engineAssoc] [methods:0\|1] [gameContent:0\|1]` | Full buildable UE project — .uproject + per-module Source/<Module>/{Public,Private}/*.h |
| `dump-bn-bundle` | `<outDir> [filter]` | Binary Ninja / IDA / Ghidra import bundle: .jmap + .hpp + Python import scripts |
| `emit-from-usmap` | `<usmap> <outDir> <proj> <module> <engineAssoc> [gobjPath]` | Convert any USMAP (Dumper-7, jmap, our own) into a UHT-style UE project. Pass Dumper-7's `GObjects-Dump-WithProperties.txt` as `gobjPath` to backfill real per-property offsets |
| `compile-check` | `<emittedDir> <hostUproject> [module] [maxHeaders] [headerFilter]` | Validate emitted UHT headers against real UHT + MSVC via UnrealBuildTool inside a host .uproject |

### `dump-ue-project` flag reference

Argument position | Meaning | Default
---|---|---
1 | `outDir` | (required)
2 | `projectName` — affects `.uproject` name and Target.cs class names. Pass `-` to infer from first module. | (inferred)
3 | `modules` — comma-separated module allow-list. `-` = all. | all `/Script/` modules
4 | `engineAssociation` — written into `.uproject`. | `4.26`
5 | `methods` — `1` to include UFUNCTION bodies + Kismet bytecode previews. 2-3× slower. | `0`
6 | `gameContent` — `1` to include `/Game/` BP-generated classes in a synthetic `Game` module. Needed to see `@kismet` previews (only BP classes have populated Script buffers). | `0`

Example — the "most complete" dump:
```powershell
UevrMcpServer.exe dump-ue-project C:\dumps\MyGame "MyGameMirror" - 4.26 1 1
```

## Patternsleuth wrappers

| Verb | Args | What it does |
|---|---|---|
| `ps-resolve` | `<exePath> [resolvers]` | Resolve UE symbols / AoB patterns in a game binary |
| `ps-xref` | `<exePath> <address>` | Find cross-references to a given RVA |
| `ps-diff` | `<exePath> [baselineJson] [outputJson]` | Diff resolver results against a baseline (for patch-day drift) |
| `ps-disasm` | `<exePath> <resolverOrRange>` | Disassemble a resolved function or address range |
| `ps-symbols` | `<exePath> <regex>` | Find PDB symbols matching a regex (requires symchk-fetched PDB) |

Example:
```powershell
UevrMcpServer.exe ps-resolve "D:\Games\Foo\Foo-Win64-Shipping.exe" "UGameEngine::Tick,UObjectBase::AddObject"
```

## Exit codes

All verbs exit `0` on success regardless of the JSON `ok` field. A non-zero
exit code means the CLI-argument parse failed or the server crashed. Check
the JSON `ok` field for tool-level errors.

## Piping output

The JSON is written to stdout. Redirect or pipe as usual:

```powershell
# Save full response
UevrMcpServer.exe dump-usmap C:\dumps\game.usmap > C:\dumps\usmap.log.json

# One-liner with jq-style extraction
UevrMcpServer.exe dump-usmap C:\dumps\game.usmap | ConvertFrom-Json | Select-Object -ExpandProperty data
```

## Common pitfalls

- **`setup-game` vs `attach`**: games with a Steam launcher stub (Hogwarts Legacy, DX12 AAA titles) spawn a child process and exit. `setup-game` tracks the stub PID and sees it exit → reports "Game process exited before we could inject." Use `attach` instead after launching via Steam.
- **Plugin HTTP not responding**: plugin initialization runs once the engine tick hook fires. On shipping games at main-menu idle this can take 5-10 s after injection. Give `wait-plugin 30000` a try.
- **`Game thread request timed out`**: reflection batches are game-thread-synchronous with a 10 s budget (20 s with methods). Large AAA games with `methods=1` need ~5 batches to walk the full UObject array; if one batch overruns, you get a partial dump. Retry with a tighter filter, or drop `methods=1` for speed.
