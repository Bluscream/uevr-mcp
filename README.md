# UEVR-MCP Server

An [MCP (Model Context Protocol)](https://modelcontextprotocol.io) server that gives AI agents live, programmatic access to any Unreal Engine game running with [UEVR](https://github.com/praydog/UEVR) — from flat-screen games injected into VR to native UE titles.

- **Inspect everything.** Every UObject, every field, every UFunction — searchable and navigable from a running game. Call any method, chain multi-step queries across the object graph, batch operations in a single tick.
- **Read and write live state.** Player health, enemy AI, physics, materials, animations, transforms — the agent sees what the game sees, in real time.
- **Full VR control.** HMD and controller poses, haptic feedback, snap turn, aim method, motion controller attachment — complete VR subsystem access.
- **Live Lua scripting.** Execute Lua code in the game process with persistent state, frame callbacks, timers, async coroutines, and a module system. Hot-reload scripts without losing state. Write and deploy scripts to the UEVR autorun folder.
- **Screenshot from the GPU.** Capture the game's D3D11 or D3D12 backbuffer as JPEG — works even when the game window is behind other windows. Handles R10G10B10A2, FP16 HDR, BGRA8, and RGBA8 formats with high-quality WIC scaling.
- **Crash diagnostics built in.** Pull a single snapshot with callback health counters, breadcrumb state, plugin inventory, latest crash dump, UEVR log tail, runtime map/controller/pawn, and live render metadata.
- **Reverse-engineer anything.** Snapshot an object's state, perform an action, diff to see what changed. Hook any UFunction to log calls, block execution, or run Lua callbacks with argument inspection. Watch properties with Lua triggers for reactive scripting.
- **Real-time event streaming.** Poll for hook fires, watch changes, and Lua output in real time via long-polling.
- **ProcessEvent listener.** Start/stop a global ProcessEvent hook to capture every Blueprint and native function call in real time — filter by name, ignore noisy ticks, establish baselines, and discover what the game does when you take an action. Equivalent to UEVR's Developer tab.
- **Motion controller attachment.** Attach any actor or component to a VR controller hand with position/rotation offsets — the core mechanism for making VR mods feel native.
- **Timer/scheduler.** Create one-shot or repeating timers that execute Lua code, with full lifecycle management (list, cancel, clear).
- **Dump a buildable UE project from a running game.** One call produces a `.uproject` + per-module `Source/<Module>/{Public,Private}/*.h` tree with real `UCLASS(Config=Game) / UPROPERTY(EditAnywhere, Replicated) / AActor* Owner` headers, real property offsets, typed object refs, enum entries, and interface inheritance — the same output jmap's reconstruction flow produces, one MCP call. Falls back to [Dumper-7](https://github.com/Encryqed/Dumper-7) + USMAP conversion for games where UEVR's render hooks are fragile.
- **Byte-compatible USMAP output.** Drop the file next to FModel / CUE4Parse / UAssetAPI to parse unversioned cooked assets for the game. Validated by round-tripping through jmap's own USMAP parser.
- **Self-discovering reflection probes.** Property-subclass fields UEVR's public API doesn't expose (FObjectProperty::PropertyClass, FMapProperty::KeyProp, UClass::ClassFlags / ClassConfigName / Interfaces[], delegate signatures) get located at runtime via SEH-guarded memory probes with UObjectHook-backed validation — works across UE4.22–UE5.4 and across games without a hardcoded offset table.
- **Works across games.** Same 168 tools work on any Unreal Engine game that UEVR supports, plus a Dumper-7 fallback path for games too fragile for UEVR's render hooks.

## Setup

### Prerequisites

- [UEVR](https://github.com/praydog/UEVR) installed and configured for your game
- [.NET 9+ SDK](https://dotnet.microsoft.com/download/dotnet/9.0) for the MCP server
- [CMake 3.21+](https://cmake.org) and Visual Studio 2022 Build Tools to compile the plugin (or use a pre-built release)

### 1. Build and install the plugin

```bash
cd plugin
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Copy `build/Release/uevr_mcp.dll` into your UEVR plugins directory for the target game:

```
%APPDATA%\UnrealVRMod\<GameExeName>\plugins\uevr_mcp.dll
```

For example: `%APPDATA%\UnrealVRMod\MyGame-Win64-Shipping\plugins\uevr_mcp.dll`

### 2. Connect an MCP client

The repo root contains `.mcp.json` — agents that support workspace-level MCP config (Claude Code, Cursor, etc.) will detect it automatically.

To register manually:

```json
{
  "mcpServers": {
    "uevr": {
      "command": "dotnet",
      "args": ["run", "--project", "/path/to/mcp-server"],
      "env": {
        "UEVR_MCP_API_URL": "http://localhost:8899"
      }
    }
  }
}
```

### 3. Launch the game

Start the game, inject UEVR, and the plugin starts automatically. The HTTP server listens on `localhost:8899`. The MCP server connects to it on first tool call.

Prefer the one-call flow below — `uevr_setup_game` handles plugin install + process launch + backend injection + verification in a single call.

## Launching and lifecycle

The `SetupTools` + `ReadinessTools` surface turns "point the MCP at a game exe" into a one-call flow. Behind the scenes the tool: copies `uevr_mcp.dll` into `%APPDATA%\UnrealVRMod\<GameName>\plugins\`, launches the game with `-windowed` (or your launch args), waits for the main window, injects `UEVRBackend.dll` via `CreateRemoteThread` + `LoadLibraryA`, and polls `Process.Modules` until both modules show up.

```
uevr_setup_paths                          # verify plugin + backend DLLs are found
uevr_find_steam_game { query: "robo" }    # locate exe under any Steam library
uevr_setup_game { gameExe: "E:\\...\\MyGame-Win64-Shipping.exe" }
uevr_wait_for_plugin { timeoutMs: 30000 } # block until HTTP :8899 responds
uevr_is_ready                             # single green/red: process+backend+plugin+HTTP
```

`uevr_setup_game` is idempotent against an already-running game — it reuses the PID and just injects. `uevr_stop_game` does a graceful `WM_CLOSE` then falls back to `Kill` after `gracefulMs`, with `force=true` for immediate termination. `uevr_uevr_log` tails `%APPDATA%\UnrealVRMod\<GameName>\log.txt` (the UEVR host log, distinct from `uevr_get_log` which is the plugin's own ring buffer).

**Fragile-game stability mode.** Some games crash UEVR's stereo render hook on the first frame after injection. For those, `uevr_write_stability_config` writes a conservative `config.txt` (ExtremeCompatibilityMode, 2DScreenMode, skip PostInitProperties, alternating render method) before launch, and `uevr_suppress_d3d_monitor` suspends UEVR's worker threads after injection — this breaks the 10-second rehook retry cycle that tears down D3D mid-render. See the [Troubleshooting](#troubleshooting--game-specific-notes) section for what works on which game class.

## Dumping a UE game to a buildable project

This is the headline workflow. The MCP produces jmap-equivalent output: a full `.uproject` tree you can drop into the UE4/UE5 editor and compile with UnrealHeaderTool.

### Quick start (~30 seconds end-to-end on a Steam indie):

```
# 1. Set up + inject
uevr_setup_game      { gameExe: "E:\\SteamLibrary\\...\\MyGame-Win64-Shipping.exe" }
uevr_wait_for_plugin { timeoutMs: 30000 }

# 2. Dump
uevr_dump_usmap      { outPath: "E:\\out\\mygame.usmap" }
uevr_dump_ue_project { outDir:  "E:\\out\\MyGameMirror",
                       projectName: "MyGameMirror",
                       engineAssociation: "4.26" }

# 3. Validate
uevr_validate_usmap  { usmapPath: "E:\\out\\mygame.usmap" }
```

Output: a real UE4 project with `Source/<Module>/{Public,Private}/*.h` files like:

```cpp
// Source/Phoenix/Public/SpellData.h
USTRUCT(BlueprintType)
struct PHOENIX_API FSpellData {
    GENERATED_BODY()
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
    FName SpellName;          // +0x8

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
    AActor* Instigator;       // +0x10  ← typed, A-prefix resolved

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
    FHitResult Hit;           // +0x18  ← nested struct
};
```

### What you get

| Output | Size on a ~big AAA UE4.27 (Hogwarts Legacy) | Size on a small indie UE4.26 (Severed Steel) |
|---|---|---|
| USMAP (jmap-compatible, byte-verified) | 2.5 MB, 16407 structs + 2626 enums | 1.1 MB, 6194 structs + 1261 enums |
| UE project | 141 modules, 12460 headers | 30 modules, 1649 headers |
| Dump time | ~20 s | ~4 s |

### Available dump tools

| Tool | Output |
|---|---|
| `uevr_dump_usmap` | `.usmap` binary — FModel / CUE4Parse / UAssetAPI compatible. Version 4 (ExplicitEnumValues). Optional Brotli compression. |
| `uevr_dump_sdk_cpp` | Cast-style C++ headers with byte offsets baked into comments. For runtime workflows (trainers, VR mods, patches) that `reinterpret_cast` game-memory addresses directly. |
| `uevr_dump_uht_sdk` | UHT-style single-dir headers with `UCLASS / USTRUCT / UPROPERTY` macros and forward decls. Use as input to your own editor project. |
| `uevr_dump_ue_project` | **Full UE project** — `.uproject`, `Target.cs`, per-module `Build.cs`, `Public/*.h`, `Private/<Module>.cpp`. The jmap-equivalent. |
| `uevr_dump_reflection_json` | Raw structured JSON of every class, struct, enum with fields, offsets, property flags, tags. Feed it into your own generator. |
| `uevr_dump_bn_ida_bundle` | Reverse-engineering bundle for Binary Ninja + IDA Pro: emits a Binary Ninja-friendly `.jmap`, a parser-friendly local-type header, import scripts for both tools, and a README with usage notes. |
| `uevr_dump_usmap_selftest` | Emit a synthetic fixture USMAP covering every tag variant — no game required. Pair with `uevr_validate_usmap` for round-trip testing. |
| `uevr_dump_cache_status` / `uevr_dump_cache_clear` | Inspect / invalidate the per-session reflection-walk cache. Consecutive dump calls with the same filter share one walk. |

### When the live pipeline doesn't work: Dumper-7 fallback

A handful of games trip UEVR's `FFakeStereoRenderingHook` on first frame — the process dies before we can dump. For those, use the vendored [Dumper-7](https://github.com/Encryqed/Dumper-7) (MIT, built as `plugin/build/Release/dumper7.dll` alongside `uevr_mcp.dll`). Dumper-7 never hooks D3D — it reads reflection without touching the render pipeline, so it survives on games where UEVR can't.

```
# 1. Launch game, wait for main menu (memory stable above a threshold)
# 2. Inject Dumper-7 — it auto-dumps into C:\Dumper-7\<GameName-Version>\
uevr_dumper7_run { pid: 12345, gameName: "MyGame-Win64-Shipping" }

# 3. Convert Dumper-7's USMAP into the same UE project we'd produce live
uevr_dump_uht_from_usmap {
    usmapPath: "C:\\Dumper-7\\MyGame-4.26.2-...\\Mappings\\4.26.2-0+++UE4+Release-4.26-MyGame.usmap",
    outDir:    "E:\\out\\MyGameMirror",
    projectName: "MyGameMirror",
    moduleName:  "MyGame",
    engineAssociation: "4.26"
}
```

Output matches `uevr_dump_ue_project` in structure but omits fields USMAP doesn't carry: real property offsets (all `+0x0`), `UCLASS(Abstract, Config=X)` class-flag specifiers, `UFUNCTION` method stubs, interface inheritance lists, per-module package origin (collapses into a single flat module). Everything else — class hierarchy, `UPROPERTY` types with typed object refs, enum entries, forward decls, A/U prefixing — is the same.

### Validating a USMAP

```
uevr_validate_usmap { usmapPath: "..." }
```

Two-stage check: (a) local header parse (magic, version, compression, names-section count), (b) optional round-trip through jmap's `usmap to-json` CLI (set `$USMAP_CLI` or drop `usmap.exe` on PATH; the local `E:\Github\jmap\target\release\usmap.exe` is tried automatically). Reports struct / enum counts post-parse so you can cross-check.

### Probe diagnostics

The reflection pipeline uses runtime probes to find FProperty / UClass field offsets that UEVR's public API doesn't expose. Probes self-discover on first use via SEH-guarded memory reads validated with `UObjectHook::exists`. Check status with:

```
uevr_probe_status
```

Output lists each probe with `status` (`resolved` / `failed` / `undiscovered`), discovered `offset`, and `hits` count. On a new game, expect all 9 probes to resolve on the first heavy dump. If any stay `undiscovered`, that game's layout is outside our candidate offset range — open an issue or extend `plugin/src/reflection/property_probes.cpp`'s candidate list.

Probes covered:
- `FObjectPropertyBase::PropertyClass` (and its SoftObject / WeakObject / LazyObject / AssetObject variants)
- `FClassProperty::MetaClass`
- `FInterfaceProperty::InterfaceClass`
- `FMapProperty::KeyProp` / `ValueProp`
- `FSetProperty::ElementProp`
- `F*DelegateProperty::SignatureFunction`
- `UClass::ClassWithin` (anchor — ClassFlags is at offset-0xC, ClassConfigName at offset+0x10)
- `UClass::Interfaces` (TArray<FImplementedInterface>)

## How it works

The plugin is a C++ DLL loaded by UEVR into the game process. It uses the UEVR C++ API to access Unreal Engine's reflection system — every UObject, UClass, UStruct, UFunction, and FProperty is reachable. The plugin exposes this as a REST API:

1. **Type queries** go straight to UE's reflection system — no live instance needed
2. **Object inspection** reads field values from live objects via FProperty offsets
3. **Method invocation** calls UFunctions through `process_event` with full parameter marshaling
4. **Chain queries** walk the object graph server-side, expanding fields, calling methods, filtering, and collecting results in a single request
5. **Screenshot capture** prefers UEVR's D3D11 post-render target, falls back to present when needed, and encodes via WIC
6. **Diagnostics snapshot** bundles structured plugin logs, callback health, breadcrumbs, render metadata, loaded plugins, crash-dump info, runtime world state, and the UEVR log tail
7. **Lua execution** runs code in an embedded Sol2/Lua 5.4 state with UEVR API bindings

The MCP server is a thin C# translation layer. Each MCP tool maps to one HTTP endpoint. A named pipe provides a secondary channel for status and log operations.

## Architecture

```
  AI Agent / MCP Client
        |
        | MCP protocol (stdio)
        v
  +-----------------+
  |   MCP Server    |  mcp-server/     .NET console app
  |  (stdio bridge) |  Translates MCP tool calls -> HTTP
  +-----------------+
        |
        | HTTP (localhost:8899)
        v
  +-----------------+
  |  Game Plugin    |  plugin/          C++ DLL (UEVR plugin)
  |  (HTTP server)  |  Runs inside the game process
  +-----------------+
        |
        | UEVR C++ API (UObject reflection, D3D11, XInput)
        v
  +-----------------+
  |  Unreal Engine  |  The actual game
  |  Game Process   |
  +-----------------+
```

**`plugin/`** — A C++ DLL loaded by UEVR inside the game. Embeds Lua 5.4 + Sol2, cpp-httplib, nlohmann/json. Starts an HTTP server on `localhost:8899` exposing the gameplay API plus a diagnostics surface for structured logs, breadcrumbs, callback counters, plugin inventory, render metadata, and crash snapshots. Hooks into the engine tick, D3D present, D3D11 post-render, and XInput callbacks. HTTP handler threads submit work to the game thread via a `GameThreadQueue` (std::promise/future, up to 16 items per tick, 5s timeout) to safely access UE internals. A named pipe (`\\.\pipe\UEVR_MCP`) provides a secondary channel for status and log operations that work even before the HTTP server is ready.

**`mcp-server/`** — A standalone .NET console app that speaks MCP over stdio. Translates tool calls into HTTP requests, falling back to the named pipe for status/log/game-info when HTTP is unavailable. Diagnostics tools map directly to the HTTP snapshot and per-surface diagnostics routes.

## 168 MCP Tools

### Setup & Lifecycle (8 tools)

| Tool | Description |
|------|-------------|
| `uevr_setup_paths` | Report the plugin DLL / backend DLL paths the setup flow will use (overridable via `$UEVR_MCP_PLUGIN_DLL` / `$UEVR_BACKEND_DLL`) |
| `uevr_find_steam_game` | Scan every Steam library's `steamapps/common/*/` for UE `*-Shipping.exe` — returns `{gameName, exe, libraryPath}` tuples |
| `uevr_install_plugin` | Copy `uevr_mcp.dll` to `%APPDATA%\UnrealVRMod\<GameName>\plugins\` (idempotent) |
| `uevr_setup_game` | One-call install + launch + inject backend + verify. Returns per-step results; reuses an already-running process if present |
| `uevr_wait_for_plugin` | Block until the plugin's HTTP endpoint responds; polls every ~300ms, bails if the game process dies |
| `uevr_is_ready` | Single-call health check: process alive, `UEVRBackend.dll` + `uevr_mcp.dll` loaded, HTTP responding |
| `uevr_stop_game` | Graceful `WM_CLOSE` then `Kill` fallback after `gracefulMs`; `force=true` skips the graceful step |
| `uevr_uevr_log` | Tail `%APPDATA%\UnrealVRMod\<GameName>\log.txt` (UEVR host log — distinct from `uevr_get_log` which is our plugin's own ring buffer) |

### Reflection Dumping (11 tools)

| Tool | Description |
|------|-------------|
| `uevr_dump_usmap` | Full `.usmap` binary (v4 / ExplicitEnumValues) with jmap-matching tag recursion; optional Brotli compression |
| `uevr_dump_usmap_selftest` | Synthesize a USMAP from a built-in fixture covering every tag variant — no game required |
| `uevr_dump_sdk_cpp` | Cast-style C++ headers with byte offsets in comments (for trainers / VR mods / raw-cast workflows) |
| `uevr_dump_uht_sdk` | UHT-style single-dir headers with `UCLASS / USTRUCT / UPROPERTY` macros, forward decls, decoded CPF/CLASS flags |
| `uevr_dump_ue_project` | Buildable UE project — `.uproject` + per-module `Source/<Module>/{Public,Private}/*.h` tree |
| `uevr_dump_uht_from_usmap` | Convert any USMAP (ours, Dumper-7's, jmap's) into the same UE project — fallback for UEVR-incompatible games |
| `uevr_dump_reflection_json` | Raw JSON of every class/struct/enum + fields + offsets + flags |
| `uevr_dump_cache_status` / `uevr_dump_cache_clear` | Inspect / clear the per-session reflection-walk cache |
| `uevr_probe_status` / `uevr_probe_reset` | Show which FProperty / UClass raw-offset probes have resolved, at which offsets; clear them for re-discovery |

### USMAP Validation & Conversion (1 tool)

| Tool | Description |
|------|-------------|
| `uevr_validate_usmap` | Local header parse + optional jmap `usmap to-json` round-trip with struct/enum counts |

### Dumper-7 Integration (5 tools)

| Tool | Description |
|------|-------------|
| `uevr_dumper7_inject` | Inject our vendored `dumper7.dll` into a running UE process (no D3D hooking — survives games UEVR crashes on) |
| `uevr_dumper7_run` | One-shot inject + wait for SDK output to finalize + return the SDK folder layout |
| `uevr_dumper7_status` | Report DLL path resolution, output roots, latest SDK folder, whether Dumper-7 is loaded in a target |
| `uevr_dumper7_list` | List files in a produced SDK folder (fuzzy pattern match, paginated) |
| `uevr_dumper7_read` | Read a single generated header by path or fuzzy package-name query with byte offset / length for paging |

### Memory Write (2 tools)

| Tool | Description |
|------|-------------|
| `uevr_write_memory` | Write raw bytes (hex string or int array) to any game-process address; SEH-guarded with VirtualProtect toggling for code patches |
| `uevr_patch_bytes` | AoB-scan + write in one call; aborts on multi-match unless `force=true` |

### External Tool Wrappers (3 tools)

| Tool | Description |
|------|-------------|
| `uevr_uesave` | Shell to [uesave](https://github.com/trumank/uesave-rs) for GVAS save-file `to-json` / `from-json` roundtrips |
| `uevr_patternsleuth` | Shell to [patternsleuth](https://github.com/trumank/patternsleuth) for UE symbol / AoB signature resolution |
| `uevr_validate_usmap` | (listed above — shells to jmap's `usmap` CLI) |

### Stability Mode for Fragile Games (2 tools)

| Tool | Description |
|------|-------------|
| `uevr_write_stability_config` | Write a conservative UEVR `config.txt` (ExtremeCompatibilityMode, 2DScreenMode, SkipPostInitProperties, etc.) to `%APPDATA%\UnrealVRMod\<GameName>\` before launch |
| `uevr_suppress_d3d_monitor` | Enumerate threads in the target, suspend the UEVR worker threads that drive the 10-second rehook retry cycle (the classic "Last chance encountered for hooking" death loop) |

### Object Exploration (13 tools)

| Tool | Description |
|------|-------------|
| `uevr_is_valid` | Check if a UObject pointer is still alive — returns class/name if valid |
| `uevr_search_objects` | Search GUObjectArray by name substring |
| `uevr_search_classes` | Search UClass objects by name |
| `uevr_get_type` | Full type schema — all fields with types/offsets, all methods with signatures |
| `uevr_inspect_object` | Full inspection of a live object — all field values and methods |
| `uevr_summary` | Lightweight one-line-per-field overview (start here) |
| `uevr_read_field` | Read a single field, follows object references automatically |
| `uevr_call_method` | Call a 0-parameter getter method |
| `uevr_objects_by_class` | Find all live instances of a class |
| `uevr_get_singletons` | List common singletons (GameEngine, World, GameMode, etc.) |
| `uevr_get_singleton` | Find first live instance of a type |
| `uevr_get_array` | Paginated array property reading |
| `uevr_chain` | Multi-step object graph traversal (field → method → array → filter → collect) |

### Mutation (4 tools)

| Tool | Description |
|------|-------------|
| `uevr_write_field` | Write any field (bool, int, float, string, struct, enum, object ref) |
| `uevr_invoke_method` | Call any UFunction with arguments |
| `uevr_batch` | Multiple operations in one game-thread tick |
| `uevr_exec_command` | Execute console commands (stat fps, show collision, etc.) |

### Memory (2 tools)

| Tool | Description |
|------|-------------|
| `uevr_read_memory` | Raw hex dump with ASCII sidebar (max 8192 bytes) |
| `uevr_read_typed` | Read typed values sequentially (u8–u64, i8–i64, f32, f64, ptr) |

### Player & Camera (5 tools)

| Tool | Description |
|------|-------------|
| `uevr_get_player` | Player controller + pawn addresses and classes |
| `uevr_set_position` | Teleport player (partial updates — omit axes to keep current) |
| `uevr_set_health` | Set health (searches common field names on pawn + components) |
| `uevr_get_camera` | Camera position, rotation, FOV |
| `uevr_get_game_info` | Game exe path, VR runtime, uptime |

### Console Variables (3 tools)

| Tool | Description |
|------|-------------|
| `uevr_list_cvars` | List console variables with optional filter |
| `uevr_get_cvar` | Read a CVar's value |
| `uevr_set_cvar` | Set a CVar's value |

### VR Controls (11 tools)

| Tool | Description |
|------|-------------|
| `uevr_vr_status` | VR runtime (OpenVR/OpenXR), HMD active, resolution, controllers |
| `uevr_vr_poses` | Live HMD + controller grip/aim poses and standing origin |
| `uevr_vr_settings` | Current snap turn, aim method, decoupled pitch settings |
| `uevr_set_vr_setting` | Change any VR setting or arbitrary mod value |
| `uevr_vr_input` | Controller input state: joystick axes, movement orientation, OpenXR action queries |
| `uevr_get_world_scale` | Get WorldToMetersScale from UWorld (default 100) |
| `uevr_set_world_scale` | Set WorldToMetersScale — lower = player feels larger, higher = smaller. Some games reset each tick; use a looping timer to persist. |
| `uevr_recenter` | Recenter the VR view |
| `uevr_haptics` | Trigger controller vibration |
| `uevr_save_config` | Save UEVR config to disk |
| `uevr_reload_config` | Reload UEVR config from disk |

### Motion Controller Attachment (4 tools)

| Tool | Description |
|------|-------------|
| `uevr_attach_to_controller` | Attach a USceneComponent to a VR controller (left/right/HMD) with position/rotation offsets. Must be a component address, not an actor — use `uevr_world_components` to find them. |
| `uevr_detach_from_controller` | Detach a component from its motion controller |
| `uevr_list_motion_controllers` | List all current motion controller attachments |
| `uevr_clear_motion_controllers` | Remove all motion controller attachments |

### Timer / Scheduler (4 tools)

| Tool | Description |
|------|-------------|
| `uevr_timer_create` | Create a one-shot or looping timer that executes Lua code after a delay |
| `uevr_timer_list` | List active timers with IDs, delays, remaining time, looping status |
| `uevr_timer_cancel` | Cancel a timer by ID |
| `uevr_timer_clear` | Cancel all active timers |

### Lua Scripting (9 tools)

| Tool | Description |
|------|-------------|
| `uevr_lua_exec` | Execute Lua with full UEVR API access. Persistent state, frame callbacks, timers, async coroutines, module system. |
| `uevr_lua_reset` | Destroy and recreate the Lua state |
| `uevr_lua_state` | Diagnostics: exec count, callback count, timer count, coroutine count, memory |
| `uevr_lua_reload` | Hot-reload a script file without losing state (preserves globals, callbacks, timers) |
| `uevr_lua_globals` | Inspect top-level Lua global variables — names, types, values |
| `uevr_lua_write_script` | Write a .lua file to UEVR scripts dir (with autorun option) |
| `uevr_lua_list_scripts` | List script files |
| `uevr_lua_read_script` | Read script content |
| `uevr_lua_delete_script` | Delete a script file |

### Blueprint / Object Lifecycle (7 tools)

| Tool | Description |
|------|-------------|
| `uevr_spawn_object` | Spawn a new UObject or Actor by class name |
| `uevr_add_component` | Add a component to an actor |
| `uevr_get_cdo` | Get Class Default Object — default field values for a class |
| `uevr_write_cdo` | Write a CDO field — affects all future spawns |
| `uevr_destroy_object` | Destroy an actor |
| `uevr_set_transform` | Set actor location, rotation, scale (partial updates OK) |
| `uevr_list_spawned` | List all MCP-spawned objects with alive/dead status |

### Screenshot (2 tools)

| Tool | Description |
|------|-------------|
| `uevr_screenshot` | Capture from D3D11/D3D12 backbuffer as JPEG. Works when game isn't in front. Handles R10G10B10A2, FP16, BGRA8, RGBA8. Configurable resolution and quality. |
| `uevr_screenshot_info` | Check if D3D capture is initialized and which renderer is in use |

### Diagnostics (4 tools)

| Tool | Description |
|------|-------------|
| `uevr_get_diagnostics` | One-shot crash/debug snapshot: callback health, breadcrumb, plugin log, render metadata, loaded plugins, latest crash dump, runtime map/controller/pawn, UEVR log tail |
| `uevr_get_callback_health` | Per-callback invocation/success/failure counters and last error info |
| `uevr_get_breadcrumb` | Read the persisted breadcrumb file/state written around risky callback boundaries |
| `uevr_get_loaded_plugins` | Inventory UEVR plugin DLLs in global and game-specific plugin folders and show which are currently loaded |

### Property Watch & Snapshot/Diff (9 tools)

| Tool | Description |
|------|-------------|
| `uevr_watch_add` | Watch a field for changes — checks every N ticks, records deltas |
| `uevr_watch_remove` | Remove a watch |
| `uevr_watch_list` | List all watches with current/previous values and change counts |
| `uevr_watch_changes` | Get recent change events across all watches |
| `uevr_watch_clear` | Clear all watches |
| `uevr_snapshot` | Snapshot all field values on an object |
| `uevr_snapshot_list` | List saved snapshots |
| `uevr_diff` | Diff a snapshot against current state — see exactly what changed |
| `uevr_snapshot_delete` | Delete a snapshot |

### World & Spatial Queries (5 tools)

| Tool | Description |
|------|-------------|
| `uevr_world_actors` | List actors in the world with optional class filter |
| `uevr_world_components` | Get all components attached to an actor |
| `uevr_line_trace` | Raycast — returns hit location, normal, distance, actor, component |
| `uevr_sphere_overlap` | Find all actors within a sphere radius |
| `uevr_hierarchy` | Get outer chain, owner, attachment parent/children, class super chain |

### Input Injection (4 tools)

| Tool | Description |
|------|-------------|
| `uevr_input_key` | Simulate keyboard key press/release/tap |
| `uevr_input_mouse` | Simulate mouse button clicks or movement |
| `uevr_input_gamepad` | Override XInput gamepad state (buttons, sticks, triggers) |
| `uevr_input_text` | Type a string of text character by character |

### Material Control (5 tools)

| Tool | Description |
|------|-------------|
| `uevr_material_create_dynamic` | Create a dynamic material instance for parameter modification |
| `uevr_material_set_scalar` | Set scalar parameter (EmissiveIntensity, Roughness, Opacity, etc.) |
| `uevr_material_set_vector` | Set vector/color parameter (BaseColor, EmissiveColor, etc.) |
| `uevr_material_params` | List scalar, vector, and texture parameters on a material |
| `uevr_material_set_on_actor` | Apply a material to an actor's mesh at a given slot |

### Animation Control (5 tools)

| Tool | Description |
|------|-------------|
| `uevr_animation_play_montage` | Play an animation montage on a skeletal mesh |
| `uevr_animation_stop_montage` | Stop a playing montage with blend-out |
| `uevr_animation_state` | Get current animation state, playing montage, anim variables |
| `uevr_animation_set_variable` | Set an animation variable (float, bool, int) on the AnimInstance |
| `uevr_animation_montages` | List available animation montages loaded in memory |

### Physics (6 tools)

| Tool | Description |
|------|-------------|
| `uevr_physics_add_impulse` | Add instant impulse to a component (knockback, launching) |
| `uevr_physics_add_force` | Add continuous force for one frame |
| `uevr_physics_set_simulate` | Enable/disable physics simulation |
| `uevr_physics_set_gravity` | Enable/disable gravity |
| `uevr_physics_set_collision` | Enable/disable collision |
| `uevr_physics_set_mass` | Set mass override |

### Asset Discovery (5 tools)

| Tool | Description |
|------|-------------|
| `uevr_asset_find` | Find a loaded asset by path |
| `uevr_asset_search` | Search loaded assets by name, optionally filtered by type |
| `uevr_asset_load` | Attempt to load or find an asset |
| `uevr_asset_classes` | List loaded asset types/classes |
| `uevr_asset_load_class` | Find or load a UClass by name |

### Deep Discovery (6 tools)

| Tool | Description |
|------|-------------|
| `uevr_subclasses` | Find all subclasses of a base class — discover game-specific types (enemies, items, etc.) |
| `uevr_search_names` | Search all reflected names (classes, properties, functions) — finds types without live instances |
| `uevr_delegates` | Inspect delegate properties and event functions on an object (OnTakeDamage, OnDeath, etc.) |
| `uevr_vtable` | Compare virtual function table against parent class — find overridden C++ functions |
| `uevr_pattern_scan` | Signature scan executable memory for byte patterns with wildcards |
| `uevr_all_children` | Brute-force enumerate all properties and functions across full inheritance chain |

### Function Hooks (5 tools)

| Tool | Description |
|------|-------------|
| `uevr_hook_add` | Hook any UFunction — log, block, or run Lua callbacks. Lua hooks receive context (object, function, args) and can conditionally block. |
| `uevr_hook_remove` | Remove a hook |
| `uevr_hook_list` | List all hooks with call counts |
| `uevr_hook_log` | Get the call log for a hook — who called what and when |
| `uevr_hook_clear` | Clear all hooks |

### ProcessEvent Listener (9 tools)

| Tool | Description |
|------|-------------|
| `uevr_process_event_start` | Start the global ProcessEvent listener — hooks UObject::ProcessEvent to capture all function calls in real time |
| `uevr_process_event_stop` | Stop the listener (hook stays installed for cheap restart) |
| `uevr_process_event_status` | Listener state: whether active, hook installed, unique functions seen, ignore list size |
| `uevr_process_event_functions` | All tracked functions sorted by call count, with search/filter/limit/sort options |
| `uevr_process_event_recent` | Most recent function calls (newest first) — the live stream |
| `uevr_process_event_ignore` | Ignore functions matching a name pattern (substring) |
| `uevr_process_event_ignore_all` | Ignore all currently seen functions — establish a clean baseline |
| `uevr_process_event_clear` | Clear all tracked data (does not affect ignore list) |
| `uevr_process_event_clear_ignored` | Clear the ignore list so all functions are tracked again |

### Macro System (5 tools)

| Tool | Description |
|------|-------------|
| `uevr_macro_save` | Save a named operation sequence with $param placeholders. Persists to disk. |
| `uevr_macro_play` | Execute a macro with parameter substitution and state propagation ($result[N].field references) |
| `uevr_macro_list` | List saved macros |
| `uevr_macro_delete` | Delete a macro |
| `uevr_macro_get` | Get a macro's full definition |

### Event Streaming (1 tool)

| Tool | Description |
|------|-------------|
| `uevr_events_poll` | Long-poll for real-time events (hook fires, watch changes, Lua output) |

### System (4 tools)

| Tool | Description |
|------|-------------|
| `uevr_get_status` | Plugin health via named pipe (works even if HTTP is down) |
| `uevr_get_log` | Recent log entries from ring buffer |
| `uevr_clear_log` | Clear the log |
| `uevr_help` | Agent navigation guide |

## Usage examples

Once connected, you can ask your agent things like:

- *"What's my current health and position?"*
- *"Find the type that manages enemies and show me its fields."*
- *"Set my health to max."*
- *"Take a screenshot and tell me what you see."*

But the real power is in open-ended requests. You don't need to know the game's internals — the agent will figure it out:

- *"Reverse-engineer the damage formula. Snapshot my health, take damage, diff to see what changed, then hook the damage function to log every call."*
- *"Make the player glow red. Find the mesh, get its material, create a dynamic instance, crank up the emissive color."*
- *"Figure out which animation plays when I dodge, then write a Lua script that plays it on a timer for testing."*
- *"Find every enemy within 10 meters of me, read their health, and set them all to 1 HP."*
- *"Attach the sword to my right VR hand with a comfortable grip angle."*
- *"Use the ProcessEvent listener to figure out what happens when I open a chest, then hook those functions."*
- *"Make the world feel twice as big — adjust the VR scale."*
- *"Dump this game as a buildable UE4 project — I want to read the source in my editor."*

### End-to-end: dump a game you've never touched before

The shortest path from "I installed the game" to "I have a UE4 editor project on disk":

```
# 1. Locate it
uevr_find_steam_game { query: "Robo" }
→ [{gameName: "RoboQuest", exe: "E:\\SteamLibrary\\steamapps\\common\\RoboQuest\\RoboQuest\\Binaries\\Win64\\RoboQuest-Win64-Shipping.exe", libraryPath: "E:\\SteamLibrary"}]

# 2. Launch + inject (or attach to a running instance)
uevr_setup_game       { gameExe: "<exe from step 1>" }
uevr_wait_for_plugin  { timeoutMs: 30000 }
uevr_is_ready
→ { ready: true, backendLoaded: true, pluginLoaded: true, httpAlive: true }

# 3. Dump
uevr_dump_usmap       { outPath: "E:\\out\\roboquest.usmap" }
uevr_dump_ue_project  { outDir:  "E:\\out\\RoboQuestMirror",
                        projectName: "RoboQuestMirror",
                        engineAssociation: "4.26" }

# 4. Verify
uevr_validate_usmap   { usmapPath: "E:\\out\\roboquest.usmap" }
→ { headerOk: true, fullParse: "ok", parsedStructCount: 5620, parsedEnumCount: 1314 }

uevr_probe_status
→ 9/9 probes resolved

# 5. Cleanup
uevr_stop_game
```

The output at `E:\out\RoboQuestMirror\` is a ready-to-open UE4 project. Open it in the UE editor, build, and you have IntelliSense / go-to-definition / decompile-Blueprints against the game's real types.

### When UEVR crashes on a specific game (Dumper-7 fallback)

```
# Launch game, wait for main menu (memory stabilizes above a threshold)
# Don't call uevr_setup_game — it would crash the game.

# Inject Dumper-7 instead
uevr_dumper7_run { pid: <running-game-pid>, gameName: "MyGame-Win64-Shipping", timeoutMs: 180000 }
→ { sdk: { Dir: "C:\\Dumper-7\\MyGame-...", UsmapCount: 1, HppCount: 910 } }

# Convert its USMAP into our UE project format
uevr_dump_uht_from_usmap {
    usmapPath: "C:\\Dumper-7\\MyGame-...\\Mappings\\<version>-MyGame.usmap",
    outDir:    "E:\\out\\MyGameMirror",
    projectName: "MyGameMirror",
    moduleName:  "MyGame"
}
```

### Reverse engineering workflow: snapshot → action → diff

The snapshot/diff system makes reverse engineering fast. Instead of manually comparing field values:

```
1. uevr_snapshot("0x...")          → capture all 200+ fields
2. (do something in-game)
3. uevr_diff(snapshotId)           → see exactly which fields changed

Result: "MaxWalkSpeed: 600.0 → 300.0, bIsCrouched: false → true"
```

Combine with function hooks for complete visibility:

```
1. uevr_hook_add("Actor", "ReceiveDamage", "log")   → start logging
2. (take damage in-game)
3. uevr_hook_log(hookId)                              → see every call with caller info
```

### ProcessEvent listener: discover what the game calls

The ProcessEvent listener captures every Blueprint/native function call in real time — equivalent to UEVR's Developer tab:

```
1. uevr_process_event_start                         → start listening
2. uevr_process_event_functions                      → see all functions sorted by call count
3. uevr_process_event_ignore("Tick")                 → filter out noisy per-frame functions
4. uevr_process_event_ignore_all                     → ignore everything seen so far
5. uevr_process_event_clear                          → reset, then do an action in-game
6. uevr_process_event_functions(search: "Damage")    → see only new functions that fired
7. uevr_process_event_recent                         → live stream of the most recent calls
```

This is the fastest way to find game-specific functions. Ignore the noise, perform an action, and see exactly which functions fire.

### Live Lua scripting

The Lua engine persists between calls and supports frame callbacks and timers:

```lua
-- Attach sword to VR right hand
local pawn = uevr.api:get_local_pawn(0)
local mesh = pawn:get_property("Mesh")
local state = uevr.uobject_hook:get_or_add_motion_controller_state(mesh)
state:set_hand(1)  -- right hand
state:set_permanent(true)

-- Run code every frame
mcp.on_frame(function(dt)
    local pos = uevr.api:get_local_pawn(0):get_property("Location")
    if pos.z < -1000 then
        mcp.log("Player fell below kill plane!")
    end
end)

-- Delayed execution
mcp.set_timer(5.0, function()
    mcp.log("5 seconds elapsed!")
end, false)  -- false = one-shot

-- Repeating timer
mcp.set_timer(1.0, function()
    mcp.log("Tick!")
end, true)  -- true = repeating

-- Async coroutines with mcp.wait()
mcp.async(function()
    mcp.log("Starting patrol...")
    mcp.wait(2.0)  -- resume after 2 seconds
    mcp.log("Moving to waypoint...")
    mcp.wait_until(function() return get_health() < 50 end)
    mcp.log("Health dropped below 50!")
end)

-- Module system (require)
local utils = require("my_utils")  -- loads scripts/my_utils.lua
utils.do_something()
```

### Spatial reasoning

Line traces and overlap tests let the agent understand 3D space:

```
uevr_line_trace(
  start={x:0, y:0, z:100},
  end={x:1000, y:0, z:100}
)
→ {hit: true, location: {x:342, y:0, z:100}, actor: "0x...", distance: 342.0}

uevr_sphere_overlap(
  center={x:0, y:0, z:0},
  radius=500
)
→ {actors: [{address: "0x...", class: "Enemy", name: "Goblin_3"}, ...], count: 4}
```

### Macros for reusable operations

Save common operation sequences as parameterized macros:

```
uevr_macro_save("kill_actor", [
  {"type": "write_field", "address": "$target", "fieldName": "Health", "value": 0}
])

uevr_macro_play("kill_actor", {target: "0x12345678"})
```

## HTTP API

All endpoints are accessible directly via HTTP at `http://localhost:8899/api`. Call `GET /api` for the full endpoint index. The MCP server is a thin wrapper — you can also use curl, a browser, or any HTTP client.

The web dashboard (if present in a `web/` folder next to the DLL) is served at `http://localhost:8899/`.

## Testing

### Unit tests (no game required)

The C# unit tests verify tool registration, parameter signatures, and HTTP contract correctness via reflection — no running game needed.

```bash
cd tests/UevrMcpTests
dotnet test
```

### Integration tests (live game required)

The Python integration tests hit the plugin's HTTP API against a running game. Install dependencies and run with pytest:

```bash
pip install pytest requests
pytest tests/integration/ -v
```

The tests use `http://localhost:8899` by default. Set `UEVR_MCP_API_URL` to override. Tests that require a game connection are marked with the `require_game` fixture and will skip if the plugin isn't reachable.

## Supported games

Any Unreal Engine game that UEVR supports. The core reflection tools work universally — they inspect whatever the game has loaded. Per-game differences are just different class names and field layouts, which the agent discovers through exploration.

Games tested with:
- Unreal Engine 4.x titles (float vectors, single-precision)
- Unreal Engine 5.x titles (double vectors, Nanite, Lumen)

### Game-by-game dump pipeline

| Game | UE ver | Protection | Renderer | Pipeline | Dump result |
|---|---|---|---|---|---|
| RoboQuest | 4.26 | — | DX11 | live `uevr_dump_*` | 5620 structs + 1314 enums, 15 modules, 1927 headers |
| Severed Steel | 4.26 | — | DX11 | live + stability mode | 6194 structs + 1261 enums, 30 modules, 1649 headers |
| Hogwarts Legacy | 4.27 | Denuvo | DX12 | live `uevr_dump_*` | **16407 structs + 2626 enums, 141 modules, 12460 headers** |
| Stellar Blade | 4.26.2 | Denuvo | DX12 | Dumper-7 + `uht_from_usmap` | 6946 structs + 1837 enums, 8783 headers |

Denuvo / DX12 / AAA-size by themselves are **not** blockers for the live pipeline — Hogwarts Legacy validates that combo cleanly. The Stellar Blade fallback is specific to that title's custom UE4.26.2 fork, which crashes UEVR's `FViewport::GetRenderTargetTexture` PointerHook on the first rendered frame (see [Troubleshooting](#troubleshooting--game-specific-notes) for the signature).

## Troubleshooting / game-specific notes

### Game crashes ~7s after `uevr_setup_game`

Check `%APPDATA%\UnrealVRMod\<GameName>\log.txt` for the last line. Two common signatures:

**Signature A: UEVR stereo render hook crash** — last lines are:
```
FFakeStereoRenderingHook.cpp:2248 Hooking FViewport::GetRenderTargetTexture
FFakeStereoRenderingHook.cpp:2260 UGameViewportClient::Draw called for the first time.
FFakeStereoRenderingHook.cpp:2050 FViewport::GetRenderTargetTexture called!
```
…followed by process exit. Something in this specific game's render pipeline is incompatible with UEVR's PointerHook. Known on Stellar Blade. Config-level `uevr_write_stability_config` does not help. Workaround: use the Dumper-7 fallback path above.

**Signature B: "Last chance encountered for hooking" retry-loop crash** — last lines cycle:
```
Last chance encountered for hooking
Sending rehook request for D3D
Hooking D3D12
```
…for a few iterations then die. UEVR's D3D monitor thread is re-hooking every 10s and racing with the game's live render. Call `uevr_suppress_d3d_monitor` immediately after `uevr_setup_game` to freeze the monitor — works on most games that hit this.

### "Process not running" error from `uevr_setup_game`

On Steam games with a launcher-stub pattern (bootstrap `<Game>.exe` at the root spawns the real game at `<Subfolder>/Binaries/Win64/<Game>.exe`), both processes share an image name. The setup tool picks the exact `MainModule.FileName` match first, but if you see this error ensure you're passing the **inner** exe path, not the root launcher.

### "Plugin DLL is locked by another process"

A previous setup injected `uevr_mcp.dll` into a now-crashed process and Windows is holding the file handle. `taskkill /F /PID <pid>` the stuck process and retry.

### `uevr_dump_reflection_json` times out / returns transport error

Two scenarios:
1. **First heavy dump** — initial probe discovery adds per-batch overhead. The reflection walk's first batch can take several seconds. Subsequent batches in the same session hit cached probe offsets. If you see `Game thread request timed out`, bump the plugin's per-batch timeout in `plugin/src/routes/explorer_routes.cpp` (default 20s with methods, 10s without).
2. **`includeMethods=true`** — method enumeration per class is ~10x more work than fields alone. `uevr_dump_sdk_cpp` and `uevr_dump_usmap` both default to `methods=false` (methods emit as comments only in the SDK and USMAP doesn't carry them). Only pass `includeMethods=true` if you explicitly need them.

### Probes stay `undiscovered`

Game's UE build puts the target field outside our candidate offset range. Extend `kUClassRange` / `kShortRange` / `kFieldRange` / `kUClassInterfacesRange` in `plugin/src/reflection/property_probes.cpp` — each is a small `int32_t[]` of candidate offsets to try. Rebuild, re-inject, re-probe.

### USMAP parses with our validator but FModel rejects it

FModel's parser is sometimes stricter about compression. Re-dump with `compression: "none"` (the default). If you passed `compression: "brotli"`, FModel may not support brotli in your build — the jmap-compatible path never compresses.

## Further improvements worth doing

Ordered by impact:

1. **`uevr_dump_project(gameExe)` — one-button smart dump.** Auto-detects: game running? launch. UEVR survives injection? live pipeline (richer output). UEVR crashes? Dumper-7 fallback. Single entry point for agents that just want "the project". Writes a `<GameName>.json` profile file with the decision so reruns are instant.

2. **Phase 4C: CDO default values.** Walk `UClass::ClassDefaultObject` (exposed by UEVR API) and emit `= 0.5f` / `= FVector(0,0,1)` initializers in UHT headers. Meaningful for mod dev — lets you see default balance constants at a glance. ~1 day.

3. **Dumper-7 → offset backfill for the USMAP-only path.** Dumper-7's `GObjects-Dump-WithProperties.txt` contains real offsets (same format as `[0x35A0] {addr} ObjectProperty AIOwner`). A small parser could backfill into our `uevr_dump_uht_from_usmap` output, closing the biggest quality gap on the fallback path. Gets Stellar Blade to offset-accurate headers without touching UEVR. ~4 hours.

4. **Game profile persistence.** Remember per-game settings in `~/.uevr-mcp/state.json` — last game exe, whether stability mode helped, preferred module filter, UE version. Second dump of the same game becomes a one-liner.

5. **UE5 test coverage.** All current probes and validation was done on UE4.22–4.27 titles. Should verify on a UE5.x game (Robocop Rogue City, Satisfactory, Senua's Saga) — the `UClass::ClassWithin` anchor may sit at a different offset.

6. **Kismet / Blueprint bytecode decompilation.** We access `UFunction::Script` but don't decompile. Integrating [KismetKompiler](https://github.com/TGE-TJ/KismetKompiler) or [Kismet Analyzer](https://github.com/trumank/kismet-analyzer) could produce real method bodies in the UHT output instead of stubs.

7. **Single-header drop-in SDK.** Alongside the multi-file `uevr_dump_uht_sdk` output, emit one `sdk.hpp` that concatenates everything. Matches what [Dumper-7](https://github.com/Encryqed/Dumper-7) ships. Useful for quick drop-in C++ mods that don't want to build a full editor project.

8. **MCP Prompts for common workflows.** Define templates like "Bring up VR for Game X", "Find the player's health field", "Snapshot → do action → diff" that agents can invoke by name.

9. **Runtime-patch UEVR's FFakeStereoRenderingHook** so the live pipeline works on Stellar Blade and similar. Find the hook-installation function via AoB scan of the "is stereo enabled called!" / "UGameViewportClient::Draw called for the first time." string xrefs, NOP the stereo-hook installation. Nontrivial but would eliminate the last known gap vs jmap.

10. **Plugin hot-reload.** Currently every plugin rebuild requires killing the game. A proxy-DLL approach (thin `uevr_mcp.dll` that `LoadLibrary`s the real impl and supports re-loading) would speed plugin dev dramatically.

## License

MIT
