#pragma once

namespace httplib { class Server; }

namespace MotionControllerRoutes {
    void register_routes(httplib::Server& server);
}
