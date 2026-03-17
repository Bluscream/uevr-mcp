#include "diagnostics_routes.h"

#include "../diagnostics.h"
#include "../game_thread_queue.h"
#include "../json_helpers.h"
#include "../pipe_server.h"
#include "../screenshot/screenshot_capture.h"

#include <Windows.h>
#include <Psapi.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <uevr/API.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using json = nlohmann::json;
using namespace uevr;

namespace DiagnosticsRoutes {
namespace {

static void send_json(httplib::Response& res, const json& data, int status = 200) {
    res.status = status;
    res.set_content(data.dump(2), "application/json");
}

std::string get_game_exe_name() {
    char exe_path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    return std::filesystem::path(exe_path).filename().string();
}

std::string lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return JsonHelpers::wide_to_utf8(value);
}

std::string normalize_path_key(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    auto final_path = ec ? path.lexically_normal() : normalized.lexically_normal();
    auto wide = final_path.wstring();
    std::transform(wide.begin(), wide.end(), wide.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return JsonHelpers::wide_to_utf8(wide);
}

json read_file_tail(const std::filesystem::path& path, size_t max_lines) {
    json result;
    result["path"] = path.string();

    if (path.empty() || !std::filesystem::exists(path)) {
        result["error"] = "File not found";
        return result;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        result["error"] = "Failed to open file";
        return result;
    }

    std::deque<std::string> tail;
    std::string line;
    while (std::getline(in, line)) {
        tail.push_back(line);
        if (tail.size() > max_lines) {
            tail.pop_front();
        }
    }

    result["lines"] = json::array();
    for (const auto& entry : tail) {
        result["lines"].push_back(entry);
    }
    result["lineCount"] = tail.size();
    return result;
}

json object_identity(API::UObject* obj) {
    if (obj == nullptr) {
        return nullptr;
    }

    json result;
    result["address"] = JsonHelpers::address_to_string(obj);

    auto* cls = obj->get_class();
    if (cls && cls->get_fname()) {
        result["className"] = JsonHelpers::fname_to_string(cls->get_fname());
    }

    result["fullName"] = JsonHelpers::wide_to_utf8(obj->get_full_name());
    return result;
}

json get_runtime_snapshot() {
    return GameThreadQueue::get().submit_and_wait([]() -> json {
        auto& api = API::get();
        if (!api) {
            return json{{"error", "API not available"}};
        }

        json result;
        result["controller"] = object_identity(api->get_player_controller(0));
        result["pawn"] = object_identity(api->get_local_pawn(0));

        auto* world_class = api->find_uobject<API::UClass>(L"Class /Script/Engine.World");
        if (world_class) {
            auto worlds = world_class->get_objects_matching<API::UObject>(false);
            for (auto* obj : worlds) {
                if (!obj || !API::UObjectHook::exists(obj)) {
                    continue;
                }

                auto full_name = JsonHelpers::wide_to_utf8(obj->get_full_name());
                if (full_name.find("Default__") != std::string::npos) {
                    continue;
                }

                result["world"] = object_identity(obj);
                break;
            }
        }

        return result;
    }, 5000);
}

std::set<std::string> enumerate_loaded_module_paths() {
    std::set<std::string> loaded;
    HMODULE modules[2048]{};
    DWORD bytes_needed = 0;

    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes_needed)) {
        return loaded;
    }

    const auto count = std::min<DWORD>(bytes_needed / sizeof(HMODULE), static_cast<DWORD>(std::size(modules)));
    for (DWORD i = 0; i < count; ++i) {
        wchar_t module_path[MAX_PATH]{};
        if (K32GetModuleFileNameExW(GetCurrentProcess(), modules[i], module_path, MAX_PATH) == 0) {
            continue;
        }

        loaded.insert(normalize_path_key(std::filesystem::path(module_path)));
    }

    return loaded;
}

json describe_plugin_file(const std::filesystem::directory_entry& entry,
                          const std::string& directory_type,
                          const std::set<std::string>& loaded_paths) {
    json result;
    result["name"] = entry.path().filename().string();
    result["path"] = entry.path().string();
    result["directoryType"] = directory_type;
    result["loaded"] = loaded_paths.contains(normalize_path_key(entry.path()));

    std::error_code ec;
    const auto file_size = entry.file_size(ec);
    if (!ec) {
        result["size"] = file_size;
    }

    const auto write_time = entry.last_write_time(ec);
    if (!ec) {
        const auto system_now = std::chrono::system_clock::now();
        const auto file_now = std::filesystem::file_time_type::clock::now();
        const auto converted = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            write_time - file_now + system_now);
        const auto tt = std::chrono::system_clock::to_time_t(converted);
        std::tm tm{};
        localtime_s(&tm, &tt);
        char buf[32]{};
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        result["lastWriteTime"] = buf;
    }

    return result;
}

