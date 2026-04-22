#pragma once

#include <uevr/API.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

// ── Kismet bytecode preview ────────────────────────────────────────
//
// UEVR doesn't expose UFunction::Script so we probe for it like the
// other property-subclass fields. Script is a TArray<uint8> at the tail
// of UFunction; layout is [UStruct base][FunctionFlags u32][NumParms u8]
// [ParmsSize u16][ReturnValueOffset u16][RPCId u16][RPCResponseId u16]
// [FirstPropertyToInit FProperty*][EventGraphFunction UFunction*]
// [EventGraphCallOffset i32][Func void*][Script TArray<u8>].
//
// The probe anchors on Func (a valid function pointer in executable memory)
// and reads Script as TArray just after it. Works on UE4.22–UE5.5 because
// that tail layout is stable across versions.
//
// This is intentionally not a full decompiler — full EX_* -> C++ is a
// separate multi-week project. Instead we produce:
//   - a byte count for gating ("did this function have Blueprint bytecode?")
//   - an opcode mnemonic list (up to N instructions) for triage
//
// That's enough to populate Blueprint-only function bodies with a
// meaningful comment in the emitter, and lets agents see WHAT the function
// does at the opcode level without a full decomp.

namespace KismetDisasm {

struct ScriptView {
    const uint8_t* data{nullptr};
    int32_t size{0};
    bool valid() const { return data != nullptr && size > 0; }
};

// Probe and return the (data, size) pair for the UFunction's Script TArray.
// Returns invalid ScriptView if not resolved or empty.
ScriptView get_function_script(uevr::API::UFunction* func);

// Walk the bytecode and return a list of opcode mnemonics. Stops at
// max_instructions or end-of-script, whichever comes first. Invalid opcodes
// abort the walk (we don't know all operand shapes so we emit what we know
// and stop cleanly rather than misalign).
std::vector<std::string> disassemble(const ScriptView& script, size_t max_instructions = 64);

// Diagnostics for uevr_probe_status: offset resolved + hit count.
nlohmann::json diagnostics();

void reset();

} // namespace KismetDisasm
