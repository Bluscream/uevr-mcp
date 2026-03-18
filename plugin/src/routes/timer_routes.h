#pragma once

namespace httplib { class Server; }

namespace TimerRoutes {
    void register_routes(httplib::Server& server);
}
