#include <llavon-debug/logger.hpp>

#include "bounded_mpmc_queue.hpp"

#include <atomic>
#include <cstdlib>
#include <string>
#include <utility>

int main() {
    using Factory = llavon::debug::Logger::MessageFactory;
    std::atomic<int> evaluations{0};
    llavon::debug::internal::BoundedMpmcQueue<Factory, 2> queue;

    Factory first = [&evaluations] {
        evaluations.fetch_add(1, std::memory_order_relaxed);
        return std::string("first");
    };
    Factory second = [&evaluations] {
        evaluations.fetch_add(1, std::memory_order_relaxed);
        return std::string("second");
    };
    Factory rejected = [&evaluations] {
        evaluations.fetch_add(1, std::memory_order_relaxed);
        return std::string("rejected");
    };

    if (!queue.try_push(std::move(first)) || !queue.try_push(std::move(second)) ||
        queue.try_push(std::move(rejected))) return EXIT_FAILURE;
    if (evaluations.load(std::memory_order_relaxed) != 0) return EXIT_FAILURE;

    Factory accepted;
    if (!queue.try_pop(accepted) || accepted() != "first") return EXIT_FAILURE;
    return evaluations.load(std::memory_order_relaxed) == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
}
