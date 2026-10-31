#include "engine/engine_worker.hpp"
#include "engine/impl/engine.h"
#include "core/buffer.hpp"

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
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
    std::atomic<int> ready_calls = 0;
    std::atomic<int> commits = 0;
    std::atomic<int> mode_queries = 0;
};

class FakeEngine final : public tsf::IEngine {
public:
    explicit FakeEngine(std::shared_ptr<State> state) : state_(std::move(state)) {}
    IAsyncAction ready() override {
        ++state_->ready_calls;
        co_return;
    }
    IAsyncAction prefill(std::u16string context) override {
        std::lock_guard lock(state_->mutex);
        state_->prefills.push_back(std::move(context));
        co_return;
    }
    IAsyncOperation<bool> predict(std::u16string context, std::vector<tsf::BopomofoPos>& padding) override {
        {
            std::lock_guard lock(state_->mutex);
            state_->predictions.push_back(context);
        }
        if (context == u"blocked") {
            state_->entered.signal();
            co_await winrt::resume_on_signal(state_->release.handle);
            if (aborted_) co_return false;
        }
        if (context == u"lost") co_return false;
        for (auto& entry : padding) {
            entry.candidates = {U'新', U'舊'};
            entry.predicted = true;
        }
        co_return true;
    }
    IAsyncOperation<std::uint64_t> record_commit(tsf::CommitSample) override {
        co_return 100 + static_cast<std::uint64_t>(++state_->commits);
    }
    IAsyncAction discard_commit(std::uint64_t token) override {
        {
            std::lock_guard lock(state_->mutex);
            state_->discarded.push_back(token);
        }
        state_->complete.signal();
        co_return;
    }
    IAsyncOperation<std::uint8_t> toggle_input_mode() override {
        mode_ = mode_ == tsf::InputMode::Chinese ? tsf::InputMode::English : tsf::InputMode::Chinese;
        co_return static_cast<std::uint8_t>(mode_);
    }
    IAsyncOperation<std::uint8_t> current_input_mode() override {
        ++state_->mode_queries;
        co_return static_cast<std::uint8_t>(mode_);
    }
    void abort() noexcept override {
        aborted_ = true;
        state_->release.signal();
    }

private:
    std::shared_ptr<State> state_;
    tsf::InputMode mode_ = tsf::InputMode::Chinese;
    std::atomic_bool aborted_ = false;
};

// Stands in for the TSF thread: tasks posted by the worker run on drain().
struct UiQueue {
    std::mutex mutex;
    std::deque<tsf::EngineWorker::UiTask> tasks;
    Event posted;

    std::function<void(tsf::EngineWorker::UiTask)> poster() {
        return [this](tsf::EngineWorker::UiTask task) {
            std::lock_guard lock(mutex);
            tasks.push_back(std::move(task));
            posted.signal();
        };
    }
    void drain() {
        std::deque<tsf::EngineWorker::UiTask> ready;
        {
            std::lock_guard lock(mutex);
            ready.swap(tasks);
            ResetEvent(posted.handle);
        }
        for (auto& task : ready) task();
    }
};

struct Harness {
    std::shared_ptr<State> state = std::make_shared<State>();
    UiQueue ui;
    std::vector<tsf::EngineWorker::Prediction> results;
    std::vector<tsf::InputMode> modes;
    std::unique_ptr<tsf::EngineWorker> worker = std::make_unique<tsf::EngineWorker>(
        [state = state] { return std::make_unique<FakeEngine>(state); }, ui.poster(),
        tsf::EngineWorker::Callbacks{
            .prediction = [this](tsf::EngineWorker::Prediction result) { results.push_back(std::move(result)); },
            .input_mode_changed = [this](tsf::InputMode mode) { modes.push_back(mode); },
        });

    ~Harness() {
        state->release.signal();
        worker.reset();
    }

    const tsf::EngineWorker::Prediction* result(std::uint64_t sequence) const {
        for (const auto& item : results) {
            if (item.sequence == sequence) return &item;
        }
        return nullptr;
    }

