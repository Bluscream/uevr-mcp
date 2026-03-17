#pragma once

#include <httplib.h>

namespace LogRoutes {
    void register_routes(httplib::Server& server);
}
