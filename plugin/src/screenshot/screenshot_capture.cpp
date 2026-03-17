#include "screenshot_capture.h"
#include "../json_helpers.h"
#include "../pipe_server.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <objbase.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>

using json = nlohmann::json;

namespace {

struct PixelConversionInfo {
    bool is_rgba8{false};
    bool is_bgra8{false};
    bool is_bgrx8{false};
    bool is_r10g10b10a2{false};
    bool is_r16g16b16a16_float{false};
};

struct FrameContentSummary {
    uint8_t max_rgb{0};
    size_t non_black_pixels{0};
    int min_x{-1};
    int min_y{-1};
    int max_x{-1};
    int max_y{-1};
};

static PixelConversionInfo get_conversion_info(DXGI_FORMAT format, size_t row_pitch, int width) {
    PixelConversionInfo info{};

    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        info.is_rgba8 = true;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        info.is_bgra8 = true;
        break;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        info.is_bgrx8 = true;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        info.is_r10g10b10a2 = true;
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        info.is_r16g16b16a16_float = true;
        break;
    default:
        break;
    }

    // If the format is opaque/unknown but the row pitch strongly suggests FP16, trust the memory layout.
    if (!info.is_rgba8 && !info.is_bgra8 && !info.is_bgrx8 &&
        !info.is_r10g10b10a2 && !info.is_r16g16b16a16_float &&
        row_pitch >= static_cast<size_t>(width) * 8) {
        info.is_r16g16b16a16_float = true;
    }

    if (!info.is_rgba8 && !info.is_bgra8 && !info.is_bgrx8 &&
        !info.is_r10g10b10a2 && !info.is_r16g16b16a16_float) {
        info.is_bgra8 = true;
    }

    return info;
}

static float half_to_float(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;

    if (exp == 0) {
        return sign ? -0.0f : 0.0f;
    }

    if (exp == 31) {
        return sign ? -1e30f : 1e30f;
    }

    float f = std::ldexp(static_cast<float>(mant | 0x400), static_cast<int>(exp) - 25);
    return sign ? -f : f;
}

static void convert_row_to_bgra8(const uint8_t* src_row, uint8_t* dst_row, int width, const PixelConversionInfo& info) {
    if (info.is_r10g10b10a2) {
        const uint32_t* pixels = reinterpret_cast<const uint32_t*>(src_row);
        for (int x = 0; x < width; ++x) {
            uint32_t p = pixels[x];
            uint8_t r = static_cast<uint8_t>(((p >> 0) & 0x3FF) >> 2);
            uint8_t g = static_cast<uint8_t>(((p >> 10) & 0x3FF) >> 2);
            uint8_t b = static_cast<uint8_t>(((p >> 20) & 0x3FF) >> 2);
            dst_row[x * 4 + 0] = b;
            dst_row[x * 4 + 1] = g;
            dst_row[x * 4 + 2] = r;
            dst_row[x * 4 + 3] = 255;
        }
        return;
    }

    if (info.is_r16g16b16a16_float) {
        const uint16_t* pixels = reinterpret_cast<const uint16_t*>(src_row);
        for (int x = 0; x < width; ++x) {
            float r = half_to_float(pixels[x * 4 + 0]);
            float g = half_to_float(pixels[x * 4 + 1]);
            float b = half_to_float(pixels[x * 4 + 2]);

            // Simple tone map into SDR.
            r = r / (1.0f + r);
            g = g / (1.0f + g);
            b = b / (1.0f + b);

            dst_row[x * 4 + 0] = static_cast<uint8_t>(b > 1.0f ? 255 : (b < 0.0f ? 0 : static_cast<int>(b * 255.0f)));
            dst_row[x * 4 + 1] = static_cast<uint8_t>(g > 1.0f ? 255 : (g < 0.0f ? 0 : static_cast<int>(g * 255.0f)));
            dst_row[x * 4 + 2] = static_cast<uint8_t>(r > 1.0f ? 255 : (r < 0.0f ? 0 : static_cast<int>(r * 255.0f)));
            dst_row[x * 4 + 3] = 255;
        }
        return;
    }

    if (info.is_rgba8) {
        for (int x = 0; x < width; ++x) {
            dst_row[x * 4 + 0] = src_row[x * 4 + 2];
            dst_row[x * 4 + 1] = src_row[x * 4 + 1];
            dst_row[x * 4 + 2] = src_row[x * 4 + 0];
            dst_row[x * 4 + 3] = src_row[x * 4 + 3];
        }
        return;
    }

    if (info.is_bgrx8) {
        for (int x = 0; x < width; ++x) {
            dst_row[x * 4 + 0] = src_row[x * 4 + 0];
            dst_row[x * 4 + 1] = src_row[x * 4 + 1];
            dst_row[x * 4 + 2] = src_row[x * 4 + 2];
            dst_row[x * 4 + 3] = 255;
        }
        return;
    }

    std::memcpy(dst_row, src_row, static_cast<size_t>(width) * 4);
}

