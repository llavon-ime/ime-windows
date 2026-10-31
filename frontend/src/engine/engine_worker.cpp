#include "engine/engine_worker.hpp"

#include <windows.h>
#include <objbase.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "engine/impl/engine.h"

namespace tsf {

struct EngineWorker::Impl {
    enum class Kind { Other, Ready, Query, Prefill, Predict };

    struct Job {
        Kind kind = Kind::Other;
        std::uint64_t sequence = 0;
        std::uint64_t group = 0;
        std::function<winrt::Windows::Foundation::IAsyncAction(IEngine&)> run;
    };

    Impl(Factory factory, std::function<void(UiTask)> post_to_ui, std::weak_ptr<UiState> ui)
        : post_to_ui_(std::move(post_to_ui)),
          ui_(std::move(ui)),
          thread_([this, factory = std::move(factory)] { run(factory); }) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            jobs_.clear();
            if (engine_) engine_->abort();
        }
        SetEvent(wake_.get());
        thread_.join();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void submit(Kind kind, std::function<winrt::Windows::Foundation::IAsyncAction(IEngine&)> run,
                std::uint64_t sequence = 0, std::uint64_t group = 0) {
        std::vector<std::uint64_t> superseded;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            if (kind != Kind::Other) {
                std::erase_if(jobs_, [&](const Job& queued) {
                    if (queued.kind != kind || queued.group != group) return false;
                    if (kind == Kind::Predict) superseded.push_back(queued.sequence);
                    return true;
                });
            }
            jobs_.push_back({kind, sequence, group, std::move(run)});
        }
        SetEvent(wake_.get());
        for (const auto dropped : superseded) {
            publish_prediction({.sequence = dropped, .status = Prediction::Status::Superseded});
        }
    }

    void to_ui(std::move_only_function<void(UiState&)> task) {
        post_to_ui_([ui = ui_, task = std::move(task)]() mutable {
            if (const auto state = ui.lock()) task(*state);
        });
    }

    void publish_mode(InputMode mode, std::uint64_t revision) {
        to_ui([mode, revision](UiState& state) {
            if (revision != state.mode_revision || state.mode == mode) return;
            state.mode = mode;
            if (state.callbacks.input_mode_changed) state.callbacks.input_mode_changed(mode);
        });
    }

    void publish_prediction(Prediction prediction) {
        to_ui([prediction = std::move(prediction)](UiState& state) mutable {
            // Without a backend there is no Bopomofo engine: English, at once,
            // regardless of any mode request still in flight.
            if (prediction.status == Prediction::Status::BackendLost && state.mode != InputMode::English) {
                state.mode = InputMode::English;
                ++state.mode_revision;
                if (state.callbacks.input_mode_changed) state.callbacks.input_mode_changed(state.mode);
            }
            if (state.callbacks.prediction) state.callbacks.prediction(std::move(prediction));
        });
    }

    winrt::Windows::Foundation::IAsyncAction pump() {
        for (;;) {
            std::optional<Job> job;
            {
                std::lock_guard lock(mutex_);
                if (stopping_) co_return;
                if (!jobs_.empty()) {
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                }
            }
            if (!job) {
                co_await winrt::resume_on_signal(wake_.get());
                continue;
            }
            try {
                co_await job->run(*engine_);
            } catch (...) {
            }
        }
    }

    void run(const Factory& factory) noexcept {
        // Service launch uses COM; this thread keeps the process MTA alive for
        // the thread-pool threads the coroutine resumes on.
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            {
                auto engine = factory();
                std::lock_guard lock(mutex_);
                engine_ = std::move(engine);
            }
            pump().get();
        } catch (...) {
        }
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            jobs_.clear();
            engine_.reset();
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
    }

    std::function<void(UiTask)> post_to_ui_;
    std::weak_ptr<UiState> ui_;

    std::mutex mutex_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    // Created, used and destroyed by the worker; abort() is the only call from
    // another thread, under mutex_.
    std::unique_ptr<IEngine> engine_;
    winrt::handle wake_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    // Only touched by the pump, in transport order.
    std::uint64_t recorded_commit_ = 0;
    std::uint64_t transport_commit_ = 0;

    std::thread thread_;  // Last: all state is initialized before run().
};

