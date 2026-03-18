#include "process_event_routes.h"
#include "../game_thread_queue.h"
#include "../json_helpers.h"
#include "../process_event_listener.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace ProcessEventRoutes {

static void send_json(httplib::Response& res, const json& data, int status = 200) {
    if (status == 200 && data.contains("error")) {
        auto err = data["error"].get<std::string>();
        status = (err.find("timeout") != std::string::npos) ? 504 :
                 (err.find("not found") != std::string::npos) ? 404 : 500;
    }
    res.status = status;
    res.set_content(data.dump(2), "application/json");
}

void register_routes(httplib::Server& server) {

    // POST /api/process_event/start — Start the ProcessEvent listener
    server.Post("/api/process_event/start", [](const httplib::Request&, httplib::Response& res) {
        auto result = GameThreadQueue::get().submit_and_wait([]() {
            return ProcessEventListener::get().start();
        });
        send_json(res, result);
    });

    // POST /api/process_event/stop — Stop the ProcessEvent listener
    server.Post("/api/process_event/stop", [](const httplib::Request&, httplib::Response& res) {
        // Toggling the atomic flag doesn't need game thread
        send_json(res, ProcessEventListener::get().stop());
    });

    // GET /api/process_event/status — Get listener status
    server.Get("/api/process_event/status", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, ProcessEventListener::get().status());
    });

    // GET /api/process_event/functions — List all called functions with filtering
    server.Get("/api/process_event/functions", [](const httplib::Request& req, httplib::Response& res) {
        int max_calls = 0;
        int limit = 200;
        std::string search;
        std::string sort = "count";

        if (req.has_param("maxCalls")) {
            try { max_calls = std::stoi(req.get_param_value("maxCalls")); } catch (...) {}
        }
        if (req.has_param("search")) {
            search = req.get_param_value("search");
        }
        if (req.has_param("limit")) {
            try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
        }
        if (req.has_param("sort")) {
            sort = req.get_param_value("sort");
        }

        send_json(res, ProcessEventListener::get().get_functions(max_calls, search, limit, sort));
    });

    // GET /api/process_event/recent — Get most recent ProcessEvent calls
    server.Get("/api/process_event/recent", [](const httplib::Request& req, httplib::Response& res) {
        int count = 50;
        if (req.has_param("count")) {
            try { count = std::stoi(req.get_param_value("count")); } catch (...) {}
        }
        send_json(res, ProcessEventListener::get().get_recent(count));
    });

    // POST /api/process_event/ignore — Ignore functions matching a name pattern
    server.Post("/api/process_event/ignore", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); } catch (...) {
            send_json(res, json{{"error", "Invalid JSON body"}}, 400);
            return;
        }

        auto pattern = body.value("pattern", "");
        if (pattern.empty()) {
            send_json(res, json{{"error", "Missing 'pattern' — substring to match function names"}}, 400);
            return;
        }

        send_json(res, ProcessEventListener::get().ignore_function(pattern));
    });

    // POST /api/process_event/ignore_all — Ignore all currently seen functions
    server.Post("/api/process_event/ignore_all", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, ProcessEventListener::get().ignore_all());
    });

    // POST /api/process_event/clear — Clear all tracked function data
    server.Post("/api/process_event/clear", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, ProcessEventListener::get().clear());
    });

    // POST /api/process_event/clear_ignored — Clear the ignore list
    server.Post("/api/process_event/clear_ignored", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, ProcessEventListener::get().clear_ignored());
    });
}

} // namespace ProcessEventRoutes
