#pragma once
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "bopomofo.hpp"
#include "engine/engine_types.hpp"
using namespace std::literals;

namespace tsf {

class Buffer {
protected:
    std::vector<BopomofoPos> buffer;

public:
    std::u16string to_string() const {
        std::u16string res;
        for (const auto& item : buffer) {
            res += item.current();
        }
        return res;
    }
    void clear() {
        buffer.clear();
    }
    bool empty() const {
        return buffer.empty();
    }
};

// The whole composition as sent to the backend, tagged with the content
// revision it was taken at.
struct ConversionSnapshot {
    std::uint64_t revision = 0;
    std::vector<BopomofoPos> entries;
};

struct MergeOutcome {
    bool changed = false;
    // The result for the locked entry, as {requested, converted}; merge it
    // once the entry is unlocked.
    std::optional<std::pair<BopomofoPos, BopomofoPos>> held;
};

class CompositionBuffer {
    std::vector<BopomofoPos> buffer;
    int idx = -1;
    bool immediate_conversion_enabled_ = false;
    std::uint64_t revision_ = 0;
    std::uint64_t next_id_ = 1;

    std::u16string display(const BopomofoPos& item) const {
        if (!immediate_conversion_enabled_ && item.conversion_pending && !item.chosen) {
            return item.to_toneless_bopomofo_string();
        }
        return item.current();
    }

    bool caret_on_entry() const {
        return idx >= 0 && idx < static_cast<int>(buffer.size());
    }

    void edited(BopomofoPos& item) {
        ++item.version;
        ++revision_;
    }

    BopomofoPos& insert_after_caret() {
        ++idx;
        auto& item = *buffer.insert(buffer.begin() + idx, BopomofoPos{});
        item.id = next_id_++;
        ++revision_;
        return item;
    }

    void erase_at_caret() {
        buffer.erase(buffer.begin() + idx);
        --idx;
        ++revision_;
    }

    int candidate_target_index() const {
        if (buffer.empty()) return -1;

        const int size = static_cast<int>(buffer.size());
        const int right_of_caret = idx + 1;
        if (right_of_caret >= 0 && right_of_caret < size) {
            return right_of_caret;
        }

        if (idx >= 0 && idx < size) {
            return idx;
        }

        return -1;
    }

