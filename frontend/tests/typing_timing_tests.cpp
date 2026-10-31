// Timing tests for the composition state machine: a fast typist (bursts,
// typos, caret wandering, candidate picks, Enter right after the last tone
// and Backspace right after Enter, switching documents mid-word) against a backend with realistic latency and
// a document whose edit sessions may be deferred. Every interleaving must end
// with exactly the text the model produces for what the user finally typed.
//
// Every run happens twice: with readings shown while converting, and with
// local candidates shown at once; each rendering is checked against what
// that mode must show. The simulated runs use a virtual clock and are
// reproducible from their seed; the threaded run drives the real
// EngineWorker in real time.

#include "core/composition_controller.hpp"
#include "engine/engine_worker.hpp"
#include "engine/impl/engine.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using tsf::BopomofoPos;
using tsf::CompositionController;
using tsf::ConversionResult;
using tsf::DocumentId;
using Time = std::int64_t;  // microseconds
using Task = std::move_only_function<void()>;

constexpr Time ms(std::int64_t value) {
    return value * 1000;
}

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string utf8(std::u16string_view text) {
    std::string out;
    utf8::utf16to8(text.begin(), text.end(), std::back_inserter(out));
    return out;
}

class Rng {
public:
    explicit Rng(std::uint64_t seed) : engine_(seed) {}
    Time between(Time low, Time high) { return std::uniform_int_distribution<Time>(low, high)(engine_); }
    int index(std::size_t size) {
        return static_cast<int>(std::uniform_int_distribution<std::size_t>(0, size - 1)(engine_));
    }
    bool chance(double probability) { return std::bernoulli_distribution(probability)(engine_); }

private:
    std::mt19937_64 engine_;
};

// ---- the fake model ------------------------------------------------------
// Each reading has two characters; which one the model puts first depends on
// the following syllable, so a result computed for older content differs
// from the final one and stale results become visible if they ever win.

std::uint32_t mix(std::u16string_view text) {
    std::uint32_t hash = 2166136261u;
    for (const char16_t ch : text) hash = (hash ^ ch) * 16777619u;
    return hash;
}

std::pair<char32_t, char32_t> model_pair(std::u16string_view reading) {
    const auto slot = mix(reading) % 8000;
    return {static_cast<char32_t>(0x4E00 + 2 * slot), static_cast<char32_t>(0x4E00 + 2 * slot + 1)};
}

std::vector<char32_t> model_candidates(std::u16string_view reading, std::uint32_t next_key) {
    const auto [first, second] = model_pair(reading);
    return (next_key & 1) ? std::vector{second, first} : std::vector{first, second};
}

std::uint32_t key_of(const BopomofoPos& entry) {
    return entry.chosen ? static_cast<std::uint32_t>(entry.current32()) : mix(entry.to_bopomofo_string());
}

void convert(std::vector<BopomofoPos>& entries) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        auto& entry = entries[index];
        entry.predicted = true;
        if (entry.chosen) continue;
        const auto next = index + 1 < entries.size() ? key_of(entries[index + 1]) : 0u;
        entry.candidates = model_candidates(entry.to_bopomofo_string(), next);
    }
}

// What the user meant: a reading, or a fixed character (punctuation, a picked
// candidate, or reading symbols left unfinished when the composition ended).
struct Token {
    std::u16string reading;
    std::optional<char32_t> fixed;
};

std::u16string oracle(const std::vector<Token>& tokens) {
    std::u16string text;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto& token = tokens[index];
        if (token.fixed) {
            text.push_back(static_cast<char16_t>(*token.fixed));
            continue;
        }
        std::uint32_t next = 0;
        if (index + 1 < tokens.size()) {
            const auto& following = tokens[index + 1];
            next = following.fixed ? static_cast<std::uint32_t>(*following.fixed) : mix(following.reading);
        }
        text.push_back(static_cast<char16_t>(model_candidates(token.reading, next).front()));
    }
    return text;
}

char32_t local_candidate(std::u16string_view reading) {
    BopomofoPos entry;
    for (const char16_t symbol : reading) entry.accept(symbol);
    return entry.current32();
}

std::vector<std::u16string> vocabulary() {
    const std::vector<std::u16string> words = {
        u"ㄋㄧˇ", u"ㄏㄠˇ", u"ㄨㄛˇ", u"ㄕˋ",   u"ㄉㄜ˙", u"ㄓㄨㄥ ", u"ㄍㄨㄛˊ", u"ㄖㄣˊ",
        u"ㄒㄩㄝˊ", u"ㄕㄥ ", u"ㄐㄧㄣ ", u"ㄊㄧㄢ ", u"ㄑㄧˋ", u"ㄌㄜ˙", u"ㄉㄚˋ", u"ㄐㄧㄚ ",
        u"ㄏㄣˇ", u"ㄎㄞ ", u"ㄒㄧㄣ ", u"ㄇㄟˇ", u"ㄌㄞˊ", u"ㄑㄩˋ", u"ㄈㄢˋ", u"ㄕㄨ ",
    };
    std::vector<std::u16string> valid;
    for (const auto& word : words) {
        BopomofoPos entry;
        for (const char16_t symbol : word) entry.accept(symbol);
        if (entry.is_compositable() && !entry.chosen) valid.push_back(word);
    }
    require(valid.size() >= 12, "the Bopomofo table is missing too many test readings");
    return valid;
}

// ---- time ------------------------------------------------------------------

class Scheduler {
public:
    virtual ~Scheduler() = default;
    virtual Time now() const = 0;
    virtual void post(Time delay, Task task) = 0;
};

class SimulatedClock final : public Scheduler {
public:
    Time now() const override { return now_; }
    void post(Time delay, Task task) override {
        events_.push(Event{now_ + std::max<Time>(delay, 0), order_++, std::make_shared<Task>(std::move(task))});
    }
    void run() {
        while (!events_.empty()) {
            const auto event = events_.top();
            events_.pop();
            now_ = event.time;
            (*event.task)();
        }
    }

private:
    struct Event {
        Time time;
        std::uint64_t order;
        std::shared_ptr<Task> task;
        bool operator>(const Event& other) const {
            return time != other.time ? time > other.time : order > other.order;
        }
    };
    Time now_ = 0;
    std::uint64_t order_ = 0;
    std::priority_queue<Event, std::vector<Event>, std::greater<>> events_;
};