static FrameContentSummary summarize_bgra8(const std::vector<uint8_t>& bgra, int width, int height) {
    FrameContentSummary summary{};
    const size_t pixel_count = bgra.size() / 4;
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t b = bgra[i * 4 + 0];
        const uint8_t g = bgra[i * 4 + 1];
        const uint8_t r = bgra[i * 4 + 2];
        uint8_t max_rgb = r;
        if (g > max_rgb) max_rgb = g;
        if (b > max_rgb) max_rgb = b;
        if (max_rgb > summary.max_rgb) {
            summary.max_rgb = max_rgb;
        }
        if (max_rgb > 4) {
            ++summary.non_black_pixels;
            const int x = static_cast<int>(i % static_cast<size_t>(width));
            const int y = static_cast<int>(i / static_cast<size_t>(width));
            if (summary.min_x < 0 || x < summary.min_x) summary.min_x = x;
            if (summary.min_y < 0 || y < summary.min_y) summary.min_y = y;
            if (summary.max_x < 0 || x > summary.max_x) summary.max_x = x;
            if (summary.max_y < 0 || y > summary.max_y) summary.max_y = y;
        }
    }
    return summary;
}

static bool looks_blank_post_render_frame(const FrameContentSummary& summary, int width, int height) {
    if (summary.max_rgb <= 4 || summary.non_black_pixels < 32 ||
        summary.min_x < 0 || summary.min_y < 0 || summary.max_x < 0 || summary.max_y < 0) {
        return true;
    }

    const double total_pixels = static_cast<double>(width) * static_cast<double>(height);
    const double coverage_ratio = static_cast<double>(summary.non_black_pixels) / total_pixels;
    const double bbox_width = static_cast<double>(summary.max_x - summary.min_x + 1);
    const double bbox_height = static_cast<double>(summary.max_y - summary.min_y + 1);
    const double bbox_area_ratio = (bbox_width * bbox_height) / total_pixels;

    // The broken RoboQuest post-render path returns a tiny UEVR overlay island on a black texture.
    if (coverage_ratio < 0.12 && bbox_area_ratio < 0.20) {
        return true;
    }

    return false;
}

static std::string format_hresult(HRESULT hr) {
    char buf[16]{};
    snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned>(hr));
    return std::string(buf);
}

} // namespace

// ---------- Box-filter downscale (averages source pixels per dest pixel) ----------

static std::vector<uint8_t> downscale_box(const uint8_t* src, int src_w, int src_h, int dst_w, int dst_h) {
    std::vector<uint8_t> dst(dst_w * dst_h * 4);
    for (int y = 0; y < dst_h; ++y) {
        int sy0 = y * src_h / dst_h;
        int sy1 = (y + 1) * src_h / dst_h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > src_h) sy1 = src_h;
        for (int x = 0; x < dst_w; ++x) {
            int sx0 = x * src_w / dst_w;
            int sx1 = (x + 1) * src_w / dst_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > src_w) sx1 = src_w;
            // Average all source pixels in [sx0,sx1) x [sy0,sy1)
            uint32_t rb = 0, gb = 0, bb = 0, ab = 0, count = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                for (int sx = sx0; sx < sx1; ++sx) {
                    const uint8_t* sp = src + (sy * src_w + sx) * 4;
                    bb += sp[0]; gb += sp[1]; rb += sp[2]; ab += sp[3];
                    ++count;
                }
            }
            uint8_t* dp = dst.data() + (y * dst_w + x) * 4;
            dp[0] = (uint8_t)(bb / count);
            dp[1] = (uint8_t)(gb / count);
            dp[2] = (uint8_t)(rb / count);
            dp[3] = (uint8_t)(ab / count);
        }
    }
    return dst;
}

// ---------- JPEG encoding via WIC (Windows Imaging Component) ----------

