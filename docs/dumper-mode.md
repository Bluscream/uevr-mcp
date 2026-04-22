# UEVR dumper mode — architecture reference

## TL;DR

UEVR's whole reason for existing is render-pipeline hooks for VR. But
those same hooks crash some UE4 forks (RoboQuest, Stellar Blade variants)
or are just dead weight when a plugin only needs UObject reflection.

Dumper mode is an opt-in runtime flag that tells UEVR:
> "Skip the render-pipeline hooks. Keep the plugin loader, UObject
> reflection, and the engine-tick hook. That's all I need."

Activation: environment variable `UEVR_DUMPER_MODE=1` **or** a sentinel
file `%APPDATA%\UnrealVRMod\<GameStem>\dumper_mode`. The sentinel is
preferred because env vars don't propagate cleanly through Steam /
Epic / anti-cheat launchers.

## Why it exists

Three problems with standard UEVR when you only want reflection:

1. **Some games crash on `FFakeStereoRenderingHook::attempt_hooking`.**
   RoboQuest 4.26 and Stellar Blade 4.26.2 die in their render thread
   within seconds of UEVR swapping the stereo device vtable. Their
   version of `FViewport::GetRenderTargetTexture` / `FSceneView`
   constructor / Slate draw thread is just different enough to blow up.
2. **The "Last chance encountered for hooking" death spiral.** UEVR's
   D3D monitor thread re-hooks D3D Present every 5 s if the game hasn't
   presented. On some games this races with the game's own render init.
3. **VR runtime init isn't free.** OpenVR / OpenXR loaders pull in a
   bunch of state that's pure overhead when nobody's wearing a headset.

Dumper mode skips all three while leaving enough of UEVR's plumbing
intact for any plugin that only uses the UObject API to work normally.

## What stays, what goes

| Component | Dumper mode | Normal UEVR |
|---|---|---|
| `Framework` construction | ✓ | ✓ |
| Plugin loader (`uevr_plugin_initialize` fires) | ✓ | ✓ |
| `UObjectHook` — existing-object snapshot | ✓ | ✓ |
| `UObjectHook` — inline `AddObject` / `destructor` trampolines | ✗ | ✓ |
| `FFakeStereoRenderingHook::attempt_hook_game_engine_tick` | **✓** (critical) | ✓ |
| Engine tick fan-out to mods → `on_pre_engine_tick` | ✓ | ✓ |
| `FFakeStereoRenderingHook::attempt_hooking` (stereo vtable) | ✗ | ✓ |
| `attempt_hook_slate_thread` | ✗ | ✓ |
| `attempt_hook_fsceneview_constructor` | ✗ | ✓ |
| `IXRTrackingSystemHook` | ✗ | ✓ |
| D3D11 / D3D12 hooks | ✗ | ✓ |
| `Framework::hook_monitor` "rehook if not presenting" | ✗ (repurposed to drive tick-hook install) | ✓ |
| `VR::on_initialize` (OpenVR / OpenXR runtime load) | ✗ (mod not added to m_mods) | ✓ |
| `WindowsMessageHook` | ✗ | ✓ |
| ImGui menu rendering | ✗ | ✓ |
| `Framework::setup_exception_handler` (crash dump writer) | ✓ (moved to Framework ctor so it fires regardless of D3D init) | ✓ |

## The critical insight: engine-tick hook install without D3D

Normally the engine-tick hook gets installed from
`FFakeStereoRenderingHook::on_frame()`, which runs every D3D Present via
`Framework::on_frame_d3d11/12`. No D3D hooks = no Present callback = no
`on_frame()` = no tick hook = no plugin callbacks.

Dumper mode repurposes `Framework::hook_monitor` (the D3D rehook watchdog
thread) to drive the tick-hook install manually:

```cpp
// Framework.cpp::hook_monitor() (dumper mode branch)
if (uevr::is_dumper_mode()) {
    // One-shot: run Mods::on_initialize + on_initialize_d3d_thread
    // (equivalent of what the first D3D Present would trigger)
    if (!m_dumper_mods_initialized) { ... }

    // Recurring: keep calling stereo_hook->on_frame() until it installs
    // the UGameEngine::Tick hook. Once installed, engine_tick_hook
    // fans out to mods including PluginLoader, same as normal mode.
    auto& stereo_hook = m_vr->get_fake_stereo_hook();
    stereo_hook->on_frame();
    return;
}
```

