# Dumper-mode recipe: from zero to a full game dump

This is the non-MCP path — bash/PowerShell only, no AI agent required.
Target audience: someone who just wants the `.usmap`, a buildable UE
project, and maybe Binary Ninja types for a game they have on disk.

## Prerequisites (one-time)

- **Windows 10/11 x64**
- **.NET 9 SDK** — [download](https://dotnet.microsoft.com/download/dotnet/9.0)
- **CMake 3.21+** — via Visual Studio 2022 installer (the "Desktop development with C++" workload pulls in CMake + MSBuild) or standalone
- **Visual Studio 2022** with the C++ workload — needed for MSBuild / dumpbin / the MSVC toolchain CMake drives for the plugin
- **The uevr-mcp repo cloned locally**
- A local **UEVR build** at `E:\Github\UEVR\build\bin\uevr\UEVRBackend.dll` (or set `$UEVR_BACKEND_DLL`). The public UEVR release is usually too old for uevr_mcp — build from source:
  ```bat
  git clone https://github.com/praydog/UEVR E:\Github\UEVR
  cd E:\Github\UEVR
  cmake -S . -B build -G "Visual Studio 17 2022" -A x64
  cmake --build build --config Release --target uevr
  ```
  (If you want the dumper-mode patches, apply the branch that adds
  `src/DumperMode.hpp` + gated calls — those aren't upstream yet.)

## Step 1. Build the tools

```powershell
cd <path-to-uevr-mcp>
.\tools\setup.ps1
```

That does:
1. Check .NET + CMake
2. `cmake --build plugin/build --config Release --target uevr_mcp` → produces `plugin\build\Release\uevr_mcp.dll`
3. `dotnet build mcp-server -c Release` → produces `mcp-server\bin\Release\net9.0\UevrMcpServer.exe`

~2 minutes cold, ~10 seconds on rebuilds.

## Step 2. Pick a game

Any Unreal Engine game that UEVR supports. For the rest of this guide we'll
use `RoboQuest`, exe at:
```
E:\SteamLibrary\steamapps\common\RoboQuest\RoboQuest\Binaries\Win64\RoboQuest-Win64-Shipping.exe
```

## Step 3. Enable dumper mode (one command)

```powershell
.\tools\enable-dumper-mode.ps1 `
  -GameExe "E:\SteamLibrary\steamapps\common\RoboQuest\RoboQuest\Binaries\Win64\RoboQuest-Win64-Shipping.exe"
```

This writes a zero-byte sentinel file at
`%APPDATA%\UnrealVRMod\RoboQuest-Win64-Shipping\dumper_mode`.

When UEVR next injects into that game, `DumperMode.hpp` sees the sentinel
and flips 6 gates:
- Skip D3D11 / D3D12 hooks
- Skip `FFakeStereoRenderingHook::attempt_hooking` (the stereo vtable hook — the render-thread crasher on RoboQuest)
- Skip `attempt_hook_slate_thread` and `attempt_hook_fsceneview_constructor`
- Skip the "re-hook D3D if not presenting" monitor thread logic
- Skip the VR mod entirely (no OpenVR / OpenXR init)
- Skip `UObjectHook`'s inline AddObject + destructor trampolines

What's kept:
- `UGameEngine::Tick` hook (so plugins get `on_pre_engine_tick` callbacks)
- Plugin loader (so `uevr_mcp.dll` loads)
- `UObjectHook`'s existing-object snapshot (25 000+ UObjects indexed at init, enough for all reflection)
- The crash handler (now installed eagerly, so any crash produces a `crash.dmp`)

## Step 4. Launch the game

Launch RoboQuest via Steam / Epic / shortcut. Get to the main menu.
**Don't** let the uevr-mcp launcher do it — for games with a launcher-stub
(Steam's `HogwartsLegacy.exe` at project root that spawns the real game in
a subfolder and exits), the launcher tracking in `setup-game` sees the
stub PID exit and reports "Game process exited." Launching the game
yourself sidesteps that.

## Step 5. Run the full dump

```powershell
.\tools\quick-dump.ps1 `
  -GameExe "E:\SteamLibrary\steamapps\common\RoboQuest\RoboQuest\Binaries\Win64\RoboQuest-Win64-Shipping.exe" `
  -OutDir C:\dumps\RoboQuest
```

That script:
1. Attaches to the running game (CreateRemoteThread + LoadLibraryA injects `UEVRBackend.dll`, which loads `uevr_mcp.dll`)
2. Waits for plugin HTTP on `127.0.0.1:8899` to respond
3. `dump-usmap` → `C:\dumps\RoboQuest\RoboQuest-Win64-Shipping.usmap`
4. `dump-ue-project methods=1 gameContent=1` → `C:\dumps\RoboQuest\MirrorProject\`
5. `dump-bn-bundle` → `C:\dumps\RoboQuest\REBundle\`

Total time: **~5-15 seconds** for smaller indie games, ~30-60s for AAA
titles with larger UObject arrays.

## Step 6. Check the output

```
C:\dumps\RoboQuest\
├── RoboQuest-Win64-Shipping.usmap   ← FModel / CUE4Parse / UAssetAPI
├── MirrorProject\                    ← buildable UE4 editor project
│   ├── MirrorProject.uproject
│   ├── MirrorProject.Target.cs
│   ├── MirrorProjectEditor.Target.cs
│   └── Source\
│       ├── RoboQuest\Public\*.h     ← game code (UCLASS, UPROPERTY, UFUNCTION)
│       ├── Game\Public\*.h          ← BP content (@kismet bytecode previews)
│       ├── OnlineSubsystem*\
│       ├── FMODStudio\
│       └── ... other modules
├── REBundle\                         ← Binary Ninja / IDA / Ghidra
│   ├── uevr_types.jmap
│   ├── uevr_types.hpp
│   ├── import_binary_ninja_types.py
│   ├── import_ida_types.py
│   └── README.txt
└── dump.log                          ← full server output per step
```

Open `MirrorProject.uproject` in UE4 / UE5 editor to get IntelliSense +
go-to-definition on all the game's types. Drop the `.usmap` next to
FModel to parse cooked `.pak` assets. Open the shipping exe in Binary
Ninja and run `import_binary_ninja_types.py` to get typed `UMyClass::Method(AActor*)` signatures instead of `sub_7FF...`.

## Step 7. Validate (optional, recommended for new games)

Run the emitted project through `UnrealBuildTool` to confirm the headers
actually compile:

```powershell
# Need a blank UE 5.5 C++ scratch project (File → New → Games → Blank → C++)
# so UBT has a host context to drop the module into.
.\mcp-server\bin\Release\net9.0\UevrMcpServer.exe compile-check `
  C:\dumps\RoboQuest\MirrorProject `
  "C:\Users\me\Documents\Unreal Projects\Scratch\Scratch.uproject"
```

Reports per-file UHT + MSVC errors. Auto-skips headers whose basename
collides with the installed engine's own types.

## Troubleshooting

**"Plugin HTTP not responding at 127.0.0.1:8899"**
- Plugin init runs on the first engine tick. At game main-menu idle this
  takes a few seconds. `Start-Sleep -Seconds 15` then retry.
- If it stays down, check `%APPDATA%\UnrealVRMod\<GameStem>\log.txt` for
  UEVR errors. The last line before silence is usually the failure point.
- Make sure `uevr_mcp.dll` in `%APPDATA%\UnrealVRMod\<GameStem>\plugins\`
  is the freshly-built one, not a stale copy: `.\tools\setup.ps1`
  rebuilds it; then `.\mcp-server\bin\Release\net9.0\UevrMcpServer.exe install-plugin <gameExe>` copies it into place.

**"Game process exited before we could inject"**
- Game has a launcher-stub. Launch via Steam / Epic yourself, then use
  `attach` (quick-dump.ps1 does this by default).

**Game crashes ~60 s after injection**
- If it's RoboQuest specifically: check that `Mods\UHTDumper\Scripts\main.lua`
  (UE4SS's pre-installed header-dump mod) is renamed to `main.lua.disabled`.
  That Lua mod calls `os.exit()` at 60 s regardless of UEVR. Was my own
  earlier-session tooling misattributed as a UEVR upstream bug.
- Other games: check `%APPDATA%\UnrealVRMod\<GameStem>\crash.dmp` and
  `C:\dumps\wer\<ExeName>.<pid>.dmp` (if you ran `enable-wer-dumps.ps1`).

**"Game thread request timed out" during dump**
- Reflection batches have a 10 s game-thread budget (20 s with methods=1).
  Large AAA games at `methods=1 gameContent=1` sometimes overrun.
  Retry without `-Methods` for a faster path, or pass a tight filter to
  the MCP tool directly.

**Kismet comments (`// @kismet N bytes: ...`) not appearing**
- Only `/Game/` BP-generated classes have populated Script buffers. Pass
  `-GameContent` to `quick-dump.ps1` (the default — if you turned it off,
  turn it back on).

## Full recipe in one block (copy/paste)

```powershell
# Assumes you cloned uevr-mcp at E:\Github\uevr-mcp and have UEVR source
# built at E:\Github\UEVR\build

cd E:\Github\uevr-mcp
.\tools\setup.ps1

$GameExe = "E:\SteamLibrary\steamapps\common\RoboQuest\RoboQuest\Binaries\Win64\RoboQuest-Win64-Shipping.exe"
.\tools\enable-dumper-mode.ps1 -GameExe $GameExe
.\tools\enable-wer-dumps.ps1 -GameExe $GameExe

# Launch the game via Steam, wait for the main menu, then:
.\tools\quick-dump.ps1 -GameExe $GameExe -OutDir C:\dumps\RoboQuest

# Output:
#   C:\dumps\RoboQuest\RoboQuest-Win64-Shipping.usmap
#   C:\dumps\RoboQuest\MirrorProject\
#   C:\dumps\RoboQuest\REBundle\
```