    void drain_until(std::uint64_t sequence) {
        for (int attempt = 0; attempt < 50 && !result(sequence); ++attempt) {
            WaitForSingleObject(ui.posted.handle, 100);
            ui.drain();
        }
        require(result(sequence) != nullptr, "prediction result never reached the UI thread");
    }
};

std::vector<tsf::BopomofoPos> one_entry() {
    std::vector<tsf::BopomofoPos> padding(1);
    padding[0].compositable = true;
    padding[0].candidates = {U'舊'};
    return padding;
}

void worker_queue_test() {
    Harness h;
    const auto padding = one_entry();
    h.worker->predict(1, 7, u"blocked", padding);
    h.state->entered.wait();
    h.worker->prefill(u"superseded prefill");
    h.worker->prefill(u"latest prefill");
    h.worker->predict(2, 7, u"superseded", padding);
    h.worker->predict(3, 7, u"latest", padding);
    h.worker->ready();
    h.worker->ready();
    h.worker->refresh_input_mode();
    h.worker->refresh_input_mode();
    require(h.worker->cached_input_mode() == tsf::InputMode::English, "mode read waited on the transport");
    const auto commit = h.worker->record_commit({});
    h.worker->discard_commit(commit);
    h.state->release.signal();
    h.state->complete.wait();
    h.drain_until(3);

    require(h.result(1) && h.result(1)->status == tsf::EngineWorker::Prediction::Status::Converted,
            "in-flight prediction was not reported");
    require(h.result(2) && h.result(2)->status == tsf::EngineWorker::Prediction::Status::Superseded,
            "superseded queued prediction was not reported");
    require(h.result(3)->status == tsf::EngineWorker::Prediction::Status::Converted &&
                h.result(3)->padding[0].candidates.front() == U'新',
            "latest prediction output missing");
    require(h.worker->cached_input_mode() == tsf::InputMode::Chinese, "mode was not refreshed");
    require(h.state->ready_calls == 1 && h.state->mode_queries == 1, "requests were not coalesced");
    std::lock_guard lock(h.state->mutex);
    require(h.state->predictions == std::vector<std::u16string>{u"blocked", u"latest"},
            "superseded queued prediction was sent");
    require(h.state->prefills == std::vector<std::u16string>{u"latest prefill"},
            "superseded queued prefill was sent");
    require(h.state->discarded == std::vector<std::uint64_t>{101}, "commit/discard order lost");
}

void worker_groups_test() {
    Harness h;
    const auto padding = one_entry();
    h.worker->predict(1, 1, u"blocked", padding);
    h.state->entered.wait();
    // A composition that left its field keeps converting while a new one starts.
    h.worker->predict(2, 1, u"detached", padding);
    h.worker->predict(3, 2, u"live", padding);
    h.state->release.signal();
    h.drain_until(3);
    h.drain_until(2);
    require(h.result(2)->status == tsf::EngineWorker::Prediction::Status::Converted &&
                h.result(3)->status == tsf::EngineWorker::Prediction::Status::Converted,
            "a prediction of another composition was superseded");
}

void backend_lost_test() {
    Harness h;
    h.worker->refresh_input_mode();
    h.worker->predict(1, 1, u"lost", one_entry());
    h.drain_until(1);
    require(h.result(1)->status == tsf::EngineWorker::Prediction::Status::BackendLost,
            "a lost backend was reported as a conversion");
    require(h.worker->cached_input_mode() == tsf::InputMode::English && !h.modes.empty() &&
                h.modes.back() == tsf::InputMode::English,
            "a lost backend did not switch to English");
}

void shutdown_test() {
    Harness h;
    h.worker->predict(1, 1, u"blocked", one_entry());
    h.state->entered.wait();
    const auto start = std::chrono::steady_clock::now();
    h.worker.reset();
    require(std::chrono::steady_clock::now() - start < 2s, "shutdown waited for the backend");
}

// ---- composition buffer --------------------------------------------------

