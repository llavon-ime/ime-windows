#pragma once

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "impl/engine.h"

namespace tsf {

// Owns one transport on one worker. No TSF/COM objects cross this boundary.
// The owner drains results on its window thread after receiving notification.
class EngineWorker final {
public:
    struct Prediction {
        std::uint64_t sequence = 0;
        std::vector<BopomofoPos> padding;
        std::chrono::steady_clock::time_point finished;
        bool succeeded = false;
    };

    using Factory = std::function<std::unique_ptr<IEngine>()>;

    EngineWorker(Factory factory, std::function<void()> notify)
        : notify_(std::move(notify)),
          thread_([this, factory = std::move(factory)] { run(factory); }) {}

    ~EngineWorker() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            jobs_.clear();
        }
        wake_.notify_one();
        // Repeat cancellation to cover a read starting just after cancellation.
        // Never detach DLL code or leave the pipe blocked during deactivation.
        while (WaitForSingleObject(thread_.native_handle(), 0) == WAIT_TIMEOUT) {
            CancelSynchronousIo(thread_.native_handle());
            WaitForSingleObject(thread_.native_handle(), 10);
        }
        thread_.join();
    }

    EngineWorker(const EngineWorker&) = delete;
    EngineWorker& operator=(const EngineWorker&) = delete;

    InputMode cached_input_mode() const {
        std::lock_guard lock(mutex_);
        return mode_;
    }

    void refresh_input_mode() {
        std::lock_guard lock(mutex_);
        if (query_pending_ || stopping_) return;
        query_pending_ = true;
        const auto revision = mode_revision_;
        enqueue_locked(Kind::Other, [this, revision](IEngine& engine) {
            InputMode mode = InputMode::English;
            try { mode = engine.current_input_mode(); } catch (...) {}
            {
                std::lock_guard guard(mutex_);
                query_pending_ = false;
            }
            publish_mode(mode, revision);
        });
    }

    void toggle_input_mode() {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        mode_ = mode_ == InputMode::Chinese ? InputMode::English : InputMode::Chinese;
        const auto revision = ++mode_revision_;
        enqueue_locked(Kind::Other, [this, revision](IEngine& engine) {
            InputMode mode = InputMode::English;
            try { mode = engine.toggle_input_mode(); } catch (...) {}
            publish_mode(mode, revision);
        });
    }

    void ready() {
        std::lock_guard lock(mutex_);
        if (ready_pending_ || stopping_) return;
        ready_pending_ = true;
        enqueue_locked(Kind::Other, [this](IEngine& engine) {
            try { engine.ready(); } catch (...) {}
            std::lock_guard guard(mutex_);
            ready_pending_ = false;
        });
    }

    void predict(std::uint64_t sequence, std::u16string context,
                 std::vector<BopomofoPos> padding) {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        // Keep the in-flight request and only the latest queued prediction.
        std::erase_if(jobs_, [](const Job& job) { return job.kind == Kind::Predict; });
        enqueue_locked(Kind::Predict,
            [this, sequence, context = std::move(context), padding = std::move(padding)]
            (IEngine& engine) mutable {
                Prediction result{.sequence = sequence, .padding = std::move(padding)};
                for (auto& entry : result.padding) entry.predicted = false;
                try {
                    engine.predict(context, result.padding);
                    result.succeeded = std::ranges::all_of(result.padding, [](const auto& entry) {
                        return entry.predicted && (entry.chosen || !entry.candidates.empty());
                    });
                } catch (...) {}
                result.finished = std::chrono::steady_clock::now();
                {
                    std::lock_guard guard(mutex_);
                    result_ = std::move(result);
                }
                notify_();
            });
    }

    std::optional<Prediction> take_prediction() {
        std::lock_guard lock(mutex_);
        return std::exchange(result_, std::nullopt);
    }

    std::uint64_t record_commit(CommitSample sample) {
        std::lock_guard lock(mutex_);
        if (stopping_) return 0;
        const auto token = ++next_commit_;
        enqueue_locked(Kind::Other, [this, token, sample = std::move(sample)](IEngine& engine) {
            recorded_commit_ = token;
            transport_commit_ = 0;
            transport_commit_ = engine.record_commit(sample);
        });
        return token;
    }

    void discard_commit(std::uint64_t token) {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        enqueue_locked(Kind::Other, [this, token](IEngine& engine) {
            if (token == recorded_commit_ && transport_commit_ != 0)
                engine.discard_commit(transport_commit_);
        });
    }

private:
    enum class Kind { Other, Predict };
    struct Job {
        Kind kind;
        std::function<void(IEngine&)> execute;
    };

    void enqueue_locked(Kind kind, std::function<void(IEngine&)> execute) {
        jobs_.push_back({kind, std::move(execute)});
        wake_.notify_one();
    }

    void publish_mode(InputMode mode, std::uint64_t revision) {
        bool changed = false;
        {
            std::lock_guard lock(mutex_);
            if (revision == mode_revision_) {
                changed = mode_ != mode;
                mode_ = mode;
            }
        }
        if (changed) notify_();
    }

    void run(const Factory& factory) noexcept {
        // Transport startup can use COM (service launch); TSF interfaces stay
        // entirely on the owning UI thread.
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            auto engine = factory();
            for (;;) {
                Job job;
                {
                    std::unique_lock lock(mutex_);
                    wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                    if (stopping_) break;
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                }
                try { job.execute(*engine); } catch (...) {}
            }
        } catch (...) {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            jobs_.clear();
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
    }

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::optional<Prediction> result_;
    bool stopping_ = false;
    bool query_pending_ = false;
    bool ready_pending_ = false;
    InputMode mode_ = InputMode::English;
    std::uint64_t mode_revision_ = 0;
    std::uint64_t next_commit_ = 0;
    // Only accessed by the worker, in transport order.
    std::uint64_t recorded_commit_ = 0;
    std::uint64_t transport_commit_ = 0;
    std::function<void()> notify_;
    std::thread thread_; // Last: all state is initialized before run().
};

} // namespace tsf
