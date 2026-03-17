#include <winsock2.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>

#include "input_routes.h"
#include "../pipe_server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <thread>
#include <chrono>

using json = nlohmann::json;

namespace InputRoutes {

static char ascii_lower_char(char ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

static std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), ascii_lower_char);
    return value;
}

GamepadOverride& get_gamepad_override() {
    static GamepadOverride s;
    return s;
}

// Find the main game window belonging to this process
static HWND find_game_window() {
    struct Finder {
        DWORD pid;
        HWND result;
    };
    Finder f{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* pf = reinterpret_cast<Finder*>(lp);
        DWORD wndPid;
        GetWindowThreadProcessId(hwnd, &wndPid);
        if (wndPid == pf->pid && IsWindowVisible(hwnd)) {
            pf->result = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.result;
}

static bool focus_game_window(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    ShowWindow(hwnd, SW_RESTORE);

    HWND foreground = GetForegroundWindow();
    DWORD current_thread = GetCurrentThreadId();
    DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;

    if (foreground_thread != 0 && foreground_thread != current_thread) {
        AttachThreadInput(foreground_thread, current_thread, TRUE);
    }

    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    if (foreground_thread != 0 && foreground_thread != current_thread) {
        AttachThreadInput(foreground_thread, current_thread, FALSE);
    }

    return GetForegroundWindow() == hwnd;
}

static bool client_point_to_screen(HWND hwnd, int client_x, int client_y, POINT& out) {
    out.x = client_x;
    out.y = client_y;
    return ClientToScreen(hwnd, &out) == TRUE;
}

static bool get_default_client_center(HWND hwnd, POINT& out_client, POINT& out_screen) {
    RECT rect{};
    if (!GetClientRect(hwnd, &rect)) {
        return false;
    }

    out_client.x = (rect.right - rect.left) / 2;
    out_client.y = (rect.bottom - rect.top) / 2;
    return client_point_to_screen(hwnd, out_client.x, out_client.y, out_screen);
}

static bool send_input_events(INPUT* events, UINT count) {
    return SendInput(count, events, sizeof(INPUT)) == count;
}

static bool send_keyboard_event(UINT vk, bool key_up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyA(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = key_up ? KEYEVENTF_KEYUP : 0;
    return send_input_events(&input, 1);
}

static bool send_mouse_move_abs(POINT screen_point) {
    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);
    if (screen_w <= 1 || screen_h <= 1) {
        return false;
    }

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    input.mi.dx = static_cast<LONG>(std::lround(screen_point.x * 65535.0 / (screen_w - 1)));
    input.mi.dy = static_cast<LONG>(std::lround(screen_point.y * 65535.0 / (screen_h - 1)));
    return send_input_events(&input, 1);
}

static bool send_mouse_button_event(DWORD flag) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    return send_input_events(&input, 1);
}

static bool send_mouse_button_input(HWND hwnd, const std::string& button, const std::string& event, const POINT* client_point, int duration_ms) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    DWORD down_flag = MOUSEEVENTF_LEFTDOWN;
    DWORD up_flag = MOUSEEVENTF_LEFTUP;
    if (button == "right") {
        down_flag = MOUSEEVENTF_RIGHTDOWN;
        up_flag = MOUSEEVENTF_RIGHTUP;
    } else if (button == "middle") {
        down_flag = MOUSEEVENTF_MIDDLEDOWN;
        up_flag = MOUSEEVENTF_MIDDLEUP;
    }

    if (client_point != nullptr) {
        POINT screen_point{};
        if (!client_point_to_screen(hwnd, client_point->x, client_point->y, screen_point)) {
            return false;
        }

        if (!send_mouse_move_abs(screen_point)) {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (event == "press") {
        return send_mouse_button_event(down_flag);
    }

    if (event == "release") {
        return send_mouse_button_event(up_flag);
    }

    const bool down_ok = send_mouse_button_event(down_flag);
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    const bool up_ok = send_mouse_button_event(up_flag);
    return down_ok && up_ok;
}

static bool post_mouse_button_messages(HWND hwnd, const std::string& button, const std::string& event, POINT client_point, int duration_ms) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    UINT down_msg = WM_LBUTTONDOWN;
    UINT up_msg = WM_LBUTTONUP;
    WPARAM down_wparam = MK_LBUTTON;

    if (button == "right") {
        down_msg = WM_RBUTTONDOWN;
        up_msg = WM_RBUTTONUP;
        down_wparam = MK_RBUTTON;
    } else if (button == "middle") {
        down_msg = WM_MBUTTONDOWN;
        up_msg = WM_MBUTTONUP;
        down_wparam = MK_MBUTTON;
    }

    const LPARAM lparam = MAKELPARAM(client_point.x, client_point.y);
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, lparam);

    if (event == "press") {
        return PostMessageW(hwnd, down_msg, down_wparam, lparam) == TRUE;
    }

    if (event == "release") {
        return PostMessageW(hwnd, up_msg, 0, lparam) == TRUE;
    }

    const bool down_ok = PostMessageW(hwnd, down_msg, down_wparam, lparam) == TRUE;
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    const bool up_ok = PostMessageW(hwnd, up_msg, 0, lparam) == TRUE;
    return down_ok && up_ok;
}