static std::vector<uint8_t> encode_jpeg(const uint8_t* bgra, int w, int h, int quality) {
    std::vector<uint8_t> result;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return result;

    IStream* stream = nullptr;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr) || !stream) { factory->Release(); return result; }

    IWICBitmapEncoder* encoder = nullptr;
    hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (FAILED(hr) || !encoder) { stream->Release(); factory->Release(); return result; }

    encoder->Initialize(stream, WICBitmapEncoderNoCache);

    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    encoder->CreateNewFrame(&frame, &props);

    // Set JPEG quality (0.0 to 1.0)
    if (props) {
        PROPBAG2 option{};
        wchar_t name[] = L"ImageQuality";
        option.pstrName = name;
        VARIANT val;
        VariantInit(&val);
        val.vt = VT_R4;
        val.fltVal = static_cast<float>(quality) / 100.0f;
        props->Write(1, &option, &val);
    }

    frame->Initialize(props);
    frame->SetSize(w, h);

    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    frame->WritePixels(h, w * 4, w * h * 4, const_cast<BYTE*>(bgra));
    frame->Commit();
    encoder->Commit();

    // Read encoded bytes from stream
    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    result.resize(stat.cbSize.LowPart);
    ULONG bytes_read = 0;
    stream->Read(result.data(), static_cast<ULONG>(result.size()), &bytes_read);
    result.resize(bytes_read);

    if (frame) frame->Release();
    if (props) props->Release();
    encoder->Release();
    stream->Release();
    factory->Release();
    return result;
}

// ---------- Base64 encoding ----------

static std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    size_t len = data.size();
    result.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t octet_a = data[i];
        uint32_t octet_b = (i + 1 < len) ? data[i + 1] : 0;
        uint32_t octet_c = (i + 2 < len) ? data[i + 2] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        result += table[(triple >> 18) & 0x3F];
        result += table[(triple >> 12) & 0x3F];
        result += (i + 1 < len) ? table[(triple >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? table[triple & 0x3F] : '=';
    }

    return result;
}

// ---------- ScreenshotCapture implementation ----------

ScreenshotCapture& ScreenshotCapture::get() {
    static ScreenshotCapture instance;
    return instance;
}

void ScreenshotCapture::initialize(void* device, void* swap_chain, void* command_queue, int renderer_type) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device = device;
    m_swap_chain = swap_chain;
    m_command_queue = command_queue;
    m_renderer_type = renderer_type;
    m_initialized = true;
}

std::string ScreenshotCapture::last_capture_source() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_capture_source;
}

void ScreenshotCapture::set_error(std::string error) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result_ready = false;
        m_result_error = std::move(error);
        m_allow_present_fallback = false;
        m_capture_requested.store(false);
    }
    m_cv.notify_all();
}

void ScreenshotCapture::set_result(std::vector<uint8_t>&& bgra, int width, int height, int dxgi_format, int row_pitch, const char* capture_source) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result_bmp = std::move(bgra);
        m_result_width = width;
        m_result_height = height;
        m_result_dxgi_format = dxgi_format;
        m_result_row_pitch = row_pitch;
        m_result_capture_source = capture_source != nullptr ? capture_source : "";
        m_last_capture_source = m_result_capture_source;
        m_last_capture_dxgi_format = dxgi_format;
        m_last_capture_row_pitch = row_pitch;
        m_last_capture_width = width;
        m_last_capture_height = height;
        m_result_ready = true;
        m_result_error.clear();
        m_allow_present_fallback = false;
        m_last_rejected_capture_source.clear();
        m_last_rejected_capture_reason.clear();
        m_capture_requested.store(false);
    }
    m_cv.notify_all();
}

