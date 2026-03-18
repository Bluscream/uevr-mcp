#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <atomic>

class ProcessEventListener {
public:
    static ProcessEventListener& get();

    // Called from routes (via GameThreadQueue)
    nlohmann::json start();
    nlohmann::json stop();
    nlohmann::json status();
    nlohmann::json get_functions(int max_calls, const std::string& search, int limit, const std::string& sort);
    nlohmann::json get_recent(int count);
    nlohmann::json ignore_function(const std::string& function_name);
    nlohmann::json ignore_all();
    nlohmann::json clear();
    nlohmann::json clear_ignored();

    // Called from the inline hook on game thread — must be extremely fast
    void on_process_event(void* obj, void* func);

private:
    ProcessEventListener() = default;

    bool find_and_hook_process_event();

    struct FunctionInfo {
        size_t call_count{0};
        std::string name; // cached full name (resolved lazily on game thread)
    };

    std::mutex m_mutex;
    std::unordered_map<void*, FunctionInfo> m_functions;
    std::deque<void*> m_recent; // most-recent UFunction* calls
    std::unordered_set<void*> m_ignored;

    std::atomic<bool> m_listening{false};
    bool m_hooked{false};
    int m_hook_id{-1};

    static constexpr size_t MAX_RECENT = 200;
};
