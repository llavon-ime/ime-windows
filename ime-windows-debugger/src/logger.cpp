#include <llavon-debug/logger.hpp>

#include "bounded_mpmc_queue.hpp"
#include "pipe_protocol.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace llavon::debug {
namespace {

constexpr std::size_t queue_capacity = 512;
constexpr DWORD reconnect_delay_ms = 250;

using PendingMessage = std::variant<std::string, Logger::MessageFactory>;

std::string materialize(PendingMessage message) {
    if (auto* text = std::get_if<std::string>(&message)) return std::move(*text);
    try {
        return std::get<Logger::MessageFactory>(message)();
    } catch (const std::exception& error) {
        return std::string("[LOGGER] message factory failed: ") + error.what();
    } catch (...) {
        return "[LOGGER] message factory failed";
    }
}

}  // namespace

class Logger::Impl final {
public:
    explicit Impl(std::string source) : source_(std::move(source)) {
        if (source_.empty()) source_ = "unknown";
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        queue_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!stop_event_ || !queue_event_) {
            close_events();
            throw std::runtime_error("unable to create logger events");
        }
        worker_ = std::thread([this] { run(); });
    }

    ~Impl() {
        connected_.store(false, std::memory_order_release);
        if (stop_event_) SetEvent(stop_event_);
        if (queue_event_) SetEvent(queue_event_);
        if (worker_.joinable()) worker_.join();
        close_events();
    }

    void push(PendingMessage message) noexcept {
        if (!connected_.load(std::memory_order_acquire)) return;
        if (!queue_.try_push(std::move(message))) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        SetEvent(queue_event_);
    }

private:
    void close_events() noexcept {
        if (queue_event_) {
            CloseHandle(queue_event_);
            queue_event_ = nullptr;
        }
        if (stop_event_) {
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
        }
    }

    bool stopping() const noexcept {
        return WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0;
    }

    void run() noexcept {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        while (!stopping()) {
            HANDLE pipe = CreateFileW(
                pipe_protocol::pipe_name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                WaitForSingleObject(stop_event_, reconnect_delay_ms);
                continue;
            }
            pump(pipe);
            connected_.store(false, std::memory_order_release);
            CancelIoEx(pipe, nullptr);
            CloseHandle(pipe);
            discard_pending();
        }
    }

    void pump(HANDLE pipe) noexcept {
        connected_.store(true, std::memory_order_release);
        HANDLE waits[] = {stop_event_, queue_event_};
        while (!stopping()) {
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wait != WAIT_OBJECT_0 + 1) return;

            PendingMessage pending;
            while (queue_.try_pop(pending)) {
                const auto dropped = dropped_.exchange(0, std::memory_order_relaxed);
                if (dropped != 0 &&
                    !write_message(pipe, "[LOGGER] dropped=" + std::to_string(dropped) +
                                             " reason=queue_full")) return;
                if (!write_message(pipe, materialize(std::move(pending)))) return;
            }
        }
    }

    bool write_message(HANDLE pipe, const std::string& message) noexcept {
        std::string payload;
        payload.reserve(source_.size() + message.size() + 32);
        payload += '[';
        payload += source_;
        payload += " pid=";
        payload += std::to_string(GetCurrentProcessId());
        payload += "] ";
        payload += message;
        if (payload.size() > pipe_protocol::maximum_message_size) return true;

        const auto length = static_cast<std::uint32_t>(payload.size());
        std::vector<std::uint8_t> frame(sizeof(length) + payload.size());
        std::memcpy(frame.data(), &length, sizeof(length));
        if (!payload.empty()) {
            std::memcpy(frame.data() + sizeof(length), payload.data(), payload.size());
        }
        return write_exact(pipe, frame.data(), frame.size());
    }

    bool write_exact(HANDLE pipe, const void* data, std::size_t size) noexcept {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) return false;
        const auto* cursor = static_cast<const std::uint8_t*>(data);
        std::size_t remaining = size;
        bool success = true;
        while (remaining != 0 && success) {
            ResetEvent(event);
            OVERLAPPED overlapped{};
            overlapped.hEvent = event;
            DWORD transferred = 0;
            if (!WriteFile(pipe, cursor, static_cast<DWORD>(remaining), &transferred, &overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) {
                    success = false;
                    break;
                }
                HANDLE waits[] = {stop_event_, event};
                const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wait != WAIT_OBJECT_0 + 1 ||
                    !GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
                    CancelIoEx(pipe, &overlapped);
                    GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
                    success = false;
                    break;
                }
            }
            if (transferred == 0) {
                success = false;
                break;
            }
            cursor += transferred;
            remaining -= transferred;
        }
        CloseHandle(event);
        return success;
    }

    void discard_pending() noexcept {
        PendingMessage pending;
        while (queue_.try_pop(pending)) {
        }
    }

    std::string source_;
    internal::BoundedMpmcQueue<PendingMessage, queue_capacity> queue_;
    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> dropped_{0};
    HANDLE stop_event_ = nullptr;
    HANDLE queue_event_ = nullptr;
    std::thread worker_;
};

Logger::Logger(std::string source) : impl_(std::make_unique<Impl>(std::move(source))) {}
Logger::~Logger() = default;

void Logger::log(std::string message) noexcept {
    impl_->push(PendingMessage(std::move(message)));
}

void Logger::log(MessageFactory make_message) noexcept {
    impl_->push(PendingMessage(std::move(make_message)));
}

}  // namespace llavon::debug
