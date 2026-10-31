#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include "core/bopomofo.hpp"

namespace tsf {

// While the candidate list of an entry is open, results for that entry wait,
// so the list and its selection never change under the user.
class CandidateLock {
public:
    struct Held {
        std::uint64_t sequence = 0;
        BopomofoPos requested;
        BopomofoPos converted;
    };

    void open(std::uint64_t entry) noexcept { entry_ = entry; }
    std::optional<std::uint64_t> entry() const noexcept { return entry_; }

    // Keeps the newest result for the locked entry.
    void hold(std::uint64_t sequence, BopomofoPos requested, BopomofoPos converted) {
        if (held_ && held_->sequence > sequence) return;
        held_ = Held{sequence, std::move(requested), std::move(converted)};
    }

    // Unlocks; returns the result to merge now.
    std::optional<Held> close() noexcept {
        entry_.reset();
        return std::exchange(held_, std::nullopt);
    }

private:
    std::optional<std::uint64_t> entry_;
    std::optional<Held> held_;
};

}  // namespace tsf
