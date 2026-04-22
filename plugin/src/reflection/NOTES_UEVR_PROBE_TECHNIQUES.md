# UEVR probe techniques — what to borrow

Cross-referenced against `E:\Github\UEVR\dependencies\submodules\UESDK\src\sdk\`
to harden our reflection probes across any UE game.

## Core insight: semantic anchors > shape heuristics

Our original approach probed candidate offsets looking for memory patterns
that *look like* a given field (valid pointer, plausible TArray shape, etc).
This is fragile because many offsets in adjacent structs produce equally
plausible-looking shapes (e.g. two consecutive `FProperty*` fields read as a
valid "empty TArray"). UEVR's approach is fundamentally stronger:
anchor on **values we know must exist** in engine-provided types every UE
game ships with.

## UEVR's anchor table

| Target field | Sentinel | Technique |
|---|---|---|
| `UObjectBase::ClassPrivate` | None; it's the first valid pointer after VTable+flags | Scan `[0x8, 0x50)`, first slot whose deref looks like a vtable with a valid vfunc in executable memory |
| `UObjectBase::NamePrivate` | `"/Script/CoreUObject"` | Scan FName slots from ClassPrivate+8, resolve `to_string_no_numbers()`, compare to literal |
| `UStruct::SuperStruct` | Chain consistency: `UClass.super==UStruct && UStruct.super==UField && UField.super==UObject` | Triple cross-check eliminates all coincidental matches |
| `UStruct::ChildProperties` | FVector has FField children named `"X"`, `"Y"`, `"Z"` | Walk pointer slots, deref, resolve FName, compare literal |
| `UStruct::PropertiesSize` | `sizeof(FVector)=12 (UE4) / 24 (UE5)`, `sizeof(FMatrix4)=64 (UE4) / 128 (UE5)` | Scan int32 slots, require BOTH matrix=mat_size AND vector=vec_size at same offset |
| `UStruct::MinAlignment` | Follows PropertiesSize (+4 bytes) | Inferred |
| `UField::Next` | Real linked list must produce ≥5 UFunctions when walked | Validate by traversal, not just shape |
| `UFunction::FunctionFlags` | `CheatManager::Fly` always has `FUNC_Exec (0x200) \| FUNC_Public (0x20000) \| FUNC_Native (0x400)` | Scan uint32 slots for exact bit pattern AND'd equal to mask |
| `UFunction::Func` (native) | Statistical: offset where most UFunctions in GameplayStatics have a pointer into a loaded module | Cross-UFunction frequency analysis, highest vote wins |
| `UClass::ClassDefaultObject` | Object at this offset on UClass_X has `get_class() == X` | Cross-class self-consistency |
| `UScriptStruct::StructOps` | StructOps vtable is readable; `StructOps->size` equals known FVector/FMatrix size | Vtable-chain + size comparison |
| `FField::ClassPtr` | `utility::scan_ptr(field, 0x50, floatProperty_addr)` — find the slot whose stored pointer equals the known `FloatProperty` class object | Known-pointer scan |
| `FFieldClass::Name` | FName at some slot must resolve to `"FloatProperty"` or `"DoubleProperty"` | String literal compare |
| `FName::a1 == a2` test | If case-preserving, `a1` and `a2` duplicate the comparison index | Fingerprint the FName encoding style |
| `ProcessEvent` vtable slot | Walk UObject vtable, disassemble each slot's function, match known ProcessEvent prologue | Instruction-level scan |

## Universally-available game-agnostic sentinels

These exist on **every** UE game (CoreUObject + Engine are mandatory):

- **Classes:** `UObject`, `UField`, `UStruct`, `UClass`, `UFunction`, `UScriptStruct`, `UEnum`
- **Structs:** `FVector` (always X/Y/Z), `FMatrix` (always 4x4), `FRotator`, `FTransform`, `FColor`
- **Properties:** `FloatProperty`, `DoubleProperty`, `IntProperty`, `BoolProperty`, `ObjectProperty`, `ClassProperty`, `ArrayProperty`, `MapProperty`, `SetProperty`, `EnumProperty`, `StructProperty`
- **Functions on known classes:**
  - `CheatManager::Fly` — always has FUNC_Exec|FUNC_Public|FUNC_Native (UEVR uses this)
  - `GameplayStatics::GetGameInstance` / `GetPlayerController` / etc. — native UFUNCTIONs
  - `Pawn::GetActorLocation` (and hundreds of engine functions with predictable flags)

## Pointer validation recipe

Every pointer deref in UEVR's update_offsets() runs this gauntlet:

```cpp
if (ptr == nullptr) continue;
if (IsBadReadPtr(ptr, sizeof(void*))) continue;
if (((uintptr_t)ptr & 1) != 0) continue;  // UE5 TObjectPtr tag catcher
const auto vtable = *(void**)ptr;
if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*))) continue;
if (((uintptr_t)vtable & 1) != 0) continue;
const auto vfunc = *(void**)vtable;
if (vfunc == nullptr || IsBadReadPtr(vfunc, sizeof(void*))) continue;
// At this point we have a very high-confidence object pointer.
```

Our current `page_readable` + `seh_read_ptr` catches page-fault risk but
doesn't add the low-bit check or vtable-and-vfunc verification chain.

## Hierarchical bootstrap

UEVR's `update_offsets()` runs once at plugin init and resolves layers
in dependency order:

1. `UObjectBase::update_offsets()` — ClassPrivate, NamePrivate (needs only vtable/FName anchors)
2. `UStruct::update_offsets()`:
   - **SuperStruct** first (triple-chain cross-check, no dependencies)
   - **ChildProperties** / **Children** (FVector's X/Y/Z FName anchor)
   - **Next** (walk linked list, require ≥5 UFunctions)
   - **NativeFunc** (statistical on GameplayStatics children)
3. `UClass::update_offsets()` — DefaultObject (self-consistency once UStruct is resolved)
4. `UScriptStruct::update_offsets()` — PropertiesSize (FVector/FMatrix size sentinel)
5. `UFunction::update_offsets()` — FunctionFlags (CheatManager::Fly flag pattern)
6. `FProperty::bruteforce_fproperty_offset(x_prop, y_prop, z_prop)` — offsets within FProperty subclasses using the already-found X/Y/Z

Each layer's resolution feeds the next. By the time UFunction probes run,
they have SuperStruct + PropertiesSize as known starting points and only
need to scan the tail of UFunction's layout.

## What our probes already do well

- SEH-guarded memory reads in noexcept helpers (matches UEVR's try/catch)
- Candidate offset ranges cover UE4.22–UE5.5 (matches UEVR's 0x300 scan range)
- Cached per-(subclass, field) atomic offset state (UEVR does the same with `static s_*_offset`)
- Lazy discovery with validation (UEVR is one-shot at init; we can match or hybridize)

## What we should borrow

Ordered by impact on robustness:

### 1. Replace shape validation with value-space anchors (biggest win)

Instead of "at offset X I read a TArray with plausible shape," lock in:
- `UStruct::PropertiesSize` via FVector=12 + FMatrix=64 int32 co-match
- Then `Script` is unambiguous at `PropertiesSize_offset + 8` (PropertiesSize + MinAlignment are fixed 8 bytes).
- Eliminates all the "coincidentally empty TArray at several offsets" ambiguity we hit on RoboQuest.

### 2. Add low-bit pointer tag check

`(ptr & 1) != 0` rejects UE5's TObjectPtr tag encodings. One line, breaks nothing, makes our probes UE5-safe.

### 3. Vtable+vfunc verification after every pointer deref

`ptr → vtable → vfunc` chain with IsBadReadPtr at each hop. Eliminates
pointer-shaped-but-not-real hits that can happen in dense heap regions.

### 4. Cross-class sentinel for SuperStruct

We currently use UEVR's `get_super_struct()` which presumably already
works. If we ever implement our own, use UClass→UStruct→UField→UObject
consistency instead of shape-based scanning.

### 5. Known-function anchor for FunctionFlags

Instead of trusting UEVR's `get_function_flags()`, we could cross-check
with `CheatManager::Fly` flag sentinel to detect if UEVR's offset
resolution failed silently on a fork game.

### 6. Statistical anchor for native Func pointer

If we ever need to find `UFunction::Func` directly: iterate all UFunction
children of GameplayStatics, bucket offsets by "has a pointer into a
loaded module." The offset with the highest vote count is `Func`. Robust
even on games that rename modules or move code around.

## Application to Script probe specifically

The current probe commits on "empty TArray at some offset with readable
data pointer." With FVector-anchored PropertiesSize it becomes:

```cpp
// Bootstrap: find PropertiesSize offset once via FVector/FMatrix sentinel
const auto fvector = find_uobject(L"ScriptStruct /Script/CoreUObject.Vector");
const auto fmatrix = find_uobject(L"ScriptStruct /Script/CoreUObject.Matrix");
for (off i in [0x40, 0x100) step 4) {
    if (*(int32*)(fvector+i) == 12 && *(int32*)(fmatrix+i) == 64) {
        // UE4 float precision — matches RoboQuest 4.26
        ps_offset = i;
        script_offset = i + 8;  // skip PropertiesSize + MinAlignment
        break;
    }
    if (*(int32*)(fvector+i) == 24 && *(int32*)(fmatrix+i) == 128) {
        // UE5 LargeWorldCoordinates — double precision
        ps_offset = i;
        script_offset = i + 8;
        break;
    }
}
```

No ambiguity: the pair (12, 64) or (24, 128) is game-agnostic and
absolutely specific. Eliminates the 32-candidate consensus path entirely.