// Map common key names to Windows virtual key codes
static UINT key_name_to_vk(const std::string& name) {
    static const std::unordered_map<std::string, UINT> map = {
        // Special keys
        {"space", VK_SPACE}, {"enter", VK_RETURN}, {"return", VK_RETURN},
        {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE},
        {"tab", VK_TAB}, {"backspace", VK_BACK},
        {"delete", VK_DELETE}, {"del", VK_DELETE}, {"insert", VK_INSERT},
        {"home", VK_HOME}, {"end", VK_END},
        {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT},
        // Arrow keys
        {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
        // Modifier keys
        {"shift", VK_SHIFT}, {"lshift", VK_LSHIFT}, {"rshift", VK_RSHIFT},
        {"ctrl", VK_CONTROL}, {"control", VK_CONTROL},
        {"lctrl", VK_LCONTROL}, {"rctrl", VK_RCONTROL},
        {"alt", VK_MENU}, {"lalt", VK_LMENU}, {"ralt", VK_RMENU},
        // Function keys
        {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4},
        {"f5", VK_F5}, {"f6", VK_F6}, {"f7", VK_F7}, {"f8", VK_F8},
        {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
        // Letter keys
        {"a", 'A'}, {"b", 'B'}, {"c", 'C'}, {"d", 'D'}, {"e", 'E'},
        {"f", 'F'}, {"g", 'G'}, {"h", 'H'}, {"i", 'I'}, {"j", 'J'},
        {"k", 'K'}, {"l", 'L'}, {"m", 'M'}, {"n", 'N'}, {"o", 'O'},
        {"p", 'P'}, {"q", 'Q'}, {"r", 'R'}, {"s", 'S'}, {"t", 'T'},
        {"u", 'U'}, {"v", 'V'}, {"w", 'W'}, {"x", 'X'}, {"y", 'Y'}, {"z", 'Z'},
        // Number keys
        {"0", '0'}, {"1", '1'}, {"2", '2'}, {"3", '3'}, {"4", '4'},
        {"5", '5'}, {"6", '6'}, {"7", '7'}, {"8", '8'}, {"9", '9'},
        // Numpad
        {"numpad0", VK_NUMPAD0}, {"numpad1", VK_NUMPAD1}, {"numpad2", VK_NUMPAD2},
        {"numpad3", VK_NUMPAD3}, {"numpad4", VK_NUMPAD4}, {"numpad5", VK_NUMPAD5},
        {"numpad6", VK_NUMPAD6}, {"numpad7", VK_NUMPAD7}, {"numpad8", VK_NUMPAD8},
        {"numpad9", VK_NUMPAD9},
        // Mouse buttons (for reference, not used with WM_KEY*)
        {"lmb", VK_LBUTTON}, {"rmb", VK_RBUTTON}, {"mmb", VK_MBUTTON},
        // Misc
        {"capslock", VK_CAPITAL}, {"numlock", VK_NUMLOCK}, {"scrolllock", VK_SCROLL},
        {"printscreen", VK_SNAPSHOT}, {"pause", VK_PAUSE},
    };

    auto it = map.find(ascii_lower(name));
    return it != map.end() ? it->second : 0;
}

// Resolve a key from JSON — accepts string name or integer VK code
static UINT resolve_vk(const json& key_val) {
    if (key_val.is_number_integer()) {
        return static_cast<UINT>(key_val.get<int>());
    }
    if (key_val.is_string()) {
        auto name = key_val.get<std::string>();
        auto vk = key_name_to_vk(name);
        if (vk != 0) return vk;

        // Single character — use its uppercase ASCII
        if (name.size() == 1) {
            char c = static_cast<char>(toupper(static_cast<unsigned char>(name[0])));
            return static_cast<UINT>(c);
        }
    }
    return 0;
}

// Build lParam for WM_KEYDOWN/WM_KEYUP
static LPARAM make_key_lparam(UINT vk, bool keyup) {
    UINT scancode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    LPARAM lp = 1; // repeat count
    lp |= (static_cast<LPARAM>(scancode) << 16);
    if (keyup) {
        lp |= (1LL << 30); // previous key state
        lp |= (1LL << 31); // transition state
    }
    return lp;
}

static void send_json(httplib::Response& res, const json& data, int status = 200) {
    if (data.contains("error") && status == 200) {
        auto err = data["error"].get<std::string>();
        status = (err.find("timeout") != std::string::npos) ? 504 :
                 (err.find("not found") != std::string::npos) ? 404 : 500;
    }
    res.status = status;
    res.set_content(data.dump(2), "application/json");
}

static int get_duration_ms(const json& body, int default_ms, bool seconds_input = false) {
    auto it = body.find("duration");
    if (it == body.end() || it->is_null()) {
        return default_ms;
    }

    double raw = 0.0;
    if (it->is_number_integer() || it->is_number_unsigned()) {
        raw = static_cast<double>(it->get<int>());
    } else if (it->is_number_float()) {
        raw = it->get<double>();
    } else {
        return default_ms;
    }

    if (seconds_input) {
        raw *= 1000.0;
    }

    const auto duration_ms = static_cast<int>(std::lround(raw));
    return duration_ms < 0 ? 0 : duration_ms;
}

static const json* require_object_body(const httplib::Request& req, httplib::Response& res) {
    static thread_local json body;
    try {
        body = json::parse(req.body);
    } catch (...) {
        send_json(res, json{{"error", "Invalid JSON body"}}, 400);
        return nullptr;
    }

    if (!body.is_object()) {
        send_json(res, json{{"error", "JSON body must be an object"}}, 400);
        return nullptr;
    }

    return &body;
}

static std::string read_string_or(const json& body, const char* key, const std::string& default_value) {
    auto it = body.find(key);
    if (it == body.end() || it->is_null()) {
        return default_value;
    }

    return it->is_string() ? it->get<std::string>() : default_value;
}

static int read_int_or(const json& body, const char* key, int default_value) {
    auto it = body.find(key);
    if (it == body.end() || it->is_null()) {
        return default_value;
    }

    if (it->is_number_integer() || it->is_number_unsigned()) {
        return it->get<int>();
    }

    if (it->is_number_float()) {
        return static_cast<int>(std::lround(it->get<double>()));
    }

    return default_value;
}

static std::optional<float> read_object_float(const json& object, const char* key) {
    auto it = object.find(key);
    if (it == object.end() || it->is_null() || !it->is_number()) {
        return std::nullopt;
    }

    return it->get<float>();
}

static bool try_get_float(const json& body, const char* key, float& out) {
    auto it = body.find(key);
    if (it == body.end() || it->is_null() || !it->is_number()) {
        return false;
    }

    out = it->get<float>();
    return true;
}

void register_routes(httplib::Server& server) {

    // POST /api/input/key — Simulate a keyboard key press/release
    server.Post("/api/input/key", [](const httplib::Request& req, httplib::Response& res) {
        const auto* body = require_object_body(req, res);
        if (!body) {
            return;
        }

        if (!body->contains("key")) {
            send_json(res, json{{"error", "Missing 'key' parameter"}}, 400);
            return;
        }

        auto vk = resolve_vk((*body)["key"]);
        if (vk == 0) {
            send_json(res, json{{"error", "Unknown key: " + (*body)["key"].dump()}}, 400);
            return;
        }

        std::string event = read_string_or(*body, "event", "tap");

        HWND hwnd = find_game_window();
        if (!hwnd) {
            send_json(res, json{{"error", "Game window not found"}}, 500);
            return;
        }

        focus_game_window(hwnd);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        PipeServer::get().log("Input: key " + event + " vk=" + std::to_string(vk));

        bool ok = true;
        if (event == "press") {
            ok = send_keyboard_event(vk, false);
        } else if (event == "release") {
            ok = send_keyboard_event(vk, true);
        } else { // "tap" — press then release
            ok = send_keyboard_event(vk, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(get_duration_ms(*body, 50)));
            ok = ok && send_keyboard_event(vk, true);
        }

        if (!ok) {
            send_json(res, json{{"error", "SendInput failed for keyboard event"}}, 500);
            return;
        }

        send_json(res, json{{"success", true}, {"key", (*body)["key"]}, {"vkCode", vk}, {"event", event}});
    });

    // POST /api/input/mouse — Simulate mouse input
    server.Post("/api/input/mouse", [](const httplib::Request& req, httplib::Response& res) {
        const auto* body = require_object_body(req, res);
        if (!body) {
            return;
        }

        HWND hwnd = find_game_window();
        if (!hwnd) {
            send_json(res, json{{"error", "Game window not found"}}, 500);
            return;
        }

        focus_game_window(hwnd);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Mouse button
        std::string button = read_string_or(*body, "button", "left");
        std::string event = read_string_or(*body, "event", "click");

        // Position for the click (optional — uses current position if not specified).
        // If "move" is provided together with a button event, treat it as the click target.
        bool has_move = body->contains("move") && !(*body)["move"].is_null();
        bool has_explicit_button_event = body->contains("button") || body->contains("event");
        int x = read_int_or(*body, "x", -1);
        int y = read_int_or(*body, "y", -1);
        if (has_move) {
            const auto& mv = (*body)["move"];
            if (!mv.is_object()) {
                send_json(res, json{{"error", "'move' must be an object with x/y fields"}}, 400);
                return;
            }

            int move_x = read_int_or(mv, "x", 0);
            int move_y = read_int_or(mv, "y", 0);

            if (!has_explicit_button_event && x < 0 && y < 0) {
                POINT screen{};
                if (!client_point_to_screen(hwnd, move_x, move_y, screen) || !send_mouse_move_abs(screen)) {
                    send_json(res, json{{"error", "Failed to move mouse cursor"}}, 500);
                    return;
                }
                PipeServer::get().log("Input: mouse move (" + std::to_string(move_x) + "," + std::to_string(move_y) + ")");
                send_json(res, json{{"success", true}, {"action", "move"}, {"x", move_x}, {"y", move_y}});
                return;
            }

            if (x < 0 && y < 0) {
                x = move_x;
                y = move_y;
            }
        }

        POINT client_point{};
        bool has_target_point = false;
        if (x >= 0 && y >= 0) {
            client_point.x = x;
            client_point.y = y;
            has_target_point = true;
        } else {
            POINT unused_screen{};
            if (!get_default_client_center(hwnd, client_point, unused_screen)) {
                send_json(res, json{{"error", "Failed to get default click position"}}, 500);
                return;
            }
        }

        PipeServer::get().log("Input: mouse " + button + " " + event);

        const int duration_ms = get_duration_ms(*body, 50);
        bool ok = send_mouse_button_input(hwnd, button, event, has_target_point ? &client_point : nullptr, duration_ms);
        if (!ok) {
            ok = post_mouse_button_messages(hwnd, button, event, client_point, duration_ms);
        }

        if (!ok) {
            send_json(res, json{{"error", "Mouse input dispatch failed"}}, 500);
            return;
        }

        send_json(res, json{{"success", true}, {"button", button}, {"event", event}, {"x", client_point.x}, {"y", client_point.y}});
    });

    // POST /api/input/gamepad — Simulate gamepad input (stored for next XInput poll)
    server.Post("/api/input/gamepad", [](const httplib::Request& req, httplib::Response& res) {
        const auto* body = require_object_body(req, res);
        if (!body) {
            return;
        }

        auto& gp = get_gamepad_override();
        std::lock_guard<std::mutex> lock(gp.mutex);

        // Map button names to XINPUT_GAMEPAD_ flags
        static const std::unordered_map<std::string, WORD> button_map = {
            {"a", XINPUT_GAMEPAD_A}, {"b", XINPUT_GAMEPAD_B},
            {"x", XINPUT_GAMEPAD_X}, {"y", XINPUT_GAMEPAD_Y},
            {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER},
            {"leftshoulder", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"rightshoulder", XINPUT_GAMEPAD_RIGHT_SHOULDER},
            {"start", XINPUT_GAMEPAD_START}, {"back", XINPUT_GAMEPAD_BACK},
            {"select", XINPUT_GAMEPAD_BACK},
            {"lthumb", XINPUT_GAMEPAD_LEFT_THUMB}, {"rthumb", XINPUT_GAMEPAD_RIGHT_THUMB},
            {"leftthumb", XINPUT_GAMEPAD_LEFT_THUMB}, {"rightthumb", XINPUT_GAMEPAD_RIGHT_THUMB},
            {"dpadup", XINPUT_GAMEPAD_DPAD_UP}, {"dpaddown", XINPUT_GAMEPAD_DPAD_DOWN},
            {"dpadleft", XINPUT_GAMEPAD_DPAD_LEFT}, {"dpadright", XINPUT_GAMEPAD_DPAD_RIGHT},
            {"up", XINPUT_GAMEPAD_DPAD_UP}, {"down", XINPUT_GAMEPAD_DPAD_DOWN},
            {"left", XINPUT_GAMEPAD_DPAD_LEFT}, {"right", XINPUT_GAMEPAD_DPAD_RIGHT},
        };

        // Reset pad state
        XINPUT_GAMEPAD pad{};

        // Buttons
        if (body->contains("buttons") && (*body)["buttons"].is_object()) {
            for (auto& [key, val] : (*body)["buttons"].items()) {
                auto it = button_map.find(ascii_lower(key));
                if (it != button_map.end() && val.is_boolean() && val.get<bool>()) {
                    pad.wButtons |= it->second;
                }
            }
        }

        // Left stick
        if (body->contains("leftStick") && (*body)["leftStick"].is_object()) {
            const auto& left_stick = (*body)["leftStick"];
            float lx = read_object_float(left_stick, "x").value_or(0.0f);
            float ly = read_object_float(left_stick, "y").value_or(0.0f);
            lx = std::clamp(lx, -1.0f, 1.0f);
            ly = std::clamp(ly, -1.0f, 1.0f);
            pad.sThumbLX = static_cast<SHORT>(lx * 32767.0f);
            pad.sThumbLY = static_cast<SHORT>(ly * 32767.0f);
        }

        // Right stick
        if (body->contains("rightStick") && (*body)["rightStick"].is_object()) {
            const auto& right_stick = (*body)["rightStick"];
            float rx = read_object_float(right_stick, "x").value_or(0.0f);
            float ry = read_object_float(right_stick, "y").value_or(0.0f);
            rx = std::clamp(rx, -1.0f, 1.0f);
            ry = std::clamp(ry, -1.0f, 1.0f);
            pad.sThumbRX = static_cast<SHORT>(rx * 32767.0f);
            pad.sThumbRY = static_cast<SHORT>(ry * 32767.0f);
        }

        // Triggers (0.0 to 1.0)
        float lt = 0.0f;
        if (try_get_float(*body, "leftTrigger", lt)) {
            lt = std::clamp(lt, 0.0f, 1.0f);
            pad.bLeftTrigger = static_cast<BYTE>(lt * 255.0f);
        }
        float rt = 0.0f;
        if (try_get_float(*body, "rightTrigger", rt)) {
            rt = std::clamp(rt, 0.0f, 1.0f);
            pad.bRightTrigger = static_cast<BYTE>(rt * 255.0f);
        }

        gp.pad = pad;
        gp.active.store(true, std::memory_order_release);

        // If duration is specified, schedule deactivation
        int duration_ms = get_duration_ms(*body, 0, true);
        if (duration_ms > 0) {
            std::thread([duration_ms]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
                auto& gp = get_gamepad_override();
                gp.active.store(false, std::memory_order_release);
            }).detach();
        }

        PipeServer::get().log("Input: gamepad override set (buttons=0x" +
            ([](WORD w) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%04X", w);
                return std::string(buf);
            })(pad.wButtons) +
            ", duration=" + std::to_string(duration_ms) + "ms)");

        send_json(res, json{
            {"success", true},
            {"buttons", pad.wButtons},
            {"leftStick", {{"x", pad.sThumbLX}, {"y", pad.sThumbLY}}},
            {"rightStick", {{"x", pad.sThumbRX}, {"y", pad.sThumbRY}}},
            {"leftTrigger", pad.bLeftTrigger},
            {"rightTrigger", pad.bRightTrigger},
            {"duration", duration_ms}
        });
    });

    // POST /api/input/text — Type a string of text
    server.Post("/api/input/text", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); } catch (...) {
            send_json(res, json{{"error", "Invalid JSON body"}}, 400);
            return;
        }

        if (!body.contains("text") || !body["text"].is_string()) {
            send_json(res, json{{"error", "Missing 'text' string parameter"}}, 400);
            return;
        }

        auto text = body["text"].get<std::string>();
        if (text.empty()) {
            send_json(res, json{{"error", "Empty text string"}}, 400);
            return;
        }

        HWND hwnd = find_game_window();
        if (!hwnd) {
            send_json(res, json{{"error", "Game window not found"}}, 500);
            return;
        }

        int delay_ms = body.value("delay", 10);

        PipeServer::get().log("Input: text input (" + std::to_string(text.size()) + " chars)");

        // Convert to wide string for WM_CHAR
        int wide_len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wtext(wide_len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &wtext[0], wide_len);

        for (wchar_t ch : wtext) {
            PostMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(ch), 0);
            if (delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }

        send_json(res, json{{"success", true}, {"length", static_cast<int>(wtext.size())}});
    });
}

} // namespace InputRoutes