EngineWorker::EngineWorker(Factory factory, std::function<void(UiTask)> post_to_ui, Callbacks callbacks)
    : ui_(std::make_shared<UiState>(UiState{.callbacks = std::move(callbacks)})),
      impl_(std::make_unique<Impl>(std::move(factory), std::move(post_to_ui), ui_)) {}

EngineWorker::~EngineWorker() {
    // Join first; queued UI tasks hold only a weak reference to ui_.
    impl_.reset();
}

InputMode EngineWorker::cached_input_mode() const noexcept {
    return ui_->mode;
}

void EngineWorker::refresh_input_mode() {
    const auto revision = ui_->mode_revision;
    impl_->submit(Impl::Kind::Query, [impl = impl_.get(), revision](IEngine& engine)
                                         -> winrt::Windows::Foundation::IAsyncAction {
        const auto mode = co_await engine.current_input_mode();
        impl->publish_mode(static_cast<InputMode>(mode), revision);
    });
}

void EngineWorker::toggle_input_mode() {
    ui_->mode = ui_->mode == InputMode::Chinese ? InputMode::English : InputMode::Chinese;
    const auto revision = ++ui_->mode_revision;
    impl_->submit(Impl::Kind::Other, [impl = impl_.get(), revision](IEngine& engine)
                                         -> winrt::Windows::Foundation::IAsyncAction {
        const auto mode = co_await engine.toggle_input_mode();
        impl->publish_mode(static_cast<InputMode>(mode), revision);
    });
}

void EngineWorker::ready() {
    impl_->submit(Impl::Kind::Ready, [](IEngine& engine) -> winrt::Windows::Foundation::IAsyncAction {
        co_await engine.ready();
    });
}

void EngineWorker::prefill(std::u16string context) {
    impl_->submit(Impl::Kind::Prefill, [context = std::move(context)](IEngine& engine)
                                           -> winrt::Windows::Foundation::IAsyncAction {
        co_await engine.prefill(context);
    });
}

void EngineWorker::predict(std::uint64_t sequence, std::uint64_t group, std::u16string context,
                           std::vector<BopomofoPos> padding) {
    for (auto& entry : padding) entry.predicted = false;
    impl_->submit(
        Impl::Kind::Predict,
        [impl = impl_.get(), sequence, context = std::move(context),
         padding = std::move(padding)](IEngine& engine) -> winrt::Windows::Foundation::IAsyncAction {
            Prediction result{.sequence = sequence, .padding = padding};
            bool answered = false;
            try {
                answered = co_await engine.predict(context, result.padding);
            } catch (...) {
            }
            result.status = answered ? Prediction::Status::Converted : Prediction::Status::BackendLost;
            result.finished = std::chrono::steady_clock::now();
            impl->publish_prediction(std::move(result));
        },
        sequence, group);
}

std::uint64_t EngineWorker::record_commit(CommitSample sample) {
    const auto token = ++next_commit_;
    impl_->submit(Impl::Kind::Other, [impl = impl_.get(), token, sample = std::move(sample)](
                                         IEngine& engine) -> winrt::Windows::Foundation::IAsyncAction {
        impl->recorded_commit_ = token;
        impl->transport_commit_ = 0;
        impl->transport_commit_ = co_await engine.record_commit(sample);
    });
    return token;
}

void EngineWorker::discard_commit(std::uint64_t token) {
    impl_->submit(Impl::Kind::Other, [impl = impl_.get(), token](IEngine& engine)
                                         -> winrt::Windows::Foundation::IAsyncAction {
        if (token == impl->recorded_commit_ && impl->transport_commit_ != 0)
            co_await engine.discard_commit(impl->transport_commit_);
    });
}

}  // namespace tsf