std::vector<tsf::BopomofoPos> converted(const tsf::ConversionSnapshot& snapshot, std::u32string_view text) {
    auto result = snapshot.entries;
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index].predicted = true;
        if (!result[index].chosen) result[index].candidates = {text[index], U'舊'};
    }
    return result;
}

void type(tsf::CompositionBuffer& buffer, std::u16string_view symbols) {
    for (const char16_t symbol : symbols) buffer.add(symbol);
}

void conversion_merge_test() {
    tsf::CompositionBuffer buffer;
    type(buffer, u"ㄋㄧˇ");
    require(buffer.to_string() == u"ㄋㄧ" && buffer.shows_unconverted() && !buffer.ready_to_commit(),
            "a converting syllable did not wait as its reading");
    const auto first = buffer.conversion_snapshot();
    require(first.has_value(), "a completed syllable was not sent");

    // Typing on while the conversion runs: the result still lands on the
    // syllable it was computed for.
    type(buffer, u"ㄏ");
    require(!buffer.conversion_snapshot(), "an unfinished syllable was sent");
    require(buffer.merge_conversion(1, first->entries, converted(*first, U"你")).changed &&
                buffer.to_string() == u"你ㄏ" && buffer.caret_offset() == 2,
            "a result was dropped because typing continued");
    type(buffer, u"ㄠˇ");
    const auto second = buffer.conversion_snapshot();
    require(second && second->revision != first->revision, "the whole composition was not sent again");
    require(buffer.merge_conversion(2, second->entries, converted(*second, U"妳好")).changed &&
                buffer.to_string() == u"妳好" && buffer.ready_to_commit() && buffer.output_is_trusted(),
            "the latest conversion did not update the whole composition");

    // An older result never overwrites a newer one.
    require(!buffer.merge_conversion(1, first->entries, converted(*first, U"泥")).changed &&
                buffer.to_string() == u"妳好",
            "an older result replaced a newer one");

    // A syllable deleted and retyped after the request keeps the new input.
    type(buffer, u"ㄇㄚ ");
    const auto third = buffer.conversion_snapshot();
    buffer.remove_last();
    type(buffer, u"ㄇㄚˇ");
    buffer.merge_conversion(3, third->entries, converted(*third, U"妳好嗎"));
    require(buffer.to_string() == u"妳好ㄇㄚ" && buffer.shows_unconverted(),
            "a result landed on a syllable edited after the request");
}

void candidate_lock_test() {
    tsf::CompositionBuffer buffer;
    type(buffer, u"ㄋㄧˇㄏㄠˇ");
    const auto snapshot = buffer.conversion_snapshot();
    buffer.merge_conversion(1, snapshot->entries, converted(*snapshot, U"你好"));
    buffer.pre();
    buffer.pre();
    const auto locked = buffer.candidate_target().id;
    const auto again = buffer.conversion_snapshot();
    const auto outcome = buffer.merge_conversion(2, again->entries, converted(*again, U"妳號"), locked);
    require(outcome.changed && outcome.held && buffer.to_string() == u"你號",
            "the entry under the open candidate list changed");
    buffer.merge_conversion(2, {outcome.held->first}, {outcome.held->second});
    require(buffer.to_string() == u"妳號", "the held result was not applied after the list closed");

    require(buffer.select_candidate(locked, 1) && buffer.to_string() == u"舊號",
            "selecting by entry id failed");
    const auto fixed = buffer.conversion_snapshot();
    require(fixed && fixed->entries.front().chosen, "a selection was not sent as fixed");
}

void detach_test() {
    tsf::CompositionBuffer buffer;
    buffer.add_chosen_candidate(U'你');
    type(buffer, u"ㄏㄠˇ");
    type(buffer, u"ㄇ");
    require(buffer.to_string() == u"你ㄏㄠㄇ", "unexpected display");
    buffer.freeze_readings();
    require(buffer.to_string() == u"你ㄏㄠㄇ" && !buffer.has_uncomposed_reading(),
            "freezing changed what the user sees");
    const auto snapshot = buffer.conversion_snapshot();
    require(snapshot.has_value(), "frozen text could not be converted");
    std::u32string text = U"你好ㄇ";
    require(buffer.merge_conversion(1, snapshot->entries, converted(*snapshot, text)).changed &&
                buffer.final_text() == u"你好ㄇ" && buffer.ready_to_commit(),
            "the conversion did not finish the frozen text");
}

