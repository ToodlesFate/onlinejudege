#include "domain/submission_types.hpp"

namespace oj::domain {

// ---------------------------------------------------------------------------
//  SubmissionStatus ↔ string
// ---------------------------------------------------------------------------
std::string_view to_string(SubmissionStatus s) noexcept {
    switch (s) {
        case SubmissionStatus::Queued:    return "queued";
        case SubmissionStatus::Compiling: return "compiling";
        case SubmissionStatus::Running:   return "running";
        case SubmissionStatus::Finished:  return "finished";
    }
    return "queued";
}

std::optional<SubmissionStatus>
submission_status_from_string(std::string_view s) noexcept {
    if (s == "queued")    return SubmissionStatus::Queued;
    if (s == "compiling") return SubmissionStatus::Compiling;
    if (s == "running")   return SubmissionStatus::Running;
    if (s == "finished")  return SubmissionStatus::Finished;
    return std::nullopt;
}

bool is_terminal_status(SubmissionStatus s) noexcept {
    // SPEC §2.3.2 状态机：queued/compiling/running → finished
    // 只有 Finished 是终态（之后填 result）
    return s == SubmissionStatus::Finished;
}

// ---------------------------------------------------------------------------
//  SubmissionResult ↔ string
// ---------------------------------------------------------------------------
std::string_view to_string(SubmissionResult r) noexcept {
    switch (r) {
        case SubmissionResult::AC:  return "AC";
        case SubmissionResult::WA:  return "WA";
        case SubmissionResult::TLE: return "TLE";
        case SubmissionResult::MLE: return "MLE";
        case SubmissionResult::OLE: return "OLE";
        case SubmissionResult::RE:  return "RE";
        case SubmissionResult::CE:  return "CE";
        case SubmissionResult::SE:  return "SE";
    }
    return "SE";
}

std::optional<SubmissionResult>
submission_result_from_string(std::string_view s) noexcept {
    if (s == "AC")  return SubmissionResult::AC;
    if (s == "WA")  return SubmissionResult::WA;
    if (s == "TLE") return SubmissionResult::TLE;
    if (s == "MLE") return SubmissionResult::MLE;
    if (s == "OLE") return SubmissionResult::OLE;
    if (s == "RE")  return SubmissionResult::RE;
    if (s == "CE")  return SubmissionResult::CE;
    if (s == "SE")  return SubmissionResult::SE;
    return std::nullopt;
}

bool is_terminal(SubmissionResult /*r*/) noexcept {
    // 8 态全部是终态
    return true;
}

bool is_early_exit(SubmissionResult r) noexcept {
    // SPEC §2.3.2：CE / SE 走 compiling → finished 直跳，绕过 running
    return r == SubmissionResult::CE || r == SubmissionResult::SE;
}

}  // namespace oj::domain