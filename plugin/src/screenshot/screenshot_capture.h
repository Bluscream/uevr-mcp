#pragma once
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <string>
#include <nlohmann/json.hpp>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

class ScreenshotCapture {
public:
    static ScreenshotCapture& get();

    void initialize(void* device, void* swap_chain, void* command_queue, int renderer_type);
    void on_present(); // Called from render/present thread
    void on_post_render_dx11(ID3D11DeviceContext* context, ID3D11Texture2D* texture); // Preferred for D3D11

    // Called from HTTP thread — blocks until capture completes or timeout
    nlohmann::json capture(int max_width = 640, int max_height = 0, int quality = 75, int timeout_ms = 5000);
    nlohmann::json get_diagnostics();

    // State queries (thread-safe)
    bool is_initialized() const { return m_initialized; }
    int renderer_type() const { return m_renderer_type; }
    std::string last_capture_source();

private:
    ScreenshotCapture() = default;

    void capture_d3d11();
    void capture_d3d12();
    bool capture_d3d11_from_texture(ID3D11DeviceContext* context, ID3D11Texture2D* texture, const char* capture_source);
    void set_error(std::string error);
    void set_result(std::vector<uint8_t>&& bgra, int width, int height, int dxgi_format, int row_pitch, const char* capture_source);

    void* m_device{nullptr};
    void* m_swap_chain{nullptr};
    void* m_command_queue{nullptr};
    int m_renderer_type{0};
    bool m_initialized{false};

    // Synchronization between HTTP thread and present thread
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_capture_requested{false};

    int m_requested_max_width{640};
    int m_requested_max_height{0}; // 0 = auto from aspect ratio

    // Result storage
    std::vector<uint8_t> m_result_bmp;
    int m_result_width{0};
    int m_result_height{0};
    bool m_result_ready{false};
    std::string m_result_error;
    int m_result_dxgi_format{0};
    int m_result_row_pitch{0};
    std::string m_result_capture_source;
    std::string m_last_capture_source;
    int m_last_capture_dxgi_format{0};
    int m_last_capture_row_pitch{0};
    int m_last_capture_width{0};
    int m_last_capture_height{0};
    int m_last_capture_sample_count{1};
    int m_last_capture_sample_quality{0};
    std::chrono::steady_clock::time_point m_last_post_render_dx11{};
    bool m_allow_present_fallback{false};
    std::string m_last_rejected_capture_source;
    std::string m_last_rejected_capture_reason;
};
