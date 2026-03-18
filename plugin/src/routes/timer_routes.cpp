#include "timer_routes.h"
#include "../game_thread_queue.h"
#include "../json_helpers.h"
#include "../pipe_server.h"
#include "../lua/lua_engine.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace TimerRoutes {

static void send_json(httplib::Response& res, const json& data, int status = 200) {
    if (status == 200 && data.contains("error")) {
        auto err = data["error"].get<std::string>();
        status = (err.find("timeout") != std::string::npos) ? 504 : 500;
    }
    res.status = status;
    res.set_content(data.dump(2), "application/json");
}

void register_routes(httplib::Server& server) {
    // POST /api/timer/create — Create a timer that runs Lua code after delay
    server.Post("/api/timer/create", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); } catch (...) {
            send_json(res, json{{"error", "Invalid JSON"}}, 400);
            return;
        }

        if (!body.contains("delay") || !body.contains("code")) {
            send_json(res, json{{"error", "Missing 'delay' and/or 'code' parameters"}}, 400);
            return;
        }

        float delay = body["delay"].get<float>();
        auto code = body["code"].get<std::string>();
        bool looping = body.value("looping", false);

        if (delay < 0.001f) {
            send_json(res, json{{"error", "Delay must be at least 0.001 seconds"}}, 400);
            return;
        }

        // Wrap the user's Lua code in a set_timer call. Escape quotes in code.
        // We use a Lua wrapper that creates the timer and returns the ID.
        std::string escaped_code;
        for (char c : code) {
            if (c == '\\') escaped_code += "\\\\";
            else if (c == '"') escaped_code += "\\\"";
            else if (c == '\n') escaped_code += "\\n";
            else if (c == '\r') escaped_code += "\\r";
            else escaped_code += c;
        }

        std::string lua_wrapper =
            "return mcp.set_timer(" + std::to_string(delay) + ", function() " + code + " end, " +
            (looping ? "true" : "false") + ")";

        auto result = GameThreadQueue::get().submit_and_wait([lua_wrapper]() {
            return LuaEngine::get().execute(lua_wrapper);
        });

        if (result.contains("success") && result["success"].get<bool>()) {
            int timer_id = 0;
            if (result.contains("result") && result["result"].is_number()) {
                timer_id = result["result"].get<int>();
            }
            PipeServer::get().log("Timer: created #" + std::to_string(timer_id));
            send_json(res, json{{"success", true}, {"timerId", timer_id}});
        } else {
            send_json(res, result);
        }
    });

    // GET /api/timer/list — List active timers
    server.Get("/api/timer/list", [](const httplib::Request&, httplib::Response& res) {
        // Read the _mcp_timers table from Lua state
        std::string lua_code = R"(
            local result = {}
            local timers = _mcp_timers or {}
            for id, t in pairs(timers) do
                if type(t) == "table" then
                    table.insert(result, {
                        id = t.id,
                        delay = t.delay,
                        remaining = t.remaining,
                        looping = t.looping or false
                    })
                end
            end
            table.sort(result, function(a, b) return a.id < b.id end)
            return result
        )";

        auto result = GameThreadQueue::get().submit_and_wait([&lua_code]() {
            return LuaEngine::get().execute(lua_code);
        });

        if (result.contains("success") && result["success"].get<bool>()) {
            auto timers = result.value("result", json::array());
            send_json(res, json{{"timers", timers}, {"count", timers.size()}});
        } else {
            send_json(res, result);
        }
    });

    // DELETE /api/timer/cancel — Cancel a timer by ID
    server.Delete("/api/timer/cancel", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); } catch (...) {
            send_json(res, json{{"error", "Invalid JSON"}}, 400);
            return;
        }

        if (!body.contains("timerId")) {
            send_json(res, json{{"error", "Missing 'timerId' parameter"}}, 400);
            return;
        }

        int timer_id = body["timerId"].get<int>();
        std::string lua_code = "mcp.clear_timer(" + std::to_string(timer_id) + ")";

        auto result = GameThreadQueue::get().submit_and_wait([&lua_code]() {
            return LuaEngine::get().execute(lua_code);
        });

        PipeServer::get().log("Timer: cancelled #" + std::to_string(timer_id));
        send_json(res, json{{"success", true}, {"timerId", timer_id}, {"cancelled", true}});
    });

    // POST /api/timer/clear — Clear all timers
    server.Post("/api/timer/clear", [](const httplib::Request&, httplib::Response& res) {
        std::string lua_code = "mcp.clear_all_timers()";

        auto result = GameThreadQueue::get().submit_and_wait([&lua_code]() {
            return LuaEngine::get().execute(lua_code);
        });

        PipeServer::get().log("Timer: cleared all");
        send_json(res, json{{"success", true}, {"cleared", true}});
    });
}

} // namespace TimerRoutes