json ScreenshotCapture::get_diagnostics() {
    void* device = nullptr;
    void* swap_chain = nullptr;
    int renderer_type = 0;
    bool initialized = false;
    bool capture_requested = false;
    std::string last_source;
    int last_dxgi_format = 0;
    int last_row_pitch = 0;
    int last_width = 0;
    int last_height = 0;
    int last_sample_count = 1;
    int last_sample_quality = 0;
    bool post_render_dx11_recent = false;
    bool allow_present_fallback = false;
    std::string last_rejected_source;
    std::string last_rejected_reason;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        device = m_device;
        swap_chain = m_swap_chain;
        renderer_type = m_renderer_type;
        initialized = m_initialized;
        capture_requested = m_capture_requested.load();
        last_source = m_last_capture_source;
        last_dxgi_format = m_last_capture_dxgi_format;
        last_row_pitch = m_last_capture_row_pitch;
        last_width = m_last_capture_width;
        last_height = m_last_capture_height;
        last_sample_count = m_last_capture_sample_count;
        last_sample_quality = m_last_capture_sample_quality;
        post_render_dx11_recent = m_last_post_render_dx11.time_since_epoch().count() != 0 &&
                                  (std::chrono::steady_clock::now() - m_last_post_render_dx11) < std::chrono::milliseconds(250);
        allow_present_fallback = m_allow_present_fallback;
        last_rejected_source = m_last_rejected_capture_source;
        last_rejected_reason = m_last_rejected_capture_reason;
    }

    json result;
    result["initialized"] = initialized;
    result["rendererType"] = renderer_type == 0 ? "D3D11" : (renderer_type == 1 ? "D3D12" : "Unknown");
    result["rendererTypeCode"] = renderer_type;
    result["captureRequested"] = capture_requested;
    result["preferredCapturePath"] = renderer_type == 0 ? "post_render_dx11" : "present_fallback";
    if (renderer_type == 0) result["postRenderDx11Active"] = post_render_dx11_recent;
    if (allow_present_fallback) result["presentFallbackArmed"] = true;
    if (!last_source.empty()) result["lastCaptureSource"] = last_source;
    if (last_dxgi_format != 0) result["lastCaptureDxgiFormat"] = last_dxgi_format;
    if (last_row_pitch != 0) result["lastCaptureRowPitch"] = last_row_pitch;
    if (last_width != 0) result["lastCaptureWidth"] = last_width;
    if (last_height != 0) result["lastCaptureHeight"] = last_height;
    if (last_sample_count > 0) result["lastCaptureSampleCount"] = last_sample_count;
    result["lastCaptureSampleQuality"] = last_sample_quality;
    if (!last_rejected_source.empty()) result["lastRejectedCaptureSource"] = last_rejected_source;
    if (!last_rejected_reason.empty()) result["lastRejectedCaptureReason"] = last_rejected_reason;

    if (!initialized || !swap_chain) {
        return result;
    }

    if (renderer_type == 0) {
        IDXGISwapChain* sc = static_cast<IDXGISwapChain*>(swap_chain);
        ID3D11Texture2D* backbuffer = nullptr;
        auto hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));
        if (SUCCEEDED(hr) && backbuffer != nullptr) {
            D3D11_TEXTURE2D_DESC desc{};
            backbuffer->GetDesc(&desc);
            result["currentTarget"] = {
                {"width", desc.Width},
                {"height", desc.Height},
                {"dxgiFormat", static_cast<int>(desc.Format)},
                {"sampleCount", desc.SampleDesc.Count},
                {"sampleQuality", desc.SampleDesc.Quality},
                {"source", "swapchain_backbuffer"}
            };
            backbuffer->Release();
        } else {
            result["currentTargetError"] = "Failed to query D3D11 backbuffer (HRESULT=0x" + format_hresult(hr) + ")";
        }
    } else if (renderer_type == 1) {
        IDXGISwapChain3* sc = nullptr;
        auto hr = static_cast<IUnknown*>(swap_chain)->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc));
        if (SUCCEEDED(hr) && sc != nullptr) {
            const auto backbuffer_index = sc->GetCurrentBackBufferIndex();
            ID3D12Resource* backbuffer = nullptr;
            hr = sc->GetBuffer(backbuffer_index, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&backbuffer));
            if (SUCCEEDED(hr) && backbuffer != nullptr) {
                auto desc = backbuffer->GetDesc();
                result["currentTarget"] = {
                    {"backbufferIndex", backbuffer_index},
                    {"width", desc.Width},
                    {"height", desc.Height},
                    {"dxgiFormat", static_cast<int>(desc.Format)},
                    {"sampleCount", desc.SampleDesc.Count},
                    {"sampleQuality", desc.SampleDesc.Quality},
                    {"source", "swapchain_backbuffer"}
                };
                backbuffer->Release();
            } else {
                result["currentTargetError"] = "Failed to query D3D12 backbuffer (HRESULT=0x" + format_hresult(hr) + ")";
            }
            sc->Release();
        } else {
            result["currentTargetError"] = "Failed to query IDXGISwapChain3 (HRESULT=0x" + format_hresult(hr) + ")";
        }
    }

    if (device != nullptr) {
        result["deviceAddress"] = JsonHelpers::address_to_string(device);
    }
    if (swap_chain != nullptr) {
        result["swapChainAddress"] = JsonHelpers::address_to_string(swap_chain);
    }

    return result;
}

