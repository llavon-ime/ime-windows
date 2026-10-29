#pragma once
#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "bopomofo.hpp"
#include "engine/impl/engine.h"
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

class CompositionBuffer {
    std::vector<BopomofoPos> buffer;
    std::mutex mutex;
    int idx = -1;
    bool immediate_conversion_enabled_ = false;

    std::u16string display(const BopomofoPos& item) const {
        if (!immediate_conversion_enabled_ && item.conversion_pending && !item.chosen) {
            return item.to_toneless_bopomofo_string();
        }
        return item.current();
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

public:
    void set_immediate_conversion(bool enabled) noexcept {
        immediate_conversion_enabled_ = enabled;
    }
    bool waiting_for_prediction() const noexcept {
        return !immediate_conversion_enabled_ && std::ranges::any_of(buffer, [](const auto& item) {
            return item.conversion_pending;
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
        std::lock_guard lock(mutex);
        if (idx < 0 || idx >= static_cast<int>(buffer.size())) return false;

        bool removed = false;
        if (buffer[idx].is_compositable()) {
            buffer.erase(buffer.begin() + idx);
            idx--;
            removed = true;
        } else {
            removed = buffer[idx].pop_last_bopomofo();
            if (!removed || buffer[idx].is_null()) {
                buffer.erase(buffer.begin() + idx);
                idx--;
                removed = true;
            }
        }

        if (buffer.empty()) {
            idx = -1;
            return removed;
        }

        if (idx >= static_cast<int>(buffer.size())) {
            idx = static_cast<int>(buffer.size()) - 1;
        }

        for (int i = 0; i <= idx; i++) {
            buffer[i].predicted = false;
        }

        return removed;
    }
    bool discard_current_reading() {
        std::lock_guard lock(mutex);
        if (idx < 0 || idx >= static_cast<int>(buffer.size()) || buffer[idx].is_compositable()) {
            return false;
        }

        buffer.erase(buffer.begin() + idx);
        --idx;
        return true;
    }
    void clear() {
        buffer.clear();
        idx = -1;
    }
    bool empty() const {
        return buffer.empty();
    }
    bool current_invalid() const {
        return idx >= 0 && idx < static_cast<int>(buffer.size()) && buffer[idx].is_invalid();
    }
    bool current_compositable() const {
        return idx >= 0 && idx < static_cast<int>(buffer.size()) && buffer[idx].is_compositable();
    }
    bool current_has_candidate_list() const {
        if (waiting_for_prediction()) return false;
        const int target = candidate_target_index();
        return target >= 0 && buffer[target].is_compositable() && buffer[target].get_candidates().size() > 1;
    }
    void invalidate_all_predictions() {
        for (int i = 0; i < static_cast<int>(buffer.size()); i++) {
            buffer[i].predicted = false;
        }
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
        std::lock_guard lock(mutex);
        if (idx >= 0 && idx < static_cast<int>(buffer.size()) && buffer[idx].is_invalid()) {
            buffer.erase(buffer.begin() + idx);
            idx--;
        }
        if (idx == -1 || !buffer[idx].accept(ch)) {
            idx++;
            // buffer.insert(buffer.begin() + idx, {}); 會炸 我操你媽的標準委員會
            buffer.insert(buffer.begin() + idx, BopomofoPos{});
            buffer[idx].accept(ch);
        } else {
            // current accept this ch
            // do nothing
        }
    }
    void add_chosen_candidate(char32_t ch) {
        std::lock_guard lock(mutex);
        if (idx >= 0 && idx < static_cast<int>(buffer.size()) && buffer[idx].is_invalid()) {
            buffer.erase(buffer.begin() + idx);
            idx--;
        }

        idx++;
        buffer.insert(buffer.begin() + idx, BopomofoPos{});
        buffer[idx].set_chosen_candidate(ch);

        for (int i = 0; i <= idx; i++) {
            buffer[i].predicted = false;
        }
        buffer[idx].predicted = true;
    }
    std::optional<std::vector<BopomofoPos>> prediction_snapshot() const {
        if (buffer.empty()) return std::nullopt;
        bool need_predict = false;
        for (const auto& item : buffer) {
            if (!item.is_predictable_by_engine()) {
                return std::nullopt;
            }
            need_predict |= !item.predicted;
        }
        if (!need_predict) return std::nullopt;
        return buffer;
    }
    bool matches_prediction(const std::vector<BopomofoPos>& snapshot) const {
        return buffer == snapshot;
    }
    bool apply_prediction(const std::vector<BopomofoPos>& snapshot,
                          std::vector<BopomofoPos> result) {
        if (result.size() != buffer.size() || !matches_prediction(snapshot)) return false;
        if (!std::ranges::all_of(result, [](const auto& item) {
                return item.predicted && (item.chosen || !item.candidates.empty());
            })) return false;
        for (auto& item : result) item.conversion_pending = false;
        buffer = std::move(result);
        return true;
    }
    BopomofoPos& cur() {
        if (idx < 0 || idx >= buffer.size()) {
            throw std::runtime_error("out of range");
        }
        return buffer[idx];
    }
    BopomofoPos& candidate_target() {
        const int target = candidate_target_index();
        if (target < 0 || target >= static_cast<int>(buffer.size())) {
            throw std::runtime_error("candidate target out of range");
        }
        return buffer[target];
    }
    std::u16string to_string() const {
        std::u16string res;
        for (const auto& item : buffer) {
            res += display(item);
        }
        return res;
    }
    CommitSample commit_sample(std::u16string context) const {
        CommitSample sample;
        sample.context = std::move(context);
        sample.input.reserve(buffer.size());
        for (const auto& item : buffer) {
            const auto output = display(item);
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