// Real time on the test thread; other threads may post.
class ThreadClock final : public Scheduler {
public:
    Time now() const override {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_)
            .count();
    }
    void post(Time delay, Task task) override {
        {
            std::lock_guard lock(mutex_);
            events_.push(Event{now() + std::max<Time>(delay, 0), order_++, std::make_shared<Task>(std::move(task))});
        }
        SetEvent(wake_);
    }
    // Runs due tasks until the deadline, or until done() holds.
    void run_until(Time deadline, const std::function<bool()>& done = {}) {
        for (;;) {
            std::shared_ptr<Task> task;
            Time next_due = deadline;
            {
                std::lock_guard lock(mutex_);
                if (!events_.empty() && events_.top().time <= now()) {
                    task = events_.top().task;
                    events_.pop();
                } else if (!events_.empty()) {
                    next_due = std::min(next_due, events_.top().time);
                }
            }
            if (task) {
                (*task)();
                continue;
            }
            if ((done && done()) || now() >= deadline) return;
            const auto wait = std::max<Time>(0, next_due - now());
            WaitForSingleObject(wake_, static_cast<DWORD>((wait + 999) / 1000));
        }
    }
    ~ThreadClock() override { CloseHandle(wake_); }

private:
    struct Event {
        Time time;
        std::uint64_t order;
        std::shared_ptr<Task> task;
        bool operator>(const Event& other) const {
            return time != other.time ? time > other.time : order > other.order;
        }
    };
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    mutable std::mutex mutex_;
    std::uint64_t order_ = 0;
    std::priority_queue<Event, std::vector<Event>, std::greater<>> events_;
    HANDLE wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
};

// ---- profiles ----------------------------------------------------------------

struct Profile {
    const char* name = "";
    bool immediate_conversion = false;
    int documents = 1;
    int compositions = 30;
    // Typist
    Time burst_low = ms(8), burst_high = ms(20);
    Time key_low = ms(25), key_high = ms(110);
    Time pause_low = ms(200), pause_high = ms(600);
    double burst = 0.12, pause = 0.05;
    double typo = 0.12, wander = 0.08, pick = 0.08, punctuation = 0.12;
    double quick_enter = 0.35;
    // Backspace right after Enter; sometimes past the start of the sentence.
    double erase_after_enter = 0.25;
    // The composition ends without Enter: switched documents or the host
    // ended it, possibly in the middle of a syllable.
    double cut = 0.05;
    // Backend
    Time service_low = ms(20), service_high = ms(150);
    double spike = 0.03;
    Time spike_high = ms(400);
    Time prefill_high = ms(30);
    Time dispatch_high = ms(2);
    // Document: share of edit requests granted a little later instead of at
    // once, and by how long at most.
    double deferred_edits = 0.5;
    Time defer_high = ms(8);
    // Share of edit requests the document holds back for a long while, as
    // Notepad does; keys keep coming meanwhile.
    double stall = 0.0;
    Time stall_low = ms(200), stall_high = ms(1500);
    // Kills the backend after this many compositions.
    std::optional<int> kill_after;
};

// ---- the document ----------------------------------------------------------
// Follows the TSF contract: the document is read and written only inside an
// edit session it grants, edits run in the order they were requested, and a
// grant may come much later than the request. The caret is always at the end.

struct Document {
    std::u16string text;
    std::optional<std::u16string> composition;
    std::optional<std::uint64_t> composition_epoch;
    struct Range {
        std::size_t start = 0;
        std::size_t length = 0;
    };
    std::map<std::uint64_t, Range> sealed;
};

struct Stats {
    std::vector<Time> conversion;  // syllable completed -> first shown converted
    std::vector<Time> sealing;     // sealed -> finished in place
    std::size_t deferred_edits = 0;
    std::size_t stalls = 0;
    std::size_t held_back = 0;
    std::size_t sealed_backspaces = 0;  // taken and applied in order
    std::size_t app_backspaces = 0;
};

class Host final : public tsf::CompositionHost {
    // Entry ids are per composition.
    using Syllable = std::pair<std::uint64_t, std::uint64_t>;

    // What a render queued at some moment must show.
    struct Expectation {
        std::uint64_t epoch = 0;
        std::u16string text;
        std::vector<Syllable> converted;
    };

    struct Queued {
        std::function<void()> edit;
        std::optional<Expectation> expectation;
    };

public:
    Host(Scheduler& clock, Rng& rng, const Profile& profile) : clock_(clock), rng_(rng), profile_(profile) {}

    CompositionController* controller = nullptr;
    std::map<DocumentId, Document> documents;
    std::vector<std::u16string> committed_answers;
    Stats stats;

    // Key handling and edit sessions run synchronously on the UI thread.
    void session(const std::function<void()>& body) {
        const bool outer = std::exchange(in_session_, true);
        body();
        in_session_ = outer;
    }

    void note_completed_syllables() {
        for (const auto& entry : controller->buffer().entries()) {
            const Syllable syllable{controller->epoch(), entry.id};
            if (entry.is_compositable() && !entry.chosen && !entry.model_converted && !reported_.contains(syllable)) {
                completed_.try_emplace(syllable, clock_.now());
            }
        }
    }

    bool edits_pending(DocumentId document) const {
        const auto queue = queues_.find(document);
        return queue != queues_.end() && !queue->second.empty();
    }

    bool any_edits_pending() const {
        return std::ranges::any_of(queues_, [](const auto& queue) { return !queue.second.empty(); });
    }

    // ---- DocumentEditor

    bool run_edit(DocumentId document, std::function<void()> edit) override {
        queues_[document].push_back({std::move(edit), expectation()});
        if (editing_ != document) request_grant(document);
        return true;
    }

    void erase_before_caret(DocumentId document) override {
        auto& target = edited(document, "Backspace applied");
        require(!target.composition, "Backspace applied inside a composition");
        if (target.text.empty()) return;
        require_outside_sealed(target, "erased sealed text before it was finished");
        target.text.pop_back();
    }

    // ---- CompositionView

    std::u16string preceding_text(DocumentId document, std::uint64_t) override {
        auto& target = edited(document, "preceding text read");
        require(!target.composition, "preceding text read while another composition is shown");
        return target.text;
    }

    void render(DocumentId document, std::uint64_t epoch, const tsf::Rendering& rendering,
                const tsf::E2eTrace*) override {
        auto& target = edited(document, "rendered");
        require(rendering.caret <= rendering.text.size(), "caret outside the composition");
        require(!target.composition || target.composition_epoch == epoch, "rendered over another composition");
        // Either the state the render was queued for, or, for a render made
        // inside an edit, the state now.
        auto matched = current_expectation_;
        if (!matched || matched->epoch != epoch || matched->text != rendering.text) {
            matched = controller->active() && controller->epoch() == epoch ? expectation() : std::nullopt;
        }
        require(matched && matched->text == rendering.text,
                "shows \"" + utf8(rendering.text) + "\" instead of \"" + (matched ? utf8(matched->text) : "") +
                    "\"");
        target.composition = rendering.text;
        target.composition_epoch = epoch;
        for (const auto& syllable : matched->converted) {
            if (const auto completed = completed_.extract(syllable)) {
                stats.conversion.push_back(clock_.now() - completed.mapped());
                reported_.insert(completed.key());
            }
        }
    }