The D3D monitor thread fires every 500 ms, so within ~1 second of
injection the engine-tick hook installs and plugins start getting
`on_pre_engine_tick` callbacks.

## Verified results

| Game | UE ver | Before dumper mode | With dumper mode |
|---|---|---|---|
| RoboQuest | 4.26 | Seemed to crash at ~60 s (later traced to a UE4SS Lua mod, not UEVR — see below) | Stable 14+ min runs verified |
| Hogwarts Legacy | 4.27 | Normal VR pipeline works fine | Also works; dumps in ~15 s |
| Stellar Blade | 4.26.2 | Crashes within seconds on render hook | Not yet retested with dumper mode (was previously handled via Dumper-7 fallback) |

The RoboQuest "60 s crash" turned out to be **UE4SS's bundled UHTDumper
Lua mod calling `os.exit()`** — not a UEVR bug. Dumper mode shipping
doesn't change this; the fix is renaming the UHTDumper mod's `main.lua`
so it doesn't run. But the investigation that surfaced the real root
cause was the point where dumper-mode builds got stable enough to rule
UEVR out as a suspect.

## Implementation layer

The entire runtime logic lives in `src/DumperMode.hpp`:

```cpp
inline bool is_dumper_mode() noexcept {
    static const bool cached = []() noexcept {
        if (/* env var check */)         return true;
        if (/* sentinel file check */)   return true;
        return false;
    }();
    return cached;
}
```

Called from 6 gate sites:
1. `FFakeStereoRenderingHook::on_frame()` — skip 3 render-hook installers
2. `FFakeStereoRenderingHook::engine_tick_hook()` — skip `attempt_hooking` + imgui + enable_engine_thread
3. `Framework::hook_monitor()` — skip D3D rehook loop; run dumper-mode driver instead
4. `Framework::hook_d3d11/12` — early-return without installing D3D hooks
5. `PluginLoader::on_initialize_d3d_thread` — don't require renderer; plugins init with null renderer_data
6. `Mods::Mods()` constructor — skip adding VR to m_mods (singleton still exists for the one thing that needs it: stereo hook access)
7. `UObjectHook::hook()` — skip inline safetyhook trampolines on AddObject + destructor

Plus one plumbing change:
- `Framework::setup_exception_handler()` moved from `Framework::initialize()`
  (D3D-init-gated) to the Framework constructor, so a minidump writer is
  installed regardless of whether D3D hooks fire. Without this, dumper-
  mode crashes were going to Windows default unhandled-exception path
  and leaving no dump.

## Trade-offs

**Pros:**
- Stable on games where standard UEVR crashes
- Faster startup (no OpenVR / OpenXR loader init, no VR mod init chain)
- Lower ongoing overhead (no per-frame VR tick work, no D3D hook in the present path)
- No risk of interfering with other VR tooling running alongside

**Cons:**
- No VR actually works in dumper mode. Obviously.
- No imgui menu — can't use UEVR's debug UI, CVar tweaking, input remapping, etc.
- Plugin's `renderer_data` is all null in dumper mode. Plugins that try
  to screenshot the backbuffer / render overlays / etc. will null-deref.
- Live tracking of new/destroyed UObjects is off. If your plugin needs
  to watch for `NotifyOnNewObject` style events across time, dumper mode
  isn't for you. One-shot snapshot dumps are fine.

## Future work

- Test Stellar Blade with dumper mode (previously required Dumper-7 fallback — dumper mode might eliminate that).
- Per-mod dumper-mode overrides. Currently it's all-or-nothing; might be useful to have a plugin opt in to get D3D data while the rest stays lean.
- Wrap this in a CMake option (`UEVR_DEFAULT_DUMPER_MODE=ON`) so a dedicated "dumper build" of UEVR can ship with the flag baked in.
