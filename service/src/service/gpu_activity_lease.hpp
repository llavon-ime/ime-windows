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
    using Boost = std::move_only_function<bool(bool)>;
    using Duration = std::chrono::steady_clock::duration;

    explicit GpuActivityLease(asio::io_context& context,
                              Duration idle_timeout = std::chrono::seconds(2))
        : state_(std::make_shared<State>(context, idle_timeout)) {}

    ~GpuActivityLease() { state_->reset(); }
    GpuActivityLease(const GpuActivityLease&) = delete;
    GpuActivityLease& operator=(const GpuActivityLease&) = delete;

    void set_backend(Boost backend) noexcept {
        state_->reset();
        state_->backend = std::move(backend);
    }

    void activate() noexcept { state_->activate(); }

private:
    struct State final : std::enable_shared_from_this<State> {
        asio::steady_timer timer;
        Duration idle_timeout;
        std::chrono::steady_clock::time_point deadline{};
        Boost backend;
        std::size_t generation = 0;
        bool active = false;

        State(asio::io_context& context, Duration timeout)
            : timer(context), idle_timeout(timeout) {}

        void reset() noexcept {
            ++generation;
            asio::error_code ignored;
            timer.cancel(ignored);

            const bool was_active = std::exchange(active, false);
            auto released_backend = std::move(backend);
            if (was_active && released_backend) {
                try {
                    (void)released_backend(false);
                } catch (...) {
                }
            }
        }

        void activate() noexcept {
            if (!backend) return;
            deadline = std::chrono::steady_clock::now() + idle_timeout;
            if (active) return;
            try {
                if (!backend(true)) {
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
                    self->active = false;
                    if (!self->backend(false)) {
                        self->reset();
                        return;
                    }
                } catch (...) {
                    self->active = false;
                    self->reset();
                    std::clog << "[WARN] GPU activity lease release failed\n";
                }
            });
        }
    };

    std::shared_ptr<State> state_;
};

} // namespace llavon::service
