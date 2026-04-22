#include "kismet_disasm.h"
#include "property_probes.h"

#include <atomic>
#include <cstring>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using json = nlohmann::json;

namespace KismetDisasm {

// ── Probe state ────────────────────────────────────────────────────

struct ProbeCache {
    std::atomic<int32_t> script_offset{-1}; // offset of Script TArray in UFunction
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> discoveries{0};
};

static ProbeCache g_cache;
static std::mutex g_discovery_mutex;

// Multi-function consensus state. Per-candidate votes accumulate across probe
// attempts. An offset is committed when either:
//   - it scores kOffsetValid on any single function (definitive — a populated
//     Script with a valid EX_* first byte can't be coincidence), OR
//   - after N probe attempts (kMinProbesForConsensus), one offset is uniquely
//     consistent (all-empty, never rejected) while at least one OTHER offset
//     has been rejected. This handles fully-native games where no UFunction
//     has populated Script.
struct CandidateVotes {
    uint32_t valid = 0;   // count>0 with valid first byte
    uint32_t empty = 0;   // count=0, plausible TArray shape
    uint32_t reject = 0;  // impossible — count out of range, etc.
};
static CandidateVotes g_votes[16]; // parallel to kScriptOffsetCandidates
static uint32_t g_probes_attempted = 0;
static const uint32_t kMinProbesForConsensus = 32;

// ── SEH primitives (no C++ objects in these frames) ────────────────

static bool page_readable(const void* p, size_t size) noexcept
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto end = reinterpret_cast<const uint8_t*>(p) + size;
    auto region_end = reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return end <= region_end;
}

static bool page_executable(const void* p) noexcept
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD exec_bits = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & exec_bits) != 0;
}

struct TArrayLike { void* data; int32_t count; int32_t capacity; };