    void commit(DocumentId document, std::uint64_t epoch) override {
        auto& target = shown(document, epoch, "committed");
        target.text += *target.composition;
        end_composition(target);
    }

    void discard(DocumentId document, std::uint64_t epoch) override {
        auto& target = edited(document, "discarded");
        if (!target.composition) return;
        require(target.composition_epoch == epoch, "discarded another composition");
        end_composition(target);
    }

    void seal(DocumentId document, std::uint64_t epoch) override {
        auto& target = shown(document, epoch, "sealed");
        target.sealed[epoch] = {target.text.size(), target.composition->size()};
        target.text += *target.composition;
        end_composition(target);
        sealed_at_[epoch] = clock_.now();
    }

    // ---- SealedTextStore

    std::optional<std::u16string> sealed_text(DocumentId document, std::uint64_t epoch) override {
        const auto& target = edited(document, "sealed text read");
        const auto found = target.sealed.find(epoch);
        if (found == target.sealed.end()) return std::nullopt;
        return target.text.substr(found->second.start, found->second.length);
    }

    void write_sealed(DocumentId document, std::uint64_t epoch, const std::u16string& text) override {
        auto& target = edited(document, "sealed text written");
        auto& range = sealed_range(target, epoch);
        target.text.replace(range.start, range.length, text);
        const auto delta = static_cast<std::ptrdiff_t>(text.size()) - static_cast<std::ptrdiff_t>(range.length);
        range.length = text.size();
        move_ranges_after(target, range.start, delta);
        written_.insert(epoch);
    }

    void erase_before_sealed(DocumentId document, std::uint64_t epoch, std::size_t count) override {
        auto& target = edited(document, "text erased");
        auto& range = sealed_range(target, epoch);
        const auto erased = std::min(count, range.start);
        const auto from = range.start - erased;
        for (const auto& [other_epoch, other] : target.sealed) {
            require(other_epoch == epoch || other.start + other.length <= from || other.start >= range.start,
                    "erased into sealed text that was not finished yet");
        }
        target.text.erase(from, erased);
        move_ranges_after(target, from, -static_cast<std::ptrdiff_t>(erased));
    }

    void release_sealed(DocumentId document, std::uint64_t epoch) override {
        auto& target = edited(document, "sealed text released");
        require(target.sealed.erase(epoch) == 1, "sealed text released twice");
        require(written_.erase(epoch) == 1, "sealed text was left as typed");
        stats.sealing.push_back(clock_.now() - sealed_at_[epoch]);
    }

    bool caret_after_sealed(DocumentId document, std::uint64_t epoch) override {
        const auto& target = edited(document, "caret read");
        const auto found = target.sealed.find(epoch);
        return !target.composition && found != target.sealed.end() &&
               found->second.start + found->second.length == target.text.size();
    }

    // ---- TrainingSink

    void record_commit(DocumentId document, std::uint64_t, tsf::CommitSample sample) override {
        edited(document, "commit recorded");
        committed_answers.push_back(std::move(sample.answer));
    }

    // ---- the application

    // Backspace the IME left to the application, which acts on it at once.
    void app_backspace(DocumentId document) {
        auto& target = documents[document];
        require(!edits_pending(document), "the application got Backspace ahead of queued edits");
        require(!target.composition, "the application got Backspace during a composition");
        if (target.text.empty()) return;
        require_outside_sealed(target, "the application erased sealed text before it was finished");
        target.text.pop_back();
    }

    // The application ends the composition it shows (focus moved, a click).
    // Like TextService, edits queued for it run first with the host's
    // cookie, so its text is complete when it is sealed. Returns false when
    // nothing is shown.
    bool terminate(DocumentId document) {
        auto& target = documents[document];
        if (!target.composition) return false;
        const auto terminated = target.composition_epoch;
        session([&] {
            const auto outer = std::exchange(editing_, document);
            if (controller->owns(document)) controller->seal();
            run_queued(document, [&] { return target.composition && target.composition_epoch == terminated; });
            if (target.composition && target.composition_epoch == terminated) {
                target.text += *target.composition;
                end_composition(target);
            }
            editing_ = outer;
        });
        if (edits_pending(document)) request_grant(document);
        return true;
    }

private:
    // What a render queued now must show, by the behaviour contract.
    std::optional<Expectation> expectation() {
        if (!controller->active()) return std::nullopt;
        Expectation expected{.epoch = controller->epoch()};
        for (const auto& entry : controller->buffer().entries()) {
            expected.text += expected_display(entry);
            if (entry.model_converted) expected.converted.push_back({controller->epoch(), entry.id});
        }
        return expected;
    }

    // What a syllable must look like (behaviour contract, items 2 and 5).
    std::u16string expected_display(const BopomofoPos& entry) {
        const Syllable syllable{controller->epoch(), entry.id};
        const auto converted = shown_converted_.find(syllable);
        const bool was_converted = converted != shown_converted_.end() && converted->second == entry.version;
        if (entry.chosen) return entry.current();
        if (!entry.is_compositable()) return entry.to_bopomofo_string();

        const auto reading = entry.to_bopomofo_string();
        if (entry.model_converted) {
            const auto [first, second] = model_pair(reading);
            require(entry.current32() == first || entry.current32() == second,
                    "a converted syllable shows something the model never produced");
            shown_converted_[syllable] = entry.version;
            return entry.current();
        }
        require(!was_converted, "a converted syllable went back to " +
                                    std::string(profile_.immediate_conversion ? "its local candidate" : "its reading"));
        if (entry.conversion_pending && !profile_.immediate_conversion) return entry.to_toneless_bopomofo_string();
        require(entry.current32() == local_candidate(reading),
                "a waiting syllable shows something other than its local candidate");
        return entry.current();
    }

    Document& edited(DocumentId document, const char* what) {
        require(editing_ == document, std::string(what) + " outside an edit session of its document");
        return documents[document];
    }

    Document& shown(DocumentId document, std::uint64_t epoch, const char* what) {
        auto& target = edited(document, what);
        require(target.composition && target.composition_epoch == epoch,
                std::string(what) + " a composition the document does not show");
        return target;
    }

    static void end_composition(Document& target) {
        target.composition.reset();
        target.composition_epoch.reset();
    }

    static void require_outside_sealed(const Document& target, const std::string& message) {
        for (const auto& [_, range] : target.sealed) {
            require(range.start + range.length < target.text.size(), message);
        }
    }

