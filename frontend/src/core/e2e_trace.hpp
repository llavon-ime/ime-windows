#pragma once

#include <chrono>
#include <cstdint>

namespace tsf {

// Key-to-screen timing of one keystroke, logged when its text is shown.
struct E2eTrace {
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point on_key_started;
    std::chrono::steady_clock::time_point mode_started;
    std::chrono::steady_clock::time_point mode_finished;
    std::chrono::steady_clock::time_point ready_started;
    std::chrono::steady_clock::time_point ready_finished;
    std::chrono::steady_clock::time_point pre_context_started;
    std::chrono::steady_clock::time_point pre_context_finished;
    std::chrono::steady_clock::time_point predict_started;
    std::chrono::steady_clock::time_point predict_finished;
    std::uint64_t key = 0;
    std::uint64_t sequence = 0;
    bool started_at_on_test = false;
};

}  // namespace tsf
