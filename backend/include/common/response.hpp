#pragma once

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "common/error_code.hpp"

namespace oj::common {

// 统一响应包络：{"code":0,"message":"ok","data":<T>} —— SPEC §5.1
class Response {
public:
    using Json = nlohmann::json;

    [[nodiscard]] static nlohmann::json ok() { return envelope(ErrorCode::Ok, "ok", nullptr); }

    [[nodiscard]] static nlohmann::json ok(Json data) {
        return envelope(ErrorCode::Ok, "ok", std::move(data));
    }

    [[nodiscard]] static nlohmann::json error(ErrorCode code, std::string message = {}) {
        if (message.empty()) {
            message = std::string{to_string(code)};
        }
        return envelope(code, std::move(message), nullptr);
    }

    static void write_ok(Json data = nullptr);

    static void write_error(ErrorCode code, std::string message = {});

private:
    [[nodiscard]] static nlohmann::json envelope(ErrorCode code, std::string message, Json data);
};

}  // namespace oj::common