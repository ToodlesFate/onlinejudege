#include "common/response.hpp"

#include <string>
#include <utility>

namespace oj::common {

nlohmann::json Response::envelope(ErrorCode code, std::string message, Json data) {
    Json body = {
        {"code",    static_cast<std::int32_t>(code)},
        {"message", std::move(message)},
    };
    if (data.is_null()) {
        body["data"] = nullptr;
    } else {
        body["data"] = std::move(data);
    }
    return body;
}

}  // namespace oj::common