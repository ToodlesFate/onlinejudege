#include "http/handlers/health_handler.hpp"

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "common/version.hpp"
#include "http/HttpServer.hpp"

namespace oj::http::handlers {

std::int64_t process_uptime_ms() {
    using namespace std::chrono;
    static const auto started = steady_clock::now();
    return duration_cast<milliseconds>(steady_clock::now() - started).count();
}

void health(const httplib::Request& /*req*/, httplib::Response& res, std::int64_t uptime_ms) {
    using namespace std::chrono;
    const auto now_unix = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();

    nlohmann::json data = {
        {"status",    "ok"},
        {"version",   OJ_VERSION_STRING},
        {"uptime_ms", uptime_ms},
        {"now_unix",  now_unix},
    };
    write_ok(res, std::move(data));
}

}  // namespace oj::http::handlers