    // TSF grants a request made inside an edit session after it; others at
    // once, a little later, or, when the document holds edits back, much
    // later.
    void request_grant(DocumentId document) {
        if (editing_) {
            grant_after(document, 0);
            return;
        }
        auto& stalled = stalled_until_[document];
        if (clock_.now() < stalled) {
            grant_after(document, stalled - clock_.now());
        } else if (rng_.chance(profile_.stall)) {
            ++stats.stalls;
            stalled = clock_.now() + rng_.between(profile_.stall_low, profile_.stall_high);
            grant_after(document, stalled - clock_.now());
        } else if (rng_.chance(profile_.deferred_edits)) {
            ++stats.deferred_edits;
            grant_after(document, rng_.between(0, profile_.defer_high));
        } else {
            grant(document);
        }
    }

    void grant_after(DocumentId document, Time delay) {
        clock_.post(delay, [this, document] {
            session([&] {
                if (clock_.now() < stalled_until_[document]) {
                    grant_after(document, stalled_until_[document] - clock_.now());
                } else if (editing_) {
                    grant_after(document, 0);
                } else {
                    grant(document);
                }
            });
        });
    }

    void grant(DocumentId document) {
        editing_ = document;
        run_queued(document, [] { return true; });
        editing_.reset();
        // Caught up: the document shows exactly the state.
        auto& target = documents[document];
        if (controller->owns(document) && !controller->buffer().empty() && !edits_pending(document)) {
            require(target.composition && target.composition_epoch == controller->epoch() &&
                        *target.composition == controller->buffer().to_string(),
                    "the document did not catch up with the composition: shows \"" +
                        utf8(target.composition.value_or(u"<none>")) + "\" for epoch " +
                        std::to_string(target.composition_epoch.value_or(0)) + ", state \"" +
                        utf8(controller->buffer().to_string()) + "\" epoch " +
                        std::to_string(controller->epoch()));
        }
    }

    void run_queued(DocumentId document, const std::function<bool()>& more) {
        while (more()) {
            auto& queue = queues_[document];
            if (queue.empty()) return;
            auto next = std::move(queue.front());
            queue.pop_front();
            current_expectation_ = std::move(next.expectation);
            next.edit();
            current_expectation_.reset();
        }
    }

    static Document::Range& sealed_range(Document& target, std::uint64_t epoch) {
        const auto found = target.sealed.find(epoch);
        require(found != target.sealed.end(), "sealed text is not tracked");
        return found->second;
    }

    // Ranges starting after position follow an edit at position.
    static void move_ranges_after(Document& target, std::size_t position, std::ptrdiff_t delta) {
        for (auto& [_, other] : target.sealed) {
            if (other.start > position) other.start = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(other.start) + delta);
        }
    }

    Scheduler& clock_;
    Rng& rng_;
    const Profile& profile_;
    bool in_session_ = false;
    std::optional<DocumentId> editing_;
    std::map<DocumentId, std::deque<Queued>> queues_;
    std::map<DocumentId, Time> stalled_until_;
    std::optional<Expectation> current_expectation_;
    std::map<Syllable, Time> completed_;
    std::set<Syllable> reported_;
    // The entry version last shown converted by the model.
    std::map<Syllable, std::uint32_t> shown_converted_;
    std::map<std::uint64_t, Time> sealed_at_;
    std::set<std::uint64_t> written_;
};

// ---- the simulated backend ---------------------------------------------------
// One pipe, one request at a time; a queued prediction is superseded by the
// next one of its composition, like the real worker.

class SimulatedBackend final : public tsf::ConversionBackend {
public:
    SimulatedBackend(Scheduler& clock, Rng& rng, const Profile& profile, CompositionController& controller)
        : clock_(clock), rng_(rng), profile_(profile), controller_(controller) {}

    void prefill(std::u16string) override {
        std::erase_if(queue_, [](const Job& job) { return job.prefill; });
        queue_.push_back({.prefill = true});
        pump();
    }

    void predict(std::uint64_t sequence, std::uint64_t group, std::u16string,
                 std::vector<BopomofoPos> entries) override {
        if (dead_) {
            deliver({.sequence = sequence, .status = ConversionResult::Status::BackendLost});
            return;
        }
        std::erase_if(queue_, [&](const Job& job) {
            if (job.prefill || job.group != group) return false;
            deliver({.sequence = job.sequence, .status = ConversionResult::Status::Superseded});
            return true;
        });
        queue_.push_back({.sequence = sequence, .group = group, .entries = std::move(entries)});
        pump();
    }

    // The process dies: the broken pipe fails everything at once.
    void kill() {
        dead_ = true;
        if (busy_ && !in_flight_prefill_) {
            deliver({.sequence = in_flight_, .status = ConversionResult::Status::BackendLost});
        }
        ++generation_;
        busy_ = false;
        for (const auto& job : queue_) {
            if (!job.prefill) deliver({.sequence = job.sequence, .status = ConversionResult::Status::BackendLost});
        }
        queue_.clear();
    }

private:
    struct Job {
        bool prefill = false;
        std::uint64_t sequence = 0;
        std::uint64_t group = 0;
        std::vector<BopomofoPos> entries;
    };

    void deliver(ConversionResult result) {
        result.finished = std::chrono::steady_clock::now();
        clock_.post(rng_.between(0, profile_.dispatch_high),
                    [this, result = std::move(result)]() mutable { controller_.on_conversion(std::move(result)); });
    }

    Time service_time(bool prefill) {
        if (prefill) return rng_.between(ms(2), profile_.prefill_high);
        if (rng_.chance(profile_.spike)) return rng_.between(profile_.service_high, profile_.spike_high);
        return rng_.between(profile_.service_low, profile_.service_high);
    }

    void pump() {
        if (busy_ || queue_.empty() || dead_) return;
        busy_ = true;
        auto job = std::move(queue_.front());
        queue_.pop_front();
        in_flight_ = job.sequence;
        in_flight_prefill_ = job.prefill;
        clock_.post(service_time(job.prefill), [this, job = std::move(job), generation = generation_]() mutable {
            if (generation != generation_) return;
            busy_ = false;
            if (!job.prefill) {
                for (auto& entry : job.entries) entry.predicted = false;
                convert(job.entries);
                deliver({.sequence = job.sequence,
                         .status = ConversionResult::Status::Converted,
                         .padding = std::move(job.entries)});
            }
            pump();
        });
    }

    Scheduler& clock_;
    Rng& rng_;
    const Profile& profile_;
    CompositionController& controller_;
    std::deque<Job> queue_;
    bool busy_ = false;
    bool dead_ = false;
    std::uint64_t in_flight_ = 0;
    bool in_flight_prefill_ = false;
    std::uint64_t generation_ = 0;
};

// ---- the typist --------------------------------------------------------------

