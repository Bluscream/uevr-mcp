#include "process_event_listener.h"
#include "json_helpers.h"
#include "pipe_server.h"

#include <uevr/API.hpp>
#include <algorithm>
#include <utility>
#include <cstdint>

using json = nlohmann::json;
using namespace uevr;

// ── Probe infrastructure to discover the ProcessEvent vtable index ──

static std::atomic<int> s_probe_detected_index{-1};

template<int N>
static void probe_fn(void* /*obj*/, void* /*func*/, void* /*params*/) {
    s_probe_detected_index.store(N, std::memory_order_relaxed);
}

static void* s_probe_vtable[128]{};

template<std::size_t... Is>
static void init_probe_vtable_impl(std::index_sequence<Is...>) {
    ((s_probe_vtable[Is] = reinterpret_cast<void*>(&probe_fn<Is>)), ...);
}

static void init_probe_vtable() {
    static bool done = false;
    if (!done) {
        init_probe_vtable_impl(std::make_index_sequence<128>{});
        done = true;
    }
}

// ── Inline hook for ProcessEvent ────────────────────────────────────

typedef void (*OriginalProcessEventFn)(void*, void*, void*);
static OriginalProcessEventFn s_original_pe = nullptr;

static void process_event_hook(void* obj, void* func, void* params) {
    ProcessEventListener::get().on_process_event(obj, func);
    if (s_original_pe) {
        s_original_pe(obj, func, params);
    }
}

// ── ProcessEventListener implementation ─────────────────────────────

ProcessEventListener& ProcessEventListener::get() {
    static ProcessEventListener instance;
    return instance;
}

bool ProcessEventListener::find_and_hook_process_event() {
    auto& api = API::get();
    if (!api) return false;

    // Step 1: Initialize the probe vtable with 128 unique thunks
    init_probe_vtable();

    // Step 2: Create a fake UObject whose vtable points to our probe table.
    //         UObjectBase's first field is the vtable pointer.
    //         The API's process_event wrapper reads *(void***)this to get the vtable,
    //         then calls vtable[s_process_event_index](this, func, params).
    void* fake_vtable_ptr = static_cast<void*>(s_probe_vtable);
    void* fake_obj = &fake_vtable_ptr; // fake_obj at this address: first qword = &s_probe_vtable

    // Step 3: Call process_event through the UEVR API with our fake object.
    //         The API wrapper will index into our probe vtable at the real ProcessEvent index.
    s_probe_detected_index.store(-1, std::memory_order_relaxed);
    auto pe_fn = api->sdk()->uobject->process_event;
    pe_fn(reinterpret_cast<UEVR_UObjectHandle>(fake_obj), nullptr, nullptr);

    int detected = s_probe_detected_index.load(std::memory_order_relaxed);
    if (detected < 0 || detected >= 128) {
        PipeServer::get().log("ProcessEventListener: probe failed — could not detect vtable index");
        return false;
    }

    PipeServer::get().log("ProcessEventListener: detected ProcessEvent at vtable index " + std::to_string(detected));

    // Step 4: Get the real ProcessEvent address from a real UObject's vtable.
    auto array = API::FUObjectArray::get();
    if (!array) return false;

    void* real_obj = nullptr;
    int obj_count = array->get_object_count();
    for (int i = 0; i < obj_count && i < 100000; ++i) {
        auto obj = array->get_object(i);
        if (obj) {
            real_obj = reinterpret_cast<void*>(obj);
            break;
        }
    }

    if (!real_obj) {
        PipeServer::get().log("ProcessEventListener: no UObject found in object array");
        return false;
    }

    auto real_vtable = *reinterpret_cast<void***>(real_obj);
    auto pe_addr = real_vtable[detected];

    if (!pe_addr) {
        PipeServer::get().log("ProcessEventListener: vtable entry is null");
        return false;
    }

    PipeServer::get().log("ProcessEventListener: ProcessEvent address = " +
                          JsonHelpers::address_to_string(reinterpret_cast<uintptr_t>(pe_addr)));

    // Step 5: Install inline hook via the UEVR API
    auto hook_id = api->param()->functions->register_inline_hook(
        pe_addr,
        reinterpret_cast<void*>(&process_event_hook),
        reinterpret_cast<void**>(&s_original_pe)
    );

    if (hook_id < 0) {
        PipeServer::get().log("ProcessEventListener: register_inline_hook failed");
        return false;
    }

    m_hook_id = hook_id;
    m_hooked = true;

    PipeServer::get().log("ProcessEventListener: hook installed (id=" + std::to_string(hook_id) + ")");
    return true;
}