static bool seh_read_tarray(const void* src, TArrayLike* out) noexcept
{
    __try { std::memcpy(out, src, sizeof(TArrayLike)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool seh_read_ptr(const void* src, void** dst) noexcept
{
    __try { *dst = *reinterpret_cast<void* const*>(src); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ── FVector/FMatrix value-space anchor (UEVR technique) ──────────
//
// Adapted from UEVR's UScriptStruct::update_offsets() in
// UESDK/src/sdk/UClass.cpp. FVector always has PropertiesSize == 12
// (UE4 float) or 24 (UE5 LargeWorldCoordinatesReal double). FMatrix4
// always has 64 (UE4) or 128 (UE5). The pair uniquely identifies the
// PropertiesSize offset in UStruct with no shape ambiguity — no other
// offset in UStruct's layout reads as (matrix=64, vector=12) because
// MinAlignment, Children pointer low-bits, etc. won't co-match those
// specific values.
//
// Once PropertiesSize is known, Script is at PropertiesSize_offset + 8
// (PropertiesSize int32 + MinAlignment int32 = 8 bytes before Script).
//
// Returns -1 on failure (FVector/FMatrix classes not found in this game),
// else the discovered Script offset.
static int32_t discover_script_via_vector_anchor() noexcept
{
    auto& api = uevr::API::get();
    if (!api) return -1;

    auto fvector = api->find_uobject<uevr::API::UStruct>(L"ScriptStruct /Script/CoreUObject.Vector");
    auto fmatrix = api->find_uobject<uevr::API::UStruct>(L"ScriptStruct /Script/CoreUObject.Matrix");
    if (!fvector || !fmatrix) return -1;

    // PropertiesSize is an int32 member. UEVR scans from SuperStruct+ptr
    // through 0x300. We scan a tighter range since we know UStruct base
    // size ~0x40 and PropertiesSize lives before Script.
    for (int32_t off = 0x40; off < 0x100; off += 4) {
        int32_t vec_size = 0, mat_size = 0;
        __try {
            vec_size = *(int32_t*)((uint8_t*)fvector + off);
            mat_size = *(int32_t*)((uint8_t*)fmatrix + off);
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }

        // UE4 float precision (RoboQuest 4.26): FVector=12, FMatrix=64
        // UE5 double precision (LWC): FVector=24, FMatrix=128
        bool ue4_match = (vec_size == 12 && mat_size == 64);
        bool ue5_match = (vec_size == 24 && mat_size == 128);
        if (!ue4_match && !ue5_match) continue;

        // PropertiesSize at `off`, MinAlignment at `off+4`, Script at `off+8`.
        return off + 8;
    }
    return -1;
}

// ── Probe UStruct::Script offset ──────────────────────────────────
//
// Script is a UStruct member (NOT a UFunction-specific one): confirmed in
// UE 4.26 + UE 5.5 Engine/Source/Runtime/CoreUObject/Public/UObject/Class.h.
// Layout on any UStruct subclass (UClass, UScriptStruct, UFunction):
//
//   [UObject      0x00 .. 0x28]
//   [UField::Next 0x28 .. 0x30]
//   [FStructBaseChain (StructBaseChainArray* + NumStructBasesInChainMinusOne + pad)
//                 0x30 .. 0x40]  (present when USTRUCT_FAST_ISCHILDOF_IMPL ==
//                                 USTRUCT_ISCHILDOF_STRUCTARRAY, the default)
//   [SuperStruct          0x40 .. 0x48]
//   [Children             0x48 .. 0x50]
//   [ChildProperties      0x50 .. 0x58]
//   [PropertiesSize + MinAlignment (2x int32)  0x58 .. 0x60]
//   [Script TArray<uint8>  0x60 .. 0x70]  ← canonical offset on UE 4.26+
//
// Stripped-shipping builds (no editor-only fields) keep this layout stable
// across UE 4.22–5.5. The probe scans a handful of candidates around 0x60
// to absorb non-standard UE builds (Stellar Blade-style forks), then locks
// the first offset where the TArray shape passes AND data[0] is a valid
// EX_* opcode when non-empty.

static const int32_t kScriptOffsetCandidates[] = {
    0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80
};

// Validate data[0] is a plausible EX_* opcode. Real UE bytecode starts in
// 0x00–0x6B (EExprToken). Values 0x80+ definitely aren't opcodes; values
// 0x6C–0x7F are unused / reserved. Reject candidate offsets where the first
// byte doesn't parse as a valid opcode — that filters out false-positives
// where the TArray-shape check passes on non-Script data (e.g. some internal
// table whose pointer happens to look like a valid buffer).
static bool seh_read_byte(const void* src, uint8_t* out) noexcept
{
    __try { *out = *reinterpret_cast<const uint8_t*>(src); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool is_plausible_bytecode(const void* data, int32_t size) noexcept
{
    if (size <= 0 || !data) return true; // empty script is always valid
    uint8_t b0 = 0;
    if (!seh_read_byte(data, &b0)) return false;
    // EX_Max is 0x7F in UE5, 0x6F in UE4.26. Accept anything < 0x80 since the
    // two ranges differ and we don't know which UE version we're on.
    return b0 < 0x80;
}

// Evaluate one candidate offset on a single UFunction. Returns:
//   kOffsetReject   — shape invalid (count out of range, capacity mismatch,
//                     unreadable data pointer, or count>0 with non-EX_* first byte)
//   kOffsetEmpty    — shape plausible, count=0 (no information — could be any
//                     offset that happens to contain zeros)
//   kOffsetValid    — shape plausible, count>0, and data[0] is a valid EX_ opcode
static constexpr int kOffsetReject = -1;
static constexpr int kOffsetEmpty  =  0;
static constexpr int kOffsetValid  =  1;

static int evaluate_candidate(void* struct_base, int32_t off) noexcept
{
    auto addr = reinterpret_cast<uint8_t*>(struct_base) + off;
    if (!page_readable(addr, sizeof(TArrayLike))) return kOffsetReject;

    TArrayLike arr{};
    if (!seh_read_tarray(addr, &arr)) return kOffsetReject;

    if (arr.count < 0 || arr.count > 0x100000) return kOffsetReject;
    if (arr.count > arr.capacity) return kOffsetReject;

    // Even for empty TArrays, the data field must be either null or a
    // readable heap pointer. This rejects offsets where the bytes at
    // [off, off+8) are actually small-int fields packed as a fake pointer
    // (e.g. UStruct PropertiesSize + MinAlignment = two int32s). Their
    // combined value is a small number (<0x10000000000) that VirtualQuery
    // rejects as unmapped. This is the key discriminator that separates
    // Script (empty → data=nullptr) from ChildProperties-adjacent reads
    // (empty → data=small-int-garbage).
    if (arr.data != nullptr && !page_readable(arr.data, 1)) return kOffsetReject;

    if (arr.count == 0) {
        // Shape plausible (data null or readable, counts consistent).
        return kOffsetEmpty;
    }
    if (!arr.data) return kOffsetReject;
    if (!page_readable(arr.data, static_cast<size_t>(arr.count))) return kOffsetReject;
    if (!is_plausible_bytecode(arr.data, arr.count)) return kOffsetReject;
    return kOffsetValid;
}

// Multi-call consensus probe. Records per-candidate votes across many
// UFunction probe attempts, commits an offset once we have a definitive
// answer (any valid non-empty Script) or strong consensus (N probes with
// one offset uniquely plausible while another offset was rejected).
//
// Returns:
//   >= 0 : offset resolved (commit to cache)
//   -1   : deferred, probe again next call
//   -2   : cannot resolve — no offset is plausible after enough attempts
static int32_t probe_script_offset(void* func_base) noexcept
{
    g_probes_attempted++;

    int any_valid_off = -1;
    constexpr size_t N = sizeof(kScriptOffsetCandidates) / sizeof(kScriptOffsetCandidates[0]);
    static_assert(N <= sizeof(g_votes) / sizeof(g_votes[0]), "votes array too small");

    for (size_t i = 0; i < N; ++i) {
        int32_t off = kScriptOffsetCandidates[i];
        int verdict = evaluate_candidate(func_base, off);
        if (verdict == kOffsetValid) {
            g_votes[i].valid++;
            if (any_valid_off < 0) any_valid_off = off;
        } else if (verdict == kOffsetEmpty) {
            g_votes[i].empty++;
        } else {
            g_votes[i].reject++;
        }
    }

    // Definitive win: any candidate with a populated Script + valid opcode.
    if (any_valid_off >= 0) return any_valid_off;

    // Consensus path. After kMinProbesForConsensus attempts, pick the
    // candidate with the most empties and zero rejects, tie-broken by
    // first-wins. Require at least ONE other candidate to have seen rejects
    // (otherwise we can't distinguish a real Script from random empty memory).
    if (g_probes_attempted >= kMinProbesForConsensus) {
        int best_idx = -1;
        uint32_t best_empty = 0;
        bool any_rejected = false;
        for (size_t i = 0; i < N; ++i) {
            if (g_votes[i].reject > 0) any_rejected = true;
            if (g_votes[i].reject == 0 && g_votes[i].empty > best_empty) {
                best_empty = g_votes[i].empty;
                best_idx = (int)i;
            }
        }
        if (best_idx >= 0 && any_rejected && best_empty >= kMinProbesForConsensus / 2) {
            return kScriptOffsetCandidates[best_idx];
        }
        // Exceeded probe budget without a winner — cap failure to avoid
        // probing every UFunction forever.
        if (g_probes_attempted >= kMinProbesForConsensus * 4) return -2;
    }

    return -1;
}

ScriptView get_function_script(uevr::API::UFunction* func)
{
    ScriptView out;
    if (!func) return out;

    int32_t cached = g_cache.script_offset.load(std::memory_order_acquire);
    if (cached < 0) {
        if (cached == -2) return out;
        std::lock_guard<std::mutex> lock(g_discovery_mutex);
        cached = g_cache.script_offset.load(std::memory_order_acquire);
        if (cached < 0) {
            // Primary path: value-space anchor via FVector/FMatrix PropertiesSize.
            // Adapted from UEVR's UScriptStruct::update_offsets(). This is a
            // one-shot discovery — no multi-call consensus needed because the
            // anchor is mathematically unique.
            int32_t result = discover_script_via_vector_anchor();
            if (result >= 0) {
                g_cache.script_offset.store(result, std::memory_order_release);
                g_cache.discoveries.fetch_add(1, std::memory_order_relaxed);
                cached = result;
            } else {
                // Fallback: consensus probe. For games where FVector/FMatrix
                // classes can't be resolved (custom cooks, unusual builds),
                // fall back to the multi-function shape-based consensus.
                result = probe_script_offset(func);
                if (result >= 0 || result == -2) {
                    g_cache.script_offset.store(result, std::memory_order_release);
                    if (result >= 0) g_cache.discoveries.fetch_add(1, std::memory_order_relaxed);
                }
                cached = result;
            }
        }
    }
    if (cached < 0) return out;

    auto script_addr = reinterpret_cast<uint8_t*>(func) + cached;
    if (!page_readable(script_addr, sizeof(TArrayLike))) return out;

    TArrayLike arr{};
    if (!seh_read_tarray(script_addr, &arr)) return out;

    if (arr.count <= 0 || !arr.data) return out;
    if (!page_readable(arr.data, static_cast<size_t>(arr.count))) return out;

    out.data = reinterpret_cast<const uint8_t*>(arr.data);
    out.size = arr.count;
    g_cache.hits.fetch_add(1, std::memory_order_relaxed);
    return out;
}

// ── EX_* opcode mnemonic table ─────────────────────────────────────
//
// Mirrored from Engine/Source/Runtime/CoreUObject/Public/UObject/Script.h
// (EExprToken). We only need names for preview purposes; full operand
// decoding is a deeper undertaking.

static const char* opcode_name(uint8_t op)
{
    switch (op) {
        case 0x00: return "LocalVariable";
        case 0x01: return "InstanceVariable";
        case 0x02: return "DefaultVariable";
        case 0x04: return "Return";
        case 0x06: return "Jump";
        case 0x07: return "JumpIfNot";
        case 0x09: return "Assert";
        case 0x0B: return "Nothing";
        case 0x0F: return "Let";
        case 0x12: return "ClassContext";
        case 0x13: return "MetaCast";
        case 0x14: return "LetBool";
        case 0x15: return "EndParmValue";
        case 0x16: return "EndFunctionParms";
        case 0x17: return "Self";
        case 0x18: return "Skip";
        case 0x19: return "Context";
        case 0x1A: return "Context_FailSilent";
        case 0x1B: return "VirtualFunction";
        case 0x1C: return "FinalFunction";
        case 0x1D: return "IntConst";
        case 0x1E: return "FloatConst";
        case 0x1F: return "StringConst";
        case 0x20: return "ObjectConst";
        case 0x21: return "NameConst";
        case 0x22: return "RotationConst";
        case 0x23: return "VectorConst";
        case 0x24: return "ByteConst";
        case 0x25: return "IntZero";
        case 0x26: return "IntOne";
        case 0x27: return "True";
        case 0x28: return "False";
        case 0x29: return "TextConst";
        case 0x2A: return "NoObject";
        case 0x2B: return "TransformConst";
        case 0x2C: return "IntConstByte";
        case 0x2D: return "NoInterface";
        case 0x2E: return "DynamicCast";
        case 0x2F: return "StructConst";
        case 0x30: return "EndStructConst";
        case 0x31: return "SetArray";
        case 0x32: return "EndArray";
        case 0x33: return "PropertyConst";
        case 0x34: return "UnicodeStringConst";
        case 0x35: return "Int64Const";
        case 0x36: return "UInt64Const";
        case 0x37: return "DoubleConst";
        case 0x38: return "Cast";
        case 0x39: return "SetSet";
        case 0x3A: return "EndSet";
        case 0x3B: return "SetMap";
        case 0x3C: return "EndMap";
        case 0x3D: return "SetConst";
        case 0x3E: return "EndSetConst";
        case 0x3F: return "MapConst";
        case 0x40: return "EndMapConst";
        case 0x41: return "Vector3fConst";
        case 0x42: return "StructMemberContext";
        case 0x43: return "LetMulticastDelegate";
        case 0x44: return "LetDelegate";
        case 0x45: return "LocalVirtualFunction";
        case 0x46: return "LocalFinalFunction";
        case 0x47: return "LocalOutVariable";
        case 0x49: return "DeprecatedOp4A";
        case 0x4A: return "InstanceDelegate";
        case 0x4B: return "PushExecutionFlow";
        case 0x4C: return "PopExecutionFlow";
        case 0x4D: return "ComputedJump";
        case 0x4E: return "PopExecutionFlowIfNot";
        case 0x4F: return "Breakpoint";
        case 0x50: return "InterfaceContext";
        case 0x51: return "ObjToInterfaceCast";
        case 0x52: return "EndOfScript";
        case 0x53: return "CrossInterfaceCast";
        case 0x54: return "InterfaceToObjCast";
        case 0x55: return "WireTracepoint";
        case 0x56: return "SkipOffsetConst";
        case 0x57: return "AddMulticastDelegate";
        case 0x58: return "ClearMulticastDelegate";
        case 0x59: return "Tracepoint";
        case 0x5A: return "LetObj";
        case 0x5B: return "LetWeakObjPtr";
        case 0x5C: return "BindDelegate";
        case 0x5D: return "RemoveMulticastDelegate";
        case 0x5E: return "CallMulticastDelegate";
        case 0x5F: return "LetValueOnPersistentFrame";
        case 0x60: return "ArrayConst";
        case 0x61: return "EndArrayConst";
        case 0x62: return "SoftObjectConst";
        case 0x63: return "CallMath";
        case 0x64: return "SwitchValue";
        case 0x65: return "InstrumentationEvent";
        case 0x66: return "ArrayGetByRef";
        case 0x67: return "ClassSparseDataVariable";
        case 0x68: return "FieldPathConst";
        case 0x69: return "AutoRtfmTransact";
        case 0x6A: return "AutoRtfmStopTransact";
        case 0x6B: return "AutoRtfmAbortIfNot";
        default:   return nullptr;
    }
}

// A few opcodes have predictable operand sizes we can skip. The rest we
// abort on because misaligned walk produces junk. This is the minimum set
// needed to get past trivial functions like `Ret;`.

static int32_t skip_bytes_for(uint8_t op)
{
    switch (op) {
        case 0x04: // Return — followed by an expression (variable length)
        case 0x0B: // Nothing
        case 0x16: // EndFunctionParms
        case 0x17: // Self
        case 0x25: // IntZero
        case 0x26: // IntOne
        case 0x27: // True
        case 0x28: // False
        case 0x2A: // NoObject
        case 0x2D: // NoInterface
        case 0x30: // EndStructConst
        case 0x32: // EndArray
        case 0x3A: // EndSet
        case 0x3C: // EndMap
        case 0x3E: // EndSetConst
        case 0x40: // EndMapConst
        case 0x4C: // PopExecutionFlow
        case 0x4F: // Breakpoint
        case 0x52: // EndOfScript
        case 0x55: // WireTracepoint
        case 0x59: // Tracepoint
        case 0x61: // EndArrayConst
            return 0;
        case 0x24: // ByteConst
        case 0x2C: // IntConstByte
            return 1;
        case 0x1D: // IntConst (int32)
        case 0x1E: // FloatConst (float32)
        case 0x06: // Jump (CodeSkipSizeType — 4 bytes in modern UE)
        case 0x18: // Skip
        case 0x4B: // PushExecutionFlow
            return 4;
        case 0x35: // Int64Const
        case 0x36: // UInt64Const
        case 0x37: // DoubleConst
            return 8;
        case 0x21: // NameConst (FName = 2x int32 usually)
            return 12; // conservative — FName on-disk is 2x int32 + i32 number
        case 0x20: // ObjectConst — ScriptPointer (8 bytes on 64-bit)
        case 0x2A + 0x100: // placeholder
            return 8;
        default:
            return -1; // unknown size → stop walking
    }
}

std::vector<std::string> disassemble(const ScriptView& script, size_t max_instructions)
{
    std::vector<std::string> out;
    if (!script.valid()) return out;

    size_t pc = 0;
    while (pc < static_cast<size_t>(script.size) && out.size() < max_instructions) {
        uint8_t op = script.data[pc++];
        const char* name = opcode_name(op);
        if (!name) {
            // Unknown opcode → emit hex and stop walking. We'd misalign otherwise.
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%02X?", op);
            out.emplace_back(buf);
            break;
        }
        out.emplace_back(name);

        int32_t skip = skip_bytes_for(op);
        if (skip < 0) {
            // We know the name but not the operand shape. Stop cleanly.
            out.emplace_back("...");
            break;
        }
        pc += static_cast<size_t>(skip);
    }
    return out;
}

// ── Diagnostics ────────────────────────────────────────────────────

json diagnostics()
{
    auto off = g_cache.script_offset.load();
    json votes = json::array();
    constexpr size_t N = sizeof(kScriptOffsetCandidates) / sizeof(kScriptOffsetCandidates[0]);
    for (size_t i = 0; i < N; ++i) {
        votes.push_back({
            {"offset", kScriptOffsetCandidates[i]},
            {"valid", g_votes[i].valid},
            {"empty", g_votes[i].empty},
            {"reject", g_votes[i].reject}
        });
    }
    return json{
        {"field", "UStruct::Script"},
        {"offset", off < 0 ? json(nullptr) : json(off)},
        {"status", off == -1 ? "undiscovered" : off == -2 ? "failed" : "resolved"},
        {"hits", g_cache.hits.load()},
        {"discoveries", g_cache.discoveries.load()},
        {"probesAttempted", g_probes_attempted},
        {"votes", votes}
    };
}

void reset()
{
    std::lock_guard<std::mutex> lock(g_discovery_mutex);
    g_cache.script_offset.store(-1);
    g_cache.hits.store(0);
    g_cache.discoveries.store(0);
    for (auto& v : g_votes) v = CandidateVotes{};
    g_probes_attempted = 0;
}

} // namespace KismetDisasm