json ScreenshotCapture::capture(int max_width, int max_height, int quality, int timeout_ms) {
    if (!m_initialized) {
        return json{{"error", "Screenshot capture not initialized. Call initialize() first."}};
    }

    // UEVR_RENDERER_D3D11 = 0, UEVR_RENDERER_D3D12 = 1
    if (m_renderer_type != 0 && m_renderer_type != 1) {
        return json{{"error", "Unknown renderer type: " + std::to_string(m_renderer_type)}};
    }

    // Set up the request
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requested_max_width = max_width;
        m_requested_max_height = max_height;
        m_result_ready = false;
        m_result_error.clear();
        m_result_bmp.clear();
        m_result_width = 0;
        m_result_height = 0;
        m_allow_present_fallback = false;
        m_last_rejected_capture_source.clear();
        m_last_rejected_capture_reason.clear();
    }

    // Signal that a capture is requested
    m_capture_requested.store(true);

    // Wait for the present thread to complete the capture
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        bool ok = m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
            return m_result_ready || !m_result_error.empty();
        });

        if (!ok) {
            m_capture_requested.store(false);
            std::string error = "Screenshot capture timeout after " + std::to_string(timeout_ms) + "ms — no render callback received";
            if (!m_last_rejected_capture_reason.empty()) {
                error += " (last rejected capture: " + m_last_rejected_capture_reason + ")";
            }
            return json{{"error", error}};
        }

        if (!m_result_error.empty()) {
            return json{{"error", m_result_error}};
        }
    }

    // Determine output dimensions
    int out_w = m_result_width;
    int out_h = m_result_height;
    if (max_width > 0 && out_w > max_width) {
        double scale = (double)max_width / (double)out_w;
        out_w = max_width;
        out_h = (int)(m_result_height * scale);
        if (out_h < 1) out_h = 1;
    }
    if (max_height > 0 && out_h > max_height) {
        double scale = (double)max_height / (double)out_h;
        out_h = max_height;
        out_w = (int)(out_w * scale);
        if (out_w < 1) out_w = 1;
    }

    // Use WIC for scaling + JPEG encoding (high-quality Fant interpolation)
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return json{{"error", "WIC factory creation failed"}};

    // Create bitmap from raw pixels
    IWICBitmap* bitmap = nullptr;
    hr = factory->CreateBitmapFromMemory(m_result_width, m_result_height, GUID_WICPixelFormat32bppBGRA,
                                          m_result_width * 4, (UINT)(m_result_width * m_result_height * 4),
                                          m_result_bmp.data(), &bitmap);
    if (FAILED(hr) || !bitmap) { factory->Release(); return json{{"error", "WIC bitmap creation failed"}}; }

    // Scale if needed
    IWICBitmapSource* source = bitmap;
    IWICBitmapScaler* scaler = nullptr;
    if (out_w != m_result_width || out_h != m_result_height) {
        factory->CreateBitmapScaler(&scaler);
        scaler->Initialize(bitmap, out_w, out_h, WICBitmapInterpolationModeFant);
        source = scaler;
    }

    // Encode to JPEG
    IStream* stream = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    IWICBitmapEncoder* encoder = nullptr;
    factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    encoder->Initialize(stream, WICBitmapEncoderNoCache);

    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    encoder->CreateNewFrame(&frame, &props);

    if (props) {
        PROPBAG2 option{};
        wchar_t name[] = L"ImageQuality";
        option.pstrName = name;
        VARIANT val;
        VariantInit(&val);
        val.vt = VT_R4;
        val.fltVal = static_cast<float>(quality) / 100.0f;
        props->Write(1, &option, &val);
    }

    frame->Initialize(props);
    frame->SetSize(out_w, out_h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    frame->WriteSource(source, nullptr); // WIC handles format conversion + scaling
    frame->Commit();
    encoder->Commit();

    // Read encoded bytes
    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    std::vector<uint8_t> jpeg_data(stat.cbSize.LowPart);
    ULONG bytes_read = 0;
    stream->Read(jpeg_data.data(), (ULONG)jpeg_data.size(), &bytes_read);
    jpeg_data.resize(bytes_read);

    // Cleanup
    if (frame) frame->Release();
    if (props) props->Release();
    encoder->Release();
    stream->Release();
    if (scaler) scaler->Release();
    bitmap->Release();
    factory->Release();

    if (jpeg_data.empty()) return json{{"error", "JPEG encoding failed"}};

    auto b64 = base64_encode(jpeg_data);

    json result;
    result["width"] = out_w;
    result["height"] = out_h;
    result["format"] = "jpeg";
    result["quality"] = quality;
    result["size"] = (int)jpeg_data.size();
    result["sourceWidth"] = m_result_width;
    result["sourceHeight"] = m_result_height;
    result["sourceDxgiFormat"] = m_result_dxgi_format;
    result["sourceRowPitch"] = m_result_row_pitch;
    result["captureSource"] = m_result_capture_source;
    result["data"] = b64;
    return result;
}