void backend_lost_buffer_test() {
    tsf::CompositionBuffer buffer;
    type(buffer, u"ㄋㄧˇ");
    require(!buffer.ready_to_commit(), "converting text was ready to commit");
    require(buffer.final_text() != buffer.to_string(), "final text did not use the local candidate");
    require(buffer.accept_local_candidates() && buffer.ready_to_commit() && !buffer.output_is_trusted() &&
                buffer.to_string() == buffer.final_text(),
            "the local fallback did not become committable, untrusted output");
}

void discard_reading_test() {
    tsf::CompositionBuffer buffer;
    require(!buffer.discard_current_reading(), "empty composition discarded a reading");
    type(buffer, u"ㄋㄧ");
    require(buffer.discard_current_reading() && buffer.empty() && buffer.caret_offset() == 0,
            "unfinished syllable was not discarded as a whole");

    buffer.add_chosen_candidate(U'你');
    type(buffer, u"ㄏㄠ");
    require(buffer.discard_current_reading() && buffer.to_string() == u"你" && buffer.caret_offset() == 1,
            "discarding a reading changed the preceding converted text");

    type(buffer, u"ㄋㄧˇ");
    const auto pending = buffer.conversion_snapshot();
    require(!buffer.discard_current_reading() && buffer.shows_unconverted(),
            "completed syllable waiting for the backend was discarded");
    type(buffer, u"ㄏㄠ");
    require(buffer.discard_current_reading() && buffer.conversion_snapshot()->entries == pending->entries,
            "discarding the unfinished syllable changed the converting text");

    buffer.clear();
    buffer.add_chosen_candidate(U'你');
    type(buffer, u"ㄏㄠ");
    buffer.add_chosen_candidate(U'！');
    buffer.pre();
    require(buffer.discard_current_reading() && buffer.to_string() == u"你！" && buffer.caret_offset() == 1,
            "discarding a reading inside composition changed surrounding text or caret");
}

void uncomposed_reading_test() {
    tsf::CompositionBuffer buffer;
    buffer.add_chosen_candidate(U'你');
    buffer.add_chosen_candidate(U'好');
    require(!buffer.has_uncomposed_reading() && buffer.ready_to_commit(), "converted text blocked commit");
    buffer.add(u'ㄅ');
    require(buffer.has_uncomposed_reading() && !buffer.ready_to_commit(), "unfinished symbol allowed commit");
    buffer.add_chosen_candidate(U'！');
    require(buffer.has_uncomposed_reading(), "unfinished input away from the caret allowed commit");
    buffer.pre();
    type(buffer, u"ㄚˊ");
    require(!buffer.has_uncomposed_reading() && buffer.shows_unconverted(),
            "completed syllable did not wait for conversion");
    buffer.clear();
    type(buffer, u"ㄅㄦ ");
    require(buffer.current_invalid() && buffer.has_uncomposed_reading(), "invalid reading allowed commit");
}

