#pragma once
namespace httplib { class Server; }
namespace ProcessEventRoutes {
    void register_routes(httplib::Server& server);
}