    BopomofoPos* find(std::uint64_t id) {
        const auto it = std::ranges::find(buffer, id, &BopomofoPos::id);
        return it == buffer.end() ? nullptr : &*it;
    }

public:
    // Changes on every user edit of the content; caret movement and applying
    // a conversion leave it unchanged.
    std::uint64_t revision() const noexcept {
        return revision_;
    }
    void set_immediate_conversion(bool enabled) noexcept {
        immediate_conversion_enabled_ = enabled;
    }
    bool has_uncomposed_reading() const noexcept {
        return std::ranges::any_of(buffer, [](const auto& item) {
            return !item.is_compositable();
        });
    }
    // Some syllable has never been converted by the backend or the local
    // fallback; committing now would not show what the user is waiting for.
    bool has_pending_conversion() const noexcept {
        return std::ranges::any_of(buffer, [](const auto& item) {
            return item.conversion_pending;
        });
    }
    // The display still shows readings in place of converted text.
    bool shows_unconverted() const noexcept {
        return !immediate_conversion_enabled_ && has_pending_conversion();
    }
    bool ready_to_commit() const noexcept {
        return !buffer.empty() && !has_uncomposed_reading() && !has_pending_conversion();
    }
    // Only backend-converted or explicitly fixed output is used for training.
    bool output_is_trusted() const noexcept {
        return std::ranges::all_of(buffer, [](const auto& item) {
            return item.chosen || item.model_converted;
        });
    }
    bool pre() {
        if (idx < 0) return false;
        idx--;
        return true;
    }
    bool next() {
        if (idx + 1 >= static_cast<int>(buffer.size())) return false;
        idx++;
        return true;
    }
    bool remove_last() {
        if (!caret_on_entry()) return false;

        auto& item = buffer[idx];
        if (item.is_compositable()) {
            erase_at_caret();
        } else if (!item.pop_last_bopomofo() || item.is_null()) {
            erase_at_caret();
        } else {
            edited(item);
        }

        if (buffer.empty()) idx = -1;
        else if (idx >= static_cast<int>(buffer.size())) idx = static_cast<int>(buffer.size()) - 1;
        return true;
    }
    bool discard_current_reading() {
        if (!caret_on_entry() || buffer[idx].is_compositable()) {
            return false;
        }
        erase_at_caret();
        return true;
    }
    void clear() {
        ++revision_;
        buffer.clear();
        idx = -1;
    }
    bool empty() const {
        return buffer.empty();
    }
    bool current_invalid() const {
        return caret_on_entry() && buffer[idx].is_invalid();
    }
    bool current_compositable() const {
        return caret_on_entry() && buffer[idx].is_compositable();
    }
    bool current_has_candidate_list() const {
        const int target = candidate_target_index();
        if (target < 0) return false;
        const auto& item = buffer[target];
        // A syllable still shown as its reading has no meaningful list yet.
        if (!immediate_conversion_enabled_ && item.conversion_pending && !item.chosen) return false;
        return item.is_compositable() && item.get_candidate_texts().size() > 1;
    }
    size_t caret_offset() const {
        size_t offset = 0;
        for (int i = 0; i <= idx && i < static_cast<int>(buffer.size()); i++) {
            offset += display(buffer[i]).size();
        }
        return offset;
    }
    std::optional<std::pair<size_t, size_t>> current_invalid_span() const {
        if (!current_invalid()) {
            return std::nullopt;
        }

        size_t start = 0;
        for (int i = 0; i < idx; i++) {
            start += display(buffer[i]).size();
        }
        return std::pair{start, display(buffer[idx]).size()};
    }
    void add(char16_t ch) {
        if (current_invalid()) erase_at_caret();
        if (caret_on_entry() && buffer[idx].accept(ch)) {
            edited(buffer[idx]);
            return;
        }
        insert_after_caret().accept(ch);
    }
    void add_chosen_candidate(char32_t ch) {
        if (current_invalid()) erase_at_caret();
        insert_after_caret().set_chosen_candidate(ch);
    }
    // Selects the displayed candidate of the given entry; it stays fixed in
    // every later conversion.
    bool select_candidate(std::uint64_t id, std::size_t displayed_index) {
        auto* item = find(id);
        if (!item || !item->is_compositable() || displayed_index >= item->get_candidate_texts().size()) {
            return false;
        }
        if (const auto reading = item->multi_reading_literal();
            reading && displayed_index == item->get_candidates().size()) {
            // Freeze each symbol as ordinary text, just like a selected single
            // reading. The extra UI candidate never enters model candidates.
            const auto position = static_cast<int>(item - buffer.data());
            std::vector<BopomofoPos> literals;
            literals.reserve(reading->size());
            for (const char16_t ch : *reading) {
                auto& literal = literals.emplace_back();
                literal.id = next_id_++;
                literal.set_chosen_candidate(ch);
                literal.manually_selected = true;
            }
            const auto after = buffer.erase(buffer.begin() + position);
            buffer.insert(after, std::make_move_iterator(literals.begin()), std::make_move_iterator(literals.end()));
            // Finalizing a candidate advances once. When the caret was before
            // this entry, keep it just before the last symbol for that step.
            if (idx >= position - 1) idx += static_cast<int>(literals.size()) - 1;
            ++revision_;
            return true;
        }
        item->set_choose_index(static_cast<int>(displayed_index));
        edited(*item);
        return true;
    }
    // Nothing to send while a syllable is unfinished or nothing is convertible.
    std::optional<ConversionSnapshot> conversion_snapshot() const {
        if (buffer.empty()) return std::nullopt;
        if (!std::ranges::all_of(buffer, &BopomofoPos::is_predictable_by_engine)) return std::nullopt;
        if (std::ranges::all_of(buffer, &BopomofoPos::chosen)) return std::nullopt;
        return ConversionSnapshot{revision_, buffer};
    }
    // Applies a backend result to every entry that is still the one it was
    // computed for, skipping a locked entry (an open candidate list). Entries
    // edited since, added since, or already converted by a newer result keep
    // their state. Returns whether the visible output may have changed.
    MergeOutcome merge_conversion(std::uint64_t sequence, const std::vector<BopomofoPos>& requested,
                                  std::vector<BopomofoPos> converted,
                                  std::optional<std::uint64_t> locked_id = std::nullopt) {
        MergeOutcome outcome;
        if (requested.size() != converted.size()) return outcome;
        for (std::size_t index = 0; index < requested.size(); ++index) {
            auto& output = converted[index];
            if (!output.predicted || (!output.chosen && output.candidates.empty())) continue;
            auto* item = find(requested[index].id);
            if (!item || item->version != requested[index].version || item->chosen ||
                item->converted_by > sequence) {
                continue;
            }
            if (locked_id == item->id) {
                outcome.held.emplace(requested[index], std::move(output));
                continue;
            }
            item->candidates = std::move(output.candidates);
            item->choose_index = output.chosen ? output.choose_index : 0;
            item->chosen = output.chosen;
            item->conversion_pending = false;
            item->model_converted = true;
            item->converted_by = sequence;
            outcome.changed = true;
        }
        return outcome;
    }
    // The composition has to leave its field: readings typed so far become
    // fixed text exactly as shown, so the rest can still be converted.
    bool freeze_readings() {
        if (!has_uncomposed_reading()) return false;
        std::vector<BopomofoPos> frozen;
        frozen.reserve(buffer.size());
        int caret = -1;
        for (int index = 0; index < static_cast<int>(buffer.size()); ++index) {
            auto& item = buffer[index];
            if (item.is_compositable()) {
                frozen.push_back(std::move(item));
            } else {
                for (const char16_t ch : item.to_bopomofo_string()) {
                    auto& literal = frozen.emplace_back();
                    literal.id = next_id_++;
                    literal.set_chosen_candidate(ch);
                }
            }
            if (index == idx) caret = static_cast<int>(frozen.size()) - 1;
        }
        buffer = std::move(frozen);
        idx = caret;
        ++revision_;
        return true;
    }
    // Backend unavailable: show and accept the local table's first candidate
    // for every syllable still waiting.
    bool accept_local_candidates() {
        bool changed = false;
        for (auto& item : buffer) {
            if (item.conversion_pending) {
                item.conversion_pending = false;
                changed = true;
            }
        }
        return changed;
    }
    const std::vector<BopomofoPos>& entries() const noexcept {
        return buffer;
    }
    BopomofoPos& cur() {
        if (!caret_on_entry()) {
            throw std::runtime_error("out of range");
        }
        return buffer[idx];
    }
    const BopomofoPos& candidate_target() const {
        const int target = candidate_target_index();
        if (target < 0) {
            throw std::runtime_error("candidate target out of range");
        }
        return buffer[target];
    }
    // UTF-16 span of the candidate target in the displayed composition.
    std::pair<std::size_t, std::size_t> candidate_target_span() const {
        const int target = candidate_target_index();
        if (target < 0) throw std::runtime_error("candidate target out of range");
        std::size_t start = 0;
        for (int index = 0; index < target; ++index) {
            start += display(buffer[index]).size();
        }
        return {start, display(buffer[target]).size()};
    }
    std::u16string to_string() const {
        std::u16string res;
        for (const auto& item : buffer) {
            res += display(item);
        }
        return res;
    }
    // What the composition becomes when it has to end now: converted text,
    // the local candidate for syllables still waiting, and readings as typed.
    std::u16string final_text() const {
        std::u16string res;
        for (const auto& item : buffer) {
            res += item.current();
        }
        return res;
    }
    CommitSample commit_sample(std::u16string context) const {
        CommitSample sample;
        sample.context = std::move(context);
        sample.input.reserve(buffer.size());
        for (const auto& item : buffer) {
            const auto output = item.current();
            sample.answer += output;
            sample.input.push_back(CommitInputEntry{
                .reading = item.to_bopomofo_string(),
                .output = output,
                .manually_selected = item.was_manually_selected(),
            });
        }
        return sample;
    }
};

}  // namespace tsf
