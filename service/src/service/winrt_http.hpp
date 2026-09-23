#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include <winrt/Windows.Foundation.h>

namespace llavon::service {

using HttpChunkSink = std::function<void(
    const std::uint8_t* bytes, std::uint32_t count,
    std::uint64_t received, std::uint64_t total)>;

// Only the dedicated service worker waits here; WinRT HTTP operations use
// co_await, and cancel() interrupts the currently awaited operation.
class WinrtHttpTransfer final {
public:
    void get_stream(std::wstring url, HttpChunkSink consume);
    void cancel() noexcept;
    void reset() noexcept;

private:
    std::mutex mutex_;
    winrt::Windows::Foundation::IAsyncAction active_{nullptr};
    bool cancelled_ = false;
};

}  // namespace llavon::service