struct FinishedComposition {
    DocumentId document = 0;
    std::vector<Token> tokens;
    // Backspaces typed right after it ended.
    std::size_t erased = 0;
};

class Typist {
public:
    Typist(const Profile& profile, Rng& rng, CompositionController& controller, Host& host,
           std::vector<std::u16string> words)
        : profile_(profile), rng_(rng), controller_(controller), host_(host), words_(std::move(words)) {}

    std::function<void()> on_composition_finished;
    std::vector<FinishedComposition> finished;

    bool done() {
        if (steps_.empty() && planned_ < profile_.compositions) plan();
        return steps_.empty();
    }
    Time next_delay() const { return steps_.front().delay; }
    void perform() {
        auto step = std::move(steps_.front());
        steps_.pop_front();
        step.act();
    }

private:
    struct Step {
        Time delay;
        std::function<void()> act;
    };

    Time key_interval() {
        if (rng_.chance(profile_.burst)) return rng_.between(profile_.burst_low, profile_.burst_high);
        if (rng_.chance(profile_.pause)) return rng_.between(profile_.pause_low, profile_.pause_high);
        return rng_.between(profile_.key_low, profile_.key_high);
    }

    // Mirrors TextService: a key in another document first seals the
    // composition still open elsewhere.
    void in_document(DocumentId document) {
        if (controller_.active() && !controller_.owns(document)) controller_.seal();
    }

    void key(DocumentId document, char16_t symbol) {
        steps_.push_back({key_interval(), [this, document, symbol] {
            in_document(document);
            controller_.type_reading(document, symbol);
            host_.note_completed_syllables();
        }});
    }

    void finish(DocumentId document) {
        finished.push_back({document, std::move(tokens_)});
        tokens_.clear();
        if (on_composition_finished) on_composition_finished();
    }

    void plan() {
        ++planned_;
        if (profile_.documents > 1 && (switch_next_ || rng_.chance(0.3))) {
            document_ = document_ % static_cast<DocumentId>(profile_.documents) + 1;
        }
        switch_next_ = false;
        const auto document = document_;
        const int count = static_cast<int>(rng_.between(3, 12));
        for (int token = 0; token < count; ++token) {
            if (token > 0 && rng_.chance(profile_.punctuation)) {
                const char16_t mark = rng_.chance(0.5) ? u'，' : u'。';
                steps_.push_back({key_interval(), [this, document, mark] {
                    in_document(document);
                    controller_.type_text(document, std::u16string(1, mark));
                    tokens_.push_back({.fixed = mark});
                }});
                continue;
            }
            const auto& word = words_[rng_.index(words_.size())];
            if (rng_.chance(profile_.typo)) {
                if (rng_.chance(0.5)) {
                    // A whole wrong syllable, then Backspace removes it.
                    for (const char16_t symbol : words_[rng_.index(words_.size())]) key(document, symbol);
                } else {
                    key(document, words_[rng_.index(words_.size())].front());
                }
                steps_.push_back({key_interval(), [this, document] { controller_.backspace(document); }});
            }
            for (const char16_t symbol : word) key(document, symbol);
            steps_.push_back({0, [this, word] { tokens_.push_back({.reading = word}); }});
            if (rng_.chance(profile_.wander)) {
                const int distance = static_cast<int>(rng_.between(1, 3));
                for (int move = 0; move < distance; ++move) {
                    steps_.push_back({key_interval(), [this, document] { controller_.move_caret(document, false); }});
                }
                for (int move = 0; move < distance + 1; ++move) {
                    steps_.push_back({key_interval(), [this, document] { controller_.move_caret(document, true); }});
                }
            }
            if (rng_.chance(profile_.pick)) {
                steps_.push_back({key_interval(), [this, document] { try_pick(document); }});
            }
        }

        if (rng_.chance(profile_.cut)) {
            // Stops mid-syllable sometimes; the symbols stay as typed.
            const auto& word = words_[rng_.index(words_.size())];
            const auto typed = static_cast<std::size_t>(rng_.between(0, static_cast<Time>(word.size()) - 2));
            for (std::size_t index = 0; index < typed; ++index) key(document, word[index]);
            steps_.push_back({key_interval(), [this, document, word, typed] {
                for (std::size_t index = 0; index < typed; ++index) tokens_.push_back({.fixed = word[index]});
                if (profile_.documents > 1 && planned_ < profile_.compositions && rng_.chance(0.6)) {
                    // Sealed by the first key typed in the other document.
                    switch_next_ = true;
                } else if (!host_.terminate(document)) {
                    // Nothing shown yet for the host to end: focus moved.
                    controller_.seal();
                }
                finish(document);
            }});
            return;
        }

        const auto delay = rng_.chance(profile_.quick_enter) ? rng_.between(ms(5), ms(30)) : key_interval();
        steps_.push_back({delay, [this, document] {
            controller_.enter(document);
            finish(document);
        }});
        if (rng_.chance(profile_.erase_after_enter)) {
            const auto erased = rng_.chance(0.15) ? rng_.between(count, count + 4) : rng_.between(1, 2);
            for (Time index = 0; index < erased; ++index) {
                const auto interval = index == 0 ? rng_.between(ms(5), ms(40)) : key_interval();
                steps_.push_back({interval, [this, document] { backspace(document); }});
            }
        }
    }

    // Mirrors TextService: Backspace outside a composition goes to the
    // application only once the document has caught up.
    void backspace(DocumentId document) {
        in_document(document);
        if (controller_.sealed_pending(document) || host_.edits_pending(document)) {
            ++host_.stats.sealed_backspaces;
            controller_.backspace_in_order(document);
        } else {
            ++host_.stats.app_backspaces;
            host_.app_backspace(document);
        }
        ++finished.back().erased;
    }

    void try_pick(DocumentId document) {
        const auto& buffer = controller_.buffer();
        if (!controller_.owns(document) || !buffer.current_has_candidate_list()) return;
        const auto& target = buffer.candidate_target();
        const auto entry = target.id;
        const auto shown = target.get_candidates();
        controller_.candidates_opened(entry);
        const bool select = rng_.chance(0.7);
        steps_.push_front({rng_.between(ms(150), ms(400)), [this, document, entry, shown, select] {
            if (select) {
                controller_.select_candidate(entry, 1);
                controller_.candidate_finalized(document);
                tokens_.back().fixed = shown[1];
            }
            controller_.candidates_closed(document);
        }});
    }

    const Profile& profile_;
    Rng& rng_;
    CompositionController& controller_;
    Host& host_;
    std::vector<std::u16string> words_;
    std::deque<Step> steps_;
    std::vector<Token> tokens_;
    int planned_ = 0;
    DocumentId document_ = 1;
    bool switch_next_ = false;
};

