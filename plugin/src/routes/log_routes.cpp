#include "log_routes.h"
#include "../pipe_server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace LogRoutes {

static void send_json(httplib::Response& res, const json& data, int status = 200) {
    res.status = status;
    res.set_content(data.dump(2), "application/json");
}

void register_routes(httplib::Server& server) {
    // GET /api/log — Read recent plugin log entries
    server.Get("/api/log", [](const httplib::Request& req, httplib::Response& res) {
        int max_entries = 100;
        if (req.has_param("maxEntries")) {
            try { max_entries = std::stoi(req.get_param_value("maxEntries")); } catch (...) {}
        }

        if (max_entries < 1) {
            max_entries = 1;
        }

        auto result = PipeServer::get().get_log_entries(max_entries);
        result["messages"] = PipeServer::get().get_log(max_entries);
        send_json(res, result);
    });

    // DELETE /api/log — Clear the plugin log ring buffer
    server.Delete("/api/log", [](const httplib::Request&, httplib::Response& res) {
        PipeServer::get().clear_log();
        send_json(res, json{{"status", "cleared"}});
    });
}

} // namespace LogRoutes
