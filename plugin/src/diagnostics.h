#pragma once

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

class Diagnostics {
public:
    class ScopedCallback {
    public:
        ScopedCallback(std::string name, std::string renderer = {}, nlohmann::json extra = {});
        ~ScopedCallback();

        void fail(const std::string& error);

    private:
        std::string m_name;
        bool m_resolved{false};
    };

    static Diagnostics& get();

    void initialize();

    void callback_enter(const std::string& name,
                        const std::string& renderer = {},
                        const nlohmann::json& extra = {});
    void callback_success(const std::string& name);
    void callback_failure(const std::string& name, const std::string& error);

    nlohmann::json get_callback_health() const;
    nlohmann::json get_breadcrumb() const;
    std::filesystem::path breadcrumb_path() const;

    static std::string describe_current_exception();

private:
    Diagnostics() = default;

    struct CallbackStats {
        uint64_t invocations{0};
        uint64_t successes{0};
        uint64_t failures{0};
        uint64_t last_duration_us{0};
        uint32_t last_thread_id{0};
        std::string renderer;
        std::string last_error;
        std::string last_success_at;
        std::string last_failure_at;
    };

    struct ActiveCallback {
        std::chrono::steady_clock::time_point started_at;
        std::string renderer;
    };

    void write_breadcrumb_locked(bool force = false);
    static std::string now_local_timestamp();

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, CallbackStats> m_callback_stats;
    std::unordered_map<std::string, ActiveCallback> m_active_callbacks;
    nlohmann::json m_breadcrumb_state;
    std::filesystem::path m_breadcrumb_path;
    std::chrono::steady_clock::time_point m_last_breadcrumb_flush{};
    std::string m_last_breadcrumb_callback;
    std::string m_last_breadcrumb_status;
    bool m_initialized{false};
    uint64_t m_breadcrumb_seq{0};
};