// ---- verification ------------------------------------------------------------

void verify(const Profile& profile, std::uint64_t seed, const Host& host, const CompositionController& controller,
            const Typist& typist, bool backend_alive) {
    const auto where = std::string(profile.name) + " seed " + std::to_string(seed) + ": ";
    require(!controller.active(), where + "a composition was left open");
    require(controller.sealed_count() == 0, where + "sealed text was never finished");
    require(controller.outstanding_conversions() == 0, where + "a conversion never returned");
    require(!host.any_edits_pending(), where + "edits were left queued");

    // Backspaces after Enter act on the finished conversion, as if Enter had
    // waited for it.
    std::map<DocumentId, std::vector<Token>> tokens;
    std::map<DocumentId, std::u16string> expected;
    // Which composition each character still in a document came from.
    std::map<DocumentId, std::vector<std::size_t>> owners;
    std::vector<std::u16string> answers;
    std::vector<bool> touched(typist.finished.size(), false);
    for (std::size_t index = 0; index < typist.finished.size(); ++index) {
        const auto& composition = typist.finished[index];
        auto& sequence = tokens[composition.document];
        auto& text = expected[composition.document];
        auto& owner = owners[composition.document];
        sequence.insert(sequence.end(), composition.tokens.begin(), composition.tokens.end());
        text += oracle(composition.tokens);
        owner.insert(owner.end(), composition.tokens.size(), index);
        const auto erased = std::min(composition.erased, sequence.size());
        for (std::size_t back = 0; back < erased; ++back) touched[owner[owner.size() - 1 - back]] = true;
        sequence.resize(sequence.size() - erased);
        text.resize(text.size() - erased);
        owner.resize(owner.size() - erased);
        answers.push_back(oracle(composition.tokens));
    }
    // A composition the user erased into may have been recorded before the
    // erasure; one left whole must be.
    std::vector<std::u16string> kept_answers;
    for (std::size_t index = 0; index < answers.size(); ++index) {
        if (!touched[index]) kept_answers.push_back(answers[index]);
    }

    for (const auto& [id, document] : host.documents) {
        require(!document.composition, where + "the document still shows a composition");
        require(document.sealed.empty(), where + "the document still holds sealed text");
        if (backend_alive) {
            require(document.text == expected[id], where + "document " + std::to_string(id) + " reads \"" +
                                                       utf8(document.text) + "\" instead of \"" +
                                                       utf8(expected[id]) + "\"");
            continue;
        }
        // After the backend died, a syllable may hold either model output
        // or, as the last resort, the local candidate; never its reading.
        const auto& sequence = tokens[id];
        require(document.text.size() == sequence.size(), where + "text was lost or duplicated");
        for (std::size_t index = 0; index < sequence.size(); ++index) {
            const auto shown = static_cast<char32_t>(document.text[index]);
            const auto& token = sequence[index];
            if (token.fixed) {
                require(shown == *token.fixed, where + "a fixed character changed");
                continue;
            }
            const auto [first, second] = model_pair(token.reading);
            require(shown == first || shown == second || shown == local_candidate(token.reading),
                    where + "syllable " + std::to_string(index) + " of document " + std::to_string(id) + " (" +
                        utf8(token.reading) + ") reads \"" + utf8(document.text) + "\"");
        }
    }

    if (backend_alive) {
        // Text erased after it was recorded is withdrawn by TextService.
        auto committed = host.committed_answers;
        std::ranges::sort(committed);
        std::ranges::sort(answers);
        std::ranges::sort(kept_answers);
        std::vector<std::u16string> unexpected;
        std::vector<std::u16string> missing;
        std::ranges::set_difference(committed, answers, std::back_inserter(unexpected));
        std::ranges::set_difference(kept_answers, committed, std::back_inserter(missing));
        const auto list = [](const std::vector<std::u16string>& texts) {
            std::string joined;
            for (const auto& text : texts) joined += " \"" + utf8(text) + "\"";
            return joined;
        };
        require(unexpected.empty() && missing.empty(), where + "training samples do not match the committed text;" +
                                                           " unexpected:" + list(unexpected) +
                                                           " missing:" + list(missing));
    }
}

Time percentile(std::vector<Time> values, double share) {
    if (values.empty()) return 0;
    std::ranges::sort(values);
    return values[static_cast<std::size_t>(share * static_cast<double>(values.size() - 1))];
}

void report(const char* name, const Stats& stats, std::size_t runs) {
    std::printf("  %-34s runs=%zu syllables=%zu  shown converted p50=%lldms p99=%lldms max=%lldms  "
                "sealed finished p50=%lldms max=%lldms  deferred edits=%zu stalls=%zu  "
                "Backspace after Enter held=%zu passed=%zu\n",
                name, runs, stats.conversion.size(), percentile(stats.conversion, 0.5) / 1000,
                percentile(stats.conversion, 0.99) / 1000, percentile(stats.conversion, 1.0) / 1000,
                percentile(stats.sealing, 0.5) / 1000, percentile(stats.sealing, 1.0) / 1000,
                stats.deferred_edits, stats.stalls, stats.sealed_backspaces, stats.app_backspaces);
}

void simulate(const Profile& profile, std::uint64_t first_seed, int runs) {
    const auto words = vocabulary();
    Stats total;
    for (int run = 0; run < runs; ++run) {
        const auto seed = first_seed + static_cast<std::uint64_t>(run);
        Rng rng(seed);
        SimulatedClock clock;
        Host host(clock, rng, profile);
        CompositionController controller(host);
        SimulatedBackend backend(clock, rng, profile, controller);
        host.controller = &controller;
        controller.set_immediate_conversion(profile.immediate_conversion);
        controller.set_backend(&backend);
        Typist typist(profile, rng, controller, host, words);
        bool alive = true;
        typist.on_composition_finished = [&] {
            if (alive && profile.kill_after && static_cast<int>(typist.finished.size()) == *profile.kill_after) {
                alive = false;
                backend.kill();
            }
        };

        std::function<void()> next = [&] {
            if (typist.done()) return;
            clock.post(typist.next_delay(), [&] {
                host.session([&] { typist.perform(); });
                next();
            });
        };
        try {
            next();
            clock.run();
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string(profile.name) + " seed " + std::to_string(seed) + ": " + error.what());
        }

        verify(profile, seed, host, controller, typist, alive);
        total.conversion.insert(total.conversion.end(), host.stats.conversion.begin(), host.stats.conversion.end());
        total.sealing.insert(total.sealing.end(), host.stats.sealing.begin(), host.stats.sealing.end());
        total.deferred_edits += host.stats.deferred_edits;
        total.stalls += host.stats.stalls;
        total.sealed_backspaces += host.stats.sealed_backspaces;
        total.app_backspaces += host.stats.app_backspaces;
    }

    report(profile.name, total, static_cast<std::size_t>(runs));
    if (!profile.kill_after && profile.stall == 0.0) {
        // Bounded by the request ahead of it, its own service time, and the
        // requests of compositions sealed just before.
        const auto bound = 4 * profile.spike_high + profile.prefill_high + profile.defer_high + ms(10);
        require(percentile(total.conversion, 1.0) <= bound, std::string(profile.name) + ": conversion too late");
        require(percentile(total.sealing, 1.0) <= bound, std::string(profile.name) + ": sealed text finished too late");
    }
}

