#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/bopomofo.hpp"
#include "core/conversion.hpp"
#include "engine/engine_types.hpp"

namespace tsf {

class IEngine;

// Owns one transport, driven by a single coroutine that sends one request at a
// time. Every public member is called on the owning UI thread, and every
// callback is delivered back to that thread through post_to_ui. No TSF/COM
// object crosses this boundary.
class EngineWorker final : public ConversionBackend {
public:
    using Prediction = ConversionResult;

    struct Callbacks {
        std::function<void(Prediction)> prediction;
        std::function<void(InputMode)> input_mode_changed;
    };

    using UiTask = std::move_only_function<void()>;
    using Factory = std::function<std::unique_ptr<IEngine>()>;

    EngineWorker(Factory factory, std::function<void(UiTask)> post_to_ui, Callbacks callbacks);
    ~EngineWorker() override;

    EngineWorker(const EngineWorker&) = delete;
    EngineWorker& operator=(const EngineWorker&) = delete;

    InputMode cached_input_mode() const noexcept;
    void refresh_input_mode();
    void toggle_input_mode();
    void ready();
    void prefill(std::u16string context) override;
    // A queued, not yet sent prediction is superseded by the next one of the
    // same group (one composition).
    void predict(std::uint64_t sequence, std::uint64_t group, std::u16string context,
                 std::vector<BopomofoPos> padding) override;
    std::uint64_t record_commit(CommitSample sample);
    void discard_commit(std::uint64_t token);

private:
    struct UiState {
        Callbacks callbacks;
        InputMode mode = InputMode::English;
        std::uint64_t mode_revision = 0;
    };
    struct Impl;

    std::shared_ptr<UiState> ui_;
    std::uint64_t next_commit_ = 0;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tsf
