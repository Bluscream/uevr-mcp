#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <string>
#include <chrono>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct LogEntry {
    uint64_t seq{0};
    std::string timestamp;
    uint32_t thread_id{0};
    std::string message;
    std::string formatted;
    std::string subsystem;
    std::string callback;
    std::string renderer;
};

class PipeServer {
public:
    static PipeServer& get() {
        static PipeServer instance;
        return instance;
    }

    bool start(const std::string& pipe_name = R"(\\.\pipe\UEVR_MCP)");
    void stop();
    bool is_running() const { return m_running.load(); }

    // Logging ring buffer
    void log(const std::string& message,
             const std::string& subsystem = {},
             const std::string& callback = {},
             const std::string& renderer = {});
    json get_log(int max_entries = 100) const;
    json get_log_entries(int max_entries = 100) const;
    void clear_log();

    // Status info
    void set_game_name(const std::string& name) { m_game_name = name; }
    void set_http_port(int port) { m_http_port.store(port); }
    int get_http_port() const { return m_http_port.load(); }
    std::chrono::steady_clock::time_point start_time() const { return m_start_time; }

private:
    PipeServer() = default;
    ~PipeServer();

    void server_thread_func();
    json handle_request(const json& request);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::string m_pipe_name;
    std::chrono::steady_clock::time_point m_start_time;

    // Log ring buffer
    mutable std::mutex m_log_mutex;
    std::deque<LogEntry> m_log_entries;
    uint64_t m_next_log_seq{1};
    static constexpr size_t MAX_LOG_ENTRIES = 500;

    std::string m_game_name;
    std::atomic<int> m_http_port{0};
};