void ScreenshotCapture::on_post_render_dx11(ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_post_render_dx11 = std::chrono::steady_clock::now();
    }

    if (!m_capture_requested.load() || m_renderer_type != 0) {
        return;
    }

    capture_d3d11_from_texture(context, texture, "post_render_dx11");
}

void ScreenshotCapture::on_present() {
    if (!m_capture_requested.load()) {
        return;
    }

    if (m_renderer_type == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (!m_allow_present_fallback &&
            m_last_post_render_dx11.time_since_epoch().count() != 0 &&
            now - m_last_post_render_dx11 < std::chrono::milliseconds(250)) {
            return;
        }
    }

    if (m_renderer_type == 0) { // UEVR_RENDERER_D3D11
        capture_d3d11();
    } else if (m_renderer_type == 1) { // UEVR_RENDERER_D3D12
        capture_d3d12();
    } else {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result_error = "Unsupported renderer type: " + std::to_string(m_renderer_type);
        m_capture_requested.store(false);
        m_cv.notify_all();
    }
}

bool ScreenshotCapture::capture_d3d11_from_texture(ID3D11DeviceContext* context, ID3D11Texture2D* texture, const char* capture_source) {
    if (!texture) {
        set_error("Failed to capture D3D11 texture: source texture was null");
        return false;
    }

    ID3D11Device* device = nullptr;
    texture->GetDevice(&device);
    if (!device) {
        set_error("Failed to capture D3D11 texture: source texture had no device");
        return false;
    }

    ID3D11DeviceContext* effective_context = context;
    bool owns_context = false;
    if (!effective_context) {
        device->GetImmediateContext(&effective_context);
        owns_context = true;
    }

    if (!effective_context) {
        device->Release();
        set_error("Failed to capture D3D11 texture: no device context available");
        return false;
    }

    D3D11_TEXTURE2D_DESC src_desc{};
    texture->GetDesc(&src_desc);
    auto conversion_info = get_conversion_info(src_desc.Format, 0, static_cast<int>(src_desc.Width));

    D3D11_TEXTURE2D_DESC staging_desc{};
    staging_desc.Width = src_desc.Width;
    staging_desc.Height = src_desc.Height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = src_desc.Format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.SampleDesc.Quality = 0;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr) || !staging) {
        if (owns_context) effective_context->Release();
        device->Release();
        set_error("Failed to create D3D11 staging texture (HRESULT=0x" + format_hresult(hr) + ")");
        return false;
    }

    ID3D11Texture2D* resolved = nullptr;
    if (src_desc.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC resolve_desc = src_desc;
        resolve_desc.SampleDesc.Count = 1;
        resolve_desc.SampleDesc.Quality = 0;
        resolve_desc.Usage = D3D11_USAGE_DEFAULT;
        resolve_desc.BindFlags = 0;
        resolve_desc.CPUAccessFlags = 0;
        resolve_desc.MiscFlags = 0;

        hr = device->CreateTexture2D(&resolve_desc, nullptr, &resolved);
        if (FAILED(hr) || !resolved) {
            staging->Release();
            if (owns_context) effective_context->Release();
            device->Release();
            set_error("Failed to create D3D11 resolve texture (HRESULT=0x" + format_hresult(hr) + ")");
            return false;
        }

        effective_context->ResolveSubresource(resolved, 0, texture, 0, src_desc.Format);
        effective_context->CopyResource(staging, resolved);
    } else {
        effective_context->CopyResource(staging, texture);
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = effective_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        if (resolved) resolved->Release();
        staging->Release();
        if (owns_context) effective_context->Release();
        device->Release();
        set_error("Failed to map D3D11 staging texture (HRESULT=0x" + format_hresult(hr) + ")");
        return false;
    }

    int src_w = static_cast<int>(src_desc.Width);
    int src_h = static_cast<int>(src_desc.Height);
    auto row_pitch = static_cast<int>(mapped.RowPitch);
    std::vector<uint8_t> src_bgra(static_cast<size_t>(src_w) * static_cast<size_t>(src_h) * 4);
    for (int y = 0; y < src_h; ++y) {
        const uint8_t* src_row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        uint8_t* dst_row = src_bgra.data() + static_cast<size_t>(y) * static_cast<size_t>(src_w) * 4;
        convert_row_to_bgra8(src_row, dst_row, src_w, conversion_info);
    }

    effective_context->Unmap(staging, 0);

    if (resolved) resolved->Release();
    staging->Release();
    if (owns_context) effective_context->Release();
    device->Release();

    if (capture_source != nullptr && std::strcmp(capture_source, "post_render_dx11") == 0) {
        const auto summary = summarize_bgra8(src_bgra, src_w, src_h);
        if (looks_blank_post_render_frame(summary, src_w, src_h)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_allow_present_fallback = true;
            m_last_rejected_capture_source = capture_source;
            m_last_rejected_capture_reason =
                "blank post-render frame (maxRgb=" + std::to_string(summary.max_rgb) +
                ", nonBlackPixels=" + std::to_string(summary.non_black_pixels) +
                ", bbox=" + std::to_string(summary.min_x) + "," + std::to_string(summary.min_y) +
                "-" + std::to_string(summary.max_x) + "," + std::to_string(summary.max_y) + ")";
            PipeServer::get().log(
                "Screenshot: rejected blank post_render_dx11 frame; falling back to present",
                "Screenshot", "capture", "D3D11");
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_capture_sample_count = static_cast<int>(src_desc.SampleDesc.Count);
        m_last_capture_sample_quality = static_cast<int>(src_desc.SampleDesc.Quality);
    }

    set_result(std::move(src_bgra), src_w, src_h, static_cast<int>(src_desc.Format), row_pitch, capture_source);
    return true;
}

