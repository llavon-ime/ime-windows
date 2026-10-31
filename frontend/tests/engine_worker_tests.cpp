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
    std::vector<std::u16string> prefills;
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
    void prefill(const std::u16string& context) override {
        std::lock_guard lock(state_->mutex);
        state_->prefills.push_back(context);
    }
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
        if (context == u"failed") {
            padding.front().candidates = {U'錯'};
            throw std::runtime_error("fake transport failure");
        }
        if (context == u"no-candidates") {
            for (auto& entry : padding) {
                entry.candidates.clear();
                entry.predicted = true;
            }
            return;
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
    worker.prefill(u"superseded prefill");
    worker.prefill(u"latest prefill");
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
        require(state->prefills == std::vector<std::u16string>{u"latest prefill"},
                "superseded queued prefill was executed");
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

void discard_reading_test() {
    tsf::CompositionBuffer buffer;
    require(!buffer.discard_current_reading(), "empty composition discarded a reading");
    buffer.add(u'ㄋ');
    buffer.add(u'ㄧ');
    require(buffer.discard_current_reading() && buffer.empty() && buffer.caret_offset() == 0,
            "unfinished syllable was not discarded as a whole");

    buffer.add_chosen_candidate(U'你');
    buffer.add(u'ㄏ');
    buffer.add(u'ㄠ');
    require(buffer.discard_current_reading() && buffer.to_string() == u"你" && buffer.caret_offset() == 1,
            "discarding a reading changed the preceding converted text");
    require(!buffer.discard_current_reading() && buffer.to_string() == u"你",
            "converted text was discarded");

    buffer.add(u'ㄋ');
    buffer.add(u'ㄧ');
    buffer.add(u'ˇ');
    const auto pending = buffer.prediction_snapshot();
    require(pending.has_value(), "completed syllable has no prediction snapshot");
    require(!buffer.discard_current_reading() && buffer.matches_prediction(*pending) &&
                buffer.waiting_for_prediction(),
            "completed syllable waiting for the backend was discarded");
    buffer.add(u'ㄏ');
    buffer.add(u'ㄠ');
    require(buffer.discard_current_reading() && buffer.matches_prediction(*pending),
            "discarding the unfinished syllable changed a pending prediction");

    buffer.clear();
    buffer.add_chosen_candidate(U'你');
    buffer.add(u'ㄏ');
    buffer.add(u'ㄠ');
    buffer.add_chosen_candidate(U'！');
    buffer.pre();
    require(buffer.discard_current_reading() && buffer.to_string() == u"你！" && buffer.caret_offset() == 1,
            "discarding a reading inside composition changed surrounding text or caret");
    buffer.pre();
    require(!buffer.discard_current_reading() && buffer.to_string() == u"你！",
            "discarding at the start of composition changed the text");
}

void uncomposed_reading_test() {
    tsf::CompositionBuffer buffer;
    require(!buffer.has_uncomposed_reading(), "empty composition blocked commit");
    buffer.add_chosen_candidate(U'你');
    buffer.add_chosen_candidate(U'好');
    require(!buffer.has_uncomposed_reading(), "converted text blocked commit");
    buffer.add(u'ㄅ');
    require(buffer.has_uncomposed_reading(), "single unfinished symbol allowed commit");
    buffer.add(u'ㄚ');
    require(buffer.has_uncomposed_reading(), "unfinished syllable allowed commit");
    buffer.add_chosen_candidate(U'！');
    require(buffer.current_compositable() && buffer.has_uncomposed_reading(),
            "unfinished input away from the caret allowed commit");
    buffer.pre();
    buffer.add(u'ˊ');
    require(!buffer.has_uncomposed_reading() && buffer.waiting_for_prediction(),
            "completed syllable did not retain the existing prediction waiting behavior");
    buffer.clear();
    buffer.add(u'ㄅ');
    buffer.add(u'ㄦ');
    buffer.add(u' ');
    require(buffer.current_invalid() && buffer.has_uncomposed_reading(),
            "invalid reading allowed commit");
}

void single_reading_candidate_test() {
    tsf::CompositionBuffer buffer;
    const std::u32string ranked = U"一衣依醫伊揖壹噫咿禕漪猗";
    for (const std::size_t count : {1u, 8u, 9u, 12u}) {
        buffer.clear();
        buffer.add(u'ㄧ');
        require(buffer.has_uncomposed_reading(), "single reading bypassed commit blocking");
        buffer.add(u' ');
        require(buffer.waiting_for_prediction() && !buffer.current_has_candidate_list(),
                "single reading bypassed model prediction");
        const auto snapshot = buffer.prediction_snapshot();
        require(snapshot && !snapshot->front().chosen && snapshot->front().to_bopomofo_string() == u"ㄧ ",
                "single reading was sent as a fixed literal instead of a first-tone syllable");
        auto result = *snapshot;
        const std::vector<char32_t> model_candidates(ranked.begin(), ranked.begin() + count);
        result.front().candidates = model_candidates;
        result.front().predicted = true;
        if (count == 1) result.front().choose_candidate(model_candidates.front());
        require(buffer.apply_prediction(*snapshot, std::move(result)), "single reading prediction rejected");
        const auto& entry = buffer.cur();
        require(buffer.to_string() == u"一" && entry.candidates == model_candidates,
                "display-only literal changed the model default or model candidates");
        auto expected = model_candidates;
        expected.insert(expected.begin() + std::min<std::size_t>(8, count), U'ㄧ');
        const auto displayed = entry.get_candidates();
        require(displayed == expected && buffer.current_has_candidate_list(),
                "literal was not at the end of the first page or displaced candidate order");
        for (std::size_t index = 0; index < displayed.size(); ++index) {
            auto selected = entry;
            selected.set_choose_index(static_cast<int>(index));
            require(selected.current32() == displayed[index], "displayed selection mapped to the wrong candidate");
        }
        const auto next_snapshot = [&] {
            buffer.invalidate_all_predictions();
            return buffer.prediction_snapshot();
        }();
        require(next_snapshot && next_snapshot->front().candidates == model_candidates,
                "display-only literal leaked into a later engine request");
    }

    auto duplicate = buffer.cur();
    duplicate.candidates.insert(duplicate.candidates.begin(), U'ㄧ');
    duplicate.candidates.push_back(U'ㄧ');
    const auto displayed = duplicate.get_candidates();
    require(std::ranges::count(displayed, U'ㄧ') == 1 && displayed[8] == U'ㄧ',
            "existing literal was duplicated or placed outside the first page");

    buffer.clear();
    buffer.add_chosen_candidate(U'你');
    buffer.add_chosen_candidate(U'好');
    buffer.add(u'ㄧ');
    buffer.add(u' ');
    const auto snapshot = buffer.prediction_snapshot();
    auto result = *snapshot;
    for (auto& item : result) item.predicted = true;
    result.back().candidates = {U'一', U'衣'};
    require(buffer.apply_prediction(*snapshot, std::move(result)), "mixed composition prediction rejected");
    buffer.add_chosen_candidate(U'！');
    buffer.pre();
    buffer.cur().set_choose_index(2);
    require(buffer.to_string() == u"你好ㄧ！" && buffer.caret_offset() == 3 &&
                !buffer.has_uncomposed_reading(),
            "selecting a literal cleared composition or changed surrounding text or caret");
    const auto sample = buffer.commit_sample(u"");
    require(sample.input[2].reading.empty() && sample.input[2].output == u"ㄧ" &&
                sample.input[2].manually_selected,
            "selected literal did not become ordinary composition text");
    buffer.add(u'ㄋ');
    require(buffer.to_string() == u"你好ㄧㄋ！" && buffer.has_uncomposed_reading(),
            "further input merged with the selected literal");

    buffer.clear();
    buffer.add(u'ㄋ');
    buffer.add(u'ㄧ');
    buffer.add(u'ˇ');
    require(buffer.cur().get_candidates() == buffer.cur().candidates,
            "multi-symbol syllable gained a literal candidate");
}

void literal_only_reading_test() {
    for (const char16_t symbol : u"ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄖㄝㄟ"sv) {
        tsf::CompositionBuffer buffer;
        buffer.add(symbol);
        require(buffer.has_uncomposed_reading(), "unfinished single reading allowed commit");
        buffer.add(u' ');
        require(buffer.to_string() == std::u16string(1, symbol) && buffer.caret_offset() == 1 &&
                    !buffer.empty() && !buffer.has_uncomposed_reading() &&
                    !buffer.waiting_for_prediction() && !buffer.commit_requires_prediction() &&
                    !buffer.prediction_snapshot(),
                "literal-only reading waited for the model or did not stay in composition");
        const auto sample = buffer.commit_sample(u"");
        require(sample.input.size() == 1 && sample.input[0].reading.empty() &&
                    sample.input[0].output == std::u16string(1, symbol),
                "literal-only reading was not ordinary composition text");
        buffer.add(u'ㄧ');
        require(buffer.to_string() == std::u16string(1, symbol) + u"ㄧ" &&
                    buffer.has_uncomposed_reading(),
                "next input merged into the literal-only reading");
    }

    tsf::CompositionBuffer buffer;
    buffer.add_chosen_candidate(U'你');
    buffer.add_chosen_candidate(U'好');
    buffer.add(u'ㄅ');
    buffer.add_chosen_candidate(U'！');
    buffer.pre();
    buffer.add(u' ');
    require(buffer.to_string() == u"你好ㄅ！" && buffer.caret_offset() == 3 &&
                !buffer.has_uncomposed_reading() && !buffer.waiting_for_prediction(),
            "literal-only conversion changed surrounding text or caret");

    for (const char16_t symbol : u"ㄓㄔㄕㄗㄘㄙㄧㄨㄩㄚㄛㄜㄞㄠㄡㄢㄣㄤㄥㄦ"sv) {
        buffer.clear();
        buffer.add(symbol);
        buffer.add(u' ');
        const auto snapshot = buffer.prediction_snapshot();
        require(buffer.waiting_for_prediction() && snapshot && !snapshot->front().chosen &&
                    snapshot->front().to_bopomofo_string() == std::u16string(1, symbol) + u" ",
                "a single-symbol syllable with character candidates bypassed the model");
    }
}

void single_reading_failed_model_test() {
    auto state = std::make_shared<State>();
    Event notified;
    tsf::EngineWorker worker([state] { return std::make_unique<FakeEngine>(state); },
                             [&] { notified.signal(); });
    tsf::CompositionBuffer buffer;
    buffer.add(u'ㄧ');
    buffer.add(u' ');
    const auto snapshot = buffer.prediction_snapshot();
    worker.predict(1, u"no-candidates", *snapshot);
    notified.wait();
    auto result = worker.take_prediction();
    require(result && !result->succeeded &&
                !buffer.apply_prediction(*snapshot, std::move(result->padding)),
            "empty model result was replaced by an unrequested literal fallback");
    require(buffer.to_string() == u"ㄧ" && buffer.waiting_for_prediction() &&
                buffer.commit_requires_prediction() && buffer.cur().to_bopomofo_string() == u"ㄧ ",
            "failed model conversion changed a valid single-symbol syllable into ordinary text");
}

void prediction_display_test() {
    tsf::CompositionBuffer buffer;
    buffer.set_immediate_conversion(true);
    buffer.add(u'ㄋ');
    buffer.add(u'ㄧ');
    require(buffer.to_string() == u"ㄋㄧ", "incomplete reading was converted");
    buffer.add(u'ˇ');
    const auto local = buffer.to_string();
    require(local.size() == 1, "immediate display did not use the local candidate");
    require(!buffer.waiting_for_prediction(), "immediate mode blocked local conversion");
    require(buffer.commit_requires_prediction(), "immediate mode allowed Enter to bypass the model");
    buffer.set_immediate_conversion(false);
    require(buffer.waiting_for_prediction(), "disabling immediate conversion accepted an unpredicted candidate");
    buffer.clear();
    buffer.add(u'ㄋ');
    buffer.add(u'ㄧ');
    buffer.add(u'ˇ');
    require(buffer.to_string() == u"ㄋㄧ" && buffer.caret_offset() == 2,
            "waiting display or caret did not retain the reading");
    require(buffer.waiting_for_prediction(), "unpredicted syllable allowed commit");
    require(!buffer.current_has_candidate_list(), "local candidates could bypass prediction waiting");
    require(buffer.commit_sample(u"").answer == buffer.to_string(),
            "external commit sample did not match the visible reading");

    auto state = std::make_shared<State>();
    Event notified;
    tsf::EngineWorker worker([state] { return std::make_unique<FakeEngine>(state); },
                             [&] { notified.signal(); });
    struct Release { State& state; ~Release() { state.release.signal(); } } release{*state};
    const auto snapshot = buffer.prediction_snapshot();
    require(snapshot.has_value(), "waiting display prevented prediction");
    worker.predict(1, u"blocked", *snapshot);
    state->entered.wait();
    require(buffer.to_string() == u"ㄋㄧ", "blocked prediction changed the display");
    require(buffer.waiting_for_prediction(), "in-flight prediction allowed commit");
    require(buffer.commit_requires_prediction(), "in-flight prediction released Enter waiting");
    state->release.signal();
    notified.wait();
    auto result = worker.take_prediction();
    require(result && result->succeeded, "prediction failed");
    require(buffer.apply_prediction(*snapshot, std::move(result->padding)), "prediction rejected");
    require(buffer.to_string() == u"新" && buffer.caret_offset() == 1,
            "completed prediction did not convert the reading and caret together");
    require(!buffer.waiting_for_prediction() && buffer.current_has_candidate_list(),
            "successful prediction did not allow commit and candidate selection");
    require(!buffer.commit_requires_prediction(), "successful prediction did not release Enter waiting");
    buffer.invalidate_all_predictions();
    require(buffer.commit_requires_prediction(), "invalidated prediction allowed Enter to commit stale output");
    require(buffer.to_string() == u"新", "invalidation reverted an existing Hanzi to reading");

    buffer.add(u'ㄋ');
    buffer.add(u'ㄧ');
    buffer.add(u'ˇ');
    require(buffer.to_string() == u"新ㄋㄧ" && buffer.caret_offset() == 3,
            "typing another syllable changed existing Hanzi");
    const auto second = buffer.prediction_snapshot();
    buffer.add(u'ㄅ');
    require(!buffer.apply_prediction(*second, *second), "stale result changed the waiting display");
    require(buffer.to_string() == u"新ㄋㄧㄅ", "typing during prediction was blocked");
    require(buffer.waiting_for_prediction(), "stale result released commit waiting");
    buffer.remove_last();
    const auto failed = buffer.prediction_snapshot();
    ResetEvent(notified.handle);
    worker.predict(2, u"failed", *failed);
    notified.wait();
    auto failure = worker.take_prediction();
    require(failure && !failure->succeeded, "fake failure was accepted as success");
    require(!buffer.apply_prediction(*failed, std::move(failure->padding)),
            "failed prediction was applied as a local fallback");
    require(buffer.to_string() == u"新ㄋㄧ" && buffer.waiting_for_prediction(),
            "failed prediction released the reading or allowed commit");
    require(!buffer.current_has_candidate_list(), "failure exposed local candidates");
    require(buffer.commit_requires_prediction(), "failed prediction released Enter waiting");
    ResetEvent(notified.handle);
    worker.predict(3, u"retry", *failed);
    notified.wait();
    auto retry = worker.take_prediction();
    require(retry && retry->succeeded && buffer.apply_prediction(*failed, std::move(retry->padding)),
            "retry did not apply a successful prediction");
    require(!buffer.waiting_for_prediction() && buffer.to_string() == u"新新",
            "successful retry did not release commit waiting");
    require(!buffer.commit_requires_prediction(), "successful retry did not release Enter waiting");
    buffer.cur().set_choose_index(1);
    require(buffer.to_string() == u"新舊", "predicted candidate could not be manually selected");
    require(buffer.commit_sample(u"").answer == buffer.to_string(), "commit and sample disagree");

    buffer.add(u'ㄋ');
    buffer.add(u'ㄧ');
    buffer.add(u'ˇ');
    require(buffer.waiting_for_prediction(), "new syllable did not block commit again");
    buffer.clear();
    require(!buffer.waiting_for_prediction(), "cancelled composition remained blocked");
    require(!buffer.commit_requires_prediction(), "cancelled composition retained Enter waiting");
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
        discard_reading_test();
        uncomposed_reading_test();
        single_reading_candidate_test();
        literal_only_reading_test();
        single_reading_failed_model_test();
        prediction_display_test();
        cancellation_test();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