json collect_loaded_plugins() {
    auto& api = API::get();
    json result;

    if (!api) {
        result["error"] = "API not available";
        return result;
    }

    const auto persistent_dir = api->get_persistent_dir();
    const auto game_plugin_dir = persistent_dir / "plugins";
    const auto global_plugin_dir = persistent_dir.parent_path() / "UEVR" / "plugins";

    result["persistentDir"] = persistent_dir.string();
    result["gamePluginDir"] = game_plugin_dir.string();
    result["globalPluginDir"] = global_plugin_dir.string();

    const auto loaded_paths = enumerate_loaded_module_paths();
    json plugins = json::array();

    auto append_dir = [&](const std::filesystem::path& path, const std::string& directory_type) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || !std::filesystem::is_directory(path, ec)) {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
            if (ec || !entry.is_regular_file()) {
                continue;
            }

            auto ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(std::towlower(c));
            });
            if (ext != L".dll") {
                continue;
            }

            plugins.push_back(describe_plugin_file(entry, directory_type, loaded_paths));
        }
    };

    append_dir(global_plugin_dir, "global");
    append_dir(game_plugin_dir, "game");

    result["plugins"] = std::move(plugins);
    result["count"] = result["plugins"].size();
    return result;
}

json latest_crash_dump() {
    json result;
    char* local_appdata_path = nullptr;
    size_t len = 0;
    if (_dupenv_s(&local_appdata_path, &len, "LOCALAPPDATA") != 0 || local_appdata_path == nullptr) {
        result["error"] = "LOCALAPPDATA unavailable";
        return result;
    }

    std::filesystem::path crash_dir = std::filesystem::path(local_appdata_path) / "CrashDumps";
    free(local_appdata_path);

    result["directory"] = crash_dir.string();

    if (!std::filesystem::exists(crash_dir)) {
        result["error"] = "CrashDumps directory not found";
        return result;
    }

    const auto exe_name = get_game_exe_name();
    std::filesystem::path newest_path;
    std::filesystem::file_time_type newest_time{};
    bool found = false;

    for (const auto& entry : std::filesystem::directory_iterator(crash_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const auto filename = entry.path().filename().string();
        if (filename.rfind(exe_name + ".", 0) != 0 || entry.path().extension() != ".dmp") {
            continue;
        }

        std::error_code ec;
        const auto write_time = entry.last_write_time(ec);
        if (ec) {
            continue;
        }

        if (!found || write_time > newest_time) {
            newest_time = write_time;
            newest_path = entry.path();
            found = true;
        }
    }

    if (!found) {
        result["latest"] = nullptr;
        return result;
    }

    std::error_code ec;
    const auto file_size = std::filesystem::file_size(newest_path, ec);
    const auto system_now = std::chrono::system_clock::now();
    const auto file_now = std::filesystem::file_time_type::clock::now();
    const auto converted = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        newest_time - file_now + system_now);
    const auto tt = std::chrono::system_clock::to_time_t(converted);
    std::tm tm{};
    localtime_s(&tm, &tt);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);

    result["latest"] = {
        {"path", newest_path.string()},
        {"timestamp", buf}
    };
    if (!ec) {
        result["latest"]["size"] = file_size;
    }

    return result;
}

json build_snapshot(int max_log_entries, int max_uevr_log_lines) {
    auto& api = API::get();
    json result;

    result["callbackHealth"] = Diagnostics::get().get_callback_health();
    result["breadcrumb"] = Diagnostics::get().get_breadcrumb();
    result["pluginLog"] = PipeServer::get().get_log_entries(max_log_entries);
    result["render"] = ScreenshotCapture::get().get_diagnostics();
    result["loadedPlugins"] = collect_loaded_plugins();
    result["latestCrashDump"] = latest_crash_dump();
    result["runtime"] = get_runtime_snapshot();

    if (api) {
        const auto persistent_dir = api->get_persistent_dir();
        result["paths"] = {
            {"persistentDir", persistent_dir.string()},
            {"uevrLog", (persistent_dir / "log.txt").string()},
            {"breadcrumb", Diagnostics::get().breadcrumb_path().string()}
        };
        result["uevrLogTail"] = read_file_tail(persistent_dir / "log.txt", static_cast<size_t>(max_uevr_log_lines));
    } else {
        result["uevrLogTail"] = json{{"error", "API not available"}};
    }

    return result;
}

} // namespace

void register_routes(httplib::Server& server) {
    server.Get("/api/diagnostics/callbacks", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, Diagnostics::get().get_callback_health());
    });

    server.Get("/api/diagnostics/breadcrumb", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, Diagnostics::get().get_breadcrumb());
    });

    server.Get("/api/diagnostics/render", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, ScreenshotCapture::get().get_diagnostics());
    });

    server.Get("/api/diagnostics/plugins", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, collect_loaded_plugins());
    });

    server.Get("/api/diagnostics/snapshot", [](const httplib::Request& req, httplib::Response& res) {
        int max_log_entries = 150;
        int max_uevr_log_lines = 120;

        if (req.has_param("maxLogEntries")) {
            try { max_log_entries = std::stoi(req.get_param_value("maxLogEntries")); } catch (...) {}
        }
        if (req.has_param("maxUevrLogLines")) {
            try { max_uevr_log_lines = std::stoi(req.get_param_value("maxUevrLogLines")); } catch (...) {}
        }

        send_json(res, build_snapshot(max_log_entries, max_uevr_log_lines));
    });
}

} // namespace DiagnosticsRoutes