void ProcessEventListener::on_process_event(void* obj, void* func) {
    if (!m_listening.load(std::memory_order_relaxed)) return;
    if (!func) return;

    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;

    // Skip ignored functions
    if (m_ignored.count(func)) return;

    auto& info = m_functions[func];
    info.call_count++;

    // Resolve name on first encounter (we are on the game thread, so API calls are safe)
    if (info.name.empty()) {
        try {
            auto ufunc = reinterpret_cast<API::UFunction*>(func);
            auto wname = ufunc->get_full_name();
            info.name = JsonHelpers::wide_to_utf8(wname);
        } catch (...) {
            info.name = "<unknown>";
        }
    }

    m_recent.push_front(func);
    if (m_recent.size() > MAX_RECENT) {
        m_recent.pop_back();
    }
}

// ── Public API ──────────────────────────────────────────────────────

json ProcessEventListener::start() {
    if (m_listening.load()) {
        return json{{"status", "already_listening"}, {"hooked", m_hooked}};
    }

    if (!m_hooked) {
        if (!find_and_hook_process_event()) {
            return json{{"error", "Failed to hook ProcessEvent — see log for details"}};
        }
    }

    m_listening.store(true);
    return json{{"status", "started"}, {"hooked", true}};
}

json ProcessEventListener::stop() {
    bool was = m_listening.exchange(false);
    return json{{"status", was ? "stopped" : "was_not_listening"}};
}

json ProcessEventListener::status() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return json{
        {"listening", m_listening.load()},
        {"hooked", m_hooked},
        {"hookId", m_hook_id},
        {"uniqueFunctions", (int)m_functions.size()},
        {"recentSize", (int)m_recent.size()},
        {"ignoredCount", (int)m_ignored.size()}
    };
}

json ProcessEventListener::get_functions(int max_calls, const std::string& search, int limit, const std::string& sort) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Collect matching functions
    struct Entry { void* ptr; std::string name; size_t count; };
    std::vector<Entry> entries;
    entries.reserve(m_functions.size());

    for (auto& [ptr, info] : m_functions) {
        // Filter by max_calls (0 = no filter)
        if (max_calls > 0 && info.call_count > static_cast<size_t>(max_calls)) continue;

        // Filter by search string
        if (!search.empty()) {
            // Case-insensitive substring search
            auto name_lower = info.name;
            auto search_lower = search;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            std::transform(search_lower.begin(), search_lower.end(), search_lower.begin(), ::tolower);
            if (name_lower.find(search_lower) == std::string::npos) continue;
        }

        entries.push_back({ptr, info.name, info.call_count});
    }

    // Sort
    if (sort == "name") {
        std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.name < b.name; });
    } else {
        // Default: sort by call count descending
        std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.count > b.count; });
    }

    // Limit
    if (limit > 0 && (int)entries.size() > limit) {
        entries.resize(limit);
    }

    json arr = json::array();
    for (auto& e : entries) {
        arr.push_back(json{
            {"name", e.name},
            {"callCount", e.count},
            {"address", JsonHelpers::address_to_string(reinterpret_cast<uintptr_t>(e.ptr))}
        });
    }

    return json{
        {"functions", arr},
        {"count", (int)arr.size()},
        {"totalUnique", (int)m_functions.size()},
        {"listening", m_listening.load()}
    };
}

json ProcessEventListener::get_recent(int count) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (count <= 0) count = 50;
    json arr = json::array();

    int added = 0;
    for (auto* func : m_recent) {
        if (added >= count) break;

        // Skip ignored
        if (m_ignored.count(func)) continue;

        auto it = m_functions.find(func);
        std::string name = (it != m_functions.end()) ? it->second.name : "<unknown>";
        size_t call_count = (it != m_functions.end()) ? it->second.call_count : 0;

        arr.push_back(json{
            {"name", name},
            {"callCount", call_count},
            {"address", JsonHelpers::address_to_string(reinterpret_cast<uintptr_t>(func))}
        });
        added++;
    }

    return json{
        {"recent", arr},
        {"count", (int)arr.size()},
        {"listening", m_listening.load()}
    };
}

json ProcessEventListener::ignore_function(const std::string& function_name) {
    std::lock_guard<std::mutex> lock(m_mutex);

    int count = 0;
    for (auto& [ptr, info] : m_functions) {
        if (info.name.find(function_name) != std::string::npos) {
            m_ignored.insert(ptr);
            count++;
        }
    }

    return json{{"ignored", count}, {"pattern", function_name}, {"totalIgnored", (int)m_ignored.size()}};
}

json ProcessEventListener::ignore_all() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& [ptr, info] : m_functions) {
        m_ignored.insert(ptr);
    }

    return json{{"ignored", (int)m_ignored.size()}};
}

json ProcessEventListener::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);

    int had = (int)m_functions.size();
    m_functions.clear();
    m_recent.clear();

    return json{{"cleared", had}};
}

json ProcessEventListener::clear_ignored() {
    std::lock_guard<std::mutex> lock(m_mutex);

    int had = (int)m_ignored.size();
    m_ignored.clear();

    return json{{"cleared", had}};
}
