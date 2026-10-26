#include "engine/engine_worker.hpp"
#include "core/buffer.hpp"

#include <atomic>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Event {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() { require(handle != nullptr, "CreateEvent failed"); }
    ~Event() { CloseHandle(handle); }
    void signal() const { SetEvent(handle); }
    void wait() const { require(WaitForSingleObject(handle, 5000) == WAIT_OBJECT_0, "event timed out"); }
};

struct State {
    Event entered;
    Event release;
    Event complete;
    std::mutex mutex;
    std::vector<std::u16string> predictions;
    std::vector<std::uint64_t> discarded;
    std::thread::id worker;
    std::atomic<int> ready_calls = 0;
    std::atomic<int> commits = 0;
    std::atomic<int> mode_queries = 0;
};

class FakeEngine : public tsf::IEngine {
public:
    explicit FakeEngine(std::shared_ptr<State> state) : state_(std::move(state)) {}
    void ready() override { ++state_->ready_calls; }
    void predict(const std::u16string& context, std::span<tsf::BopomofoPos> padding) override {
        {
            std::lock_guard lock(state_->mutex);
            state_->worker = std::this_thread::get_id();
            state_->predictions.push_back(context);
        }
        if (context == u"blocked") {
            state_->entered.signal();
            state_->release.wait();
        }
        for (auto& entry : padding) {
            entry.candidates = {U'新', U'舊'};
            entry.predicted = true;
        }
    }
    std::uint64_t record_commit(const tsf::CommitSample&) override {
        return 100 + static_cast<std::uint64_t>(++state_->commits);
    }
    void discard_commit(std::uint64_t token) override {
        std::lock_guard lock(state_->mutex);
        state_->discarded.push_back(token);
        state_->complete.signal();
    }
    tsf::InputMode toggle_input_mode() override {
        mode_ = mode_ == tsf::InputMode::Chinese ? tsf::InputMode::English : tsf::InputMode::Chinese;
        return mode_;
    }
    tsf::InputMode current_input_mode() override {
        ++state_->mode_queries;
        return mode_;
    }
private:
    std::shared_ptr<State> state_;
    tsf::InputMode mode_ = tsf::InputMode::Chinese;
};

void queue_and_snapshot_test() {
    auto state = std::make_shared<State>();
    Event notified;
    tsf::EngineWorker worker([state] { return std::make_unique<FakeEngine>(state); },
                             [&] { notified.signal(); });
    // Always release a blocked fake before worker destruction on a failed check.
    struct Release { State& state; ~Release() { state.release.signal(); } } release{*state};
    std::vector<tsf::BopomofoPos> padding(1);
    padding[0].compositable = true;
    padding[0].candidates = {U'舊'};
    worker.predict(1, u"blocked", padding);
    state->entered.wait();
    worker.predict(2, u"superseded", padding);
    worker.predict(3, u"latest", padding);
    worker.ready();
    worker.ready();
    worker.refresh_input_mode();
    worker.refresh_input_mode();
    require(worker.cached_input_mode() == tsf::InputMode::English, "mode read waited on transport");
    const auto commit = worker.record_commit({});
    worker.discard_commit(commit);
    // The caller can keep editing while the fake transport is blocked.
    require(padding[0].candidates.front() == U'舊', "worker mutated caller's input");
    state->release.signal();
    state->complete.wait();
    const auto result = worker.take_prediction();
    require(result && result->sequence == 3 && result->succeeded, "latest prediction missing");
    require(result->padding[0].candidates.front() == U'新', "prediction output missing");
    require(worker.cached_input_mode() == tsf::InputMode::Chinese, "mode was not refreshed");
    require(state->ready_calls == 1 && state->mode_queries == 1, "requests were not coalesced");
    {
        std::lock_guard lock(state->mutex);
        require(state->worker != std::this_thread::get_id(), "transport ran on caller thread");
        require(state->predictions == std::vector<std::u16string>{u"blocked", u"latest"},
                "superseded queued prediction was executed");
        require(state->discarded == std::vector<std::uint64_t>{101}, "commit/discard order lost");
    }

    tsf::CompositionBuffer buffer;
    buffer.add_chosen_candidate(U'甲');
    buffer.invalidate_all_predictions();
    const auto snapshot = buffer.prediction_snapshot();
    require(snapshot.has_value(), "predictable snapshot missing");
    buffer.add_chosen_candidate(U'乙');
    require(!buffer.apply_prediction(*snapshot, *snapshot), "stale typing result accepted");
    buffer.clear();
    require(!buffer.apply_prediction(*snapshot, *snapshot), "result resurrected cleared composition");
    buffer.add_chosen_candidate(U'甲');
    buffer.invalidate_all_predictions();
    const auto current = buffer.prediction_snapshot();
    auto updated = *current;
    updated[0].predicted = true;
    require(buffer.apply_prediction(*current, updated), "current result was rejected");
    require(!buffer.prediction_snapshot(), "applied prediction remained dirty");
}

// A real synchronous ReadFile that never receives a reply verifies that
// deactivation cancels outstanding I/O instead of hanging in thread::join().
class SilentPipeEngine final : public FakeEngine {
public:
    SilentPipeEngine(std::shared_ptr<State> state, std::wstring path)
        : FakeEngine(state), state_(std::move(state)), path_(std::move(path)) {}
    void ready() override {
        const HANDLE pipe = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return;
        state_->entered.signal();
        DWORD received = 0;
        char value = 0;
        ReadFile(pipe, &value, 1, &received, nullptr);
        CloseHandle(pipe);
    }
private:
    std::shared_ptr<State> state_;
    std::wstring path_;
};

void cancellation_test() {
    const auto path = L"\\\\.\\pipe\\llavon-worker-test-" + std::to_wstring(GetCurrentProcessId());
    const HANDLE server = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_WAIT, 1, 1024, 1024, 0, nullptr);
    require(server != INVALID_HANDLE_VALUE, "test pipe creation failed");
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{server};
    auto state = std::make_shared<State>();
    auto worker = std::make_unique<tsf::EngineWorker>(
        [state, path] { return std::make_unique<SilentPipeEngine>(state, path); }, [] {});
    worker->ready();
    const bool connected = ConnectNamedPipe(server, nullptr) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
    require(connected, "test client did not connect");
    state->entered.wait();
    const auto start = std::chrono::steady_clock::now();
    worker.reset();
    require(std::chrono::steady_clock::now() - start < 2s, "shutdown waited for server response");
}

} // namespace

int main() {
    try {
        queue_and_snapshot_test();
        cancellation_test();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
