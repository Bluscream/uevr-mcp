#pragma once

#include <httplib.h>

namespace DiagnosticsRoutes {
    void register_routes(httplib::Server& server);
}
