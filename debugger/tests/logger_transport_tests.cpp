#include "debugger/pipe_server.hpp"
#include "pipe_protocol.hpp"
#include "service/debug/core_logger_adapter.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using llavon::debug::LogInformation;
using namespace std::chrono_literals;

static_assert(llavon::debug::pipe_protocol::decode_information(0) == LogInformation::general);
static_assert(llavon::debug::pipe_protocol::decode_information(1) == LogInformation::context);
static_assert(llavon::debug::pipe_protocol::decode_information(2) == LogInformation::debug);
static_assert(llavon::debug::pipe_protocol::decode_information(255) == LogInformation::general);
static_assert(llavon::debug::pipe_protocol::decode_information(258) == LogInformation::general);

struct Record {
    LogInformation information;
    std::string message;
};

} // namespace

int main() {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Record> records;
    bool ready = false;
    std::thread::id factory_thread;
    int evaluations = 0;
    const auto caller_thread = std::this_thread::get_id();

    llavon::debugger::PipeServer server(
        [](int) {},
        [&](LogInformation information, std::string message) {
            std::lock_guard lock(mutex);
            if (message.ends_with("ready")) {
                ready = true;
            } else {
                records.push_back({information, std::move(message)});
            }
            changed.notify_all();
        });
    llavon::service::debug::CoreLoggerAdapter logger("candidate-ui-test");

    // The producer drops records until its asynchronous connection is ready.
    {
        std::unique_lock lock(mutex);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!ready && std::chrono::steady_clock::now() < deadline) {
            logger.log("ready");
            changed.wait_for(lock, 10ms, [&] { return ready; });
        }
        if (!ready) return EXIT_FAILURE;
    }

    using CoreInformation = llavon::ime::core::LogInformation;
    logger.log(CoreInformation::general, "diagnostics");
    logger.log(CoreInformation::context, "6\n中文中文");
    logger.log(CoreInformation::debug, "debug message 中文");
    logger.log(CoreInformation::debug, [&] {
        std::lock_guard lock(mutex);
        ++evaluations;
        factory_thread = std::this_thread::get_id();
        return std::string("lazy debug 中文");
    });

    std::unique_lock lock(mutex);
    if (!changed.wait_for(lock, 5s, [&] { return records.size() == 4; })) return EXIT_FAILURE;
    const auto matches = [&](std::size_t index, LogInformation information, const char* payload) {
        return records[index].information == information &&
               records[index].message.starts_with("[candidate-ui-test pid=") &&
               records[index].message.ends_with(std::string("] ") + payload);
    };
    return matches(0, LogInformation::general, "diagnostics") &&
                   matches(1, LogInformation::context, "6\n中文中文") &&
                   matches(2, LogInformation::debug, "debug message 中文") &&
                   matches(3, LogInformation::debug, "lazy debug 中文") &&
                   evaluations == 1 && factory_thread != caller_thread
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
