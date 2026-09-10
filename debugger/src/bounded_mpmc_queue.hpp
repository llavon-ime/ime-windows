#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace llavon::debug::internal {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

template <typename Value, std::size_t Capacity>
class BoundedMpmcQueue final {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "queue capacity must be a power of two");

    struct Cell {
        std::atomic<std::size_t> sequence{0};
        std::optional<Value> value;
    };

public:
    BoundedMpmcQueue() noexcept {
        for (std::size_t index = 0; index < Capacity; ++index) {
            cells_[index].sequence.store(index, std::memory_order_relaxed);
        }
    }

    bool try_push(Value value) noexcept {
        Cell* cell = nullptr;
        std::size_t position = enqueue_position_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[position & mask];
            const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::intptr_t>(sequence) -
                                    static_cast<std::intptr_t>(position);
            if (difference == 0) {
                if (enqueue_position_.compare_exchange_weak(
                        position, position + 1, std::memory_order_relaxed)) break;
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue_position_.load(std::memory_order_relaxed);
            }
        }
        cell->value.emplace(std::move(value));
        cell->sequence.store(position + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(Value& value) noexcept {
        Cell* cell = nullptr;
        std::size_t position = dequeue_position_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[position & mask];
            const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::intptr_t>(sequence) -
                                    static_cast<std::intptr_t>(position + 1);
            if (difference == 0) {
                if (dequeue_position_.compare_exchange_weak(
                        position, position + 1, std::memory_order_relaxed)) break;
            } else if (difference < 0) {
                return false;
            } else {
                position = dequeue_position_.load(std::memory_order_relaxed);
            }
        }
        value = std::move(*cell->value);
        cell->value.reset();
        cell->sequence.store(position + Capacity, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::size_t mask = Capacity - 1;
    alignas(64) std::array<Cell, Capacity> cells_{};
    alignas(64) std::atomic<std::size_t> enqueue_position_{0};
    alignas(64) std::atomic<std::size_t> dequeue_position_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace llavon::debug::internal