// ---- step by step ----------------------------------------------------------------

const char* mode_name(bool immediate) {
    return immediate ? "immediate candidates" : "readings while converting";
}

class Script {
public:
    explicit Script(const Profile& profile) : profile_(profile) {
        host_.controller = &controller_;
        controller_.set_immediate_conversion(profile.immediate_conversion);
        controller_.set_backend(&backend_);
    }

    void after(Time delay, std::function<void()> act) {
        at_ += delay;
        clock_.post(at_ - clock_.now(), [this, act = std::move(act)] { host_.session(act); });
    }

    std::vector<Token> type(std::initializer_list<std::u16string_view> words) {
        std::vector<Token> tokens;
        for (const auto word : words) {
            for (const char16_t symbol : word) {
                after(ms(12), [this, symbol] { controller_.type_reading(1, symbol); });
            }
            tokens.push_back({.reading = std::u16string(word)});
        }
        return tokens;
    }

    void enter(Time delay) {
        after(delay, [this] { controller_.enter(1); });
    }

    // sealed: the IME has to take the key and apply it in order. keeps_text:
    // the sealed text is not erased completely, so nothing changes until its
    // conversion lands.
    void backspace(Time delay, bool sealed, bool keeps_text = false) {
        after(delay, [this, sealed, keeps_text] {
            const auto before = visible();
            const bool held = controller_.sealed_pending(1);
            require(held || !sealed, "Backspace after sealed text went to the application");
            if (held || host_.edits_pending(1)) {
                controller_.backspace_in_order(1);
            } else {
                host_.app_backspace(1);
            }
            if (keeps_text) {
                require(visible() == before, "Backspace changed sealed text before its conversion: \"" +
                                                 utf8(before) + "\" -> \"" + utf8(visible()) + "\"");
            }
        });
    }

    void expect_pending() {
        after(0, [this] { require(controller_.sealed_count() > 0, "Enter did not wait for the conversion"); });
    }

    void expect_candidate_list(Time delay, bool available, const char* message) {
        after(delay, [this, available, message] {
            require(controller_.buffer().current_has_candidate_list() == available, message);
        });
    }

    // Opens the list of the syllable the candidate window would show.
    void open_candidates(Time delay) {
        after(delay, [this] {
            const auto& target = controller_.buffer().candidate_target();
            locked_ = target.id;
            listed_ = target.get_candidates();
            shown_ = target.current();
            controller_.candidates_opened(locked_);
        });
    }

    // A reply that arrived meanwhile must not touch the open list.
    void expect_list_unchanged(Time delay) {
        after(delay, [this] {
            require(controller_.outstanding_conversions() == 0, "the reply had not arrived yet");
            const auto& entries = controller_.buffer().entries();
            const auto entry = std::ranges::find(entries, locked_, &BopomofoPos::id);
            require(entry != entries.end() && entry->get_candidates() == listed_ && entry->current() == shown_,
                    "a reply changed the open candidate list");
        });
    }

    void pick(Time delay, std::size_t index) {
        after(delay, [this, index] {
            picked_ = listed_.at(index);
            controller_.select_candidate(locked_, index);
            controller_.candidate_finalized(1);
            controller_.candidates_closed(1);
        });
    }

    void close_candidates(Time delay) {
        after(delay, [this] { controller_.candidates_closed(1); });
    }

    char32_t picked() const noexcept { return picked_; }

    std::u16string run() {
        clock_.run();
        require(!controller_.active() && controller_.sealed_count() == 0 &&
                    controller_.outstanding_conversions() == 0 && !host_.any_edits_pending(),
                "the script left work unfinished");
        return text();
    }

private:
    std::u16string text() { return host_.documents[1].text; }
    std::u16string visible() {
        const auto& document = host_.documents[1];
        return document.text + document.composition.value_or(std::u16string{});
    }

    Profile profile_;
    Rng rng_{1};
    SimulatedClock clock_;
    Host host_{clock_, rng_, profile_};
    CompositionController controller_{host_};
    SimulatedBackend backend_{clock_, rng_, profile_, controller_};
    Time at_ = 0;
    std::uint64_t locked_ = 0;
    std::vector<char32_t> listed_;
    std::u16string shown_;
    char32_t picked_ = 0;
};

Profile steady(bool immediate, Time service = ms(100)) {
    return {.immediate_conversion = immediate,
            .service_low = service,
            .service_high = service,
            .spike = 0.0,
            .dispatch_high = 0};
}

void enter_then_backspace(bool immediate) {
    const auto profile = steady(immediate);
    const auto drop = [](std::u16string text, std::size_t count) {
        text.resize(text.size() - std::min(count, text.size()));
        return text;
    };

    {
        Script script(profile);
        const auto sentence = script.type({u"ㄋㄧˇ", u"ㄏㄠˇ", u"ㄨㄛˇ"});
        script.enter(ms(5));
        script.expect_pending();
        script.backspace(ms(5), true, true);
        const auto text = script.run();
        require(text == drop(oracle(sentence), 1), "Enter, Backspace: reads \"" + utf8(text) + "\"");
    }
    {
        // Typing on right away, then erasing past the start of the second
        // sentence while the first one is still converting.
        Script script(steady(immediate, ms(300)));
        const auto first = script.type({u"ㄋㄧˇ", u"ㄏㄠˇ", u"ㄨㄛˇ", u"ㄕˋ"});
        script.enter(ms(5));
        script.backspace(ms(5), true);
        const auto second = script.type({u"ㄉㄚˋ", u"ㄐㄧㄚ "});
        script.enter(ms(5));
        for (int key = 0; key < 4; ++key) script.backspace(ms(8), true);
        const auto text = script.run();
        require(text == drop(oracle(first), 3), "erasing across two sealed sentences: reads \"" + utf8(text) + "\"");
    }
    {
        // Converted before Enter: committed at once, Backspace is the
        // application's.
        Script script(profile);
        const auto sentence = script.type({u"ㄋㄧˇ", u"ㄏㄠˇ"});
        script.enter(ms(300));
        script.backspace(ms(5), false);
        const auto text = script.run();
        require(text == drop(oracle(sentence), 1), "committed, Backspace: reads \"" + utf8(text) + "\"");
    }
    {
        // Holding Backspace through all of it, and on into earlier text.
        Script script(profile);
        const auto first = script.type({u"ㄋㄧˇ", u"ㄏㄠˇ", u"ㄨㄛˇ"});
        script.enter(ms(300));
        script.type({u"ㄕˋ", u"ㄉㄜ˙"});
        script.enter(ms(5));
        script.backspace(ms(30), true);
        script.backspace(ms(30), true);
        script.backspace(ms(30), false);
        const auto text = script.run();
        require(text == drop(oracle(first), 1), "holding Backspace: reads \"" + utf8(text) + "\"");
    }
    std::printf("  Enter then Backspace (%s)%*sok\n", mode_name(immediate), immediate ? 1 : 3, "");
}