void single_reading_candidate_test() {
    tsf::CompositionBuffer buffer;
    const std::u32string ranked = U"一衣依醫伊揖壹噫咿禕漪猗";
    for (const std::size_t count : {1u, 8u, 9u, 12u}) {
        buffer.clear();
        buffer.add(u'ㄧ');
        require(buffer.has_uncomposed_reading(), "single reading bypassed commit blocking");
        buffer.add(u' ');
        require(buffer.shows_unconverted() && !buffer.current_has_candidate_list(),
                "single reading bypassed model conversion");
        const auto snapshot = buffer.conversion_snapshot();
        require(snapshot && !snapshot->entries.front().chosen &&
                    snapshot->entries.front().to_bopomofo_string() == u"ㄧ ",
                "single reading was sent as a fixed literal instead of a first-tone syllable");
        auto result = snapshot->entries;
        const std::vector<char32_t> model_candidates(ranked.begin(), ranked.begin() + count);
        result.front().candidates = model_candidates;
        result.front().predicted = true;
        if (count == 1) result.front().choose_candidate(model_candidates.front());
        require(buffer.merge_conversion(1, snapshot->entries, std::move(result)).changed,
                "single reading conversion rejected");
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
        if (count > 1) {
            const auto next = buffer.conversion_snapshot();
            require(next && next->entries.front().candidates == model_candidates,
                    "display-only literal leaked into a later engine request");
        }
    }

    buffer.clear();
    buffer.add_chosen_candidate(U'你');
    buffer.add_chosen_candidate(U'好');
    type(buffer, u"ㄧ ");
    const auto snapshot = buffer.conversion_snapshot();
    auto result = snapshot->entries;
    for (auto& item : result) item.predicted = true;
    result.back().candidates = {U'一', U'衣'};
    require(buffer.merge_conversion(1, snapshot->entries, std::move(result)).changed,
            "mixed composition conversion rejected");
    buffer.add_chosen_candidate(U'！');
    buffer.pre();
    require(buffer.select_candidate(buffer.cur().id, 2) && buffer.to_string() == u"你好ㄧ！" &&
                buffer.caret_offset() == 3 && !buffer.has_uncomposed_reading(),
            "selecting a literal cleared composition or changed surrounding text or caret");
    const auto sample = buffer.commit_sample(u"");
    require(sample.input[2].reading.empty() && sample.input[2].output == u"ㄧ" &&
                sample.input[2].manually_selected,
            "selected literal did not become ordinary composition text");
    buffer.add(u'ㄋ');
    require(buffer.to_string() == u"你好ㄧㄋ！" && buffer.has_uncomposed_reading(),
            "further input merged with the selected literal");
}

void literal_only_reading_test() {
    for (const char16_t symbol : u"ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄖㄝㄟ"sv) {
        tsf::CompositionBuffer buffer;
        buffer.add(symbol);
        require(buffer.has_uncomposed_reading(), "unfinished single reading allowed commit");
        buffer.add(u' ');
        require(buffer.to_string() == std::u16string(1, symbol) && buffer.caret_offset() == 1 &&
                    buffer.ready_to_commit() && !buffer.shows_unconverted() && !buffer.conversion_snapshot(),
                "literal-only reading waited for the model or did not stay in composition");
    }

    for (const char16_t symbol : u"ㄓㄔㄕㄗㄘㄙㄧㄨㄩㄚㄛㄜㄞㄠㄡㄢㄣㄤㄥㄦ"sv) {
        tsf::CompositionBuffer buffer;
        buffer.add(symbol);
        buffer.add(u' ');
        const auto snapshot = buffer.conversion_snapshot();
        require(buffer.shows_unconverted() && snapshot && !snapshot->entries.front().chosen &&
                    snapshot->entries.front().to_bopomofo_string() == std::u16string(1, symbol) + u" ",
                "a single-symbol syllable with character candidates bypassed the model");
    }
}

void empty_result_test() {
    tsf::CompositionBuffer buffer;
    type(buffer, u"ㄧ ");
    const auto snapshot = buffer.conversion_snapshot();
    auto result = snapshot->entries;
    result.front().candidates.clear();
    result.front().predicted = true;
    require(!buffer.merge_conversion(1, snapshot->entries, std::move(result)).changed &&
                buffer.to_string() == u"ㄧ" && buffer.shows_unconverted() && !buffer.ready_to_commit(),
            "an empty model result was replaced by an unrequested literal fallback");
}

}  // namespace

int main() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        worker_queue_test();
        worker_groups_test();
        backend_lost_test();
        shutdown_test();
        conversion_merge_test();
        candidate_lock_test();
        detach_test();
        backend_lost_buffer_test();
        discard_reading_test();
        uncomposed_reading_test();
        single_reading_candidate_test();
        literal_only_reading_test();
        empty_result_test();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    } catch (const winrt::hresult_error& error) {
        std::fprintf(stderr, "%ls\n", error.message().c_str());
        return 1;
    }
}
