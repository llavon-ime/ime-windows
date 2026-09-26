#pragma once

#include <asio.hpp>

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>

namespace llavon::service {

// Confined to the inference executor. Renewing an active lease does not make
// another driver call or cancel/reallocate a timer on every keystroke.
class GpuActivityLease final {
public:
    // A successful enable may be a no-op. A failed release, regardless of
    // result, must retain the backend because it may still own a clock change.
    enum class Result { success, retry_later, unavailable };
    using Boost = std::move_only_function<Result(bool)>;
    using Duration = std::chrono::steady_clock::duration;

    explicit GpuActivityLease(asio::io_context& context,
                              Duration idle_timeout = std::chrono::seconds(2),
                              Duration retry_delay = std::chrono::seconds(1))
        : state_(std::make_shared<State>(context, idle_timeout, retry_delay)) {}

    ~GpuActivityLease() { state_->reset(); }
    GpuActivityLease(const GpuActivityLease&) = delete;
    GpuActivityLease& operator=(const GpuActivityLease&) = delete;

    void set_backend(Boost backend) noexcept {
        state_->reset();
        state_->backend = std::move(backend);
    }

    void activate() noexcept { state_->activate(); }

    // Disabling prevents renewals; an existing lease expires at its usual deadline.
    void set_enabled(bool enabled) noexcept { state_->enabled = enabled; }

private:
    struct State final : std::enable_shared_from_this<State> {
        asio::steady_timer timer;
        Duration idle_timeout;
        Duration retry_delay;
        std::chrono::steady_clock::time_point deadline{};
        std::chrono::steady_clock::time_point retry_after{};
        Boost backend;
        std::size_t generation = 0;
        bool active = false;
        bool enabled = true;
        bool releasing = false;
        unsigned release_failures = 0;
        static constexpr unsigned max_release_attempts = 4;

        State(asio::io_context& context, Duration timeout, Duration retry)
            : timer(context), idle_timeout(timeout), retry_delay(retry) {}

        void reset() noexcept {
            ++generation;
            asio::error_code ignored;
            timer.cancel(ignored);

            const bool was_active = std::exchange(active, false);
            releasing = false;
            release_failures = 0;
            retry_after = {};
            auto released_backend = std::move(backend);
            if (was_active && released_backend) {
                try {
                    if (released_backend(false) != Result::success) {
                        std::clog << "[WARN] GPU boost restoration still pending at shutdown/replacement\n";
                    }
                } catch (...) {
                    std::clog << "[WARN] GPU boost restoration threw at shutdown/replacement\n";
                }
            }
        }

        void activate() noexcept {
            if (!enabled || !backend || releasing) return;
            const auto now = std::chrono::steady_clock::now();
            if (now < retry_after) return;
            deadline = now + idle_timeout;
            if (active) return;
            try {
                const auto result = backend(true);
                if (result == Result::retry_later) {
                    retry_after = now + retry_delay;
                    return;
                }
                if (result == Result::unavailable) {
                    reset();
                    return;
                }
                active = true;
                arm();
            } catch (...) {
                reset();
                std::clog << "[WARN] GPU activity lease unavailable\n";
            }
        }

        void release() noexcept {
            releasing = true;
            try {
                if (backend(false) == Result::success) {
                    active = false;
                    releasing = false;
                    release_failures = 0;
                    return;
                }
            } catch (...) {
                // Keep the backend and its saved settings for another attempt.
            }
            if (++release_failures >= max_release_attempts) {
                std::clog << "[WARN] GPU boost restoration failed after 4 attempts; "
                             "further boosts suspended, saved state retained until shutdown/replacement\n";
                return;
            }
            deadline = std::chrono::steady_clock::now() + retry_delay;
            try {
                arm();
            } catch (...) {
                std::clog << "[WARN] GPU boost restoration retry could not be scheduled\n";
            }
        }

        void arm() {
            timer.expires_at(deadline);
            timer.async_wait([weak = weak_from_this(), expected = generation](
                                 const asio::error_code& error) {
                const auto self = weak.lock();
                if (!self || error || expected != self->generation) return;
                try {
                    if (std::chrono::steady_clock::now() < self->deadline) {
                        self->arm();
                        return;
                    }
                    self->release();
                } catch (...) {
                    // A timer allocation failure must not discard restoration state.
                    self->releasing = true;
                    std::clog << "[WARN] GPU activity lease timer failed; saved state retained\n";
                }
            });
        }
    };

    std::shared_ptr<State> state_;
};

} // namespace llavon::service
