#pragma once

#include <cstdint>
#include <string_view>

namespace oj::common {

enum class ErrorCode : std::int32_t {
    Ok              = 0,
    BadRequest      = 1001,
    Unauthorized    = 1002,
    Forbidden       = 1003,
    NotFound        = 1004,
    Conflict        = 1005,
    TooLarge        = 1006,
    Internal        = 1007,
    SystemError     = 1008,
};

constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:           return "ok";
        case ErrorCode::BadRequest:   return "bad request";
        case ErrorCode::Unauthorized: return "unauthorized";
        case ErrorCode::Forbidden:    return "forbidden";
        case ErrorCode::NotFound:     return "not found";
        case ErrorCode::Conflict:     return "conflict";
        case ErrorCode::TooLarge:     return "payload too large";
        case ErrorCode::Internal:     return "internal server error";
        case ErrorCode::SystemError:  return "system error";
    }
    return "unknown";
}

constexpr int to_http_status(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:           return 200;
        case ErrorCode::BadRequest:   return 400;
        case ErrorCode::Unauthorized: return 401;
        case ErrorCode::Forbidden:    return 403;
        case ErrorCode::NotFound:     return 404;
        case ErrorCode::Conflict:     return 409;
        case ErrorCode::TooLarge:     return 413;
        case ErrorCode::Internal:     return 500;
        case ErrorCode::SystemError:  return 500;
    }
    return 500;
}

}  // namespace oj::common