// Behaviour contract, item 6.
void candidate_lists(bool immediate) {
    const auto profile = steady(immediate);
    if (!immediate) {
        Script script(profile);
        script.type({u"ㄋㄧˇ", u"ㄏㄠˇ"});
        script.expect_candidate_list(ms(5), false, "a list opened on a syllable still shown as its reading");
        script.expect_candidate_list(ms(300), true, "no list on a converted syllable");
        script.enter(ms(5));
        script.run();
    } else {
        {
            // The local candidate picked while converting stays.
            Script script(profile);
            auto sentence = script.type({u"ㄋㄧˇ", u"ㄏㄠˇ"});
            script.expect_candidate_list(ms(5), true, "no list on a syllable showing its local candidate");
            script.open_candidates(0);
            script.expect_list_unchanged(ms(300));
            script.pick(ms(5), 1);
            script.enter(ms(300));
            const auto text = script.run();
            sentence.back() = {.fixed = script.picked()};
            require(text == oracle(sentence), "picked local candidate: reads \"" + utf8(text) + "\"");
        }
        {
            // Closing the list without a pick lets the held reply land.
            Script script(profile);
            const auto sentence = script.type({u"ㄋㄧˇ", u"ㄏㄠˇ"});
            script.open_candidates(ms(5));
            script.expect_list_unchanged(ms(300));
            script.close_candidates(ms(5));
            script.enter(ms(5));
            const auto text = script.run();
            require(text == oracle(sentence), "held reply: reads \"" + utf8(text) + "\"");
        }
    }
    std::printf("  candidate lists (%s)%*sok\n", mode_name(immediate), immediate ? 6 : 8, "");
}

// ---- the real worker, in real time -------------------------------------------

class TimedEngine final : public tsf::IEngine {
public:
    TimedEngine(std::uint64_t seed, Time low, Time high) : rng_(seed), low_(low), high_(high) {}
    IAsyncAction ready() override { co_await winrt::resume_after(std::chrono::milliseconds(1)); }
    IAsyncAction prefill(std::u16string) override { co_await winrt::resume_after(std::chrono::milliseconds(2)); }
    IAsyncOperation<bool> predict(std::u16string, std::vector<BopomofoPos>& padding) override {
        co_await winrt::resume_after(std::chrono::microseconds(rng_.between(low_, high_)));
        convert(padding);
        co_return true;
    }
    IAsyncOperation<std::uint64_t> record_commit(tsf::CommitSample) override { co_return 1; }
    IAsyncAction discard_commit(std::uint64_t) override { co_return; }
    IAsyncOperation<std::uint8_t> toggle_input_mode() override { co_return 0; }
    IAsyncOperation<std::uint8_t> current_input_mode() override { co_return 0; }
    void abort() noexcept override {}

private:
    Rng rng_;
    Time low_;
    Time high_;
};

void threaded(std::uint64_t seed, bool immediate) {
    Profile profile{.name = immediate ? "real worker, immediate candidates" : "real worker, readings",
                    .immediate_conversion = immediate,
                    .documents = 2,
                    .compositions = 6,
                    .key_low = ms(10),
                    .key_high = ms(45),
                    .pause_low = ms(60),
                    .pause_high = ms(120),
                    .quick_enter = 0.8,
                    .erase_after_enter = 0.6,
                    .cut = 0.2,
                    .defer_high = ms(4)};
    Rng rng(seed);
    ThreadClock clock;
    Host host(clock, rng, profile);
    CompositionController controller(host);
    host.controller = &controller;
    controller.set_immediate_conversion(immediate);
    tsf::EngineWorker worker(
        [seed] { return std::make_unique<TimedEngine>(seed, ms(3), ms(30)); },
        [&clock](tsf::EngineWorker::UiTask task) { clock.post(0, std::move(task)); },
        {.prediction = [&controller](ConversionResult result) { controller.on_conversion(std::move(result)); }});
    controller.set_backend(&worker);
    Typist typist(profile, rng, controller, host, vocabulary());

    while (!typist.done()) {
        clock.run_until(clock.now() + typist.next_delay());
        host.session([&] { typist.perform(); });
    }
    clock.run_until(clock.now() + ms(5000), [&] {
        return !controller.active() && controller.sealed_count() == 0 && controller.outstanding_conversions() == 0 &&
               !host.any_edits_pending();
    });
    verify(profile, seed, host, controller, typist, true);
    report(profile.name, host.stats, 1);
}

}  // namespace

int main() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        std::printf("fast typist timing:\n");
        const Profile profiles[] = {
            {.name = "typing"},
            {.name = "two documents, switching mid-word", .documents = 2, .cut = 0.3},
            {.name = "slow backend with spikes", .spike = 0.25, .spike_high = ms(450)},
            {.name = "edits always deferred", .deferred_edits = 1.0, .defer_high = ms(40)},
            {.name = "very fast bursts",
             .key_low = ms(10),
             .key_high = ms(40),
             .burst = 0.4,
             .pause = 0.0,
             .quick_enter = 0.8},
            {.name = "backend dies mid-session", .documents = 2, .cut = 0.2, .kill_after = 8},
            {.name = "document holds edits back", .documents = 2, .cut = 0.2, .stall = 0.03},
        };
        for (const bool immediate : {false, true}) {
            std::printf("%s:\n", mode_name(immediate));
            enter_then_backspace(immediate);
            candidate_lists(immediate);
            std::uint64_t seed = immediate ? 100000 : 1000;
            for (auto profile : profiles) {
                profile.immediate_conversion = immediate;
                simulate(profile, seed, profile.kill_after || profile.deferred_edits == 1.0 ? 200 : 300);
                seed += 1000;
            }
            for (std::uint64_t run = 1; run <= 3; ++run) threaded(run, immediate);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    } catch (const winrt::hresult_error& error) {
        std::fprintf(stderr, "%ls\n", error.message().c_str());
        return 1;
    }
}
