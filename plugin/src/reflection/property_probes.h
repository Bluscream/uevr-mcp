#pragma once

#include <uevr/API.hpp>
#include <nlohmann/json.hpp>

// ── Property subclass probes ────────────────────────────────────────
//
// UEVR's public API exposes FProperty subclass helpers for the common cases
// (FArrayProperty::get_inner, FBoolProperty::get_field_mask, FStructProperty::
// get_struct, FEnumProperty::get_enum) but *not* for property-class fields on
// FObjectProperty / FClassProperty / FInterfaceProperty / FSoftObjectProperty,
// nor for FMapProperty / FSetProperty inner properties, nor for delegate
// signature functions. Those fields live at UE-version-dependent offsets
// inside each property subclass.
//
// This namespace provides a self-discovering probe: the first time we need
// a given field, we try candidate offsets on a live FProperty, validate the
// result via UEVR's UObjectHook::exists, and cache the winning offset for
// the rest of the session. One cache per (property-subclass, field) pair.
// No version table baked in — works on any UE where the targeted layout
// fits the candidate range.

namespace PropertyProbes {

// For FObjectProperty / FClassProperty / FInterfaceProperty / FSoftObjectProperty /
// FSoftClassProperty / FAssetObjectProperty — the concrete class the property
// can point at. Used to render `AActor*` instead of `void* /*UObject*/`.
uevr::API::UStruct* get_property_class(uevr::API::FProperty* prop);

// For FClassProperty specifically — the "meta class" (TSubclassOf<T>'s T).
// FClassProperty is an FObjectProperty subclass with an additional MetaClass
// field beyond PropertyClass. For FClassProperty, get_property_class() returns
// UClass and get_meta_class() returns T. Callers should prefer get_meta_class()
// for ClassProperty types and get_property_class() for everything else.
uevr::API::UStruct* get_meta_class(uevr::API::FProperty* prop);

// For FInterfaceProperty — the interface class (TScriptInterface<IFoo>'s IFoo).
uevr::API::UStruct* get_interface_class(uevr::API::FProperty* prop);

// For FDelegateProperty / FMulticastDelegateProperty — the UFunction that
// describes the delegate's parameter signature. Used to emit real parameter
// lists instead of opaque `void* /*delegate*/`.
uevr::API::UFunction* get_signature_function(uevr::API::FProperty* prop);

// For FMapProperty / FSetProperty — the inner FProperty descriptors. Validated
// by checking they look like an FField with a "*Property" class name.
uevr::API::FProperty* get_map_key_prop(uevr::API::FProperty* prop);
uevr::API::FProperty* get_map_value_prop(uevr::API::FProperty* prop);
uevr::API::FProperty* get_set_element_prop(uevr::API::FProperty* prop);

// ─── UClass-side probes (Phase 4) ──────────────────────────────────
//
// UEVR's public UClass wrapper exposes get_class_default_object but not the
// classic UCLASS-metadata fields (ClassFlags, ClassConfigName, ClassWithin,
// Interfaces[]). All live at fixed UE-version-dependent offsets within UClass.
// We anchor on ClassWithin because it's a UClass* — easy to validate via
// UObjectHook::exists — then derive ClassFlags / ClassCastFlags /
// ClassConfigName from its known relative layout in UE4.22–UE5.4.

// Returns the ClassWithin pointer (and caches the offset for all future
// lookups including classFlags/classConfigName).
uevr::API::UStruct* get_class_within(uevr::API::UStruct* cls);

// 32-bit EClassFlags value.
uint32_t get_class_flags(uevr::API::UStruct* cls);

// FName of ClassConfigName (returned as wide string; empty if unresolved).
std::wstring get_class_config_name(uevr::API::UStruct* cls);

// Collect the list of UClass* implemented interfaces. Empty on probe failure
// or when the class doesn't implement any.
std::vector<uevr::API::UStruct*> get_interfaces(uevr::API::UStruct* cls);

// Diagnostics: return a JSON object describing which probes have succeeded,
// at which offsets, and how many times each has been hit. Used by the
// uevr_probe_status MCP tool so agents can sanity-check cross-game behaviour.
nlohmann::json diagnostics();

// Reset all caches. Primarily for testing / game hot-reload scenarios.
void reset();

} // namespace PropertyProbes