void ScreenshotCapture::capture_d3d11() {
    IDXGISwapChain* swapchain = static_cast<IDXGISwapChain*>(m_swap_chain);
    ID3D11Texture2D* backbuffer = nullptr;
    HRESULT hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);
    if (FAILED(hr) || !backbuffer) {
        set_error("Failed to get backbuffer from swap chain (HRESULT=0x" + format_hresult(hr) + ")");
        return;
    }

    capture_d3d11_from_texture(nullptr, backbuffer, "present_fallback_d3d11");
    backbuffer->Release();
}

void ScreenshotCapture::capture_d3d12() {
    ID3D12Device* device = static_cast<ID3D12Device*>(m_device);
    IDXGISwapChain3* swapchain = nullptr;

    // The swap chain from UEVR might be IDXGISwapChain1/3/4 — QI for IDXGISwapChain3
    HRESULT hr = static_cast<IUnknown*>(m_swap_chain)->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&swapchain);
    if (FAILED(hr) || !swapchain) {
        // Try as plain IDXGISwapChain
        IDXGISwapChain* sc1 = static_cast<IDXGISwapChain*>(m_swap_chain);
        hr = sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&swapchain);
        if (FAILED(hr) || !swapchain) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_result_error = "Failed to get IDXGISwapChain3";
            m_capture_requested.store(false);
            m_cv.notify_all();
            return;
        }
    }

    UINT backbuffer_idx = swapchain->GetCurrentBackBufferIndex();

    ID3D12Resource* backbuffer = nullptr;
    hr = swapchain->GetBuffer(backbuffer_idx, __uuidof(ID3D12Resource), (void**)&backbuffer);
    if (FAILED(hr) || !backbuffer) {
        swapchain->Release();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result_error = "Failed to get D3D12 backbuffer";
        m_capture_requested.store(false);
        m_cv.notify_all();
        return;
    }

    D3D12_RESOURCE_DESC bb_desc = backbuffer->GetDesc();
    int src_w = (int)bb_desc.Width;
    int src_h = (int)bb_desc.Height;

    // Get the actual GPU footprint for proper row pitch alignment
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size_bytes = 0;
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&bb_desc, 0, 1, 0, &footprint, &num_rows, &row_size_bytes, &total_bytes);

    UINT64 row_pitch = footprint.Footprint.RowPitch;

    // Create a readback buffer (heap)
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC readback_desc{};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = total_bytes;
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.Format = DXGI_FORMAT_UNKNOWN;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* readback = nullptr;
    hr = device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          __uuidof(ID3D12Resource), (void**)&readback);
    if (FAILED(hr) || !readback) {
        backbuffer->Release();
        swapchain->Release();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result_error = "Failed to create D3D12 readback buffer";
        m_capture_requested.store(false);
        m_cv.notify_all();
        return;
    }

    // Create a command allocator and command list for the copy
    ID3D12CommandAllocator* alloc = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&alloc);
    if (FAILED(hr) || !alloc) {
        readback->Release();
        backbuffer->Release();
        swapchain->Release();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result_error = "Failed to create D3D12 command allocator";
        m_capture_requested.store(false);
        m_cv.notify_all();
        return;
    }

    ID3D12GraphicsCommandList* cmd = nullptr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                    __uuidof(ID3D12GraphicsCommandList), (void**)&cmd);
    if (FAILED(hr) || !cmd) {
        alloc->Release();
        readback->Release();
        backbuffer->Release();
        swapchain->Release();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result_error = "Failed to create D3D12 command list";
        m_capture_requested.store(false);
        m_cv.notify_all();
        return;
    }

    // Transition backbuffer to COPY_SOURCE
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);

    // Copy backbuffer to readback
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = backbuffer;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = readback;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = footprint;

    cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    // Transition backbuffer back to PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd->ResourceBarrier(1, &barrier);

    cmd->Close();

    // Use UEVR's command queue (or create one as fallback)
    ID3D12CommandQueue* queue = nullptr;
    bool own_queue = false;
    if (m_command_queue) {
        queue = static_cast<ID3D12CommandQueue*>(m_command_queue);
    } else {
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), (void**)&queue);
        own_queue = true;
        if (FAILED(hr) || !queue) {
            cmd->Release();
            alloc->Release();
            readback->Release();
            backbuffer->Release();
            swapchain->Release();
            std::lock_guard<std::mutex> lock(m_mutex);
            m_result_error = "Failed to create D3D12 command queue";
            m_capture_requested.store(false);
            m_cv.notify_all();
            return;
        }
    }

    ID3D12Fence* fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence);
    HANDLE fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    ID3D12CommandList* lists[] = { cmd };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, 1);
    fence->SetEventOnCompletion(1, fence_event);
    WaitForSingleObject(fence_event, 5000);
    CloseHandle(fence_event);

    // Map readback and read pixels
    void* mapped = nullptr;
    D3D12_RANGE read_range{0, total_bytes};
    hr = readback->Map(0, &read_range, &mapped);
    if (FAILED(hr) || !mapped) {
        fence->Release();
        if (own_queue) queue->Release();
        cmd->Release();
        alloc->Release();
        readback->Release();
        backbuffer->Release();
        swapchain->Release();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result_error = "Failed to map D3D12 readback buffer";
        m_capture_requested.store(false);
        m_cv.notify_all();
        return;
    }

    auto conversion_info = get_conversion_info(bb_desc.Format, static_cast<size_t>(row_size_bytes), src_w);

    std::vector<uint8_t> src_bgra(src_w * src_h * 4);
    for (int y = 0; y < src_h; ++y) {
        const uint8_t* src_row = static_cast<const uint8_t*>(mapped) + y * row_pitch;
        uint8_t* dst_row = src_bgra.data() + y * src_w * 4;
        convert_row_to_bgra8(src_row, dst_row, src_w, conversion_info);
    }

    D3D12_RANGE empty_range{0, 0};
    readback->Unmap(0, &empty_range);

    // Cleanup D3D12 resources
    fence->Release();
    if (own_queue) queue->Release();
    cmd->Release();
    alloc->Release();
    readback->Release();
    backbuffer->Release();
    swapchain->Release();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_capture_sample_count = static_cast<int>(bb_desc.SampleDesc.Count);
        m_last_capture_sample_quality = static_cast<int>(bb_desc.SampleDesc.Quality);
    }

    set_result(std::move(src_bgra), src_w, src_h, static_cast<int>(bb_desc.Format), static_cast<int>(row_pitch), "present_fallback_d3d12");
}
